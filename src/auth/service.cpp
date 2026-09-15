#include "auth/service.h"

#include "auth/password.h"
#include "auth/username.h"

#include <cstdlib>

namespace chess {
namespace auth {

using namespace chess::storage;

RegisterResult register_password_user(Database& db,
                                      const std::string& username,
                                      const std::string& password) {
    RegisterResult r;

    if (!valid_username(username)) {
        r.status = RegisterResult::Status::InvalidUsername;
        return r;
    }
    if (password.size() < PASSWORD_MIN_LEN || password.size() > PASSWORD_MAX_LEN) {
        r.status = RegisterResult::Status::WeakPassword;
        return r;
    }

    const std::string phc = hash_password(password);
    if (phc.empty()) {
        r.status = RegisterResult::Status::InternalError;
        return r;
    }

    const std::string username_ci = to_lower_ascii(username);
    auto ins = db.exec(
        "INSERT INTO players(username, username_ci, password_hash)"
        " VALUES($1,$2,$3) RETURNING id, elo_rating",
        {Param::text(username),
         Param::text(username_ci),
         Param::text(phc)});

    if (!ins.ok) {
        // 23505 is unique_violation. The trigger and the CHECK produce their
        // own SQLSTATEs, but valid_username + a real hash make those paths
        // unreachable — so unique-violation is the only classified failure.
        if (ins.sqlstate == pg_errors::UNIQUE_VIOLATION) {
            r.status = RegisterResult::Status::UsernameTaken;
        } else {
            r.status = RegisterResult::Status::DatabaseError;
        }
        return r;
    }
    if (ins.rows.empty()) {
        r.status = RegisterResult::Status::DatabaseError;
        return r;
    }

    r.status     = RegisterResult::Status::Ok;
    r.player_id  = std::strtoll(ins.first().at(0).c_str(), nullptr, 10);
    r.elo_rating = std::atoi(ins.first().at(1).c_str());
    r.username   = username;
    return r;
}

LoginResult authenticate_password(Database& db,
                                  const std::string& username,
                                  const std::string& password) {
    LoginResult r;

    // Validate the username BEFORE the DB call. This isn't just cheap failure —
    // it means a wildly malformed username (a very long string, control bytes)
    // never reaches libpq, keeping error rates the same across the branches.
    if (!valid_username(username)) {
        // Still run Argon2id against the dummy hash so the wall-clock time of
        // "bad username" matches "wrong password". Without this, "malformed
        // username" would be a distinct, faster branch that an attacker could
        // use to probe the input filter.
        (void)verify_password(password, dummy_phc());
        return r;   // InvalidCredentials
    }
    if (password.size() < PASSWORD_MIN_LEN || password.size() > PASSWORD_MAX_LEN) {
        (void)verify_password(password, dummy_phc());
        return r;
    }

    const std::string username_ci = to_lower_ascii(username);
    auto sel = db.exec(
        "SELECT id, username, password_hash, elo_rating, token_epoch"
        "  FROM players WHERE username_ci=$1",
        {Param::text(username_ci)});

    // Three failure branches to merge into one wall-clock: DB error, no such
    // user, and password-only account with a NULL hash (a Google-only user
    // trying to sign in with a password they never set).
    const bool have_user =
        sel.ok && !sel.rows.empty() && !sel.first().is_null(2);

    const std::string phc = have_user ? sel.first().at(2) : dummy_phc();
    const bool matched    = verify_password(password, phc);
    if (!have_user || !matched) return r;

    r.status      = LoginResult::Status::Ok;
    r.player_id   = std::strtoll(sel.first().at(0).c_str(), nullptr, 10);
    r.username    = sel.first().at(1);           // original case
    r.elo_rating  = std::atoi(sel.first().at(3).c_str());
    r.token_epoch = std::atoi(sel.first().at(4).c_str());

    // Update last_login. This is best-effort — a failure here does not
    // invalidate the login the user already completed.
    db.exec("UPDATE players SET last_login=now() WHERE id=$1",
            {Param::int64(r.player_id)});

    // Re-hash with today's parameters if the stored version is weaker. The
    // upgrade is transparent: the user notices nothing, but the row now costs
    // an attacker the new amount of memory to grind against.
    if (needs_upgrade(phc)) {
        const std::string upgraded = hash_password(password);
        if (!upgraded.empty()) {
            db.exec("UPDATE players SET password_hash=$1 WHERE id=$2",
                    {Param::text(upgraded), Param::int64(r.player_id)});
            r.hash_needs_upgrade = true;
        }
    }
    return r;
}

} // namespace auth
} // namespace chess
