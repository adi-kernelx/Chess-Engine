/**
 * websocket.h — WebSocket protocol implementation (RFC 6455).
 * 
 * The WebSocket protocol has two phases:
 * 
 * 1. HANDSHAKE: The client sends a normal HTTP GET request with special
 *    headers asking to "upgrade" the connection. The server responds with
 *    HTTP 101 and a cryptographic proof that it understood the request.
 *    After this, the TCP connection switches from HTTP to WebSocket framing.
 * 
 * 2. DATA TRANSFER: Messages are sent as "frames" — small binary headers
 *    followed by payload data. Client→server frames are XOR-masked (a
 *    security measure to prevent proxy cache poisoning). Server→client
 *    frames are sent unmasked.
 * 
 * Frame format (simplified):
 *   [FIN|opcode] [MASK|length] [mask_key (if masked)] [payload]
 *     1 byte       1+ bytes       0 or 4 bytes         N bytes
 */

#pragma once

#include "connection.h"
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace chess {
namespace net {

// WebSocket opcodes (the type of each frame)
enum class WsOpcode : uint8_t {
    CONTINUATION = 0x0,  // Part of a multi-frame message
    TEXT         = 0x1,  // UTF-8 text payload
    BINARY       = 0x2,  // Raw binary payload
    CLOSE        = 0x8,  // Connection close request
    PING         = 0x9,  // Keepalive ping
    PONG         = 0xA   // Keepalive pong (response to ping)
};

// A fully decoded WebSocket frame
struct WsFrame {
    bool fin;              // Is this the final fragment?
    WsOpcode opcode;
    std::vector<uint8_t> payload;
};

class WebSocket {
public:
    /**
     * Try to perform the WebSocket handshake on a connection.
     * Looks for HTTP Upgrade request in the read buffer, computes the
     * accept key, and writes the 101 response to the write buffer.
     * Returns true if handshake was completed successfully.
     */
    static bool try_handshake(Connection& conn);

    /**
     * Try to parse one complete WebSocket frame from the connection's
     * read buffer. If a full frame is available, populates 'frame',
     * consumes the bytes from the buffer, and returns true.
     * If not enough data yet, returns false (caller should wait for more).
     */
    static bool try_read_frame(Connection& conn, WsFrame& frame);

    /**
     * Write a WebSocket frame to the connection's write buffer.
     * Server→client frames are NOT masked (per RFC 6455 §5.1).
     */
    static void write_frame(Connection& conn, WsOpcode opcode, const std::string& payload);
    static void write_frame(Connection& conn, WsOpcode opcode, const uint8_t* data, size_t length);

    /**
     * Send a close frame with an optional status code.
     */
    static void send_close(Connection& conn, uint16_t status_code = 1000);

    /**
     * Send a pong frame (reply to a ping, with the same payload).
     */
    static void send_pong(Connection& conn, const std::vector<uint8_t>& ping_payload);

private:
    // Compute the Sec-WebSocket-Accept value from the client's key.
    // accept = Base64( SHA1( client_key + magic_guid ) )
    static std::string compute_accept_key(const std::string& client_key);
    
    // Parse HTTP headers from raw text
    static bool parse_http_request(const std::string& raw,
                                   std::unordered_map<std::string, std::string>& headers);
    
    // The magic GUID defined by RFC 6455. Every WebSocket implementation
    // in the world uses this exact string. It's not a secret — it's just
    // a way for both sides to prove they understand the protocol.
    static constexpr const char* WEBSOCKET_MAGIC_GUID = "258EAFA5-E914-47DA-95CA-5AB5DC65C3B2";
};

// Callback type for when a complete WebSocket message is received
using WsMessageHandler = std::function<void(Connection& conn, const std::string& message)>;

/**
 * MessageRouter — dispatches incoming WebSocket JSON messages by "type" field.
 * 
 * Expected message format: { "type": "some_type", "data": { ... } }
 * 
 * Usage:
 *   router.register_handler("login", [](Connection& c, const std::string& msg) { ... });
 *   router.route(conn, raw_json_string);
 */
class MessageRouter {
public:
    void register_handler(const std::string& type, WsMessageHandler handler);
    void route(Connection& conn, const std::string& message);
    
    void set_default_handler(WsMessageHandler handler) { default_handler_ = std::move(handler); }

private:
    std::unordered_map<std::string, WsMessageHandler> handlers_;
    WsMessageHandler default_handler_;
};

} // namespace net
} // namespace chess
