/**
 * service.h — the "did the user prove who they are" layer.
 *
 * Sits between the WebSocket handlers (which do JSON) and the database (which
 * does SQL). Handlers hand it a username and a password; it returns a
 * decision plus, on success, the row a session will be issued for. Nothing
 * here writes to the wire, and nothing here holds a session — Phase 7.7 will
 * add that on top.
 *
 * The interface is deliberately small: an handler that has these two
 * functions has everything it needs for password auth. Adding Google Sign-In
 * (§7.8) will add a third function next to them; every existing call site
 * stays the same.
 */

#pragma once

#include "storage/database.h"

#include <cstdint>
#include <string>

namespace chess {
namespace auth {

struct RegisterResult {
    enum class Status {
        Ok,
        InvalidUsername,       ///< fails valid_username()
        WeakPassword,          ///< fails length rules
        UsernameTaken,         ///< SQLSTATE 23505 on username_ci
        DatabaseError,
        InternalError,         ///< hash generation failed
    };

    Status      status = Status::InternalError;
    int64_t     player_id = 0;
    std::string username;      ///< original case, as inserted
    int         elo_rating = 0;
};

struct LoginResult {
    enum class Status {
        Ok,
        /// Wrong password OR unknown username OR database error — deliberately
        /// merged: the handler must send the same "Invalid credentials" for
        /// all of them, and this enum reflects what the handler will do.
        InvalidCredentials,
    };

    Status      status = Status::InvalidCredentials;
    int64_t     player_id = 0;
    std::string username;
    int         elo_rating = 0;
    int         token_epoch = 0;

    /// True if the stored hash used weaker parameters than today's defaults.
    /// Set only on Ok. The handler re-hashes with fresh parameters and updates
    /// the row — password rotation for free, transparent to the user.
    bool        hash_needs_upgrade = false;
};

/// Password policy — length only. Composition rules ("must contain a digit")
/// hurt more than they help; the KDF cost is where password security lives.
constexpr size_t PASSWORD_MIN_LEN = 8;
constexpr size_t PASSWORD_MAX_LEN = 256;   // Argon2 handles more, but this is
                                           // a size bound on request payload.

/**
 * Create a password account.
 *
 * The transaction is a single INSERT: the DB enforces uniqueness (23505 →
 * UsernameTaken) and the `has_a_login_method` CHECK (impossible here — we
 * always pass a hash). Returns InvalidUsername / WeakPassword synchronously
 * without touching the DB.
 */
RegisterResult register_password_user(storage::Database& db,
                                      const std::string& username,
                                      const std::string& password);

/**
 * Look up `username` (case-insensitive) and verify `password`.
 *
 * On failure the function still runs Argon2id against a dummy hash of the same
 * parameters, so the wrong-username branch and the wrong-password branch take
 * the same wall-clock time. Without that, an attacker who cannot see the DB
 * can still enumerate valid usernames by request latency alone.
 *
 * On success the row is stamped with `last_login = now()`, but `token_epoch`
 * is NOT bumped — that field is for logout_all, not routine login.
 */
LoginResult authenticate_password(storage::Database& db,
                                  const std::string& username,
                                  const std::string& password);

} // namespace auth
} // namespace chess
