#include "crypto/sha256.h"

#include <cstring>

namespace chess {
namespace crypto {

namespace {

// FIPS 180-4 §4.2.2 — the first 32 bits of the fractional parts of the cube
// roots of the first 64 primes. "Nothing up my sleeve" numbers: they are
// derived from a public formula so nobody can claim a backdoor was chosen.
const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

inline uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

// FIPS 180-4 §4.1.2
inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z)  { return (x & y) ^ (~x & z); }
inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
inline uint32_t big_sigma0(uint32_t x)   { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
inline uint32_t big_sigma1(uint32_t x)   { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
inline uint32_t small_sigma0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
inline uint32_t small_sigma1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

inline uint32_t load_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           (static_cast<uint32_t>(p[3]));
}

inline void store_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

inline void store_be64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
    }
}

} // namespace

Sha256::Sha256() { reset(); }

void Sha256::reset() {
    // FIPS 180-4 §5.3.3 — fractional parts of the square roots of the first
    // eight primes.
    state_[0] = 0x6a09e667u;
    state_[1] = 0xbb67ae85u;
    state_[2] = 0x3c6ef372u;
    state_[3] = 0xa54ff53au;
    state_[4] = 0x510e527fu;
    state_[5] = 0x9b05688cu;
    state_[6] = 0x1f83d9abu;
    state_[7] = 0x5be0cd19u;

    std::memset(buffer_, 0, sizeof(buffer_));
    buffer_len_  = 0;
    total_bytes_ = 0;
}

void Sha256::process_block(const uint8_t* block) {
    uint32_t w[64];

    // First 16 words come straight from the block, big-endian.
    for (int i = 0; i < 16; ++i) {
        w[i] = load_be32(block + i * 4);
    }
    // The remaining 48 are derived — this is the "message schedule" that
    // diffuses every input bit across the whole round sequence.
    for (int i = 16; i < 64; ++i) {
        w[i] = small_sigma1(w[i - 2]) + w[i - 7] + small_sigma0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + big_sigma1(e) + ch(e, f, g) + K[i] + w[i];
        const uint32_t t2 = big_sigma0(a) + maj(a, b, c);
        h = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }

    // Davies-Meyer feed-forward: adding the previous state back is what makes
    // the compression function one-way. Without it the round function would be
    // invertible and the whole hash would collapse.
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::update(const uint8_t* data, size_t len) {
    if (len == 0 || data == nullptr) return;
    total_bytes_ += len;

    // Top up any partial block first.
    if (buffer_len_ > 0) {
        const size_t need = BLOCK_SIZE - buffer_len_;
        const size_t take = (len < need) ? len : need;
        std::memcpy(buffer_ + buffer_len_, data, take);
        buffer_len_ += take;
        data += take;
        len  -= take;

        if (buffer_len_ == BLOCK_SIZE) {
            process_block(buffer_);
            buffer_len_ = 0;
        }
    }

    // Consume whole blocks directly from the caller's memory.
    while (len >= BLOCK_SIZE) {
        process_block(data);
        data += BLOCK_SIZE;
        len  -= BLOCK_SIZE;
    }

    // Stash the remainder for next time.
    if (len > 0) {
        std::memcpy(buffer_, data, len);
        buffer_len_ = len;
    }
}

void Sha256::update(const std::string& s) {
    update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

Sha256::Digest Sha256::finish() {
    // Padding: 0x80, then zeros, then the 64-bit big-endian BIT length.
    const uint64_t bit_len = total_bytes_ * 8u;

    uint8_t pad[BLOCK_SIZE * 2];
    std::memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;

    // How many bytes until the length field is 64-byte aligned at the end?
    // buffer_len_ + 1 (the 0x80) + pad_zeros + 8 (length) ≡ 0 (mod 64)
    const size_t rem = (buffer_len_ + 1) % BLOCK_SIZE;
    const size_t zeros = (rem <= 56) ? (56 - rem) : (56 + BLOCK_SIZE - rem);
    const size_t pad_len = 1 + zeros;

    store_be64(pad + pad_len, bit_len);

    // update() would add these bytes to total_bytes_, which must not happen —
    // the length field encodes the ORIGINAL message length. Feed blocks directly.
    const size_t total_pad = pad_len + 8;
    {
        size_t off = 0;
        // Complete the pending buffer, then process full blocks of padding.
        while (off < total_pad) {
            const size_t need = BLOCK_SIZE - buffer_len_;
            const size_t take = (total_pad - off < need) ? (total_pad - off) : need;
            std::memcpy(buffer_ + buffer_len_, pad + off, take);
            buffer_len_ += take;
            off += take;
            if (buffer_len_ == BLOCK_SIZE) {
                process_block(buffer_);
                buffer_len_ = 0;
            }
        }
    }

    Digest out{};
    for (int i = 0; i < 8; ++i) {
        store_be32(out.data() + i * 4, state_[i]);
    }
    return out;
}

Sha256::Digest Sha256::hash(const uint8_t* data, size_t len) {
    Sha256 h;
    h.update(data, len);
    return h.finish();
}

Sha256::Digest Sha256::hash(const std::string& s) {
    return hash(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

Sha256::Digest Sha256::hash(const std::vector<uint8_t>& v) {
    return hash(v.data(), v.size());
}

std::string Sha256::to_hex(const Digest& d) {
    static const char* HEX = "0123456789abcdef";
    std::string out;
    out.reserve(DIGEST_SIZE * 2);
    for (size_t i = 0; i < DIGEST_SIZE; ++i) {
        out.push_back(HEX[d[i] >> 4]);
        out.push_back(HEX[d[i] & 0x0f]);
    }
    return out;
}

} // namespace crypto
} // namespace chess
