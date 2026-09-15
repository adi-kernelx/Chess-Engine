/**
 * session.h — refresh-token rotation, family revocation, logout, logout_all.
 *
 * The access token is a JWT and self-verifies; nothing here touches it. The
 * refresh token is the state-bearing half: rotated on every use, tracked as
 * a "family" (rotation lineage), and revocable in three ways:
 *
 *   1. Voluntary sign-out on this device — `logout()` deletes just this
 *      token's family.
 *
 *   2. Global sign-out — `logout_all()` deletes every session for the
 *      player AND bumps `players.token_epoch`. The epoch bump is what makes
 *      outstanding access tokens die instantly; without it a stolen access
 *      token would remain valid for up to 15 minutes after "sign out
 *      everywhere".
 *
 *   3. Reuse detection — if a refresh token that has already been rotated
 *      is presented, we delete the WHOLE family it came from and return an
 *      error. This is OAuth 2.1 §4.14.2's "descendant revocation": either
 *      the legitimate client is confused, or an attacker cloned the token,
 *      and either way the safest response is to kill the family and force
 *      re-authentication. Failing OPEN here would be the whole point of
 *      rotation defeated.
 *
 * WHY THE `sessions` TABLE STORES A HASH, NEVER THE TOKEN
 *
 * A DB read compromise (SQL injection, leaked backup, disgruntled DBA) must
 * not hand the attacker every user's live session. The token is SHA-384'd
 * before insert, and every lookup hashes the presented token first. That
 * means:
 *
 *   - The attacker who reads the whole table cannot log in as anyone.
 *   - We cannot email a user their forgotten refresh token, but we never
 *     want to do that anyway.
 *   - Hash collisions here would need to be preimage collisions in SHA-384.
 */

#pragma once

#include "auth/token.h"
#include "storage/database.h"

#include <cstdint>
#include <string>

namespace chess {
namespace auth {

/// The pair a caller returns to the client on register / login / refresh.
struct SessionTokens {
    std::string access_token;         ///< the JWT
    std::string refresh_token;        ///< opaque; the raw value the client keeps
    int64_t     access_expires_in = 0;///< seconds — clients read this to set a
                                      ///< refresh timer without parsing the JWT
    int64_t     issued_at = 0;        ///< the `iat` written into the JWT
    int64_t     expires_at = 0;       ///< the `exp` written into the JWT
    bool        ok = false;
};

/**
 * Issue a fresh (access, refresh) pair for a freshly authenticated player.
 *
 * @param now_unix  clock, injected so tests can be deterministic
 *
 * Creates a NEW family — this is always the correct choice for a login and
 * for a registration. Rotation reuses the family; only this call starts one.
 */
SessionTokens issue_session(storage::Database& db,
                            const TokenSigner& signer,
                            int64_t player_id,
                            const std::string& username,
                            int token_epoch,
                            int64_t now_unix);

enum class RefreshOutcome {
    Ok,
    InvalidOrRevoked,     ///< unknown, expired, or reuse → family destroyed
    DatabaseError,
};

/**
 * Exchange `refresh_token` for a new pair. Rotates: the old token is marked
 * `rotated=true`, a fresh one is issued into the same family.
 *
 * If the presented token is ALREADY rotated, the family is deleted and
 * InvalidOrRevoked is returned. That branch is the reuse-detection path.
 */
SessionTokens refresh_session(storage::Database& db,
                              const TokenSigner& signer,
                              const std::string& refresh_token,
                              int64_t now_unix,
                              RefreshOutcome& out);

/**
 * Delete the family the refresh token belongs to. Unknown token = no-op —
 * we do not want to distinguish "wrong token" from "already logged out" on
 * the wire.
 */
void logout(storage::Database& db, const std::string& refresh_token);

/**
 * Delete every session for the player AND increment their `token_epoch`.
 * The epoch bump is what makes still-valid access tokens die at once.
 */
bool logout_all(storage::Database& db, int64_t player_id);

/**
 * Read the player's current `token_epoch`. The auth gate calls this once per
 * request to reject access tokens whose claim is stale — after a logout_all,
 * every token with the pre-bump epoch is instantly invalid.
 *
 * Returns -1 if the player is not found (deleted account, wrong id).
 */
int current_token_epoch(storage::Database& db, int64_t player_id);

/**
 * The whole gate: parse the access token, verify the signature and exp, and
 * check the epoch against the database. Fills `out_claims` only on Ok.
 *
 * This is what a handler calls to answer "who is this connection". Nothing
 * downstream should ever take a `username` from a client-supplied field
 * again (spec migration note under §7.7).
 */
enum class GateOutcome {
    Ok,
    RejectedBadToken,     ///< signature or shape fails
    RejectedExpired,
    RejectedRevoked,      ///< epoch mismatch after logout_all
    RejectedUnknownUser,
};

GateOutcome authorize_access_token(storage::Database& db,
                                   const TokenSigner& signer,
                                   const std::string& jwt,
                                   int64_t now_unix,
                                   AccessClaims& out_claims);

} // namespace auth
} // namespace chess
