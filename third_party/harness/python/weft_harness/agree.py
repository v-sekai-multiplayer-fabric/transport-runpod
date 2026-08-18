"""What the two halves of this bus must agree about, as text.

Separate from `bus.py` because it must run where the bus cannot: a checkout with no iceoryx2
installed can still be asked whether its constants still match `weft/command.hpp`, and that is
exactly the check worth running on every machine rather than only on one that could serve a
request. Importing the binding to compare two strings would have made the cheapest check the
most expensive one to run.

SPDX-License-Identifier: Apache-2.0 OR MIT
"""

from __future__ import annotations

import re
from pathlib import Path

COMMAND_SERVICE_NAME = "weft/harness/command"
REPLY_SERVICE_NAME = "weft/harness/reply"
PAYLOAD_TYPE = "weft::byte"
MESSAGE_BYTES = 128 * 1024
HEADER_BYTES = 8
BODY_MAX = MESSAGE_BYTES - HEADER_BYTES


def write_header(request_id: int) -> bytes:
    """The C++ side memcpys a uint64_t, so this is native order. Both ends of this bus are the
    same container on the same machine -- shared memory is not a wire, and a byte order
    conversion here would be a conversion to nowhere."""
    return request_id.to_bytes(HEADER_BYTES, "little", signed=False)


def read_header(buf: bytes) -> int:
    return int.from_bytes(buf[:HEADER_BYTES], "little", signed=False)

def check_matches_header(header_path: Path) -> list[str]:
    """Reads the four agreements back out of `include/weft/command.hpp`.

    A test calls this. The values are duplicated in this file because a Python process cannot
    parse a C++ header at import time in a container that may not ship one -- so the duplicate
    is deliberate and this is the check that keeps it honest.
    """
    text = Path(header_path).read_text()
    problems = []

    def literal(pattern: str, name: str, expected: str) -> None:
        match = re.search(pattern, text)
        if not match:
            problems.append(f"{name}: no longer stated in {header_path}")
        elif match.group(1) != expected:
            problems.append(f"{name}: header says {match.group(1)!r}, python says {expected!r}")

    literal(r'COMMAND_SERVICE_NAME\s*=\s*"([^"]+)"', "COMMAND_SERVICE_NAME", COMMAND_SERVICE_NAME)
    literal(r'REPLY_SERVICE_NAME\s*=\s*"([^"]+)"', "REPLY_SERVICE_NAME", REPLY_SERVICE_NAME)
    literal(r'PAYLOAD_TYPE\s*=\s*"([^"]+)"', "PAYLOAD_TYPE", PAYLOAD_TYPE)

    if "sizeof(std::uint64_t)" not in text:
        problems.append("HEADER_BYTES: the header no longer says sizeof(std::uint64_t)")
    if "limits::VALUE_BYTES" not in text:
        problems.append("MESSAGE_BYTES: the header no longer says limits::VALUE_BYTES")
    return problems
