/**
 * auth_handler.h — WebSocket bridge for the Phase 7 auth surface.
 *
 * Wires every §7.6–§7.9 primitive into the MessageRouter so a browser can
 * actually sign in. This is the counterpart to game_handler.h for auth.
 *
 * Wire messages handled (all rate-limited before any expensive work):
 *
 *   → { type: "seal_request" }
 *   ← { type: "seal_key",     key_id, master_b64, expires_in, offer_sig,
 *                             identity_pk }
 *
 *   → { type: "register",     username, password }
 *   ← { type: "auth_ok",      username, elo, access_token, refresh_token,
 *                             access_expires_in }
 *   ← { type: "auth_error",   code: "invalid_username" | "weak_password" |
 *                                   "username_taken" | "internal" }
 *
 *   → { type: "login",        username, password }
 *   ← { type: "auth_ok", ... }   as above
 *   ← { type: "auth_error", code: "invalid_credentials" | "rate_limited" | "internal" }
 *
 *   → { type: "refresh",      refresh_token }
 *   ← { type: "auth_ok", ... }
 *   ← { type: "auth_error",   code: "invalid_refresh" }
 *
 *   → { type: "logout",       refresh_token }
 *   ← { type: "logout_ok" }
 *
 *   → { type: "logout_all",   access_token }
 *   ← { type: "logout_ok" } | { type: "auth_error", code: "unauthorized" }
 *
 *   → { type: "google_auth",  supabase_jwt }
 *   ← { type: "auth_ok", ... }
 *   ← { type: "auth_error",   code: "invalid_google" | "email_collision" |
 *                                   "google_disabled" | "rate_limited" | "internal" }
 *
 *   → { type: "link_google",  access_token, supabase_jwt }
 *   ← { type: "link_ok" } | { type: "auth_error", code: ... }
 *
 *   → { type: "unlink_google", access_token }
 *   ← { type: "link_ok" } | { type: "auth_error", code: "last_login_method" | ... }
 *
 * Deliberate discipline:
 *   - All string values pass through nlohmann::json::dump() — no
 *     hand-concatenation, same rule as build_match_found in §7.9.
 *   - Every response has "type" as the first key (websocket.cpp finds
 *     `type` by string-scan, not JSON parse).
 *   - Every error reply is JSON with a machine-readable `code`, never a
 *     free-text `message` — the frontend switches on codes.
 *   - The password/login gate NEVER leaks whether the username exists.
 *     The dummy-hash defense in service.cpp already handles timing;
 *     this handler ensures the wire response is identical.
 *
 * "auth_disabled" reply: when the server was started without the required
 * env vars (no DB, no signing key), auth handlers are simply NOT registered
 * — the default handler answers with the generic "Unknown message type",
 * which is what capability.js already knows how to interpret as "preview
 * mode." That keeps `chess_server` runnable for local Phase 4-6 dev without
 * Postgres, without a special DISABLED response type in the protocol.
 */

#pragma once

#include "auth/rate_limiter.h"
#include "auth/oauth_verify.h"
#include "auth/token.h"
#include "crypto/sealed_registry.h"
#include "net/websocket.h"
#include "storage/database.h"

#include <memory>
#include <string>

namespace chess {
namespace auth {

class AuthHandler {
public:
    /**
     * @param db          shared Database; must outlive this handler.
     * @param signer      shared TokenSigner; must outlive this handler.
     * @param google      optional — pass nullptr to disable Google Sign-In
     *                    (missing SUPABASE_* env vars).
     * @param sealed_reg  optional — pass nullptr to disable seal_request
     *                    (missing server_identity.key on start-up).
     */
    AuthHandler(storage::Database& db,
                TokenSigner& signer,
                SupabaseVerifier* google,
                crypto::SealedRegistry* sealed_reg);

    /// Install every auth handler this instance provides.
    void register_handlers(net::MessageRouter& router);

private:
    void handle_seal_request(net::Connection& conn, const std::string& message);
    void handle_register    (net::Connection& conn, const std::string& message);
    void handle_login       (net::Connection& conn, const std::string& message);
    void handle_refresh     (net::Connection& conn, const std::string& message);
    void handle_logout      (net::Connection& conn, const std::string& message);
    void handle_logout_all  (net::Connection& conn, const std::string& message);
    void handle_google_auth (net::Connection& conn, const std::string& message);
    void handle_link_google (net::Connection& conn, const std::string& message);
    void handle_unlink_google(net::Connection& conn, const std::string& message);

    storage::Database&        db_;
    TokenSigner&              signer_;
    SupabaseVerifier*         google_;
    crypto::SealedRegistry*   sealed_reg_;

    // One limiter per surface, keyed as §7.9 specifies. Constructed once,
    // BEFORE any handler runs — this is the rate-limit-first ordering rule.
    RateLimiter rl_login_ip_;
    RateLimiter rl_login_account_;
    RateLimiter rl_register_ip_;
    RateLimiter rl_google_ip_;
    RateLimiter rl_seal_ip_;
};

} // namespace auth
} // namespace chess
