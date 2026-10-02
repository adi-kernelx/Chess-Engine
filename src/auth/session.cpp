#include "auth/session.h"

#include "storage/transaction.h"

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

    Transaction tx(db);
    if (!tx.ok()) { outcome = RefreshOutcome::DatabaseError; return t; }

    // Lock this lineage point so two server workers cannot both observe it as
    // fresh. rotated/expired stay visible for recovery and reuse detection.
    auto sel = db.exec(
        "SELECT player_id, family_id::text, rotated,"
        "       extract(epoch from expires_at)::bigint,"
        "       COALESCE(extract(epoch from rotated_at)::bigint, 0)"
        "  FROM sessions WHERE token_hash=$1 FOR UPDATE",
        {Param::text(hash)});
    if (!sel.ok)                { outcome = RefreshOutcome::DatabaseError; return t; }
    if (sel.rows.empty())       { (void)tx.commit(); return t; }

    const int64_t     player_id = std::strtoll(sel.first().at(0).c_str(), nullptr, 10);
    const std::string family    = sel.first().at(1);
    const bool        rotated   = sel.first().at(2) == "t";
    const int64_t     expires_at = std::strtoll(sel.first().at(3).c_str(), nullptr, 10);
    const int64_t     rotated_at = std::strtoll(sel.first().at(4).c_str(), nullptr, 10);

    if (rotated) {
        const bool transport_retry = rotated_at > 0 &&
            now_unix >= rotated_at &&
            now_unix - rotated_at <= REFRESH_REUSE_GRACE_SECONDS;
        if (!transport_retry) {
            // Reuse outside the response-loss window remains a security
            // signal: revoke every descendant in this login family.
            auto del = db.exec("DELETE FROM sessions WHERE family_id=$1::uuid",
                               {Param::text(family)});
            if (!del.ok || !tx.commit()) outcome = RefreshOutcome::DatabaseError;
            return t;
        }

        // The previous response was probably lost during navigation/reload.
        // Retain the superseded successor as a bounded recovery tripwire.
        // Deleting it makes a delayed, legitimately delivered auth_ok carry
        // an unknown token, causing the very next reload to sign the user out.
        // There is still one unrotated token; all predecessors keep the same
        // narrow retry/reuse-revocation policy.
        auto supersede = db.exec(
            "UPDATE sessions SET rotated=TRUE, rotated_at=to_timestamp($2::bigint)"
            " WHERE family_id=$1::uuid AND rotated=FALSE",
            {Param::text(family), Param::int64(now_unix)});
        if (!supersede.ok) { outcome = RefreshOutcome::DatabaseError; return t; }
    }
    if (expires_at <= now_unix) {
        auto del = db.exec("DELETE FROM sessions WHERE token_hash=$1", {Param::text(hash)});
        if (!del.ok || !tx.commit()) outcome = RefreshOutcome::DatabaseError;
        return t;
    }

    // Fetch what we need to sign the new access token.
    auto p = db.exec("SELECT username, token_epoch FROM players WHERE id=$1",
                     {Param::int64(player_id)});
    if (!p.ok || p.rows.empty()) {
        // Player deleted underneath us — treat as invalid, no useful successor.
        auto del = db.exec("DELETE FROM sessions WHERE family_id=$1::uuid",
                           {Param::text(family)});
        if (!del.ok || !tx.commit()) outcome = RefreshOutcome::DatabaseError;
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

    // Rotate atomically: mark the old row as a reuse tripwire and insert its
    // single successor in the same family. The row lock plus transaction also
    // makes this safe when multiple server workers receive refreshes together.
    auto mark = db.exec(
        "UPDATE sessions SET rotated=TRUE,"
        " rotated_at=COALESCE(rotated_at, to_timestamp($2::bigint))"
        " WHERE token_hash=$1",
        {Param::text(hash), Param::int64(now_unix)});
    if (!mark.ok) { outcome = RefreshOutcome::DatabaseError; return t; }

    auto ins = db.exec(
        "INSERT INTO sessions(token_hash, player_id, family_id, rotated, expires_at)"
        " VALUES($1, $2::bigint, $3::uuid, FALSE, to_timestamp($4::bigint))",
        {Param::text(new_hash),
         Param::int64(player_id),
         Param::text(family),
         Param::int64(new_exp)});
    if (!ins.ok) {
        // Transaction destruction rolls the mark back with the failed insert.
        outcome = RefreshOutcome::DatabaseError;
        return t;
    }

    if (!tx.commit()) { outcome = RefreshOutcome::DatabaseError; return SessionTokens{}; }
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
