"""The command bus, in Python, over the iceoryx2 Python binding.

`weft/command.hpp` and `weft/loop.hpp` are the C++ halves of this and it must agree with them
byte for byte, because the pair exists so a Python interactor can be swapped for a C++ one
behind the same transport layer. Agreement is four things: the two service names, the payload
type (`weft::byte`, DYNAMIC, size 1, align 1), the 8-byte little-endian request-id prefix, and
the 128 KiB bound on one message. `check_matches_header` reads all of them back out of
`include/weft/command.hpp` so a drift fails a test rather than a deployment.

This is the harness's file rather than either caller's, for the reason the harness exists: a
decision written twice drifts, and the stale copy still reads as authoritative.

SPDX-License-Identifier: Apache-2.0 OR MIT
"""

from __future__ import annotations

import ctypes
import re
import time
from pathlib import Path

import iceoryx2 as iox2
from iceoryx2 import Slice, TypeDetail, TypeName, TypeVariant

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


def _service(node, name):
    """The pub/sub service the C++ harness opens, opened again from Python.

    The binding derives the payload type name from the ctypes type, which gives `u8` where the
    C++ side says `weft::byte`, and iceoryx2 refuses to connect two services whose payload type
    names differ. So the type detail is set explicitly. The attributes used for that are the
    binding's own internals, which is a real dependency on an implementation detail of
    iceoryx2 0.9.3 -- it is here rather than hidden because the alternative was renaming the
    payload type in a C++ repository that has already proved itself with this one.
    """
    builder = node.service_builder(iox2.ServiceName.new(name))
    pub_sub = getattr(builder, "__publish_subscribe")()
    getattr(pub_sub, "__set_payload_type")(Slice[ctypes.c_uint8])
    pub_sub = getattr(pub_sub, "__payload_type_details")(
        TypeDetail.new()
        .type_variant(TypeVariant.Dynamic)
        .type_name(TypeName.new(PAYLOAD_TYPE))
        .size(1)
        .alignment(1)
    )
    pub_sub = getattr(pub_sub, "__user_header_type_details")(
        TypeDetail.new()
        .type_variant(TypeVariant.FixedSize)
        .type_name(TypeName.new("()"))
        .size(0)
        .alignment(1)
    )
    return pub_sub.open_or_create()


class Bus:
    """One node, both services, and the one port of each this process needs.

    `role` decides which way round the ports go: a client publishes commands and subscribes to
    replies, and a server does the opposite. Nothing else differs between the two.
    """

    def __init__(self, role: str) -> None:
        if role not in ("client", "server"):
            raise ValueError("role is 'client' or 'server'")
        self.role = role
        self.node = iox2.NodeBuilder.new().create(iox2.ServiceType.Ipc)
        commands = _service(self.node, COMMAND_SERVICE_NAME)
        replies = _service(self.node, REPLY_SERVICE_NAME)

        out_service, in_service = (
            (commands, replies) if role == "client" else (replies, commands)
        )
        self.publisher = (
            out_service.publisher_builder().initial_max_slice_len(MESSAGE_BYTES).create()
        )
        self.subscriber = in_service.subscriber_builder().create()

    def send(self, request_id: int, body: bytes) -> None:
        if len(body) > BODY_MAX:
            raise ValueError(f"{len(body)} bytes is more than the bus carries ({BODY_MAX})")
        msg = write_header(request_id) + body
        sample = self.publisher.loan_slice_uninit(len(msg))
        ctypes.memmove(sample.payload_ptr, msg, len(msg))
        sample.assume_init().send()

    def receive(self):
        """One message, or None. Returns (request_id, body); a message shorter than the prefix
        has nothing to correlate on and is dropped rather than returned."""
        sample = self.subscriber.receive()
        if sample is None:
            return None
        n = sample.payload().number_of_elements
        raw = ctypes.string_at(sample.payload_ptr, n)
        if n < HEADER_BYTES:
            return None
        return read_header(raw), raw[HEADER_BYTES:]

    def wait(self, seconds: float) -> None:
        self.node.wait(iox2.Duration.from_secs_f64(seconds))


def ask(bus: Bus, request_id: int, command: bytes, timeout_s: float, poll_s: float = 0.002):
    """One command out, the reply carrying that id back, or None at the deadline.

    A reply carrying any other id belongs to a job this caller already gave up on and is
    dropped. Answering the current command with it would be a wrong answer rather than a
    missing one, which is the failure this correlation exists to prevent.
    """
    bus.send(request_id, command)
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        got = bus.receive()
        if got is None:
            bus.wait(poll_s)
            continue
        rid, body = got
        if rid == request_id:
            return body
    return None


def serve(bus: Bus, handler, poll_s: float = 0.01) -> int:
    """Every command that arrives, answered by `handler(command_bytes) -> reply_bytes`, until
    it raises `StopServing`. The C++ `run_command_loop` is the same loop with `*stop`."""
    while True:
        got = bus.receive()
        if got is None:
            bus.wait(poll_s)
            continue
        request_id, command = got
        try:
            reply = handler(command)
        except StopServing:
            return 0
        bus.send(request_id, reply)


class StopServing(Exception):
    """A handler asks the loop to wind down by raising this."""


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
