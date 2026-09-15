/**
 * random.h — Cryptographically secure randomness.
 *
 * This is the one primitive in the whole project that is NEVER hand-rolled,
 * regardless of what the developer has studied. A weak or predictable CSPRNG
 * silently destroys every other guarantee in the system: salts become
 * guessable, session tokens become forgeable, and ephemeral keys become
 * recoverable — all while every functional test still passes, because the
 * output still *looks* random.
 *
 * std::mt19937 and std::random_device are NOT acceptable substitutes.
 * Mersenne Twister is fully predictable after observing 624 outputs, and
 * std::random_device is permitted by the standard to be a deterministic
 * counter (it historically was, on some MinGW builds).
 *
 * We use OpenSSL's RAND_bytes, which draws from the OS entropy source
 * (getrandom(2) on Linux).
 *
 * Failure policy: abort, never degrade. RAND_bytes can fail if the entropy
 * source is unavailable. Returning zeros or falling back to a weaker generator
 * would turn a loud, immediate failure into a silent, permanent compromise.
 */

#pragma once

#include "crypto/secure_buffer.h"

#include <cstddef>
#include <cstdint>

namespace chess {
namespace crypto {

/**
 * Fill `out` with `len` cryptographically secure random bytes.
 * Terminates the process if the system CSPRNG fails — see failure policy above.
 */
void secure_random_bytes(uint8_t* out, size_t len);

/// Allocate a SecureBuffer of `len` random bytes.
SecureBuffer secure_random_buffer(size_t len);

} // namespace crypto
} // namespace chess
