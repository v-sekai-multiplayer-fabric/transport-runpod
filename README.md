# transport-runpod

The RunPod Serverless transport: it takes jobs from a RunPod endpoint's queue and hands each one
to an interactor as a command.

```
include/runpod/    the transport, and the HTTP seam it needs
src/               the worker protocol
proof/             what the protocol does, checked without a network
thirdparty/interactor   contract-command: a command in, reply bytes out
```

## What this is

A transport is bytes to and from a wire and has no idea what the command means. This one is the
odd member of the family: it has **no listening socket**. A RunPod Serverless worker is a client
of its own endpoint — it asks the queue for work, so the wire runs outwards.

That falls out of what RunPod is rather than a preference. A queue endpoint hands a worker one
job at a time over HTTP and waits for the worker to post the result back; there is nothing to
listen on and no proxy to traverse. It also removes the two limits that make the alternative
unusable here: a load-balancing endpoint routes HTTP to a port inside the worker, but caps
processing at 5.5 minutes per request, and can only be created from the console. A queue
endpoint has a configurable `executionTimeoutMs` and is creatable from the REST API.

## Composition

Not `weft_transport_t`. RunPod's queue protocol is a worker *polling* a queue over HTTP, not a
worker with a socket a service polls -- there is no `fds()`/`ready()` shape here, because there
is nothing to `poll(2)`. The seam is a function instead: `include/runpod/worker.h` exports

```c
int rp_worker_run(weft_interactor_t in);
```

which is the whole job loop -- heartbeat, job-take, `weft_ask`, result post, proved end to end
against a real endpoint (see the job trace below). An interactor vendors this repo as a subtree,
links the `rp-worker` CMake target, and calls `rp_worker_run(its_own_interactor)` from `main()`.
`src/main.cpp` here is that pattern applied to a trivial echo interactor -- read it first, then
look at `interactor-qwen35-defiant`/`interactor-gemma4-composer` for a real one.

This file includes `weft/interactor.h` and no header of any interactor's. A RunPod worker for a
different interactor is this same library with a different `weft_interactor_t`.

## The worker protocol

RunPod documents its *client* SDKs (Python, JavaScript, Go) and its handler functions. It does
not document the wire a worker actually speaks, so the following is derived from
`runpod/runpod-python` (`runpod/serverless/modules/rp_job.py`, `rp_http.py`, `rp_ping.py`) and is
what this transport implements. Treat it as observed, not specified.

Every URL arrives in the environment as a template. `$ID` is the worker id when it appears in the
job-take URL and the **job** id when it appears in the output URL — the same token meaning two
different things is the sharpest edge in this protocol.

| Variable | Use |
| --- | --- |
| `RUNPOD_POD_ID` | the worker id |
| `RUNPOD_AI_API_KEY` | `Authorization` on the ping |
| `RUNPOD_WEBHOOK_GET_JOB` | GET one job. `$ID` -> worker id |
| `RUNPOD_WEBHOOK_POST_OUTPUT` | POST the result. `$RUNPOD_POD_ID` -> worker id, then `$ID` -> job id |
| `RUNPOD_WEBHOOK_PING` | GET, liveness. `$RUNPOD_POD_ID` -> worker id |
| `RUNPOD_PING_INTERVAL` | milliseconds between pings, default 10000 |

Taking a job appends `&job_in_progress=0|1`, and the reply is read by status, not by body:

| Status | Meaning |
| --- | --- |
| 200 | a job, as JSON with `id` and `input` |
| 204 | no job. Not an error, and the common case |
| 400 | no job, expected when FlashBoot is on. Also not an error |
| 429 | asked too often; back off |

Posting the result appends `&isStream=false` and sets `X-Request-ID` to the job id. The body is
`{"output": ...}` or `{"error": ...}`.

## What a command is

The queue carries JSON and an interactor takes a NUL-terminated string, so the job's `input` is
turned into one command line. A job of `{"input": {"command": "render /in.png --res 1280"}}` is
sent as that string and nothing else — this transport does not invent verbs, because the verbs
belong to whichever interactor is on the other side and it does not know which one that is.

An interactor's reply is CBOR bytes. JSON cannot carry them, so the reply is base64 in
`{"output": {"cbor": "..."}}`. Decoding it is the caller's, and the reply's shape is the
interactor's to document.

## The HTTP seam

`include/runpod/rp_http.h` is four function pointers. The protocol above is written against that
seam rather than against a client library, so it can be proved without a network — `proof/` runs
the whole job cycle against a table of canned responses.

The backend intended for real use is h2o, which this stack has used before. **It is not written
yet**: `rp_open(in, NULL)` returns a transport whose HTTP calls fail, which is honest rather than
a stub that pretends. The protocol, the templating and the status handling are complete and
proved; what remains is one file that fills in the seam.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target rp-worker-echo
./build/rp-worker-echo   # exits 1 outside a RunPod worker; that is correct
```

## Job trace (2026-08-14, RTX 3090 endpoint, echo worker)

```
worker.start   worker up, id=xnkdrdwl3ktmje, log=/runpod-volume/transport-runpod.ndjson
worker.env     env: GET_JOB=set POST_OUTPUT=set PING=set API_KEY=set
worker.ping    ping -> 200
worker.take    job-take -> 200 {"delayTime":273894,"id":"b9b0d0d7-...","input":{"proof":"c++ worker"}}
worker.job     job b9b0d0d7-...
worker.done    posted b9b0d0d7-... -> 200
```

Result: `{"status":"COMPLETED","executionTime":134,"output":{"language":"c++","echo":"..."}}`.
Scale-from-zero, separately measured: 2.9s queue delay, 101ms execution, FlashBoot on.
