// A command in, reply bytes out, over the bus.
//
// `weft/snapshot.hpp`'s Snapshot is a fixed 40-byte struct: it is the smallest payload
// that proves the bus, and every field is fixed-width because the bus is proven for
// exactly one payload variant (`iox2_type_variant_e_FIXED_SIZE`). A command is not
// fixed-width -- a prompt is however long a caller wrote it, and a reply is however much
// an interactor had to say -- so this is the harness's first use of the *other* variant,
// `iox2_type_variant_e_DYNAMIC`: a slice of bytes, loaned at the length one message
// actually needs (`iox2_publisher_loan_slice_uninit(..., element_count)`), not the
// fixed-struct-and-memcpy shape `proof/publisher.cpp` uses.
//
// This is new, unproven ABI usage in this repository -- `proof/command_publisher.cpp` and
// `proof/command_subscriber.cpp` exist because of the same rule that produced
// `proof/publisher.cpp`: "It is the one thing that has to work before a harness is worth
// writing." Nothing here has been proven to work on real iceoryx2 yet -- see this
// repository's README for how to run the proof, and run it before trusting this header
// in anything that serves a real request.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef WEFT_COMMAND_HPP
#define WEFT_COMMAND_HPP

#include "weft/limits.hpp"

#include <cstdint>
#include <cstring>

namespace weft {

// Two services, not one request-response service: the vendored `iceoryx2.sigs` lists only
// the pub_sub builder functions (see README.md's "Nothing links iceoryx2" section on why
// the ABI surface is generated from an explicit list, not the whole library) -- adding
// request-response would mean extending that generated stub table, unverified, in the
// same change that introduces the first DYNAMIC payload. One capability at a time.
// A command and its reply are correlated instead by an 8-byte request id, sent as the
// first 8 bytes of each message's payload -- see `command_header`/`command_body` below.
inline constexpr const char *COMMAND_SERVICE_NAME = "weft/harness/command";
inline constexpr const char *REPLY_SERVICE_NAME = "weft/harness/reply";
inline constexpr const char *PAYLOAD_TYPE = "weft::byte";

// The largest command or reply this bus carries in one message, request id included.
// `weft::limits::VALUE_BYTES` (128KiB) is the harness's own established bound for "one
// value" -- a command/reply pair obeys it rather than choosing its own, the same reason
// `weft/limits.hpp` exists at all. A caller needing more (a large generated document, a
// big image) chunks across several messages or uses a different transport for the bulk
// payload and a command/reply here only to coordinate it; this bus is not sized for that.
inline constexpr std::size_t MESSAGE_BYTES = limits::VALUE_BYTES;
inline constexpr std::size_t HEADER_BYTES = sizeof(std::uint64_t);
inline constexpr std::size_t BODY_MAX = MESSAGE_BYTES - HEADER_BYTES;

// Writes the 8-byte request id prefix. `buf` must have at least HEADER_BYTES writable.
inline void command_write_header(unsigned char *buf, std::uint64_t request_id) {
	std::memcpy(buf, &request_id, HEADER_BYTES);
}

inline std::uint64_t command_read_header(const unsigned char *buf) {
	std::uint64_t id = 0;
	std::memcpy(&id, buf, HEADER_BYTES);
	return id;
}

} // namespace weft

#endif
