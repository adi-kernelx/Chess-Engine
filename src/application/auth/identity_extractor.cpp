/**
 * application/auth/identity_extractor.cpp — see header for design.
 *
 * The four failure branches (no auth wired, no token field, bad token,
 * unknown player) mirror `GameHandler::extract_identity` bit-for-bit,
 * including the exact human-readable messages the client sees. That is
 * on purpose: LLD-2 promises to preserve wire behaviour.
 */

#include "application/auth/identity_extractor.h"

#include <ctime>

#include "auth/session.h"
#include "storage/player_repo.h"

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
                                     int64_t (*now_unix)())
    : db_(db), signer_(signer),
      now_unix_(now_unix ? now_unix : &default_now_unix) {}

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

    // Full gate: signature, exp, epoch, player-still-exists.
    // authorize_access_token distinguishes RejectedRevoked /
    // RejectedUnknownUser internally; from the client's perspective
    // all failures look the same "your session is no longer valid,
    // sign in again" — we do not leak which branch tripped, matching
    // the timing-uniform discipline from §7.6.
    chess::auth::AccessClaims claims;
    const int64_t now = now_unix_();
    const auto outcome = chess::auth::authorize_access_token(
        *db_, *signer_, msg["access_token"].get<std::string>(), now, claims);

    if (outcome != chess::auth::GateOutcome::Ok) {
        return fail("Session invalid or expired — please sign in again");
    }

    // Snapshot the authenticated identity from the players row. If the
    // row disappeared between the epoch check and this read (an admin
    // `DELETE FROM players` mid-request), treat it as auth failure.
    auto profile = chess::storage::find_player_by_id(*db_, claims.player_id);
    if (!profile) {
        return fail("Account no longer exists — please sign in again");
    }

    AuthenticatedIdentity id;
    id.player_id  = profile->player_id;
    id.username   = profile->username;
    id.elo_rating = profile->elo_rating;
    return Result<AuthenticatedIdentity>::ok(id);
}

} // namespace chess::application::auth
