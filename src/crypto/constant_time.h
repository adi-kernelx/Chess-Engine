/**
 * constant_time.h — Comparisons whose duration does not depend on the data.
 *
 * The problem with memcmp() on secrets:
 *   memcmp returns as soon as it finds the first differing byte. That makes
 *   its running time a function of how many leading bytes matched. An attacker
 *   who can submit guesses and measure response time recovers a MAC or token
 *   one byte at a time — roughly 256 * N guesses instead of 256^N. This is a
 *   practical remote attack, not a theoretical one; it has been demonstrated
 *   across networks against real HMAC verification code.
 *
 * The fix is to look at every byte regardless, accumulating differences with
 * OR, and only collapse to a boolean at the very end. There are no branches on
 * secret data and no early exit.
 *
 * `volatile` on the accumulator stops the compiler from "helpfully" rewriting
 * the loop into an early-exit form.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace chess {
namespace crypto {

/**
 * Compare two byte ranges of equal length in time independent of their content.
 *
 * Note that `len` itself is NOT secret — only the bytes are. Callers comparing
 * values of differing length should treat a length mismatch as a plain failure,
 * which is what the overload below does.
 */
inline bool constant_time_equals(const uint8_t* a, const uint8_t* b, size_t len) {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    }
    return diff == 0;
}

/// Length-checked variant. A length mismatch short-circuits — lengths are public.
inline bool constant_time_equals(const uint8_t* a, size_t a_len,
                                 const uint8_t* b, size_t b_len) {
    if (a_len != b_len) return false;
    return constant_time_equals(a, b, a_len);
}

} // namespace crypto
} // namespace chess
