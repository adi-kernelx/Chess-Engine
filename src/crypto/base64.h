/**
 * base64.h — RFC 4648 standard base64, encode and decode.
 *
 * Not cryptography, but on the critical path of everything that follows: the
 * sealed envelope moves 1184-byte public keys and 3309-byte signatures through
 * a JSON channel that cannot carry raw bytes.
 *
 * Hand-written for two reasons beyond the project rule. OpenSSL's EVP_EncodeBlock
 * is already used in the WebSocket handshake but has no matching strict decoder —
 * EVP_DecodeBlock silently ignores invalid characters and cannot tell you how many
 * padding bytes it dropped, so a caller cannot recover the exact original length.
 * For key material, "close enough" is a bug.
 *
 * decode() is therefore STRICT: it rejects any character outside the alphabet
 * (including whitespace and newlines), rejects wrong-length input, and rejects
 * misplaced padding. Every base64 field in the wire protocol has a known exact
 * length, so a lenient decoder would only ever turn a detectable protocol error
 * into an undetectable one.
 *
 * Note on what this is NOT: the decoder is not constant-time. It only ever
 * handles public values — key ids, public keys, ciphertexts, signatures — whose
 * bytes are not secret. Secrets in this project never travel as base64.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace chess {
namespace crypto {

std::string encode_base64(const uint8_t* data, size_t len);

inline std::string encode_base64(const std::vector<uint8_t>& v) {
    return encode_base64(v.data(), v.size());
}

/**
 * Strict decode. Returns false and leaves `out` empty on any malformed input.
 * @param expected_len  if non-zero, also require exactly this many output bytes
 */
bool decode_base64(const std::string& text, std::vector<uint8_t>& out,
                   size_t expected_len = 0);

// ── base64url (RFC 4648 §5), used by JWT (Phase 7.7) ─────────────────────────
//
// Two differences from the standard alphabet: `+` → `-`, `/` → `_`, and the
// padding is dropped entirely. Feeding a base64url string to decode_base64
// silently succeeds against some inputs and fails against others (they share
// 62 characters), which is exactly the kind of "close enough" bug the strict
// decoder exists to avoid. Keep the two codecs separate.

std::string encode_base64url(const uint8_t* data, size_t len);
inline std::string encode_base64url(const std::vector<uint8_t>& v) {
    return encode_base64url(v.data(), v.size());
}

/**
 * Strict decode. No padding accepted; a decoder that also handled the padded
 * form would let two different strings decode to the same bytes.
 */
bool decode_base64url(const std::string& text, std::vector<uint8_t>& out,
                      size_t expected_len = 0);

} // namespace crypto
} // namespace chess
