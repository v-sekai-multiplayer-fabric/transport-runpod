// One job, one command, one reply, correlated by request id.
//
// SPDX-License-Identifier: Apache-2.0

#include "runpod/bus_ask.h"

#include "weft/cbor.h"
#include "weft/command.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

uint64_t now_ms() {
	using namespace std::chrono;
	return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint64_t env_u64(const char *key, uint64_t fallback) {
	const char *v = std::getenv(key);
	if (!v || !*v) {
		return fallback;
	}
	char *end = nullptr;
	const unsigned long long n = std::strtoull(v, &end, 10);
	return (end && *end == '\0' && n > 0) ? (uint64_t)n : fallback;
}

// A CBOR map of one key, so a caller decoding the job output finds an error where it would
// have found the interactor's answer. The reply is the interactor's to shape and this
// transport layer never writes one -- except here, where there is no interactor answer to
// pass on and silence would be indistinguishable from an interactor with nothing to say.
size_t error_reply(unsigned char *reply, size_t cap, const char *text) {
	weft_cbor_t c = weft_cbor_to(reply, cap);
	weft_cbor_map(&c, 1);
	weft_cbor_kv_text(&c, "error", text);
	return c.n;
}

} // namespace

void bus_ask_init(bus_ask_t *b, bus_port_t port) {
	b->port = port;
	b->timeout_ms = env_u64("BUS_ASK_TIMEOUT_MS", 900000);
	b->poll_ns = env_u64("BUS_ASK_POLL_NS", 2'000'000); // 2ms
	// Not randomness for its own sake: the interactor drops a reply whose id it does not
	// recognise, so a restarted worker that began again at 1 would collide with ids the
	// interactor is still answering from the run before.
	b->next_id = now_ms() << 16;
	b->failed = 0;
}

void bus_ask_close(bus_ask_t *b) {
	if (b->port.close) {
		b->port.close(b->port.ctx);
	}
	b->port.ctx = nullptr;
}

size_t bus_ask(void *ctx, const char *command, unsigned char *reply, size_t cap, int *stop) {
	bus_ask_t *b = (bus_ask_t *)ctx;
	const size_t body_len = std::strlen(command);

	if (b->failed || !b->port.send || !b->port.recv) {
		*stop = 1;
		return error_reply(reply, cap, "bus unavailable");
	}
	// The bus carries one message of at most weft::MESSAGE_BYTES, which is the harness's own
	// bound for one value. A command longer than that is the caller's bug and is refused
	// here rather than truncated into something the interactor would run.
	if (body_len > weft::BODY_MAX) {
		return error_reply(reply, cap, "command longer than the bus carries");
	}

	const uint64_t request_id = ++b->next_id;

	std::vector<unsigned char> msg(weft::HEADER_BYTES + body_len);
	weft::command_write_header(msg.data(), request_id);
	std::memcpy(msg.data() + weft::HEADER_BYTES, command, body_len);

	if (b->port.send(b->port.ctx, msg.data(), msg.size()) != 0) {
		b->failed = 1;
		*stop = 1;
		return error_reply(reply, cap, "bus send failed");
	}

	std::vector<unsigned char> in(weft::MESSAGE_BYTES);
	const uint64_t deadline = now_ms() + b->timeout_ms;

	while (now_ms() < deadline) {
		const size_t n = b->port.recv(b->port.ctx, in.data(), in.size(), b->poll_ns);
		if (n == 0) {
			continue;
		}
		if (n < weft::HEADER_BYTES) {
			continue; // shorter than the request-id prefix: malformed, not ours
		}
		if (weft::command_read_header(in.data()) != request_id) {
			continue; // a reply to a job this worker already gave up on
		}
		const size_t got = n - weft::HEADER_BYTES;
		const size_t take = got < cap ? got : cap;
		std::memcpy(reply, in.data() + weft::HEADER_BYTES, take);
		return take;
	}

	// The interactor may still be working, and the next job's reply would then arrive
	// carrying this id. That is what the id check above is for: it is dropped, not
	// mistaken for the next answer.
	return error_reply(reply, cap, "no reply from the interactor before the deadline");
}
