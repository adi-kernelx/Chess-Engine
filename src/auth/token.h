/**
 * token.h — JWT (RFC 7519) built from scratch on hand-written HMAC-SHA-384.
 *
 * WHY BUILD JWT BY HAND
 *
 * A production system would take a well-audited library. This is Aditya's
 * educational systems project and the JWS spec is small: base64url, HMAC, a
 * strict header check. What isn't small is the list of ways a token library
 * can go wrong, and every one of them is instructive:
 *
 *   1. `alg=none`. The RFC says a token can declare its own algorithm as
 *      "none" and carry no signature. The correct answer is always to reject
 *      it. Every couple of years a shipped library forgets and it becomes a
 *      CVE — most famously CVE-2015-9235.
 *
 *   2. Algorithm confusion. A verifier that accepts BOTH RS256 (asymmetric,
 *      verifies with a public key) AND HS256 (symmetric, verifies with a
 *      key that is *also* the signing key) can be tricked: the attacker
 *      sends an HS256 header, signs the token with the server's *public*
 *      key — which the server happily uses as the HMAC key. This verifier
 *      accepts only one algorithm, HS384, and refuses to try any other.
 *
 *   3. Non-canonical base64url. Trailing bits that are not zero, or accepting
 *      the padded form, means two distinct strings can round-trip to the
 *      same bytes. `decode_base64url` in this project rejects both.
 *
 *   4. Signing over the base64url form, not the raw JSON. Every JWT library
 *      does this because two encoders can produce different byte sequences
 *      for the same JSON — spaces, key order, escape choices — and the
 *      signature must be over what the wire actually carried.
 *
 * WHY HS384 AND NOT HS256
 *
 * The whole cryptographic sub-project is oriented around NIST Category 3 =
 * ~192-bit classical strength (see the AEAD tag choice in aead.h). Using
 * HS256 here would make the JWT the weakest link at 128 bits. Cost is 16
 * additional bytes per signature.
 *
 * WHY REFRESH TOKENS ARE OPAQUE, NOT JWTs
 *
 * A refresh token needs to be REVOCABLE. A JWT is a bearer credential the
 * server signs and forgets about — that is the whole design goal — so
 * revoking one requires an allowlist that defeats the point. Refresh tokens
 * here are 32-byte random values; the sessions table stores their SHA-384
 * hash. Rotation, family revocation, and logout_all are all row deletes.
 * The access token stays a JWT (stateless, cheap to check).
 *
 * WHERE `token_epoch` FITS
 *
 * Bumping `players.token_epoch` invalidates every unexpired access token for
 * that player at once — that is what makes `logout_all` real for JWTs whose
 * lifetimes we cannot revoke individually. The claim carries an `epoch`; the
 * auth gate re-reads the row's `token_epoch` and rejects mismatches.
 */

#pragma once

#include "crypto/secure_buffer.h"

#include <cstdint>
#include <string>

namespace chess {
namespace auth {

/// The claims we sign. Deliberately small: an access token that carries less
/// carries fewer accidents.
struct AccessClaims {
    int64_t     player_id   = 0;   ///< "sub" — subject
    std::string username;          ///< "username" — for logging; the DB is truth
    int         token_epoch = 0;   ///< "epoch"
    int64_t     issued_at   = 0;   ///< "iat" (unix seconds)
    int64_t     expires_at  = 0;   ///< "exp" (unix seconds)
};

/// Default access-token lifetime. Short because compromise = full account
/// access; refreshes let the client stay signed in without lengthening it.
constexpr int64_t ACCESS_TOKEN_TTL_SECONDS  = 15 * 60;

/// Refresh-token lifetime. A month is a common compromise between "user
/// doesn't have to log in every day" and "a stolen refresh token has a
/// finite window before natural death removes it".
constexpr int64_t REFRESH_TOKEN_TTL_SECONDS = 30 * 24 * 60 * 60;

/// Raw refresh tokens are 32 bytes = 256 bits before base64url.
constexpr size_t  REFRESH_TOKEN_BYTES = 32;

/// The signing key wrapper. Move-only because it holds a secret.
class TokenSigner {
public:
    TokenSigner() = default;
    TokenSigner(const TokenSigner&)            = delete;
    TokenSigner& operator=(const TokenSigner&) = delete;
    TokenSigner(TokenSigner&&) noexcept        = default;
    TokenSigner& operator=(TokenSigner&&) noexcept = default;

    /**
     * Adopt an existing 32-byte key. The buffer is moved in; callers do not
     * see it again.
     */
    static TokenSigner from_key(crypto::SecureBuffer key);

    /**
     * Read a base64-encoded 32-byte key from `$JWT_SIGNING_KEY`. Returns an
     * invalid signer with `out_error` set if the env var is missing or
     * malformed. Never logs the value.
     */
    static TokenSigner from_env(std::string& out_error);

    /// Generate a fresh signer with a random key. For tests and for
    /// first-run bootstrapping (the operator captures the printed key).
    static TokenSigner generate_random();

    bool valid() const { return key_.size() == 32; }

    /// Encode the claims as a signed HS384 JWT. Empty on failure.
    std::string issue_access(const AccessClaims& c) const;

    /**
     * Verify and parse. Returns false for any failure — bad signature, bad
     * alg, wrong header, expired, unparseable payload — with no way for the
     * caller to distinguish. All failures write nothing to `out`.
     *
     * @param now_unix  current time in seconds since epoch. Injected rather
     *                  than read from `time()` so tests can be exact.
     */
    bool verify_access(const std::string& jwt, int64_t now_unix,
                       AccessClaims& out) const;

private:
    explicit TokenSigner(crypto::SecureBuffer key) : key_(std::move(key)) {}
    crypto::SecureBuffer key_;
};

// ── Refresh tokens ───────────────────────────────────────────────────────────

/**
 * Fresh 32 random bytes as base64url. The value returned here is the ONLY
 * copy the server ever holds in the clear; the sessions table stores its
 * SHA-384 hash. Never log this.
 */
std::string mint_refresh_token();

/// SHA-384 hash of the token, hex-encoded, suitable for the `token_hash` PK.
std::string refresh_token_hash(const std::string& token);

/**
 * Version-4 UUID as canonical text ("xxxxxxxx-xxxx-4xxx-Nxxx-xxxxxxxxxxxx",
 * N ∈ {8,9,a,b}). Used for session family ids; kept here because the
 * `sessions` table stores it and every caller of session.h needs one.
 */
std::string new_uuid_v4();

} // namespace auth
} // namespace chess
