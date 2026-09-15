/**
 * auth_handler.cpp — see header for the wire contract.
 *
 * Every reply is built with nlohmann::json so string values are escaped, then
 * manually re-serialised with "type" as the first key. That is the same
 * trick §7.9 used in game/match_notify.cpp — websocket.cpp's router finds
 * `type` by string-scan not by JSON parse, so the FIRST key on the wire
 * must be "type".
 */

#include "auth/auth_handler.h"

#include "auth/password.h"
#include "auth/service.h"
#include "auth/session.h"
#include "auth/username.h"
#include "core/logger.h"
#include "net/connection.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <utility>

using json = nlohmann::json;

namespace chess {
namespace auth {

namespace {

// ── JSON helpers ────────────────────────────────────────────────────────────
//
// Build a reply object with `type` and any additional fields, then serialise
// so `"type"` is the first key on the wire. websocket.cpp finds `type` via
// std::string::find rather than JSON parsing, and takes the first match.

std::string serialize_type_first(const std::string& type, const json& extra) {
    std::string out;
    out.reserve(64 + extra.dump().size());
    out.push_back('{');
    out.append("\"type\":");
    out.append(json(type).dump());
    if (extra.is_object()) {
        for (auto it = extra.begin(); it != extra.end(); ++it) {
            out.push_back(',');
            out.append(json(it.key()).dump());
            out.push_back(':');
            out.append(it.value().dump());
        }
    }
    out.push_back('}');
    return out;
}

void send_json(net::Connection& conn, const std::string& body) {
    net::WebSocket::write_frame(conn, net::WsOpcode::TEXT, body);
}

void send_auth_error(net::Connection& conn, const char* code) {
    send_json(conn, serialize_type_first("auth_error", { {"code", code} }));
}

// Try to parse the frame; on failure send a generic auth_error and return
// false. We never distinguish "malformed JSON" from "wrong shape" on the
// wire, so an attacker cannot probe with malformed frames.
bool parse_or_fail(net::Connection& conn, const std::string& message, json& out) {
    out = json::parse(message, nullptr, /*allow_exceptions=*/false);
    if (out.is_discarded() || !out.is_object()) {
        send_auth_error(conn, "invalid_request");
        return false;
    }
    return true;
}

// Extract a required string field; on failure send auth_error and return
// false. Values must be strings — a numeric password would silently coerce
// without this check.
bool require_string(net::Connection& conn, const json& j, const char* key,
                    std::string& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        send_auth_error(conn, "invalid_request");
        return false;
    }
    out = it->get<std::string>();
    return true;
}

int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Build the shared success payload for register / login / refresh / google.
std::string build_auth_ok(const std::string& username, int elo,
                          const SessionTokens& t) {
    return serialize_type_first("auth_ok", {
        {"username",           username},
        {"elo",                elo},
        {"access_token",       t.access_token},
        {"refresh_token",      t.refresh_token},
        {"access_expires_in",  t.access_expires_in},
    });
}

} // namespace

// ── Ctor + registration ─────────────────────────────────────────────────────

AuthHandler::AuthHandler(storage::Database& db,
                         TokenSigner& signer,
                         SupabaseVerifier* google,
                         crypto::SealedRegistry* sealed_reg)
    : db_(db),
      signer_(signer),
      google_(google),
      sealed_reg_(sealed_reg),
      rl_login_ip_     (rate_presets::login_per_ip()),
      rl_login_account_(rate_presets::login_per_account()),
      rl_register_ip_  (rate_presets::register_per_ip()),
      rl_google_ip_    (rate_presets::google_auth_per_ip()),
      rl_seal_ip_      (rate_presets::seal_request_per_ip())
{}

void AuthHandler::register_handlers(net::MessageRouter& router) {
    if (sealed_reg_) {
        router.register_handler("seal_request",
            [this](net::Connection& c, const std::string& m) { handle_seal_request(c, m); });
    }
    router.register_handler("register",
        [this](net::Connection& c, const std::string& m) { handle_register(c, m); });
    router.register_handler("login",
        [this](net::Connection& c, const std::string& m) { handle_login(c, m); });
    router.register_handler("refresh",
        [this](net::Connection& c, const std::string& m) { handle_refresh(c, m); });
    router.register_handler("logout",
        [this](net::Connection& c, const std::string& m) { handle_logout(c, m); });
    router.register_handler("logout_all",
        [this](net::Connection& c, const std::string& m) { handle_logout_all(c, m); });
    if (google_) {
        router.register_handler("google_auth",
            [this](net::Connection& c, const std::string& m) { handle_google_auth(c, m); });
        router.register_handler("link_google",
            [this](net::Connection& c, const std::string& m) { handle_link_google(c, m); });
        router.register_handler("unlink_google",
            [this](net::Connection& c, const std::string& m) { handle_unlink_google(c, m); });
    }
}

// ── Handlers ────────────────────────────────────────────────────────────────

void AuthHandler::handle_seal_request(net::Connection& conn, const std::string&) {
    // Rate-limit BEFORE any key generation — key material is expensive.
    if (!rl_seal_ip_.try_take(conn.get_ip())) {
        send_auth_error(conn, "rate_limited");
        return;
    }
    std::string reply = sealed_reg_->handle_seal_request(conn.get_ip());
    if (reply.empty()) {
        // Client is over the store's per-IP cap — deliberately vague.
        send_auth_error(conn, "rate_limited");
        return;
    }
    // sealed_registry writes the JSON directly, `"type":"seal_key"` first.
    send_json(conn, reply);
}

void AuthHandler::handle_register(net::Connection& conn, const std::string& message) {
    if (!rl_register_ip_.try_take(conn.get_ip())) {
        send_auth_error(conn, "rate_limited");
        return;
    }
    json j;
    if (!parse_or_fail(conn, message, j)) return;

    std::string username, password;
    if (!require_string(conn, j, "username", username)) return;
    if (!require_string(conn, j, "password", password)) return;

    auto r = register_password_user(db_, username, password);
    switch (r.status) {
        case RegisterResult::Status::InvalidUsername:
            send_auth_error(conn, "invalid_username"); return;
        case RegisterResult::Status::WeakPassword:
            send_auth_error(conn, "weak_password"); return;
        case RegisterResult::Status::UsernameTaken:
            send_auth_error(conn, "username_taken"); return;
        case RegisterResult::Status::DatabaseError:
        case RegisterResult::Status::InternalError:
            send_auth_error(conn, "internal"); return;
        case RegisterResult::Status::Ok:
            break;
    }
    // Fresh account: token_epoch starts at 0.
    auto tokens = issue_session(db_, signer_, r.player_id, r.username, 0, now_seconds());
    if (!tokens.ok) { send_auth_error(conn, "internal"); return; }
    send_json(conn, build_auth_ok(r.username, r.elo_rating, tokens));
}

void AuthHandler::handle_login(net::Connection& conn, const std::string& message) {
    if (!rl_login_ip_.try_take(conn.get_ip())) {
        send_auth_error(conn, "rate_limited");
        return;
    }
    json j;
    if (!parse_or_fail(conn, message, j)) return;

    std::string username, password;
    if (!require_string(conn, j, "username", username)) return;
    if (!require_string(conn, j, "password", password)) return;

    // Per-account limiter, only meaningful after username is known. This is a
    // "known-username brute force" guard — the per-IP limit above catches the
    // broad enumeration case first.
    if (!rl_login_account_.try_take(to_lower_ascii(username))) {
        // Deliberately the same "invalid_credentials" answer — a distinct
        // "rate_limited" here would confirm the username exists.
        send_auth_error(conn, "invalid_credentials");
        return;
    }
    auto r = authenticate_password(db_, username, password);
    if (r.status != LoginResult::Status::Ok) {
        send_auth_error(conn, "invalid_credentials");
        return;
    }
    auto tokens = issue_session(db_, signer_, r.player_id, r.username,
                                r.token_epoch, now_seconds());
    if (!tokens.ok) { send_auth_error(conn, "internal"); return; }
    send_json(conn, build_auth_ok(r.username, r.elo_rating, tokens));
}

void AuthHandler::handle_refresh(net::Connection& conn, const std::string& message) {
    // No per-IP limit on refresh — a legitimate active client refreshes
    // roughly every 15 minutes, and rate-limiting refresh has been an
    // observed cause of "silent logout at scale" bugs. The rotate-or-die
    // reuse-detection in session.cpp is the real defense.
    json j;
    if (!parse_or_fail(conn, message, j)) return;

    std::string refresh_token;
    if (!require_string(conn, j, "refresh_token", refresh_token)) return;

    RefreshOutcome outcome;
    auto tokens = refresh_session(db_, signer_, refresh_token, now_seconds(), outcome);
    if (outcome != RefreshOutcome::Ok || !tokens.ok) {
        send_auth_error(conn, "invalid_refresh");
        return;
    }
    // A refreshed token is bound to the SAME player — reconstruct the reply
    // from the JWT so we don't need a second DB round-trip for the username.
    AccessClaims claims;
    if (!signer_.verify_access(tokens.access_token, now_seconds(), claims)) {
        send_auth_error(conn, "internal");
        return;
    }
    // elo is not carried in claims; fetch from DB. If the row is gone
    // (deleted between rotate and this read) fall back to 0 rather than
    // fail — the tokens are still valid until logout_all.
    auto qr = db_.exec("SELECT elo_rating FROM players WHERE id = $1",
                       { storage::Param::int64(claims.player_id) });
    int elo = (qr.ok && !qr.rows.empty()) ? std::stoi(qr.rows[0].at(0)) : 0;
    send_json(conn, build_auth_ok(claims.username, elo, tokens));
}

void AuthHandler::handle_logout(net::Connection& conn, const std::string& message) {
    json j;
    if (!parse_or_fail(conn, message, j)) return;
    std::string refresh_token;
    if (!require_string(conn, j, "refresh_token", refresh_token)) return;
    logout(db_, refresh_token);
    send_json(conn, serialize_type_first("logout_ok", json::object()));
}

void AuthHandler::handle_logout_all(net::Connection& conn, const std::string& message) {
    json j;
    if (!parse_or_fail(conn, message, j)) return;
    std::string access;
    if (!require_string(conn, j, "access_token", access)) return;

    AccessClaims claims;
    auto gate = authorize_access_token(db_, signer_, access, now_seconds(), claims);
    if (gate != GateOutcome::Ok) { send_auth_error(conn, "unauthorized"); return; }
    if (!logout_all(db_, claims.player_id)) {
        send_auth_error(conn, "internal");
        return;
    }
    send_json(conn, serialize_type_first("logout_ok", json::object()));
}

void AuthHandler::handle_google_auth(net::Connection& conn, const std::string& message) {
    if (!rl_google_ip_.try_take(conn.get_ip())) {
        send_auth_error(conn, "rate_limited");
        return;
    }
    json j;
    if (!parse_or_fail(conn, message, j)) return;
    std::string supabase_jwt;
    if (!require_string(conn, j, "supabase_jwt", supabase_jwt)) return;

    auto r = google_sign_in(db_, *google_, supabase_jwt, now_seconds());
    switch (r.status) {
        case GoogleSignInStatus::InvalidToken:
            send_auth_error(conn, "invalid_google"); return;
        case GoogleSignInStatus::EmailCollision:
            send_auth_error(conn, "email_collision"); return;
        case GoogleSignInStatus::DatabaseError:
        case GoogleSignInStatus::InternalError:
            send_auth_error(conn, "internal"); return;
        case GoogleSignInStatus::Ok:
            break;
    }
    auto tokens = issue_session(db_, signer_, r.player_id, r.username,
                                r.token_epoch, now_seconds());
    if (!tokens.ok) { send_auth_error(conn, "internal"); return; }
    send_json(conn, build_auth_ok(r.username, r.elo_rating, tokens));
}

void AuthHandler::handle_link_google(net::Connection& conn, const std::string& message) {
    json j;
    if (!parse_or_fail(conn, message, j)) return;
    std::string access, supabase_jwt;
    if (!require_string(conn, j, "access_token", access)) return;
    if (!require_string(conn, j, "supabase_jwt", supabase_jwt)) return;

    AccessClaims claims;
    if (authorize_access_token(db_, signer_, access, now_seconds(), claims) != GateOutcome::Ok) {
        send_auth_error(conn, "unauthorized");
        return;
    }
    auto ls = link_google(db_, *google_, claims.player_id, supabase_jwt, now_seconds());
    switch (ls) {
        case LinkStatus::InvalidToken:           send_auth_error(conn, "invalid_google");         return;
        case LinkStatus::AlreadyLinkedElsewhere: send_auth_error(conn, "already_linked_elsewhere"); return;
        case LinkStatus::AlreadyHasGoogle:       send_auth_error(conn, "already_has_google");     return;
        case LinkStatus::DatabaseError:          send_auth_error(conn, "internal");               return;
        case LinkStatus::Ok:                     break;
    }
    send_json(conn, serialize_type_first("link_ok", json::object()));
}

void AuthHandler::handle_unlink_google(net::Connection& conn, const std::string& message) {
    json j;
    if (!parse_or_fail(conn, message, j)) return;
    std::string access;
    if (!require_string(conn, j, "access_token", access)) return;

    AccessClaims claims;
    if (authorize_access_token(db_, signer_, access, now_seconds(), claims) != GateOutcome::Ok) {
        send_auth_error(conn, "unauthorized");
        return;
    }
    auto us = unlink_google(db_, claims.player_id);
    switch (us) {
        case UnlinkStatus::LastLoginMethod: send_auth_error(conn, "last_login_method"); return;
        case UnlinkStatus::NotLinked:       send_auth_error(conn, "not_linked");        return;
        case UnlinkStatus::DatabaseError:   send_auth_error(conn, "internal");          return;
        case UnlinkStatus::Ok:              break;
    }
    send_json(conn, serialize_type_first("link_ok", json::object()));
}

} // namespace auth
} // namespace chess
