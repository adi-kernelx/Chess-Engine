/**
 * sha256.h — SHA-256, implemented from FIPS 180-4. Hand-written, not OpenSSL.
 *
 * SHA-256 is a Merkle-Damgård construction:
 *
 *   message ──padding──> [block 0][block 1]...[block n]
 *                            │        │           │
 *                   IV ──> compress ─> compress ─> compress ──> digest
 *
 * The message is split into 512-bit blocks. A fixed 256-bit initial value is
 * fed through a compression function once per block, each time mixing in the
 * next block. The final internal state IS the digest — there is no separate
 * output transform.
 *
 * The two details that are easy to get wrong, and which the test vectors in
 * tests/test_hash.cpp specifically target:
 *
 *   1. PADDING. Append a single 0x80 byte, then enough 0x00 bytes so the total
 *      length ≡ 56 (mod 64), then the ORIGINAL message length in BITS as a
 *      64-bit big-endian integer. If the 0x80 lands in the last 8 bytes of a
 *      block there is no room for the length, so an entire extra block is
 *      required. Off-by-one here produces correct-looking digests for most
 *      inputs and wrong ones at specific lengths — exactly why the tests cover
 *      55, 56, 63, 64 and 119 bytes.
 *
 *   2. ENDIANNESS. Everything is big-endian on the wire, which is the opposite
 *      of x86's native order. Both the message schedule load and the digest
 *      store must convert explicitly.
 *
 * The implementation is streaming (update/finish) because HMAC and HKDF need to
 * hash concatenations without first materialising them in memory.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace chess {
namespace crypto {

class Sha256 {
public:
    static constexpr size_t DIGEST_SIZE = 32;  // 256 bits
    static constexpr size_t BLOCK_SIZE  = 64;  // 512 bits

    using Digest = std::array<uint8_t, DIGEST_SIZE>;

    Sha256();

    /// Feed more data. May be called any number of times before finish().
    void update(const uint8_t* data, size_t len);
    void update(const std::string& s);

    /// Produce the digest. The object is left finalised; call reset() to reuse.
    Digest finish();

    /// Return to the initial state so the object can hash a fresh message.
    void reset();

    /// One-shot convenience wrappers.
    static Digest hash(const uint8_t* data, size_t len);
    static Digest hash(const std::string& s);
    static Digest hash(const std::vector<uint8_t>& v);

    /// Lowercase hex, 64 characters. Handy for logs, tests and PGN tags.
    static std::string to_hex(const Digest& d);

private:
    void process_block(const uint8_t* block);

    uint32_t state_[8];                 // h0..h7
    uint8_t  buffer_[BLOCK_SIZE];       // partial block awaiting more data
    size_t   buffer_len_;               // bytes currently in buffer_
    uint64_t total_bytes_;              // total message length, for padding
};

} // namespace crypto
} // namespace chess
