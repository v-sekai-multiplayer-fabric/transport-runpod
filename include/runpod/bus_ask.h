// The client half of the harness command bus, wearing the interactor contract.
//
// `weft/loop.hpp` in the harness is the server half: an interactor subscribes to the command
// service, answers, and publishes on the reply service. Nothing there is the caller, because
// until now the caller was a proof program. This is the caller, and it is shaped as
// `weft_interactor_t::ask` on purpose: `rp_worker_run` takes exactly that, so a worker whose
// interactor is another process reaches it with no change to the queue protocol above.
//
// So an interactor over this transport layer is a separate process. What crosses the process
// boundary is one command and one reply, which is the contract `contract-command` already
// states; this adds only the correlation an asynchronous bus needs.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef RUNPOD_BUS_ASK_H
#define RUNPOD_BUS_ASK_H

#include "runpod/port.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	bus_port_t port;

	// How long one command may take before the reply is given up on. A see-through
	// decomposition at the settings its own README calls production takes about six minutes
	// on a 4090, so a default measured in seconds would time out every real job; this
	// defaults to 900000 (15 minutes) and is read from BUS_ASK_TIMEOUT_MS.
	uint64_t timeout_ms;

	// How long each receive waits before the deadline is re-checked. Small enough that a
	// stop is noticed promptly, large enough that a fifteen-minute wait is not a spin.
	uint64_t poll_ns;

	// The next request id. Seeded away from zero so two workers on one machine, or one
	// worker restarted, do not reuse ids an interactor may still be answering.
	uint64_t next_id;

	// Set once the port has failed. A worker that cannot reach its interactor answers
	// nothing useful, so it asks the job loop to wind down instead of failing every job.
	int failed;
} bus_ask_t;

// Seeds the ids and reads the timeout from the environment. Takes the port by value; the
// caller keeps ownership and closes it through `bus_ask_close`.
void bus_ask_init(bus_ask_t *b, bus_port_t port);
void bus_ask_close(bus_ask_t *b);

// `weft_interactor_t::ask`. Publishes the command, waits for the reply carrying its own
// request id, and writes the reply body into `reply`. Replies carrying any other id are
// dropped -- they belong to a job this worker already gave up on.
//
// Never returns 0 for a job it was given: a deadline and a dead bus are both answered with a
// CBOR error map, because `rp_worker_run` posts whatever comes back as the job's output
// and an empty output tells the caller nothing about which of the two happened.
size_t bus_ask(void *ctx, const char *command, unsigned char *reply, size_t cap, int *stop);

#ifdef __cplusplus
}
#endif

#endif
