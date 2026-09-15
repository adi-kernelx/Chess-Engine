#include "crypto/aes.h"

#include <openssl/crypto.h>

#include <cstring>

namespace chess {
namespace crypto {

namespace {

// FIPS 197 Figure 7 — the AES S-box. Each entry is the multiplicative inverse
// in GF(2^8) followed by an affine transformation over GF(2). It is a fixed
// public table, not a secret.
const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

// Round constants: Rcon[i] = x^(i-1) in GF(2^8). Only 1..7 are reached by
// AES-256 (word index 56 / Nk 8 = 7).
const uint8_t RCON[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

/**
 * Multiply by x in GF(2^8) modulo m(x) = x^8 + x^4 + x^3 + x + 1.
 *
 * Shift left by one. If bit 7 was set the result has degree 8 and must be
 * reduced, which means XORing the low byte of m(x) — 0x1B. The branch is
 * written arithmetically rather than with an `if` so it does not depend on
 * secret data in a way a compiler might turn into a jump.
 */
inline uint8_t xtime(uint8_t a) {
    return static_cast<uint8_t>((a << 1) ^ (((a >> 7) & 1u) * 0x1bu));
}

inline void sub_bytes(uint8_t s[16]) {
    for (int i = 0; i < 16; ++i) s[i] = SBOX[s[i]];
}

/**
 * ShiftRows — row r rotates LEFT by r bytes.
 *
 * With the column-major layout, row r occupies indices r, r+4, r+8, r+12.
 * Row 0 is untouched; rows 1..3 rotate by 1..3.
 */
inline void shift_rows(uint8_t s[16]) {
    uint8_t t;

    // Row 1: left by 1
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;

    // Row 2: left by 2 (swap the two pairs)
    t = s[2];  s[2]  = s[10]; s[10] = t;
    t = s[6];  s[6]  = s[14]; s[14] = t;

    // Row 3: left by 3, i.e. right by 1
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}

/**
 * MixColumns — treat each column as a polynomial over GF(2^8) and multiply by
 * the fixed polynomial {03}x^3 + {01}x^2 + {01}x + {02}, modulo x^4 + 1.
 *
 * In matrix form each column c becomes:
 *     [02 03 01 01]   [s0]
 *     [01 02 03 01] . [s1]
 *     [01 01 02 03]   [s2]
 *     [03 01 01 02]   [s3]
 *
 * The identity 03·a = (02·a) ^ a lets everything reduce to xtime and XOR.
 */
inline void mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; ++c) {
        uint8_t* col = s + 4 * c;
        const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        const uint8_t all = static_cast<uint8_t>(a0 ^ a1 ^ a2 ^ a3);

        // s'0 = 02·a0 ^ 03·a1 ^ a2 ^ a3
        //     = a0 ^ (a0^a1 doubled) ^ (a0^a1^a2^a3)     [algebraic rearrangement]
        col[0] = static_cast<uint8_t>(a0 ^ all ^ xtime(static_cast<uint8_t>(a0 ^ a1)));
        col[1] = static_cast<uint8_t>(a1 ^ all ^ xtime(static_cast<uint8_t>(a1 ^ a2)));
        col[2] = static_cast<uint8_t>(a2 ^ all ^ xtime(static_cast<uint8_t>(a2 ^ a3)));
        col[3] = static_cast<uint8_t>(a3 ^ all ^ xtime(static_cast<uint8_t>(a3 ^ a0)));
    }
}

inline void add_round_key(uint8_t s[16], const uint8_t* rk) {
    for (int i = 0; i < 16; ++i) s[i] ^= rk[i];
}

} // namespace

Aes256::Aes256(const uint8_t key[KEY_SIZE]) {
    expand_key(key);
}

Aes256::~Aes256() {
    // The expanded schedule is as sensitive as the key itself — the original
    // key can be recovered from the last round key.
    OPENSSL_cleanse(round_keys_, sizeof(round_keys_));
}

/**
 * FIPS 197 §5.2 key expansion, Nk = 8 (256-bit key), Nr = 14.
 *
 * Words 0..7 are the key itself. Each later word w[i] = w[i-8] XOR f(w[i-1]),
 * where f depends on the position:
 *
 *   i % 8 == 0  ->  RotWord, SubWord, then XOR Rcon[i/8]
 *   i % 8 == 4  ->  SubWord only          <-- AES-256 ONLY
 *   otherwise   ->  identity
 *
 * That second branch exists only for Nk > 6 and is the single most commonly
 * omitted line when writing AES-256 by hand. Leaving it out still produces a
 * cipher that encrypts and decrypts self-consistently — it just is not AES,
 * which is precisely why the FIPS 197 known-answer test is non-negotiable.
 */
void Aes256::expand_key(const uint8_t key[KEY_SIZE]) {
    constexpr int Nk = 8;                       // key length in 32-bit words
    constexpr int total_words = 4 * (NUM_ROUNDS + 1);   // 60

    std::memcpy(round_keys_, key, KEY_SIZE);

    uint8_t temp[4];
    for (int i = Nk; i < total_words; ++i) {
        std::memcpy(temp, round_keys_ + 4 * (i - 1), 4);

        if (i % Nk == 0) {
            // RotWord: [a0,a1,a2,a3] -> [a1,a2,a3,a0]
            const uint8_t t = temp[0];
            temp[0] = temp[1]; temp[1] = temp[2]; temp[2] = temp[3]; temp[3] = t;
            // SubWord
            for (int j = 0; j < 4; ++j) temp[j] = SBOX[temp[j]];
            // Round constant, applied to the first byte only
            temp[0] ^= RCON[i / Nk];
        } else if (i % Nk == 4) {
            // AES-256 only (Nk > 6): an extra SubWord with no rotation.
            for (int j = 0; j < 4; ++j) temp[j] = SBOX[temp[j]];
        }

        for (int j = 0; j < 4; ++j) {
            round_keys_[4 * i + j] =
                static_cast<uint8_t>(round_keys_[4 * (i - Nk) + j] ^ temp[j]);
        }
    }

    OPENSSL_cleanse(temp, sizeof(temp));
}

void Aes256::encrypt_block(const uint8_t in[BLOCK_SIZE], uint8_t out[BLOCK_SIZE]) const {
    uint8_t state[BLOCK_SIZE];
    std::memcpy(state, in, BLOCK_SIZE);

    // Initial whitening with round key 0.
    add_round_key(state, round_keys_);

    // Rounds 1 .. Nr-1: the full transformation.
    for (size_t round = 1; round < NUM_ROUNDS; ++round) {
        sub_bytes(state);
        shift_rows(state);
        mix_columns(state);
        add_round_key(state, round_keys_ + round * BLOCK_SIZE);
    }

    // Final round: NO MixColumns. Required for the structure to be invertible.
    sub_bytes(state);
    shift_rows(state);
    add_round_key(state, round_keys_ + NUM_ROUNDS * BLOCK_SIZE);

    std::memcpy(out, state, BLOCK_SIZE);
    OPENSSL_cleanse(state, sizeof(state));
}

} // namespace crypto
} // namespace chess
