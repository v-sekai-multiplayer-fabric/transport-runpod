// The job cycle, with no bus and no interactor.
//
// What has to be right here is the correlation: an asynchronous bus can hand this worker a
// reply to a job it already gave up on, and answering the current job with it would be a
// wrong answer rather than an error. That is the case the fake port below replays, along
// with the deadline and the two malformed shapes.
//
// This proves nothing about iceoryx2. `src/port_iox2.cpp` says so at the top of the file.
//
// SPDX-License-Identifier: Apache-2.0

#include "runpod/bus_ask.h"

#include "weft/command.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Canned {
	std::vector<unsigned char> sent;   // what the fake port was asked to send
	std::vector<std::vector<unsigned char>> to_deliver; // replies, in order
	size_t at = 0;
	bool send_fails = false;
};

int fake_send(void *ctx, const unsigned char *msg, size_t n) {
	Canned *c = (Canned *)ctx;
	if (c->send_fails) {
		return 1;
	}
	c->sent.assign(msg, msg + n);
	return 0;
}

size_t fake_recv(void *ctx, unsigned char *buf, size_t cap, uint64_t) {
	Canned *c = (Canned *)ctx;
	if (c->at >= c->to_deliver.size()) {
		return 0;
	}
	const std::vector<unsigned char> &m = c->to_deliver[c->at++];
	const size_t take = m.size() < cap ? m.size() : cap;
	std::memcpy(buf, m.data(), take);
	return take;
}

void fake_close(void *) {}

std::vector<unsigned char> envelope(uint64_t id, const std::string &body) {
	std::vector<unsigned char> m(weft::HEADER_BYTES + body.size());
	weft::command_write_header(m.data(), id);
	std::memcpy(m.data() + weft::HEADER_BYTES, body.data(), body.size());
	return m;
}

int failures = 0;

void check(bool ok, const char *what) {
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		++failures;
	}
}

// The id `bus_ask` will use for its next command, which the fake port has to know to write a
// matching reply. Reading it from the struct rather than predicting it keeps the proof honest
// about the seeding in `bus_ask_init`.
uint64_t next_id(const bus_ask_t &b) { return b.next_id + 1; }

bool is_error_reply(const unsigned char *reply, size_t n) {
	// CBOR: a1 (map of 1) 65 "error". The whole decoder is not needed to tell an error from
	// an interactor's answer, and the interactor's answer is not this repository's to parse.
	return n > 7 && reply[0] == 0xa1 && std::memcmp(reply + 2, "error", 5) == 0;
}

} // namespace

int main() {
	unsigned char reply[4096];

	{ // The command reaches the bus in one envelope, and the reply comes back whole.
		Canned c;
		bus_ask_t b;
		bus_ask_init(&b, bus_port_t{ fake_send, fake_recv, fake_close, &c });
		c.to_deliver = { envelope(next_id(b), "layers: 7") };

		int stop = 0;
		const size_t n = bus_ask(&b, "decompose /in.png --res 1280 --steps 30", reply,
				sizeof(reply), &stop);

		check(n == 9 && std::memcmp(reply, "layers: 7", 9) == 0, "the reply body comes back whole");
		check(stop == 0, "an answered job does not stop the worker");
		check(c.sent.size() == weft::HEADER_BYTES + 39, "the command is sent with the id prefix");
		check(std::memcmp(c.sent.data() + weft::HEADER_BYTES, "decompose", 9) == 0,
				"the command body is sent unchanged");
	}

	{ // A reply to a job this worker gave up on must not answer the one it is holding.
		Canned c;
		bus_ask_t b;
		bus_ask_init(&b, bus_port_t{ fake_send, fake_recv, fake_close, &c });
		const uint64_t mine = next_id(b);
		c.to_deliver = { envelope(mine - 1, "an older job's answer"), envelope(mine, "mine") };

		int stop = 0;
		const size_t n = bus_ask(&b, "decompose /in.png", reply, sizeof(reply), &stop);
		check(n == 4 && std::memcmp(reply, "mine", 4) == 0, "a stale reply is dropped, not returned");
	}

	{ // Short of the request-id prefix there is nothing to correlate on.
		Canned c;
		bus_ask_t b;
		bus_ask_init(&b, bus_port_t{ fake_send, fake_recv, fake_close, &c });
		c.to_deliver = { { 0x01, 0x02 }, envelope(next_id(b), "ok") };

		int stop = 0;
		const size_t n = bus_ask(&b, "decompose /in.png", reply, sizeof(reply), &stop);
		check(n == 2 && std::memcmp(reply, "ok", 2) == 0, "a message shorter than the header is dropped");
	}

	{ // Nothing arrives. The job still gets an answer, and it says which failure this was.
		Canned c;
		bus_ask_t b;
		bus_ask_init(&b, bus_port_t{ fake_send, fake_recv, fake_close, &c });
		b.timeout_ms = 20;

		int stop = 0;
		const size_t n = bus_ask(&b, "decompose /in.png", reply, sizeof(reply), &stop);
		check(is_error_reply(reply, n), "a deadline is answered with a CBOR error");
		check(stop == 0, "a slow interactor does not stop the worker");
	}

	{ // A dead bus is a different thing from a slow interactor, and stops the worker.
		Canned c;
		c.send_fails = true;
		bus_ask_t b;
		bus_ask_init(&b, bus_port_t{ fake_send, fake_recv, fake_close, &c });

		int stop = 0;
		const size_t n = bus_ask(&b, "decompose /in.png", reply, sizeof(reply), &stop);
		check(is_error_reply(reply, n), "a failed send is answered with a CBOR error");
		check(stop == 1, "a failed send stops the worker");
	}

	{ // Longer than the bus carries: refused here rather than truncated into a command the
	  // interactor would run against the wrong arguments.
		Canned c;
		bus_ask_t b;
		bus_ask_init(&b, bus_port_t{ fake_send, fake_recv, fake_close, &c });
		const std::string huge(weft::BODY_MAX + 1, 'x');

		int stop = 0;
		const size_t n = bus_ask(&b, huge.c_str(), reply, sizeof(reply), &stop);
		check(is_error_reply(reply, n), "an oversized command is refused");
		check(c.sent.empty(), "an oversized command is not sent");
	}

	std::printf("%s\n", failures ? "roundtrip: FAILED" : "roundtrip: all checks passed");
	return failures ? 1 : 0;
}
