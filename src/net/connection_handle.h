/**
 * net/connection_handle.h — stable identifier for a live connection.
 *
 * A raw file descriptor is not a stable identity. The kernel recycles
 * fd numbers, so `send_json_to_fd(42, frame)` can safely deliver bytes
 * to whoever holds fd 42 *right now* — which is not necessarily the
 * connection the caller thinks it is.
 *
 * `ConnectionHandle` pairs the fd with a monotonically increasing
 * per-fd generation. `TcpServer` bumps the generation each time it
 * accepts a new connection on a previously-used fd. A sink that
 * compares its stored handle against the current generation refuses
 * to write when they disagree — a foreign delivery scheduled before
 * the previous socket closed cannot land on the next holder of that fd.
 *
 * Kept POD; passed by value; hashable if we ever need it in a set.
 */

#pragma once

#include <cstdint>

namespace chess::net {

struct ConnectionHandle {
    int      fd         = -1;
    uint64_t generation = 0;

    bool valid() const { return fd >= 0; }
    bool operator==(const ConnectionHandle& other) const {
        return fd == other.fd && generation == other.generation;
    }
    bool operator!=(const ConnectionHandle& other) const {
        return !(*this == other);
    }
};

} // namespace chess::net
