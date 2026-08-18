// The worker: RunPod's job loop, with the bus as its interactor.
//
// There is nothing else to it, and that is the claim this repository makes. `rp_worker_run`
// is `transport-runpod`'s proven loop -- heartbeat, job-take, ask, post -- and `bus_ask` is
// an interactor as far as it can tell. Which interactor is answering is decided by whichever
// process is running `weft::run_command_loop` on the same machine, and this binary never
// learns its name.
//
// SPDX-License-Identifier: Apache-2.0

#include "runpod/bus_ask.h"
#include "runpod/worker.h"

int main() {
	bus_ask_t bus;
	bus_ask_init(&bus, bus_port_iox2_open());

	weft_interactor_t in = { bus_ask, &bus };
	const int rc = rp_worker_run(in);

	bus_ask_close(&bus);
	return rc;
}
