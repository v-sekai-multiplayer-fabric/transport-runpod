// The RunPod Serverless transport's public seam.
//
// `rp_worker_run` is the job loop `src/worker.cpp` proved end to end on a real
// endpoint: heartbeat, job-take, `weft_ask`, result post, all authenticated and
// all logged to the network-volume OTel file. It is a function, not a `main()`,
// so an interactor can link it as a library and supply its own
// `weft_interactor_t` instead of forking this repository per model.
//
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "weft/interactor.h"

// Runs forever: heartbeat on its own thread, job-take/verify/post on this one.
// Reads its RunPod wiring from the environment (RUNPOD_WEBHOOK_GET_JOB, etc,
// documented in README.md) and its log path from `LOGF`
// (default `/runpod-volume/transport-runpod.ndjson`), exactly as the standalone
// worker does. Returns only if the environment says this is not a worker
// (RUNPOD_WEBHOOK_GET_JOB/_POST_OUTPUT unset), in which case it returns 1.
//
// A command's `input` JSON is turned into one line and handed to `in.ask`
// verbatim -- see README.md's "What a command is". The reply is `weft_ask`'s
// CBOR bytes, base64'd into `{"output":{"cbor":"..."}}`.
int rp_worker_run(weft_interactor_t in);
