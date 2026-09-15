#include "crypto/base64.h"

namespace chess {
namespace crypto {

namespace {

const char* ALPHABET =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Reverse table: index by character, -1 means "not in the alphabet".
struct ReverseTable {
    int8_t v[256];
    ReverseTable() {
        for (int i = 0; i < 256; ++i) v[i] = -1;
        for (int i = 0; i < 64; ++i) {
            v[static_cast<unsigned char>(ALPHABET[i])] = static_cast<int8_t>(i);
        }
    }
};

const ReverseTable& reverse_table() {
    static const ReverseTable t;
    return t;
}

} // namespace

std::string encode_base64(const uint8_t* data, size_t len) {
    std::string out;
    if (data == nullptr || len == 0) return out;
    out.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                            static_cast<uint32_t>(data[i + 2]);
        out.push_back(ALPHABET[(v >> 18) & 0x3F]);
        out.push_back(ALPHABET[(v >> 12) & 0x3F]);
        out.push_back(ALPHABET[(v >> 6) & 0x3F]);
        out.push_back(ALPHABET[v & 0x3F]);
    }

    // Tail: 1 or 2 leftover bytes, padded to a full quantum with '='.
    if (i < len) {
        const bool two = (i + 1 < len);
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (two) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        out.push_back(ALPHABET[(v >> 18) & 0x3F]);
        out.push_back(ALPHABET[(v >> 12) & 0x3F]);
        out.push_back(two ? ALPHABET[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

bool decode_base64(const std::string& text, std::vector<uint8_t>& out,
                   size_t expected_len) {
    out.clear();

    if (text.empty()) return expected_len == 0;
    if (text.size() % 4 != 0) return false;   // no unpadded input accepted

    // Padding may only occupy the final one or two characters.
    size_t pad = 0;
    if (text[text.size() - 1] == '=') {
        pad = (text.size() >= 2 && text[text.size() - 2] == '=') ? 2 : 1;
    }

    const ReverseTable& rev = reverse_table();
    const size_t body = text.size() - pad;
    out.reserve((text.size() / 4) * 3 - pad);

    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < body; ++i) {
        const int8_t d = rev.v[static_cast<unsigned char>(text[i])];
        if (d < 0) { out.clear(); return false; }   // includes '=' inside the body
        acc = (acc << 6) | static_cast<uint32_t>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }

    // Leftover bits belong to a partial byte and MUST be zero. Non-zero here
    // means two distinct strings would decode to the same bytes, so a signature
    // or key id could be re-encoded into a different-looking value.
    if (bits > 0 && (acc & ((1u << bits) - 1)) != 0) { out.clear(); return false; }

    if (expected_len != 0 && out.size() != expected_len) { out.clear(); return false; }
    return true;
}

// ── base64url (RFC 4648 §5) ─────────────────────────────────────────────────

namespace {

const char* URL_ALPHABET =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

struct UrlReverseTable {
    int8_t v[256];
    UrlReverseTable() {
        for (int i = 0; i < 256; ++i) v[i] = -1;
        for (int i = 0; i < 64; ++i) {
            v[static_cast<unsigned char>(URL_ALPHABET[i])] = static_cast<int8_t>(i);
        }
    }
};

const UrlReverseTable& url_reverse_table() {
    static const UrlReverseTable t;
    return t;
}

} // namespace

std::string encode_base64url(const uint8_t* data, size_t len) {
    std::string out;
    if (data == nullptr || len == 0) return out;
    out.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                            static_cast<uint32_t>(data[i + 2]);
        out.push_back(URL_ALPHABET[(v >> 18) & 0x3F]);
        out.push_back(URL_ALPHABET[(v >> 12) & 0x3F]);
        out.push_back(URL_ALPHABET[(v >> 6)  & 0x3F]);
        out.push_back(URL_ALPHABET[ v        & 0x3F]);
    }

    if (i < len) {
        const bool two = (i + 1 < len);
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (two) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        out.push_back(URL_ALPHABET[(v >> 18) & 0x3F]);
        out.push_back(URL_ALPHABET[(v >> 12) & 0x3F]);
        // No padding — the whole point of base64url as JWT uses it. The two
        // trailing '=' characters that the standard encoder emits are omitted.
        if (two) out.push_back(URL_ALPHABET[(v >> 6) & 0x3F]);
    }
    return out;
}

bool decode_base64url(const std::string& text, std::vector<uint8_t>& out,
                      size_t expected_len) {
    out.clear();
    if (text.empty()) return expected_len == 0;

    // A length of exactly 1 mod 4 is impossible; every other remainder means a
    // definite number of output bytes (2 for r=2, 3 for r=3, 0 for r=0).
    const size_t r = text.size() % 4;
    if (r == 1) return false;

    const UrlReverseTable& rev = url_reverse_table();
    out.reserve((text.size() * 3) / 4);

    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : text) {
        const int8_t d = rev.v[c];
        // Explicitly reject '=': base64url does not use padding, and accepting
        // it here would mean two different strings decode to the same bytes.
        if (d < 0) { out.clear(); return false; }
        acc = (acc << 6) | static_cast<uint32_t>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    // Trailing bits must be zero — non-canonical encodings would let an
    // attacker rewrite a signed JWT into a different byte string with the
    // same signature relative to some alternate decoder.
    if (bits > 0 && (acc & ((1u << bits) - 1)) != 0) { out.clear(); return false; }

    if (expected_len != 0 && out.size() != expected_len) { out.clear(); return false; }
    return true;
}

} // namespace crypto
} // namespace chess
