#include "crypto/sha512.h"

#include <cstring>

namespace chess {
namespace crypto {
namespace detail {

namespace {

// FIPS 180-4 §4.2.3 — first 64 bits of the fractional parts of the cube roots
// of the first 80 primes.
const uint64_t K[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull
};

inline uint64_t rotr(uint64_t x, unsigned n) {
    return (x >> n) | (x << (64u - n));
}

inline uint64_t ch(uint64_t x, uint64_t y, uint64_t z)  { return (x & y) ^ (~x & z); }
inline uint64_t maj(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (x & z) ^ (y & z); }
inline uint64_t big_sigma0(uint64_t x)   { return rotr(x, 28) ^ rotr(x, 34) ^ rotr(x, 39); }
inline uint64_t big_sigma1(uint64_t x)   { return rotr(x, 14) ^ rotr(x, 18) ^ rotr(x, 41); }
inline uint64_t small_sigma0(uint64_t x) { return rotr(x, 1) ^ rotr(x, 8) ^ (x >> 7); }
inline uint64_t small_sigma1(uint64_t x) { return rotr(x, 19) ^ rotr(x, 61) ^ (x >> 6); }

inline uint64_t load_be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<uint64_t>(p[i]);
    }
    return v;
}

inline void store_be64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
    }
}

} // namespace

void Sha512Core::init_512() {
    // FIPS 180-4 §5.3.5 — fractional parts of the square roots of primes 2..19.
    state_[0] = 0x6a09e667f3bcc908ull;
    state_[1] = 0xbb67ae8584caa73bull;
    state_[2] = 0x3c6ef372fe94f82bull;
    state_[3] = 0xa54ff53a5f1d36f1ull;
    state_[4] = 0x510e527fade682d1ull;
    state_[5] = 0x9b05688c2b3e6c1full;
    state_[6] = 0x1f83d9abfb41bd6bull;
    state_[7] = 0x5be0cd19137e2179ull;
    std::memset(buffer_, 0, sizeof(buffer_));
    buffer_len_  = 0;
    total_bytes_ = 0;
}

void Sha512Core::init_384() {
    // FIPS 180-4 §5.3.4 — square roots of primes 23..53. A DIFFERENT IV, which
    // is why SHA-384 is not simply "truncated SHA-512": the two produce
    // unrelated outputs even before truncation.
    state_[0] = 0xcbbb9d5dc1059ed8ull;
    state_[1] = 0x629a292a367cd507ull;
    state_[2] = 0x9159015a3070dd17ull;
    state_[3] = 0x152fecd8f70e5939ull;
    state_[4] = 0x67332667ffc00b31ull;
    state_[5] = 0x8eb44a8768581511ull;
    state_[6] = 0xdb0c2e0d64f98fa7ull;
    state_[7] = 0x47b5481dbefa4fa4ull;
    std::memset(buffer_, 0, sizeof(buffer_));
    buffer_len_  = 0;
    total_bytes_ = 0;
}

void Sha512Core::process_block(const uint8_t* block) {
    uint64_t w[80];

    for (int i = 0; i < 16; ++i) {
        w[i] = load_be64(block + i * 8);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = small_sigma1(w[i - 2]) + w[i - 7] + small_sigma0(w[i - 15]) + w[i - 16];
    }

    uint64_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint64_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int i = 0; i < 80; ++i) {
        const uint64_t t1 = h + big_sigma1(e) + ch(e, f, g) + K[i] + w[i];
        const uint64_t t2 = big_sigma0(a) + maj(a, b, c);
        h = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }

    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha512Core::update(const uint8_t* data, size_t len) {
    if (len == 0 || data == nullptr) return;
    total_bytes_ += len;

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

    while (len >= BLOCK_SIZE) {
        process_block(data);
        data += BLOCK_SIZE;
        len  -= BLOCK_SIZE;
    }

    if (len > 0) {
        std::memcpy(buffer_, data, len);
        buffer_len_ = len;
    }
}

void Sha512Core::finish(uint8_t* out, size_t out_len) {
    const uint64_t bit_len = total_bytes_ * 8u;

    // 0x80, zeros, then a 128-bit big-endian bit length. The length must land
    // so the final block ends exactly on a 128-byte boundary: the padded
    // length must be ≡ 112 (mod 128) before the 16-byte length field.
    uint8_t pad[BLOCK_SIZE * 2];
    std::memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;

    const size_t rem   = (buffer_len_ + 1) % BLOCK_SIZE;
    const size_t zeros = (rem <= 112) ? (112 - rem) : (112 + BLOCK_SIZE - rem);
    const size_t pad_len = 1 + zeros;

    // High 64 bits of the 128-bit length stay zero — see the header note.
    store_be64(pad + pad_len, 0);
    store_be64(pad + pad_len + 8, bit_len);

    const size_t total_pad = pad_len + 16;
    size_t off = 0;
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

    // Emit only as many bytes as the caller wants: 64 for SHA-512, 48 for
    // SHA-384. The truncation is what defeats length extension.
    uint8_t full[64];
    for (int i = 0; i < 8; ++i) {
        store_be64(full + i * 8, state_[i]);
    }
    std::memcpy(out, full, out_len);
}

std::string to_hex(const uint8_t* data, size_t len) {
    static const char* HEX = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(HEX[data[i] >> 4]);
        out.push_back(HEX[data[i] & 0x0f]);
    }
    return out;
}

} // namespace detail
} // namespace crypto
} // namespace chess
