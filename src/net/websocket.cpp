/**
 * websocket.cpp — WebSocket protocol implementation.
 * 
 * This file implements three core pieces of the WebSocket protocol:
 * 
 * 1. HANDSHAKE (try_handshake):
 *    - Parse the incoming HTTP request to find "Upgrade: websocket"
 *    - Extract the Sec-WebSocket-Key header
 *    - Concatenate it with the RFC 6455 magic GUID
 *    - SHA1 hash the result, then Base64 encode it
 *    - Send back HTTP 101 with the computed accept key
 * 
 * 2. FRAME READING (try_read_frame):
 *    - Parse the binary frame header (FIN, opcode, mask, length)
 *    - Handle 3 length encodings: 7-bit (≤125), 16-bit (126), 64-bit (127)
 *    - Unmask client data by XOR-ing each byte with a 4-byte rotating key
 * 
 * 3. FRAME WRITING (write_frame):
 *    - Build the binary frame header
 *    - Server frames are NOT masked (per spec)
 */

#include "websocket.h"
#include "../core/logger.h"
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <sstream>
#include <algorithm>
#include <cstring>

namespace chess {
namespace net {

// ──────────────────────────────────────────────
// FOOLPROOF Base64 encoding using EVP_EncodeBlock
// ──────────────────────────────────────────────
static std::string base64_encode(const unsigned char* data, size_t length) {
    // EVP_EncodeBlock requires exactly: 4 * ((length + 2) / 3) + 1 bytes
    std::vector<unsigned char> out(4 * ((length + 2) / 3) + 1);
    
    // Encodes the block and returns the exact length (without newlines)
    int out_len = EVP_EncodeBlock(out.data(), data, length);
    
    return std::string(reinterpret_cast<char*>(out.data()), out_len);
}

// ──────────────────────────────────────────────
// Compute Sec-WebSocket-Accept (RFC 6455 §4.2.2)
// ──────────────────────────────────────────────
std::string WebSocket::compute_accept_key(const std::string& client_key) {
    // Hardcoding the exact RFC 6455 Magic GUID here to rule out any 
    // invisible typos or whitespace in your websocket.h macro
    const std::string GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    
    // Step 1: Concatenate
    std::string combined = client_key + GUID;
    
    // Step 2: SHA1 Hash
    unsigned char sha1_hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(combined.c_str()),
         combined.length(), sha1_hash);
    
    // Step 3: Base64 Encode
    std::string accept_key = base64_encode(sha1_hash, SHA_DIGEST_LENGTH);
    
    // Log the exact keys wrapped in brackets to expose any hidden spaces!
    core::Logger::debug("net", "WebSocket", "Client Key: [" + client_key + "]");
    core::Logger::debug("net", "WebSocket", "Accept Key: [" + accept_key + "]");
    
    return accept_key;
}



// ──────────────────────────────────────────────
// Parse HTTP request headers
// ──────────────────────────────────────────────
bool WebSocket::parse_http_request(const std::string& raw,
                                    std::unordered_map<std::string, std::string>& headers) {
    std::istringstream stream(raw);
    std::string line;
    
    // First line: "GET / HTTP/1.1"
    if (!std::getline(stream, line)) return false;
    
    // Remove \r if present (HTTP uses \r\n line endings)
    if (!line.empty() && line.back() == '\r') line.pop_back();
    
    // Verify it's a GET request
    if (line.find("GET") != 0) return false;
    
    // Parse remaining header lines: "Key: Value"
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;  // Empty line = end of headers
        
        auto colon_pos = line.find(':');
        if (colon_pos == std::string::npos) continue;
        
        std::string key = line.substr(0, colon_pos);
        std::string value = line.substr(colon_pos + 1);
        
        // Trim leading and trailing whitespace from value
        size_t start = value.find_first_not_of(" \t\r\n");
        size_t end = value.find_last_not_of(" \t\r\n");
        if (start != std::string::npos && end != std::string::npos) {
            value = value.substr(start, end - start + 1);
        } else {
            value = "";
        }
        
        // Convert key to lowercase for case-insensitive comparison
        // HTTP headers are case-insensitive per RFC 2616
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        
        headers[key] = value;
    }
    
    return true;
}

// ──────────────────────────────────────────────
// WebSocket Handshake
// ──────────────────────────────────────────────
bool WebSocket::try_handshake(Connection& conn) {
    const auto& buf = conn.get_read_buffer();
    
    // Check if we have a complete HTTP request (ends with \r\n\r\n)
    std::string raw(buf.begin(), buf.end());
    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;  // Haven't received full headers yet
    }
    
    // Parse the HTTP headers
    std::unordered_map<std::string, std::string> headers;
    if (!parse_http_request(raw, headers)) {
        core::Logger::warn("net", "WebSocket", "Failed to parse HTTP request from " + conn.get_ip());
        return false;
    }
    
    // Verify this is a WebSocket upgrade request
    auto upgrade_it = headers.find("upgrade");
    if (upgrade_it == headers.end() || upgrade_it->second != "websocket") {
        core::Logger::warn("net", "WebSocket", "Not a WebSocket upgrade request from " + conn.get_ip());
        return false;
    }
    
    // Extract the client's random key
    auto key_it = headers.find("sec-websocket-key");
    if (key_it == headers.end()) {
        core::Logger::warn("net", "WebSocket", "Missing Sec-WebSocket-Key from " + conn.get_ip());
        return false;
    }
    
    // Compute our response key
    std::string accept_key = compute_accept_key(key_it->second);
    
    // Build the HTTP 101 Switching Protocols response
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept_key + "\r\n"
        "\r\n";
    
    // Consume the HTTP request from the read buffer
    conn.consume_read_buffer(header_end + 4);  // +4 for \r\n\r\n
    
    // Queue the 101 response for sending
    conn.append_to_write_buffer(response);
    
    // Mark connection as upgraded
    conn.set_upgraded(true);
    
    core::Logger::info("net", "WebSocket", "Handshake complete for " + conn.get_ip());
    return true;
}

// ──────────────────────────────────────────────
// WebSocket Frame Reader
// ──────────────────────────────────────────────
bool WebSocket::try_read_frame(Connection& conn, WsFrame& frame) {
    const auto& buf = conn.get_read_buffer();
    
    // Minimum frame size: 2 bytes (header) + 0 bytes payload
    if (buf.size() < 2) return false;
    
    size_t pos = 0;
    
    // ─── Byte 0: FIN bit + opcode ───
    // Bit layout: [FIN][RSV1][RSV2][RSV3][OPCODE × 4]
    //
    // FIN=1 means this is the complete message (not fragmented).
    // We read it by masking with 0x80 (10000000 in binary).
    // The opcode is the lower 4 bits, masked with 0x0F (00001111).
    frame.fin = (buf[0] & 0x80) != 0;
    frame.opcode = static_cast<WsOpcode>(buf[0] & 0x0F);
    
    // ─── Byte 1: MASK bit + payload length ───
    // Bit layout: [MASK][LENGTH × 7]
    //
    // MASK=1 means the payload is XOR-encrypted with a 4-byte key.
    // Client→server frames MUST be masked. Server→client MUST NOT.
    bool masked = (buf[1] & 0x80) != 0;
    uint64_t payload_length = buf[1] & 0x7F;  // Lower 7 bits
    pos = 2;
    
    // ─── Extended payload length ───
    // If the 7-bit length field is:
    //   0-125:  That's the actual length. Done.
    //   126:    The next 2 bytes are the actual length (big-endian uint16).
    //   127:    The next 8 bytes are the actual length (big-endian uint64).
    //
    // This encoding is clever: small messages (≤125 bytes, which is most
    // chat messages) have zero overhead. Large messages pay 2 or 8 extra bytes.
    
    if (payload_length == 126) {
        if (buf.size() < pos + 2) return false;  // Need more data
        payload_length = (static_cast<uint64_t>(buf[pos]) << 8) |
                          static_cast<uint64_t>(buf[pos + 1]);
        pos += 2;
    } else if (payload_length == 127) {
        if (buf.size() < pos + 8) return false;
        payload_length = 0;
        for (int i = 0; i < 8; ++i) {
            payload_length = (payload_length << 8) | static_cast<uint64_t>(buf[pos + i]);
        }
        pos += 8;
    }
    
    // ─── Masking key (4 bytes, only if masked) ───
    uint8_t mask_key[4] = {0};
    if (masked) {
        if (buf.size() < pos + 4) return false;
        memcpy(mask_key, &buf[pos], 4);
        pos += 4;
    }
    
    // ─── Check we have the full payload ───
    if (buf.size() < pos + payload_length) return false;
    
    // ─── Extract and unmask payload ───
    // The masking algorithm is simple XOR with a rotating 4-byte key:
    //   decoded[i] = encoded[i] XOR mask_key[i % 4]
    //
    // Why masking? Not for encryption (the key is sent in the clear!).
    // It prevents a class of attacks where a malicious page could craft
    // WebSocket frames that look like valid HTTP requests to intermediate
    // proxies, poisoning their caches.
    frame.payload.resize(payload_length);
    for (uint64_t i = 0; i < payload_length; ++i) {
        if (masked) {
            frame.payload[i] = buf[pos + i] ^ mask_key[i % 4];
        } else {
            frame.payload[i] = buf[pos + i];
        }
    }
    
    // Consume the entire frame from the read buffer
    conn.consume_read_buffer(pos + payload_length);
    
    return true;
}

// ──────────────────────────────────────────────
// WebSocket Frame Writer
// ──────────────────────────────────────────────
void WebSocket::write_frame(Connection& conn, WsOpcode opcode, const uint8_t* data, size_t length) {
    std::vector<uint8_t> frame_data;
    
    // Byte 0: FIN=1 (complete message) + opcode
    frame_data.push_back(0x80 | static_cast<uint8_t>(opcode));
    
    // Byte 1+: Payload length (MASK=0 for server→client)
    if (length <= 125) {
        frame_data.push_back(static_cast<uint8_t>(length));
    } else if (length <= 65535) {
        frame_data.push_back(126);
        frame_data.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
        frame_data.push_back(static_cast<uint8_t>(length & 0xFF));
    } else {
        frame_data.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame_data.push_back(static_cast<uint8_t>((length >> (8 * i)) & 0xFF));
        }
    }
    
    // Payload (no masking for server→client)
    frame_data.insert(frame_data.end(), data, data + length);
    
    conn.append_to_write_buffer(frame_data.data(), frame_data.size());
}

void WebSocket::write_frame(Connection& conn, WsOpcode opcode, const std::string& payload) {
    write_frame(conn, opcode,
                reinterpret_cast<const uint8_t*>(payload.data()),
                payload.size());
}

// ──────────────────────────────────────────────
// WebSocket Close Frame
// ──────────────────────────────────────────────
void WebSocket::send_close(Connection& conn, uint16_t status_code) {
    // Close frame payload: 2-byte status code in big-endian
    uint8_t payload[2];
    payload[0] = static_cast<uint8_t>((status_code >> 8) & 0xFF);
    payload[1] = static_cast<uint8_t>(status_code & 0xFF);
    write_frame(conn, WsOpcode::CLOSE, payload, 2);
}

// ──────────────────────────────────────────────
// WebSocket Pong Frame
// ──────────────────────────────────────────────
void WebSocket::send_pong(Connection& conn, const std::vector<uint8_t>& ping_payload) {
    // Per RFC 6455 §5.5.3: A pong frame must echo the ping's payload exactly
    write_frame(conn, WsOpcode::PONG, ping_payload.data(), ping_payload.size());
}

// ──────────────────────────────────────────────
// Message Router
// ──────────────────────────────────────────────
void MessageRouter::register_handler(const std::string& type, WsMessageHandler handler) {
    handlers_[type] = std::move(handler);
}

void MessageRouter::route(Connection& conn, const std::string& message) {
    // Simple JSON "type" extraction without a full JSON parser.
    // Looks for: "type":"some_value" or "type": "some_value"
    // We'll replace this with nlohmann/json later, but this works
    // for bootstrapping and testing without extra dependencies.
    
    size_t type_pos = message.find("\"type\"");
    if (type_pos == std::string::npos) {
        if (default_handler_) {
            default_handler_(conn, message);
        } else {
            core::Logger::warn("net", "MessageRouter", "No 'type' field in message: " + message);
        }
        return;
    }
    
    // Find the value after "type":
    size_t colon_pos = message.find(':', type_pos + 6);
    if (colon_pos == std::string::npos) return;
    
    size_t quote_start = message.find('"', colon_pos + 1);
    if (quote_start == std::string::npos) return;
    
    size_t quote_end = message.find('"', quote_start + 1);
    if (quote_end == std::string::npos) return;
    
    std::string type = message.substr(quote_start + 1, quote_end - quote_start - 1);
    
    auto it = handlers_.find(type);
    if (it != handlers_.end()) {
        it->second(conn, message);
    } else if (default_handler_) {
        default_handler_(conn, message);
    } else {
        core::Logger::warn("net", "MessageRouter", "No handler for type: " + type);
    }
}

} // namespace net
} // namespace chess
