/**
 * application/auth/identity_extractor.cpp — see header for design.
 *
 * Signature/expiry and a fresh one-statement profile/epoch read authenticate
 * every typed command. Storage failure is retryable, never session revocation.
 */

#include "application/auth/identity_extractor.h"

#include <ctime>

#include "auth/session.h"

namespace chess::application::auth {

namespace {

int64_t default_now_unix() {
    return static_cast<int64_t>(std::time(nullptr));
}

Result<AuthenticatedIdentity>
fail(std::string message) {
    return Result<AuthenticatedIdentity>::err(
        ResultCode::Unauthorized, std::move(message));
}

} // namespace

IdentityExtractor::IdentityExtractor(chess::storage::Database* db,
                                     chess::auth::TokenSigner* signer,
                                     int64_t (*now_unix)(),
                                     chess::storage::DatabasePool* read_pool)
    : db_(db), signer_(signer),
      now_unix_(now_unix ? now_unix : &default_now_unix), read_pool_(read_pool) {}

Result<AuthenticatedIdentity>
IdentityExtractor::extract(const nlohmann::json& msg) const {
    // If the server was launched without a DB or TokenSigner, no game
    // command can be authenticated. Fail closed rather than silently
    // downgrade — the wire contract is "authenticated or nothing" for
    // routes that call the extractor.
    if (!db_ || !signer_) {
        return fail("Authentication is not configured on this server");
    }

    if (!msg.contains("access_token") || !msg["access_token"].is_string()) {
        return fail("Missing 'access_token' — please sign in");
    }

    // Full gate: signature, expiry, epoch, player-still-exists.
    chess::auth::AccessClaims claims;
    const int64_t now = now_unix_();
    if (!signer_->verify_access(msg["access_token"].get<std::string>(), now, claims)) {
        return fail("Session invalid or expired — please sign in again");
    }

    // Validate revocation and read the identity atomically in ONE round trip.
    // Keep the epoch check per request: no stale cache after logout-all.
    std::optional<chess::storage::DatabasePool::Lease> lease;
    if (read_pool_) {
        lease = read_pool_->acquire();
        if (!lease) return Result<AuthenticatedIdentity>::err(ResultCode::Unavailable,
            "Authentication is temporarily unavailable. Please retry; you are not signed out.");
    }
    auto& database = lease ? lease->database() : *db_;
    auto profile = database.exec("SELECT username,elo_rating,token_epoch FROM players WHERE id=$1",
        {chess::storage::Param::int64(claims.player_id)});
    if (!profile.ok) {
        return Result<AuthenticatedIdentity>::err(ResultCode::Unavailable,
            "Authentication is temporarily unavailable. Please retry; you are not signed out.");
    }
    if (profile.empty()) {
        return fail("Account no longer exists — please sign in again");
    }
    if (std::stoi(profile.first().at(2)) != claims.token_epoch) {
        return fail("Session invalid or expired — please sign in again");
    }

    AuthenticatedIdentity id;
    id.player_id  = claims.player_id;
    id.username   = profile.first().at(0);
    id.elo_rating = std::stoi(profile.first().at(1));
    return Result<AuthenticatedIdentity>::ok(id);
}

} // namespace chess::application::auth
