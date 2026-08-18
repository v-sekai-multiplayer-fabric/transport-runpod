// The server half of the command/reply proof. Runs weft::run_command_loop itself, not raw
// ABI calls -- this is what proves the loop, not just the DYNAMIC payload path underneath
// it. An echo `ask`: reflects "ping N" back as "pong N", and stops after `expect` commands.
//
// SPDX-License-Identifier: Apache-2.0
#include "weft/loop.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

struct Ctx {
	int expect;
	int seen = 0;
};

size_t echo_ask(void *ctx_v, const char *command, unsigned char *reply, size_t cap, int *stop) {
	Ctx *ctx = (Ctx *)ctx_v;
	ctx->seen++;
	std::fprintf(stderr, "command_subscriber: got %s\n", command);
	std::fflush(stderr);

	std::string out = "pong";
	if (const char *sp = std::strchr(command, ' ')) {
		out += sp; // "ping 3" -> "pong 3"
	}
	const size_t n = out.size() < cap ? out.size() : cap;
	std::memcpy(reply, out.data(), n);

	if (ctx->seen >= ctx->expect) {
		*stop = 1;
	}
	return n;
}

} // namespace

int main(int argc, char **argv) {
	Ctx ctx;
	ctx.expect = (argc > 1) ? std::atoi(argv[1]) : 8;

	const int rc = weft::run_command_loop(&ctx, echo_ask);
	if (rc != 0 || ctx.seen != ctx.expect) {
		std::fprintf(stderr, "command_subscriber: wanted %d, got %d, loop rc=%d\n", ctx.expect,
				ctx.seen, rc);
		return 1;
	}
	std::printf("command_subscriber: answered %d, in order\n", ctx.seen);
	return 0;
}
