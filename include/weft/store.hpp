// The store service: what a caller asks the store plane, and what it answers.
//
// The plane holds one SQLite database for each avatar, over the VFS whose pages live in
// FoundationDB. A caller never opens a database. It names an avatar and the plane owns
// every handle, which is what keeps one owner and one fence per database.
//
// Request and reply are two publish and subscribe services rather than one
// request-response service, because `iceoryx2.sigs` lists the publish and subscribe C ABI
// and nothing else. Adding the request-response ABI is a change to the signature file and
// to every plane that links this, so the correlation lives in the payload instead:
// `request_id` is echoed, and a caller matches on it.
//
// Both ends must agree on the type name, the size, and the alignment. iceoryx2 rejects the
// second port when they differ, and that check is what stops one process reading another
// process's layout as its own. So these structs are the contract, not a convenience.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef WEFT_STORE_HPP
#define WEFT_STORE_HPP

#include "weft/limits.hpp"

#include <cstdint>
#include <cstdio>

namespace weft {

// What the caller wants done to one avatar's database.
enum StoreOp : std::uint32_t {
    STORE_OPEN = 1,   // raise the fence and take ownership
    STORE_READ = 2,   // one statement, rows back in the reply body
    STORE_COMMIT = 3, // one statement inside a transaction, which is one FoundationDB commit
    STORE_CLOSE = 4,  // flush and give up the handle
};

// The body is `Weft.Limits`' value bound, not a number chosen here. A plane that picked its
// own size would be guessing at a workload weft has not seen.
inline constexpr std::size_t STORE_BODY_BYTES = limits::VALUE_BYTES;

struct StoreRequest {
    std::uint64_t request_id; // echoed in the reply, so a caller can match
    std::uint64_t avatar;     // which database, and therefore which fence
    std::uint32_t op;         // a StoreOp
    std::uint32_t length;     // bytes of body in use
    std::uint8_t body[STORE_BODY_BYTES];
};

struct StoreReply {
    std::uint64_t request_id;
    std::uint64_t avatar;
    std::int32_t code;    // an SQLite result code, so SQLITE_READONLY means the fence moved
    std::uint32_t length; // bytes of body in use
    std::uint8_t body[STORE_BODY_BYTES];
};

// One name for one concept, on both sides.
inline constexpr const char* STORE_REQUEST_SERVICE = "weft/store/request";
inline constexpr const char* STORE_REPLY_SERVICE = "weft/store/reply";

// The payload type names iceoryx2 records with each service.
inline constexpr const char* STORE_REQUEST_TYPE = "weft::StoreRequest";
inline constexpr const char* STORE_REPLY_TYPE = "weft::StoreReply";

// A service for each shard, because publish and subscribe is a broadcast: every subscriber
// on one service receives every message. Sharing a single service across a thread for each
// core would hand every request to every thread and make the loop do N times the work.
//
// So a shard is a service, and an avatar belongs to exactly one. The owner of an avatar's
// database is then the one thread subscribed to that shard, which is what lets the handle
// cache and its fence live in that thread with no lock around either.
inline std::uint32_t store_shard_of(std::uint64_t avatar, std::uint32_t shards) {
    return shards ? static_cast<std::uint32_t>(avatar % shards) : 0u;
}

// Writes "<base>/<shard>" into `out`, and returns its length. Both ends build the name the
// same way or they open different services and neither ever hears the other.
inline std::size_t store_service_name(char* out, std::size_t cap, const char* base,
                                      std::uint32_t shard) {
    const int n = std::snprintf(out, cap, "%s/%u", base, shard);
    return (n < 0) ? 0u : static_cast<std::size_t>(n);
}

} // namespace weft

#endif
