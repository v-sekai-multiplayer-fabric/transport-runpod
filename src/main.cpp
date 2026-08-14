// The standalone proof-of-life worker: echoes `input.command` back.
//
// Kept exactly as it was proved on a real RunPod endpoint (see README.md's job
// trace) so it stays a working reference for anyone wiring a new interactor --
// `rp_worker_run` is what they link; this file is what they read first.
//
// SPDX-License-Identifier: Apache-2.0

#include "runpod/worker.h"

#include "weft/cbor.h"

#include <cstring>

namespace {

size_t echo_ask(void *, const char *command, unsigned char *reply, size_t cap, int *) {
	weft_cbor_t c = weft_cbor_to(reply, cap);
	weft_cbor_map(&c, 2);
	weft_cbor_kv_text(&c, "language", "c++");
	weft_cbor_kv_text(&c, "echo", command);
	return c.n;
}

} // namespace

int main() {
	weft_interactor_t in = { echo_ask, nullptr };
	return rp_worker_run(in);
}
