/**
 * oauth_verify.h — Google Sign-In via Supabase Auth.
 *
 * The rule under §7.8 is scope discipline. Supabase is an **identity
 * provider** here, nothing more: it terminates the OAuth handshake with
 * Google and hands us a JWT that says "this Google identity signed in".
 * Our server verifies that JWT, records the mapping in `players.google_sub`,
 * and then issues its OWN session tokens (§7.7) — the Supabase JWT is used
 * exactly once and forgotten. One session mechanism, one revocation path,
 * one `logout_all` that actually works.
 *
 * THE ALGORITHM
 *
 * Supabase projects come in two JWT flavours:
 *
 *   HS256 — a shared secret you paste into `$SUPABASE_JWT_SECRET`. This is
 *   a legacy compatibility mode. Verification uses HMAC-SHA-256 without
 *   a network call.
 *
 *   ES256 with JWKS — verifies P-256 signatures using public keys fetched
 *   over certificate-verified HTTPS and cached for five minutes. No shared
 *   Supabase secret is required. RS256 is not supported.
 *
 * WHICH ONE YOUR PROJECT USES is a Supabase-dashboard setting. Keep the
 * algorithm explicitly selected using SUPABASE_JWT_ALGORITHM.
 *
 * THE ALGORITHM-CONFUSION RULE STILL APPLIES
 *
 * The verifier accepts only its configured algorithm; ES256 never falls
 * back to HS256. An attacker who observed an ES256 public
 * key (which is public!) could otherwise sign an HS256 token with it and
 * we would happily HMAC against the public key. Same reasoning as
 * token.h's HS384-only rule.
 *
 * WHAT WE CHECK
 *
 *   1. Signature      — ES256 public-key verification or legacy HS256.
 *   2. iss            — must match the configured Supabase project URL.
 *   3. aud            — must be "authenticated".
 *   4. exp            — must be in the future.
 *   5. iat            — must not be in the far future (>60 s skew).
 *   6. email_verified — must be true. Supabase surfaces this on the top
 *                       level for OAuth identities; a false value here
 *                       means Google itself has not verified the address.
 *   7. app_metadata.provider — must be the configured provider (default
 *                       "google"). A Supabase user who signed up with
 *                       email/password would have `provider: "email"` and
 *                       must not be able to impersonate a Google identity.
 *
 * WHY `sub` IS THE ACCOUNT KEY, NOT EMAIL
 *
 * Emails are transferable and reused; a stale account name that once
 * belonged to a former colleague can end up owned by their replacement.
 * `sub` is Supabase's opaque, stable, immutable user id — the correct
 * primary key for "this specific person's account".
 *
 * The players table calls this column `google_sub` for readability, but
 * what it actually stores is Supabase's `sub` scoped to this Supabase
 * project. Migrating to a different Supabase project would break the
 * mapping; that is a deliberate boundary, not a bug.
 *
 * VERIFIED EMAIL LINKING
 *
 * A matching password account is reused only when its stored email was
 * verified and the validated Google JWT proves the same email. A typed or
 * unverified email never authorizes linking. Another Google sub is never
 * replaced. Existing Google mappings remain keyed by stable Supabase sub.
 */

#pragma once

#include "auth/token.h"
#include "auth/jwks_verifier.h"
#include "crypto/secure_buffer.h"
#include "storage/database.h"

#include <cstdint>
#include <string>

namespace chess {
namespace auth {

/// What we extract from a valid Supabase JWT.
struct SupabaseIdentity {
    std::string sub;             ///< Supabase user id (the stable key)
    std::string email;           ///< canonical form Supabase resolved for the user
    bool        email_verified = false;
    std::string provider;        ///< "google", from app_metadata.provider
};

class SupabaseVerifier {
public:
    SupabaseVerifier() = default;
    SupabaseVerifier(const SupabaseVerifier&) = delete;
    SupabaseVerifier& operator=(const SupabaseVerifier&) = delete;
    SupabaseVerifier(SupabaseVerifier&&) noexcept = default;
    SupabaseVerifier& operator=(SupabaseVerifier&&) noexcept = default;

    /**
     * Build from an HS256 shared secret plus the project-scoped invariants.
     *
     * @param issuer   e.g. "https://xyz.supabase.co/auth/v1"
     * @param audience e.g. "authenticated"
     * @param provider e.g. "google" — the app_metadata.provider we require.
     *                 A Supabase account created via email/password has
     *                 provider="email" and must be refused on this path.
     */
    static SupabaseVerifier make(crypto::SecureBuffer hs256_secret,
                                 std::string issuer,
                                 std::string audience,
                                 std::string provider);
    static SupabaseVerifier make_es256(std::string issuer, std::string audience,
                                      std::string provider,
                                      Es256JwksVerifier::Fetch fetch = {},
                                      Es256JwksVerifier::Clock clock = {});

    /**
     * Read issuer, audience and algorithm from the environment. Provider
     * defaults to google. ES256 needs no secret; legacy HS256 requires
     * SUPABASE_JWT_SECRET. With no explicit algorithm, an existing secret
     * selects legacy HS256; otherwise ES256 is selected.
     */
    static SupabaseVerifier from_env(std::string& out_error);

    bool valid() const { return jwks_ ? jwks_->valid() : !secret_.empty(); }

    /**
     * Verify signature + iss/aud/exp/iat/email_verified/provider.
     * Returns false on any failure with nothing written to `out`; the caller
     * cannot tell which check failed.
     */
    bool verify(const std::string& jwt, int64_t now_unix,
                SupabaseIdentity& out) const;

private:
    SupabaseVerifier(crypto::SecureBuffer s, std::string i, std::string a, std::string p)
        : secret_(std::move(s)), issuer_(std::move(i)),
          audience_(std::move(a)), provider_(std::move(p)) {}

    crypto::SecureBuffer secret_;
    std::shared_ptr<Es256JwksVerifier> jwks_;
    std::string          issuer_;
    std::string          audience_;
    std::string          provider_;
};

// ── Service layer — the same "did the user prove who they are" shape as ─────
// service.h's password functions. Handlers stay thin; the DB, verifier and
// a clock come in, a result comes out.

enum class GoogleSignInStatus {
    Ok,
    InvalidToken,       ///< signature/claims/provider/verified all failure paths
    EmailCollision,     ///< a different account already has this email
    DatabaseError,
    InternalError,      ///< username derivation could not find a free slot
};

struct GoogleSignInResult {
    GoogleSignInStatus status = GoogleSignInStatus::InvalidToken;
    int64_t     player_id = 0;
    std::string username;
    int         elo_rating = 0;
    int         token_epoch = 0;
    bool        created_new_account = false;
};

/**
 * Sign in (or first-sign-up) with a Google identity via its Supabase JWT.
 *
 * If the `sub` is known, the existing account is returned. If not:
 *   - matching verified password email → safely attach Google to that row
 *   - unverified email/different Google identity → EmailCollision
 *   - otherwise                          → create a new row
 *
 * The generated username derives from the email local-part, filtered to the
 * §7.6 whitelist. If the derived name is taken, `_N` suffixes are tried with
 * random N (5 attempts) before returning InternalError.
 */
GoogleSignInResult google_sign_in(storage::Database& db,
                                  const SupabaseVerifier& verifier,
                                  const std::string& supabase_jwt,
                                  int64_t now_unix);

enum class LinkStatus {
    Ok,
    InvalidToken,
    AlreadyLinkedElsewhere,  ///< another account owns this `sub`
    AlreadyHasGoogle,        ///< this account already has a different `sub`
    DatabaseError,
};

/**
 * Attach a Google identity to an already-authenticated account. The caller has
 * already verified the session (via the §7.7 gate); we only need to prove the
 * Google half.
 */
LinkStatus link_google(storage::Database& db,
                       const SupabaseVerifier& verifier,
                       int64_t player_id,
                       const std::string& supabase_jwt,
                       int64_t now_unix);

enum class UnlinkStatus {
    Ok,
    LastLoginMethod,     ///< would leave the account with no way to sign in
    NotLinked,           ///< no `google_sub` to remove
    DatabaseError,
};

/**
 * Detach the Google identity. Refuses when it would leave the account with
 * neither a password nor a Google link — the schema's `has_a_login_method`
 * CHECK is the database backstop, and this function is the human-friendly
 * error path so the user sees "set a password first" rather than a raw 23514.
 */
UnlinkStatus unlink_google(storage::Database& db, int64_t player_id);

} // namespace auth
} // namespace chess
