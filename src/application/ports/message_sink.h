/**
 * application/ports/message_sink.h — one-way delivery to a specific
 * recipient, without letting the use case reach into the transport.
 *
 * A migrated handler builds a fully-formed frame string with the JSON
 * codec, then hands it to a MessageSink. The sink decides how the bytes
 * get on the wire (real socket / test-side capture / metered wrapper).
 *
 * Contract:
 *   - `send()` returns true iff the frame was scheduled for delivery.
 *   - Returning false means the recipient is no longer reachable; the
 *     caller should NOT retry. Distinguishing gone-vs-flushed-vs-full
 *     is deliberately out of scope for LLD-1; the plan doc pushes
 *     bounded outgoing queues / backpressure to LLD-6.
 *   - Implementations must be safe to call from any worker thread. The
 *     socket adapter serialises through the existing connections_mutex_;
 *     a fake test sink is single-threaded by construction.
 *
 * A sink represents ONE recipient. Fan-out to multiple recipients is a
 * caller concern in LLD-1 (broadcasts still walk seat/spectator lists
 * exactly as before). LLD-4 will introduce a broadcast primitive.
 */

#pragma once

#include <string>

namespace chess::application {

class MessageSink {
public:
    virtual ~MessageSink() = default;

    /// Schedule a text frame for delivery to this sink's recipient.
    ///
    /// The frame is a UTF-8 JSON object; the sink does not parse or
    /// validate it. The value is passed by value so the sink can hand
    /// ownership to a background writer without copying.
    virtual bool send(std::string frame) = 0;
};

} // namespace chess::application
