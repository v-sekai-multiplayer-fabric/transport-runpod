"""Two processes, one bus, and a reply that comes back to the right one.

The C++ proof for this is `proof/command_publisher.cpp` and `proof/command_subscriber.cpp`,
which ran on Windows against iceoryx2 v0.9.3. This is the same proof for the Python binding,
and it is the one that has to pass before a Python interactor is put behind a transport layer:
what it checks is that the service name, the payload type and the request-id envelope agree
well enough for a message to survive the trip.

    python3 proof/roundtrip.py server &
    python3 proof/roundtrip.py client 8

SPDX-License-Identifier: Apache-2.0 OR MIT
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from weft_harness import Bus, StopServing, ask, serve  # noqa: E402


def server() -> int:
    bus = Bus("server")

    def handler(command: bytes) -> bytes:
        if command == b"quit":
            raise StopServing
        # "ping N" -> "pong N", so the reply is derived from the command rather than constant:
        # a constant reply cannot tell a delivered message from an ignored one.
        if not command.startswith(b"ping "):
            return b"?"
        return b"pong " + command[5:]

    print("server: up", flush=True)
    return serve(bus, handler)


def client(count: int) -> int:
    bus = Bus("client")
    failures = 0
    for tick in range(1, count + 1):
        reply = ask(bus, request_id=tick, command=f"ping {tick}".encode(), timeout_s=5.0)
        want = f"pong {tick}".encode()
        if reply == want:
            print(f"client: tick {tick} ok", flush=True)
        else:
            print(f"client: tick {tick} FAIL, got {reply!r} want {want!r}", flush=True)
            failures += 1
    ask(bus, request_id=count + 1, command=b"quit", timeout_s=0.2)
    print(f"client: {'FAILED' if failures else f'sent and confirmed {count}'}", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    role = sys.argv[1] if len(sys.argv) > 1 else "client"
    sys.exit(server() if role == "server" else client(int(sys.argv[2]) if len(sys.argv) > 2 else 8))
