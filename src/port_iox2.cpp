// The bus seam over iceoryx2: a publisher on the command service, a subscriber on the reply
// service. The mirror image of what `weft::run_command_loop` opens on the interactor's side,
// and it must stay that way -- the two agree on the service names, the payload variant and
// the request-id envelope, all of which come from `weft/command.hpp` rather than from either
// side's own constants.
//
// UNPROVEN against real iceoryx2. The harness's own README says the same of the loop this
// talks to: `run_command_loop` ran on Windows 11 against iceoryx2 v0.9.3 and has never run on
// Linux, which is this stack's target. Nothing here has run at all. `proof/roundtrip.cpp`
// covers the correlation and the deadline and cannot cover this file, so a first run against
// a real bus is still owed and this comment is where it gets replaced by a log.
//
// SPDX-License-Identifier: Apache-2.0

#include "runpod/port.h"

#include "iox2_api.h"
#include "weft/bus.hpp"
#include "weft/command.hpp"

#include <cstdio>
#include <cstring>
#include <new>

namespace {

struct Iox2Port {
	iox2_node_h node = nullptr;
	iox2_port_factory_pub_sub_h cmd_service = nullptr;
	iox2_port_factory_pub_sub_h reply_service = nullptr;
	iox2_publisher_h pub = nullptr;
	iox2_subscriber_h sub = nullptr;
	bool open = false;
};

// Same builder call `weft::run_command_loop` makes, and same reason for the DYNAMIC variant:
// a command is however long a caller wrote it, so the slice length is chosen per loan.
iox2_port_factory_pub_sub_h open_service(iox2_node_h *node, const char *name) {
	iox2_service_name_h svc_name = nullptr;
	if (iox2_service_name_new(nullptr, name, std::strlen(name), &svc_name) != IOX2_OK) {
		return nullptr;
	}
	auto builder = iox2_service_builder_pub_sub(
			iox2_node_service_builder(node, nullptr, iox2_cast_service_name_ptr(svc_name)));
	if (iox2_service_builder_pub_sub_set_payload_type_details(&builder,
				iox2_type_variant_e_DYNAMIC, weft::PAYLOAD_TYPE, std::strlen(weft::PAYLOAD_TYPE), 1,
				1) != IOX2_OK) {
		iox2_service_name_drop(svc_name);
		return nullptr;
	}
	iox2_port_factory_pub_sub_h service = nullptr;
	const int rc = iox2_service_builder_pub_sub_open_or_create(builder, nullptr, &service);
	iox2_service_name_drop(svc_name);
	return rc == IOX2_OK ? service : nullptr;
}

int port_send(void *ctx, const unsigned char *msg, size_t n) {
	Iox2Port *p = (Iox2Port *)ctx;
	if (!p->open) {
		return 1;
	}
	iox2_sample_mut_h out = nullptr;
	if (iox2_publisher_loan_slice_uninit(&p->pub, nullptr, &out, n) != IOX2_OK) {
		return 1;
	}
	void *payload = nullptr;
	size_t cap = 0;
	iox2_sample_mut_payload_mut(&out, &payload, &cap);
	std::memcpy(payload, msg, n);
	return iox2_sample_mut_send(out, nullptr) == IOX2_OK ? 0 : 1;
}

size_t port_recv(void *ctx, unsigned char *buf, size_t cap, uint64_t wait_ns) {
	Iox2Port *p = (Iox2Port *)ctx;
	if (!p->open) {
		return 0;
	}
	iox2_sample_h sample = nullptr;
	if (iox2_subscriber_receive(&p->sub, nullptr, &sample) != IOX2_OK) {
		return 0;
	}
	if (!sample) {
		// The one place this file blocks. iox2_node_wait is a periodic sleep, which is all a
		// worker waiting on one reply needs; the WaitSet the harness README describes is for
		// a process holding a socket as well, and this one holds none.
		(void)iox2_node_wait(&p->node, 0, (uint32_t)wait_ns);
		return 0;
	}
	const void *payload = nullptr;
	size_t n = 0;
	iox2_sample_payload(&sample, &payload, &n);
	const size_t take = n < cap ? n : cap;
	std::memcpy(buf, payload, take);
	iox2_sample_drop(sample);
	return take;
}

void port_close(void *ctx) {
	Iox2Port *p = (Iox2Port *)ctx;
	if (p->open) {
		iox2_publisher_drop(p->pub);
		iox2_subscriber_drop(p->sub);
		iox2_port_factory_pub_sub_drop(p->reply_service);
		iox2_port_factory_pub_sub_drop(p->cmd_service);
		iox2_node_drop(p->node);
	}
	delete p;
}

} // namespace

bus_port_t bus_port_iox2_open(void) {
	Iox2Port *p = new Iox2Port();
	bus_port_t port = { port_send, port_recv, port_close, p };

	// Every failure below leaves `open` false and returns the port anyway. A worker that
	// cannot reach the bus must still take the job it was handed and answer it with an
	// error, because a worker that exits before its first job-take looks to RunPod like a
	// worker that was never asked.
	if (!weft::load_bus()) {
		return port;
	}
	iox2_set_log_level_from_env_or(iox2_log_level_e_ERROR);

	if (iox2_node_builder_create(iox2_node_builder_new(nullptr), nullptr,
				iox2_service_type_e_IPC, &p->node) != IOX2_OK) {
		std::fprintf(stderr, "runpod: no iceoryx2 node\n");
		return port;
	}
	p->cmd_service = open_service(&p->node, weft::COMMAND_SERVICE_NAME);
	p->reply_service = open_service(&p->node, weft::REPLY_SERVICE_NAME);
	if (!p->cmd_service || !p->reply_service) {
		std::fprintf(stderr, "runpod: no service\n");
		return port;
	}

	auto pub_builder = iox2_port_factory_pub_sub_publisher_builder(&p->cmd_service, nullptr);
	iox2_port_factory_publisher_builder_set_initial_max_slice_len(&pub_builder,
			weft::MESSAGE_BYTES);
	if (iox2_port_factory_publisher_builder_create(pub_builder, nullptr, &p->pub) != IOX2_OK) {
		std::fprintf(stderr, "runpod: no publisher on the command service\n");
		return port;
	}
	if (iox2_port_factory_subscriber_builder_create(
				iox2_port_factory_pub_sub_subscriber_builder(&p->reply_service, nullptr), nullptr,
				&p->sub) != IOX2_OK) {
		std::fprintf(stderr, "runpod: no subscriber on the reply service\n");
		return port;
	}

	p->open = true;
	return port;
}
