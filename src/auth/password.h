/**
 * password.h — Argon2id password hashing via OpenSSL's `ARGON2ID` EVP_KDF.
 *
 * WHY ARGON2ID AND NOT BCRYPT / PBKDF2 / SCRYPT
 *
 * bcrypt caps at 72 input bytes and has no memory-hardness parameter, so an
 * ASIC attacker gets a per-guess cost of a fraction of a cent. PBKDF2 is
 * likewise memory-cheap. Scrypt is memory-hard but exposes only one tuning
 * knob (N), which forces a coupling between time and memory. Argon2id is the
 * Password Hashing Competition winner and the RFC 9106 recommendation; the
 * `id` variant is a data-dependent/independent hybrid that resists both
 * side-channel and time-memory tradeoff attacks.
 *
 * WHY THE "SECOND" RFC 9106 PROFILE (m = 19 456 KiB, t = 2, p = 1)
 *
 * The maximalist profile is m=2 GiB. That is exactly the wrong shape for a
 * Cloud Run deployment: memory is billed per instance-second, and 2 GiB times
 * concurrent logins turns any authentication burst into a self-inflicted DoS.
 * The 19 MiB profile is RFC 9106 §4's low-memory recommendation, verifies in
 * roughly 30 ms on the target CPUs, and costs an attacker with a commodity
 * GPU on the order of $10K per billion guesses even before you add rate
 * limiting on top.
 *
 * WHY PHC STRINGS ARE THE STORAGE FORMAT
 *
 *   $argon2id$v=19$m=19456,t=2,p=1$<salt-b64>$<hash-b64>
 *
 * Every parameter needed to verify the hash is embedded, so:
 *
 *   1. Bumping m/t/p later is transparent: `needs_upgrade()` returns true on
 *      an older row, and the next successful login re-hashes with the new
 *      parameters. Old and new coexist forever.
 *   2. Hashes migrate cleanly to another service (or in from one) that also
 *      speaks PHC.
 *   3. The verify() path derives its parameters from the string, so a rolled
 *      hash cannot be defeated by changing the compiled defaults.
 *
 * WHY VERIFICATION MUST NOT USE ==
 *
 * A byte-by-byte `memcmp` returns as soon as the first differing byte is
 * found. The timing of that early exit is a secret-dependent side channel: an
 * attacker who can measure request latency to microsecond precision (e.g. on
 * the same physical machine) learns the correct hash prefix one byte at a
 * time. `verify_password()` uses `CRYPTO_memcmp` — the OpenSSL constant-time
 * comparison — for the same reason `constant_time_equals` is used on MAC tags.
 *
 * WHY THE DUMMY HASH IS PART OF THIS API
 *
 * A wrong-username lookup that returns "no such user" without doing Argon2id
 * work takes microseconds. A wrong-password check for a real user takes
 * ~30 ms. That difference is a username oracle: an attacker learns which
 * accounts exist by wall-clock timing alone, then focuses their password
 * guessing. `dummy_phc()` returns a fixed valid PHC string with today's
 * parameters, so the wrong-username branch can call `verify_password()`
 * against it and take the same amount of time. `test_password` measures this.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace chess {
namespace auth {

/// Argon2id parameters currently in use. Raising them requires no schema
/// change; the next login upgrades that row automatically.
namespace argon2 {
constexpr uint32_t MEMCOST_KIB     = 19456;   // 19 MiB per verification
constexpr uint32_t ITERATIONS      = 2;
constexpr uint32_t PARALLELISM     = 1;
constexpr size_t   SALT_SIZE       = 16;      // 128 bits — RFC 9106 §3.1
constexpr size_t   HASH_SIZE       = 32;      // 256 bits — RFC 9106 §3.1
constexpr uint32_t VERSION         = 0x13;    // 19
} // namespace argon2

/**
 * Hash `password` and return a PHC string.
 *
 * Empty on failure. Password may be arbitrary bytes — no length limit and no
 * character restriction (that is the whole point of a KDF over a hash). An
 * empty password is legal for the API and produces a valid PHC string; the
 * calling handler MUST enforce a minimum length before invoking this.
 */
std::string hash_password(const std::string& password);

/**
 * Constant-time verify. Returns false for a wrong password, an empty PHC, or
 * any parse error — with no way for the caller to tell which. That
 * indistinguishability is what stops error-message oracles.
 */
bool verify_password(const std::string& password, const std::string& phc);

/**
 * True if the parameters in `phc` are weaker than the current defaults. Callers
 * upgrade opportunistically: on a successful login with an old hash, re-hash
 * with the fresh parameters and UPDATE the row.
 */
bool needs_upgrade(const std::string& phc);

/**
 * A pre-computed valid PHC string with the current parameters. Handlers hit
 * this when a login arrives for a non-existent username so that wall-clock
 * timing is the same as a real user with a wrong password.
 */
const std::string& dummy_phc();

/**
 * Hash with explicit parameters. Exposed for two reasons that both matter:
 *
 *   1. Tests that construct a "row from an older deployment" cannot fake it
 *      by string-editing an existing PHC — the digest was derived under the
 *      original params, so verification would fail. Producing the row with
 *      the weaker params from the start is the only correct way.
 *   2. A future config could raise `MEMCOST_KIB` and want to keep a lighter
 *      profile for a specific write path (a batch importer, say). Having the
 *      knob visible is cheaper than adding it later.
 *
 * Bounds identical to parse_phc(): m ≤ 1 GiB, t ≤ 64, lanes ≤ 16.
 */
std::string hash_password_with_params(const std::string& password,
                                      uint32_t memcost_kib,
                                      uint32_t iterations,
                                      uint32_t parallelism);

} // namespace auth
} // namespace chess
