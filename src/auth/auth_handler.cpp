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
                         crypto::SealedRegistry* sealed_reg, EmailRecovery* email)
    : db_(db),
      signer_(signer),
      google_(google),
      sealed_reg_(sealed_reg),
      email_(email),
      rl_login_ip_     (rate_presets::login_per_ip()),
      rl_login_account_(rate_presets::login_per_account()),
      rl_register_ip_  (rate_presets::register_per_ip()),
      rl_google_ip_    (rate_presets::google_auth_per_ip()),
      rl_seal_ip_      (rate_presets::seal_request_per_ip())
{}

void AuthHandler::register_handlers(protocol::RequestPipeline& pipeline) {
    using protocol::RoutePolicy;
    // Every auth route registers as a "raw" route on the pipeline —
    // SealOpen still runs, but the handler owns its own JSON parse,
    // rate limits, and `auth_error{code:...}` frames.
    auto raw = [](auto method) {
        return [method](net::Connection& c,
                        const std::string& m,
                        chess::application::MessageSink&) {
            method(c, m);
        };
    };
    if (sealed_reg_) {
        pipeline.register_raw_route(RoutePolicy{"seal_request"},
            [this](net::Connection& c, const std::string& m,
                   chess::application::MessageSink&) { handle_seal_request(c, m); });
    }
    pipeline.register_raw_route(RoutePolicy{"register"},
        [this](net::Connection& c, const std::string& m,
               chess::application::MessageSink&) { handle_register(c, m); });
    pipeline.register_raw_route(RoutePolicy{"login"},
        [this](net::Connection& c, const std::string& m,
               chess::application::MessageSink&) { handle_login(c, m); });
    pipeline.register_raw_route(RoutePolicy{"refresh"},
        [this](net::Connection& c, const std::string& m,
               chess::application::MessageSink&) { handle_refresh(c, m); });
    pipeline.register_raw_route(RoutePolicy{"logout"},
        [this](net::Connection& c, const std::string& m,
               chess::application::MessageSink&) { handle_logout(c, m); });
    pipeline.register_raw_route(RoutePolicy{"logout_all"},
        [this](net::Connection& c, const std::string& m,
               chess::application::MessageSink&) { handle_logout_all(c, m); });
    if (google_) {
        pipeline.register_raw_route(RoutePolicy{"google_auth"},
            [this](net::Connection& c, const std::string& m,
                   chess::application::MessageSink&) { handle_google_auth(c, m); });
        pipeline.register_raw_route(RoutePolicy{"link_google"},
            [this](net::Connection& c, const std::string& m,
                   chess::application::MessageSink&) { handle_link_google(c, m); });
        pipeline.register_raw_route(RoutePolicy{"unlink_google"},
            [this](net::Connection& c, const std::string& m,
                   chess::application::MessageSink&) { handle_unlink_google(c, m); });
    }
    (void)raw;  // helper kept for future use if migration wants it
    for(const std::string type:{"request_password_reset","reset_password","verify_email","set_recovery_email"}) {
        pipeline.register_raw_route(RoutePolicy{type},
            [this,type](net::Connection& c,const std::string& m,chess::application::MessageSink&){handle_email(c,m,type);});
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

    std::string username, email;
    if (!require_string(conn, j, "username", username)) return;
    if (!require_string(conn,j,"email",email)) return;
    if(!email_ || !email_->enabled()) {send_auth_error(conn,"email_unavailable");return;}
    const auto address=canonical_email(email);
    if(!address.empty() && !rl_email_address_.try_take(address)) {
        send_auth_error(conn,"rate_limited"); return;
    }
    const auto status=email_->register_user(username,email,now_seconds());
    if(status==EmailStatus::Ok) send_json(conn,serialize_type_first("email_sent",json::object()));
    else send_auth_error(conn,status==EmailStatus::InvalidEmail?"invalid_email":
        status==EmailStatus::InvalidUsername?"invalid_username":status==EmailStatus::UsernameTaken?"username_taken":
        status==EmailStatus::EmailTaken?"email_taken":status==EmailStatus::RateLimited?"rate_limited":
        status==EmailStatus::WeakPassword?"weak_password":
        status==EmailStatus::Unavailable?"email_unavailable":"internal");
}

void AuthHandler::handle_email(net::Connection& conn,const std::string& message,const std::string& type) {
    if(!rl_email_ip_.try_take(conn.get_ip())) {send_auth_error(conn,"rate_limited");return;}
    json j; if(!parse_or_fail(conn,message,j)) return;
    if(!email_ || !email_->enabled()) {send_auth_error(conn,"email_unavailable");return;}
    EmailStatus status=EmailStatus::InvalidToken;
    std::string token,password,email,activated_username;
    if(type=="reset_password") {
        if(!require_string(conn,j,"email_token",token) || !require_string(conn,j,"password",password)) return;
        status=email_->reset_password(token,password,now_seconds(),&activated_username);
    } else if(type=="verify_email") {
        if(!require_string(conn,j,"email_token",token)) return;
        if(j.contains("password") && !require_string(conn,j,"password",password)) return;
        status=email_->confirm_email(token,now_seconds(),password,&activated_username);
    } else {
        if(!require_string(conn,j,"email",email)) return;
        const auto address=canonical_email(email);
        if(type=="set_recovery_email") {
            std::string access;
            if(!require_string(conn,j,"access_token",access) || !require_string(conn,j,"password",password)) return;
            AccessClaims claims;
            if(authorize_access_token(db_,signer_,access,now_seconds(),claims)!=GateOutcome::Ok) {
                send_auth_error(conn,"unauthorized");return;
            }
            if(!address.empty() && !rl_email_address_.try_take(address)) {
                send_json(conn,serialize_type_first("email_sent",json::object()));return;
            }
            status=email_->request_recovery_email(claims.player_id,email,password,now_seconds());
        } else {
            if(!address.empty() && !rl_email_address_.try_take(address)) {
                send_json(conn,serialize_type_first("email_sent",json::object()));return;
            }
            status=email_->request_reset(email,now_seconds());
        }
    }
    if(status==EmailStatus::Ok) {
        json payload=json::object();
        if(!activated_username.empty()) payload["username"]=activated_username;
        send_json(conn,serialize_type_first(
            type=="reset_password" || type=="verify_email"?"auth_action_ok":"email_sent",payload));
    }
    else send_auth_error(conn,status==EmailStatus::InvalidEmail?"invalid_email":
        status==EmailStatus::WeakPassword?"weak_password":status==EmailStatus::InvalidToken?"invalid_email_token":
        status==EmailStatus::Unauthorized?"invalid_credentials":status==EmailStatus::Conflict?"email_conflict":
        status==EmailStatus::Unavailable?"email_unavailable":"internal");
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
    // Capture rotation time after waiting for this PG session. A request that
    // queued behind another rotation must not compare an earlier timestamp
    // against that successor's later rotated_at and revoke a valid family.
    auto operation = db_.acquire_operation();
    auto tokens = refresh_session(db_, signer_, refresh_token, now_seconds(), outcome);
    if (outcome != RefreshOutcome::Ok || !tokens.ok) {
        // A database/transport problem is retryable and must not instruct the
        // browser to erase an otherwise valid persisted login. Only a token
        // the session layer actually rejected is reported as invalid_refresh.
        send_auth_error(conn, outcome == RefreshOutcome::DatabaseError
            ? "internal" : "invalid_refresh");
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
