"""The weft harness, for a plane or a transport layer written in Python.

The agreement -- the service names, the payload type, the envelope, and the check that they
still match `weft/command.hpp` -- imports eagerly, because it is pure text and must be
answerable on a machine with no bus installed. The ports import on first use, because they
need the iceoryx2 binding and a checkout that only wants to be checked should not have to
install one.

SPDX-License-Identifier: Apache-2.0 OR MIT
"""

from .agree import (  # noqa: F401
    BODY_MAX,
    COMMAND_SERVICE_NAME,
    HEADER_BYTES,
    MESSAGE_BYTES,
    PAYLOAD_TYPE,
    REPLY_SERVICE_NAME,
    check_matches_header,
    read_header,
    write_header,
)

_FROM_BUS = ("Bus", "StopServing", "ask", "serve")

__all__ = [
    "BODY_MAX",
    "COMMAND_SERVICE_NAME",
    "HEADER_BYTES",
    "MESSAGE_BYTES",
    "PAYLOAD_TYPE",
    "REPLY_SERVICE_NAME",
    "check_matches_header",
    "read_header",
    "write_header",
    *_FROM_BUS,
]


def __getattr__(name):
    """PEP 562: the ports arrive when something actually asks for one."""
    if name in _FROM_BUS:
        from . import bus

        return getattr(bus, name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
