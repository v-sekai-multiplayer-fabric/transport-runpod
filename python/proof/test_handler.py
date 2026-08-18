"""The handler, with no bus, no network and no RunPod.

The C++ proof for this half is `proof/roundtrip.cpp`. This one checks the same decisions in
the same order, against a fake bus: a missing command, a deadline, and the base64 shape a job
result carries. What it cannot check is the queue protocol, which belongs to RunPod's SDK here
rather than to this repository -- that is what the endpoint run in README.md is for.

SPDX-License-Identifier: Apache-2.0
"""

import base64
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT.parent / "third_party/harness/python"))

FAILURES = []


def check(ok, what):
    print(f"{'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        FAILURES.append(what)


def main():
    from rp_bus.handler import Handler

    h = Handler(timeout_s=0.05)

    check(h({"input": {}}).get("error", "").startswith("input.command"),
          "a job with no command is refused, and says which field")
    check("error" in h({}), "a job with no input at all is refused")

    # A bus that never answers. The handler must still return a job result: an exception here
    # would be recorded by RunPod as a failed worker rather than as a job that timed out.
    class Silent:
        def send(self, request_id, body):
            pass

        def receive(self):
            return None

        def wait(self, seconds):
            pass

    h.bus = Silent()
    out = h({"input": {"command": "decompose /in.png --res 1280 --steps 30"}})
    check("error" in out and "deadline" in out["error"], "a deadline is a job result, not a crash")

    # A bus that answers whatever it was asked, so the base64 is checked against real bytes.
    class Echo:
        def __init__(self):
            self.pending = None

        def send(self, request_id, body):
            self.pending = (request_id, b"\xa1\x66layers\x07")

        def receive(self):
            got, self.pending = self.pending, None
            return got

        def wait(self, seconds):
            pass

    h.bus = Echo()
    out = h({"input": {"command": "decompose /in.png --res 1280 --steps 30"}})
    check("cbor" in out, "an answered job carries the reply under `cbor`")
    check(base64.b64decode(out["cbor"]) == b"\xa1\x66layers\x07",
          "the reply bytes survive the base64 unchanged")

    print("handler: FAILED" if FAILURES else "handler: all checks passed")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    raise SystemExit(main())
