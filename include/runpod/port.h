// The bus seam, so the correlation can be proved without a bus.
//
// `src/port_iox2.cpp` is the real one: an iceoryx2 publisher on the command service and a
// subscriber on the reply service, which is the mirror image of what `weft::run_command_loop`
// opens on the interactor's side. `proof/roundtrip.cpp` supplies a table of canned messages
// instead, so the request-id correlation, the stale-reply drop and the deadline are checked
// on a machine with no shared memory and no interactor running.
//
// The protocol is the part that has to be right, and a protocol written against a library
// cannot be tested without one. The webhook side of this repository is still written against
// libcurl directly and has no such seam, which is why the job trace in `README.md` is its
// only evidence and this half has a `proof/` instead.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef RUNPOD_PORT_H
#define RUNPOD_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	// Sends one message whole. Returns 0 on success, non-zero if the bus is gone -- which
	// is not the same as a slow interactor, and `bus_ask` treats it differently.
	int (*send)(void *ctx, const unsigned char *msg, size_t n);

	// Receives at most one message, waiting up to `wait_ns` for it. Returns how many bytes
	// were written into `buf`, and 0 when nothing arrived, which is the ordinary case: a
	// caller polls this until its own deadline.
	size_t (*recv)(void *ctx, unsigned char *buf, size_t cap, uint64_t wait_ns);

	void (*close)(void *ctx);
	void *ctx;
} bus_port_t;

// The iceoryx2 port. Returns a port whose `send` and `recv` fail if the bus could not be
// loaded or the services could not be opened, rather than aborting: a worker that cannot
// reach its interactor still has to say so on the job it was given.
bus_port_t bus_port_iox2_open(void);

#ifdef __cplusplus
}
#endif

#endif
