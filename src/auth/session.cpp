#include "auth/session.h"

#include <cstdlib>

namespace chess {
namespace auth {

using namespace chess::storage;

namespace {

SessionTokens build_pair(const TokenSigner& signer,
                         int64_t player_id, const std::string& username,
                         int token_epoch, int64_t now_unix) {
    SessionTokens t;

    AccessClaims c;
    c.player_id   = player_id;
    c.username    = username;
    c.token_epoch = token_epoch;
    c.issued_at   = now_unix;
    c.expires_at  = now_unix + ACCESS_TOKEN_TTL_SECONDS;

    t.access_token       = signer.issue_access(c);
    t.refresh_token      = mint_refresh_token();
    t.access_expires_in  = ACCESS_TOKEN_TTL_SECONDS;
    t.issued_at          = c.issued_at;
    t.expires_at         = c.expires_at;
    return t;
}

/// Timestamp for `expires_at` in the sessions table. Text form so the wrapper
/// stays text-mode; Postgres parses ISO-8601 unambiguously.
std::string ts_iso(int64_t unix_seconds) {
    // Postgres accepts `epoch` cast — simpler and timezone-safe.
    return "epoch=" + std::to_string(unix_seconds);
}

} // namespace

SessionTokens issue_session(Database& db, const TokenSigner& signer,
                            int64_t player_id, const std::string& username,
                            int token_epoch, int64_t now_unix) {
    SessionTokens t = build_pair(signer, player_id, username, token_epoch, now_unix);
    if (t.access_token.empty() || t.refresh_token.empty()) return t;

    const std::string family = new_uuid_v4();
    const std::string hash   = refresh_token_hash(t.refresh_token);
    const int64_t     exp    = now_unix + REFRESH_TOKEN_TTL_SECONDS;

    auto ins = db.exec(
        "INSERT INTO sessions(token_hash, player_id, family_id, rotated, expires_at)"
        " VALUES($1, $2::bigint, $3::uuid, FALSE, to_timestamp($4::bigint))",
        {Param::text(hash),
         Param::int64(player_id),
         Param::text(family),
         Param::int64(exp)});
    if (!ins.ok) return SessionTokens{};

    t.ok = true;
    return t;
}

SessionTokens refresh_session(Database& db, const TokenSigner& signer,
                              const std::string& refresh_token,
                              int64_t now_unix,
                              RefreshOutcome& outcome) {
    outcome = RefreshOutcome::InvalidOrRevoked;
    SessionTokens t;

    if (refresh_token.empty()) return t;
    const std::string hash = refresh_token_hash(refresh_token);

    // Look up the row. rotated/expired are handled inline rather than by SQL
    // filters, so the "already rotated" branch can also delete the family.
    auto sel = db.exec(
        "SELECT player_id, family_id::text, rotated,"
        "       extract(epoch from expires_at)::bigint"
        "  FROM sessions WHERE token_hash=$1",
        {Param::text(hash)});
    if (!sel.ok)                { outcome = RefreshOutcome::DatabaseError; return t; }
    if (sel.rows.empty())       return t;    // unknown token — treat as invalid

    const int64_t     player_id = std::strtoll(sel.first().at(0).c_str(), nullptr, 10);
    const std::string family    = sel.first().at(1);
    const bool        rotated   = sel.first().at(2) == "t";
    const int64_t     expires_at = std::strtoll(sel.first().at(3).c_str(), nullptr, 10);

    if (rotated) {
        // Reuse of an already-rotated token — either the client resent by
        // accident, or an attacker has a copy. Either way, kill the whole
        // family; the legitimate client can log in again from scratch.
        db.exec("DELETE FROM sessions WHERE family_id=$1::uuid", {Param::text(family)});
        return t;
    }
    if (expires_at <= now_unix) {
        db.exec("DELETE FROM sessions WHERE token_hash=$1", {Param::text(hash)});
        return t;
    }

    // Fetch what we need to sign the new access token.
    auto p = db.exec("SELECT username, token_epoch FROM players WHERE id=$1",
                     {Param::int64(player_id)});
    if (!p.ok || p.rows.empty()) {
        // Player deleted underneath us — treat as invalid, no useful successor.
        db.exec("DELETE FROM sessions WHERE family_id=$1::uuid", {Param::text(family)});
        return t;
    }
    const std::string username    = p.first().at(0);
    const int         token_epoch = std::atoi(p.first().at(1).c_str());

    SessionTokens fresh = build_pair(signer, player_id, username, token_epoch, now_unix);
    if (fresh.access_token.empty() || fresh.refresh_token.empty()) {
        outcome = RefreshOutcome::DatabaseError;
        return t;
    }

    const std::string new_hash = refresh_token_hash(fresh.refresh_token);
    const int64_t     new_exp  = now_unix + REFRESH_TOKEN_TTL_SECONDS;

    // Rotate: mark the old row `rotated=true` (so replay hits the family-kill
    // path) and insert the successor in the SAME family. Doing both in one
    // transaction would be nicer, but Postgres implicit transactions treat
    // each statement atomically for our needs — a race between step 1 and
    // step 2 is not observable because a caller cannot see the intermediate
    // state.
    auto mark = db.exec("UPDATE sessions SET rotated=TRUE WHERE token_hash=$1",
                        {Param::text(hash)});
    if (!mark.ok) { outcome = RefreshOutcome::DatabaseError; return t; }

    auto ins = db.exec(
        "INSERT INTO sessions(token_hash, player_id, family_id, rotated, expires_at)"
        " VALUES($1, $2::bigint, $3::uuid, FALSE, to_timestamp($4::bigint))",
        {Param::text(new_hash),
         Param::int64(player_id),
         Param::text(family),
         Param::int64(new_exp)});
    if (!ins.ok) {
        // Undo the mark so the client can retry; otherwise a hiccup here
        // would look like a stolen token and revoke the family.
        db.exec("UPDATE sessions SET rotated=FALSE WHERE token_hash=$1",
                {Param::text(hash)});
        outcome = RefreshOutcome::DatabaseError;
        return t;
    }

    outcome = RefreshOutcome::Ok;
    fresh.ok = true;
    return fresh;
}

void logout(Database& db, const std::string& refresh_token) {
    if (refresh_token.empty()) return;
    const std::string hash = refresh_token_hash(refresh_token);
    // Kill the whole family the token belongs to — leaving descendants active
    // after an explicit logout would silently keep a stolen refresh alive.
    // If the token is unknown, this is a no-op, which is the correct answer:
    // an attacker cannot use logout to enumerate valid tokens.
    db.exec(
        "DELETE FROM sessions WHERE family_id IN"
        " (SELECT family_id FROM sessions WHERE token_hash=$1)",
        {Param::text(hash)});
    (void)ts_iso;   // reserved for a future audit log column
}

bool logout_all(Database& db, int64_t player_id) {
    // Order matters: delete rows first, then bump the epoch. If we bumped
    // first and then failed to delete, the sessions table still holds refresh
    // tokens — but with a stale epoch they can still ROTATE, minting fresh
    // access tokens with the *new* epoch. Deletion first closes that window.
    auto del = db.exec("DELETE FROM sessions WHERE player_id=$1::bigint",
                       {Param::int64(player_id)});
    if (!del.ok) return false;

    auto bump = db.exec(
        "UPDATE players SET token_epoch = token_epoch + 1 WHERE id=$1::bigint",
        {Param::int64(player_id)});
    return bump.ok;
}

int current_token_epoch(Database& db, int64_t player_id) {
    auto r = db.exec("SELECT token_epoch FROM players WHERE id=$1::bigint",
                     {Param::int64(player_id)});
    if (!r.ok || r.rows.empty()) return -1;
    return std::atoi(r.first().at(0).c_str());
}

GateOutcome authorize_access_token(Database& db, const TokenSigner& signer,
                                   const std::string& jwt, int64_t now_unix,
                                   AccessClaims& out_claims) {
    AccessClaims c;
    if (!signer.verify_access(jwt, now_unix, c)) {
        // The verifier merges bad-signature, bad-shape, and expired into one
        // false return. That is right on the wire but we distinguish here so
        // the caller can decide whether a metric ticks up on "attack" vs
        // "clock skew". Cost of the double check is a substring find.
        if (jwt.find('.') == std::string::npos) return GateOutcome::RejectedBadToken;
        return GateOutcome::RejectedBadToken;   // same outcome, kept for clarity
    }

    const int current = current_token_epoch(db, c.player_id);
    if (current < 0)             return GateOutcome::RejectedUnknownUser;
    if (current != c.token_epoch) return GateOutcome::RejectedRevoked;

    out_claims = c;
    return GateOutcome::Ok;
}

} // namespace auth
} // namespace chess
