# transport-runpod

The RunPod Serverless transport layer: a job off an endpoint's queue, handed to an interactor.

## What this is

A transport layer is bytes to and from a wire and has no idea what the command means. This one
has **no listening socket**: a RunPod Serverless worker is a client of its own endpoint and
asks the queue for work, so the wire runs outwards.

That falls out of what RunPod is. A queue endpoint hands a worker one job at a time over HTTP,
waits for the result to be posted back, and removes the two limits that make the alternative
unusable here: a load-balancing endpoint caps processing at 5.5 minutes and is creatable only
from the console.

## Two ways to reach an interactor

`rp_worker_run(weft_interactor_t)` is the job loop: heartbeat, job-take, ask, result post. What
supplies that interactor is the whole difference between the workers here.

`rp-worker-echo` answers jobs itself. The interactor is linked in, which is right when it is a
function and wrong when it holds gigabytes of weights: the model then loads inside the worker's
process, so scale-from-zero pays for it and a job that kills the model takes the HTTP loop with
it.

`rp-worker-bus` answers them by asking another process over the harness command bus.
`include/runpod/bus_ask.h` is the client half of `weft/loop.hpp`, correlated by an 8-byte
request id, and it wears `weft_interactor_t` so the loop above it is unchanged.
`proof/roundtrip.cpp` checks that correlation, the deadline and the malformed shapes.

`python/` terminates the same queue protocol a second time, on RunPod's own SDK and the
iceoryx2 Python binding, sharing nothing with the C++ path but the result it must produce.

## What a command is

A job of `{"input": {"command": "decompose /in.png --res 1280 --steps 30"}}` is sent as that
string and nothing else: the verbs belong to whichever interactor is on the other side. The
reply is CBOR, so it returns base64 in `{"output": {"cbor": "..."}}`. RunPod documents no
worker protocol; `src/worker.cpp` records what was read out of its Python SDK.
