// The client half of the command/reply proof. Sends "ping N" as command N's body, waits
// for reply N's body to read "pong N", and fails on the first mismatch or timeout -- same
// bar as proof/subscriber.cpp: a message that arrives is not proof on its own.
//
// Raw ABI here, not weft::run_command_loop -- the loop is the *server* shape (one `ask`
// answering many commands); a client sending one command and waiting for its own reply is
// a different shape, and inventing a client-side loop wrapper just to keep this proof
// symmetric would be exactly the kind of premature abstraction this repo's own philosophy
// (`README.md`: "each would grow its own copy... and the copies would drift") argues
// against for a two-line difference.
//
// SPDX-License-Identifier: Apache-2.0
#include "iox2_api.h"
#include "weft/bus.hpp"
#include "weft/command.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char **argv) {
	const int count = (argc > 1) ? std::atoi(argv[1]) : 8;

	if (!weft::load_bus()) {
		std::fprintf(stderr, "command_publisher: no bus\n");
		return 1;
	}
	iox2_set_log_level_from_env_or(iox2_log_level_e_ERROR);

	iox2_node_h node = nullptr;
	if (iox2_node_builder_create(iox2_node_builder_new(nullptr), nullptr,
				 iox2_service_type_e_IPC, &node) != IOX2_OK) {
		std::fprintf(stderr, "command_publisher: no node\n");
		return 1;
	}

	auto open_service = [&](const char *name) -> iox2_port_factory_pub_sub_h {
		iox2_service_name_h svc_name = nullptr;
		if (iox2_service_name_new(nullptr, name, std::strlen(name), &svc_name) != IOX2_OK) {
			return nullptr;
		}
		auto builder = iox2_service_builder_pub_sub(
				iox2_node_service_builder(&node, nullptr, iox2_cast_service_name_ptr(svc_name)));
		if (iox2_service_builder_pub_sub_set_payload_type_details(&builder,
					iox2_type_variant_e_DYNAMIC, weft::PAYLOAD_TYPE, std::strlen(weft::PAYLOAD_TYPE),
					1, 1) != IOX2_OK) {
			iox2_service_name_drop(svc_name);
			return nullptr;
		}
		iox2_port_factory_pub_sub_h service = nullptr;
		const int rc = iox2_service_builder_pub_sub_open_or_create(builder, nullptr, &service);
		iox2_service_name_drop(svc_name);
		return rc == IOX2_OK ? service : nullptr;
	};

	iox2_port_factory_pub_sub_h cmd_service = open_service(weft::COMMAND_SERVICE_NAME);
	iox2_port_factory_pub_sub_h reply_service = open_service(weft::REPLY_SERVICE_NAME);
	if (!cmd_service || !reply_service) {
		std::fprintf(stderr, "command_publisher: no service\n");
		return 1;
	}

	auto pub_builder = iox2_port_factory_pub_sub_publisher_builder(&cmd_service, nullptr);
	iox2_port_factory_publisher_builder_set_initial_max_slice_len(&pub_builder, weft::MESSAGE_BYTES);
	iox2_publisher_h pub = nullptr;
	if (iox2_port_factory_publisher_builder_create(pub_builder, nullptr, &pub) != IOX2_OK) {
		std::fprintf(stderr, "command_publisher: no publisher\n");
		return 1;
	}

	iox2_subscriber_h sub = nullptr;
	if (iox2_port_factory_subscriber_builder_create(
				iox2_port_factory_pub_sub_subscriber_builder(&reply_service, nullptr), nullptr,
				&sub) != IOX2_OK) {
		std::fprintf(stderr, "command_publisher: no reply subscriber\n");
		return 1;
	}

	const int give_up_after = weft::limits::ACTION_MS / 10;

	for (int tick = 1; tick <= count; ++tick) {
		const std::string body = "ping " + std::to_string(tick);
		const size_t total = weft::HEADER_BYTES + body.size();

		iox2_sample_mut_h sample = nullptr;
		if (iox2_publisher_loan_slice_uninit(&pub, nullptr, &sample, total) != IOX2_OK) {
			std::fprintf(stderr, "command_publisher: no loan at tick %d\n", tick);
			return 1;
		}
		void *payload = nullptr;
		size_t elements = 0;
		iox2_sample_mut_payload_mut(&sample, &payload, &elements);
		weft::command_write_header((unsigned char *)payload, (std::uint64_t)tick);
		std::memcpy((unsigned char *)payload + weft::HEADER_BYTES, body.data(), body.size());
		if (iox2_sample_mut_send(sample, nullptr) != IOX2_OK) {
			std::fprintf(stderr, "command_publisher: send failed at tick %d\n", tick);
			return 1;
		}

		const std::string want = "pong " + std::to_string(tick);
		bool matched = false;
		for (int idle = 0; idle < give_up_after && !matched; ++idle) {
			iox2_sample_h reply = nullptr;
			if (iox2_subscriber_receive(&sub, nullptr, &reply) != IOX2_OK) {
				std::fprintf(stderr, "command_publisher: reply receive failed\n");
				return 1;
			}
			if (!reply) {
				(void)iox2_node_wait(&node, 0, 10 * 1000 * 1000);
				continue;
			}
			const void *rp = nullptr;
			size_t rn = 0;
			iox2_sample_payload(&reply, &rp, &rn);
			if (rn < weft::HEADER_BYTES) {
				iox2_sample_drop(reply);
				continue;
			}
			const std::uint64_t rid = weft::command_read_header((const unsigned char *)rp);
			std::string got((const char *)rp + weft::HEADER_BYTES, rn - weft::HEADER_BYTES);
			iox2_sample_drop(reply);
			if (rid != (std::uint64_t)tick) {
				continue; // a reply to an earlier tick, racing this one; keep waiting
			}
			if (got != want) {
				std::fprintf(stderr, "command_publisher: tick %d wanted '%s' got '%s'\n", tick,
						want.c_str(), got.c_str());
				return 1;
			}
			matched = true;
		}
		if (!matched) {
			std::fprintf(stderr, "command_publisher: tick %d timed out waiting for a reply\n", tick);
			return 1;
		}
		std::printf("command_publisher: tick %d ok\n", tick);
		std::fflush(stdout);
	}

	std::printf("command_publisher: sent and confirmed %d\n", count);
	return 0;
}
