"""The RunPod queue protocol, terminated a second time, on a stack that shares nothing.

`src/worker.cpp` is the first termination: libcurl, a hand-written job loop, and the protocol
read out of `runpod/runpod-python`'s source because RunPod documents its client SDKs and not
the wire a worker speaks. This one uses that SDK directly, so the two disagree about
everything except the result -- which is the point, the same way `transport-gateway-python`
disagrees with `transport-gateway-c`.

What both must produce is the same job output: `{"cbor": "<base64>"}`, the interactor's reply
bytes and nothing added.

SPDX-License-Identifier: Apache-2.0
"""

from __future__ import annotations

import base64
import itertools
import os
import time

from weft_harness import Bus, ask

# Seeded away from a fixed start for the reason the C++ client seeds: the interactor drops a
# reply whose id it does not recognise, so a restarted worker beginning again at 1 would
# collide with ids the interactor is still answering from the run before.
_ids = itertools.count(int(time.time()) << 16)


class Handler:
    """One bus, held across jobs.

    Opened lazily and once: a handler that opened a node per job would pay the service
    open on every one, and RunPod hands a warm worker many.
    """

    def __init__(self, timeout_s: float | None = None) -> None:
        self.bus = None
        # A see-through decomposition at production settings takes about six minutes on a
        # 4090, so a default in seconds would time out every real job.
        self.timeout_s = timeout_s or float(os.environ.get("BUS_ASK_TIMEOUT_MS", 900000)) / 1000.0

    def _open(self):
        if self.bus is None:
            self.bus = Bus("client")
        return self.bus

    def __call__(self, job: dict) -> dict:
        command = (job.get("input") or {}).get("command")
        if not command:
            return {"error": "input.command missing or empty"}

        try:
            bus = self._open()
        except Exception as why:  # noqa: BLE001 - the job result is the only log some runs leave
            return {"error": f"bus unavailable: {why}"}

        reply = ask(bus, next(_ids), command.encode("utf-8"), timeout_s=self.timeout_s)
        if reply is None:
            return {"error": "no reply from the interactor before the deadline"}
        # base64 because the queue carries JSON and a reply is CBOR. Decoding it is the
        # caller's, and the reply's shape is the interactor's to document.
        return {"cbor": base64.b64encode(reply).decode("ascii")}
