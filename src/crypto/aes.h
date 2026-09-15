/**
 * aes.h — AES-256 block cipher from FIPS 197. Hand-written, not OpenSSL.
 *
 * AES is a substitution-permutation network. One block is 16 bytes held as a
 * 4x4 matrix in COLUMN-MAJOR order — byte i of the input sits at row i%4,
 * column i/4. Getting that layout backwards is the classic first bug, and it
 * produces output that looks perfectly random while being completely wrong,
 * which is why the FIPS 197 vectors in tests/test_aes.cpp matter.
 *
 * Each round applies four transformations:
 *
 *   SubBytes    — every byte through a fixed 256-entry S-box (non-linearity)
 *   ShiftRows   — row r rotated left by r bytes (diffusion across columns)
 *   MixColumns  — each column multiplied by a fixed matrix in GF(2^8)
 *   AddRoundKey — XOR with this round's 16-byte subkey
 *
 * AES-256 runs 14 rounds. The final round OMITS MixColumns — not an
 * optimisation, a requirement: without that omission encryption and decryption
 * would not be symmetric.
 *
 * The GF(2^8) arithmetic in MixColumns is the part that connects directly to
 * the finite-field coursework. Bytes are treated as polynomials over GF(2),
 * multiplied modulo the irreducible polynomial
 *
 *     m(x) = x^8 + x^4 + x^3 + x + 1      (0x11B)
 *
 * `xtime()` below is multiplication by x: shift left, and if the result
 * overflowed 8 bits, reduce by XORing 0x1B. Every other multiplier is built
 * from that — 0x03·a = xtime(a) ^ a, because 3 = x + 1.
 *
 * ─────────────────────────────────────────────────────────────────────────
 * ONLY ENCRYPTION IS IMPLEMENTED. This is deliberate and worth understanding:
 * this codebase uses AES exclusively in CTR mode, and CTR never needs the
 * inverse cipher. It encrypts a counter to make a keystream and XORs — so
 * decryption also calls the FORWARD direction. Skipping InvSubBytes,
 * InvShiftRows, InvMixColumns and the inverse key schedule removes roughly
 * half the code and half the places a bug could hide.
 * ─────────────────────────────────────────────────────────────────────────
 *
 * SIDE-CHANNEL CAVEAT (documented, not hidden): this is a table-driven
 * implementation, so S-box lookups have data-dependent cache behaviour. An
 * attacker able to run code on the same physical machine could in principle
 * recover key bits by observing cache timing. That is outside this project's
 * threat model — the adversary is on the network, not on the host — but it is
 * exactly why production stacks use AES-NI hardware instructions instead.
 * Recorded in docs/SECURITY.md rather than left as a surprise.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace chess {
namespace crypto {

class Aes256 {
public:
    static constexpr size_t KEY_SIZE   = 32;   // 256 bits
    static constexpr size_t BLOCK_SIZE = 16;   // 128 bits — fixed for all AES
    static constexpr size_t NUM_ROUNDS = 14;   // AES-256

    /// Expand a 32-byte key into the 15 round keys (240 bytes).
    explicit Aes256(const uint8_t key[KEY_SIZE]);
    ~Aes256();

    // The expanded key schedule is secret material; do not copy it around.
    Aes256(const Aes256&)            = delete;
    Aes256& operator=(const Aes256&) = delete;

    /// Encrypt exactly one 16-byte block. `in` and `out` may alias.
    void encrypt_block(const uint8_t in[BLOCK_SIZE], uint8_t out[BLOCK_SIZE]) const;

private:
    void expand_key(const uint8_t key[KEY_SIZE]);

    // 15 round keys x 16 bytes, stored so that round n occupies [16n, 16n+16).
    uint8_t round_keys_[(NUM_ROUNDS + 1) * BLOCK_SIZE];
};

} // namespace crypto
} // namespace chess
