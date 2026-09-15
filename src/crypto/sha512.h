/**
 * sha512.h — SHA-512 and SHA-384 from FIPS 180-4. Hand-written, not OpenSSL.
 *
 * Same Merkle-Damgård skeleton as SHA-256, scaled up:
 *
 *                        SHA-256          SHA-512 / SHA-384
 *   word size            32 bits          64 bits
 *   block size           64 bytes         128 bytes
 *   rounds               64               80
 *   length field         64 bits          128 bits
 *   rotation amounts     2/13/22 ...      28/34/39 ...
 *
 * SHA-384 is NOT a separate algorithm. It is SHA-512 with (a) a different
 * initial value and (b) the output truncated to the first 48 bytes. That
 * truncation is what makes it resistant to length-extension attacks, which
 * plain SHA-512 (and SHA-256) are vulnerable to.
 *
 * Why SHA-384 is here at all: Phase 7 signs session tokens with HMAC-SHA-384.
 * ML-KEM-768 and ML-DSA-65 both sit at NIST Category 3 (≈AES-192, ≈96-bit
 * post-Grover). SHA-384 gives ~192 bits against Grover, so the symmetric half
 * of the suite matches the post-quantum half instead of becoming the weak link.
 *
 * On the 128-bit length field: we store the message length in the low 64 bits
 * and zero the high 64. That is exact for any message under 2^61 bytes
 * (2 exabytes), which no chess server will encounter.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace chess {
namespace crypto {

namespace detail {

/// Shared SHA-512 engine. SHA-512 and SHA-384 differ only in IV and output length.
class Sha512Core {
public:
    static constexpr size_t BLOCK_SIZE = 128;

    void init_512();
    void init_384();
    void update(const uint8_t* data, size_t len);
    /// Writes the first `out_len` bytes of the final state (64 for -512, 48 for -384).
    void finish(uint8_t* out, size_t out_len);

private:
    void process_block(const uint8_t* block);

    uint64_t state_[8];
    uint8_t  buffer_[BLOCK_SIZE];
    size_t   buffer_len_  = 0;
    uint64_t total_bytes_ = 0;
};

std::string to_hex(const uint8_t* data, size_t len);

} // namespace detail

class Sha512 {
public:
    static constexpr size_t DIGEST_SIZE = 64;
    static constexpr size_t BLOCK_SIZE  = detail::Sha512Core::BLOCK_SIZE;
    using Digest = std::array<uint8_t, DIGEST_SIZE>;

    Sha512() { core_.init_512(); }

    void update(const uint8_t* data, size_t len) { core_.update(data, len); }
    void update(const std::string& s) {
        core_.update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
    Digest finish() {
        Digest d{};
        core_.finish(d.data(), d.size());
        return d;
    }
    void reset() { core_ = detail::Sha512Core{}; core_.init_512(); }

    static Digest hash(const uint8_t* data, size_t len) {
        Sha512 h; h.update(data, len); return h.finish();
    }
    static Digest hash(const std::string& s) {
        return hash(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
    static std::string to_hex(const Digest& d) { return detail::to_hex(d.data(), d.size()); }

private:
    detail::Sha512Core core_;
};

class Sha384 {
public:
    static constexpr size_t DIGEST_SIZE = 48;
    static constexpr size_t BLOCK_SIZE  = detail::Sha512Core::BLOCK_SIZE;
    using Digest = std::array<uint8_t, DIGEST_SIZE>;

    Sha384() { core_.init_384(); }

    void update(const uint8_t* data, size_t len) { core_.update(data, len); }
    void update(const std::string& s) {
        core_.update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
    Digest finish() {
        Digest d{};
        core_.finish(d.data(), d.size());
        return d;
    }
    void reset() { core_ = detail::Sha512Core{}; core_.init_384(); }

    static Digest hash(const uint8_t* data, size_t len) {
        Sha384 h; h.update(data, len); return h.finish();
    }
    static Digest hash(const std::string& s) {
        return hash(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
    static std::string to_hex(const Digest& d) { return detail::to_hex(d.data(), d.size()); }

private:
    detail::Sha512Core core_;
};

} // namespace crypto
} // namespace chess
