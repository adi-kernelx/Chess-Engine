#pragma once
#include <functional>
#include <string>
#include "storage/database.h"

namespace chess::auth {
// Enqueue, not SMTP I/O. Empty body is an indistinguishable no-op job for an
// unknown address. Transport never receives passwords, only a one-time link.
using EmailSender = std::function<bool(const std::string&, const std::string&, const std::string&)>;
enum class EmailStatus { Ok, InvalidEmail, InvalidUsername, WeakPassword,
    InvalidToken, Unauthorized, Conflict, Unavailable, DatabaseError };
std::string canonical_email(const std::string& input);
std::string email_token_hash(const std::string& token);

class EmailRecovery {
public:
    EmailRecovery(storage::Database& db, EmailSender send, std::string public_url)
        : db_(db), send_(std::move(send)), public_url_(std::move(public_url)) {}
    bool enabled() const { return static_cast<bool>(send_); }
    EmailStatus register_user(const std::string& username, const std::string& email,
                              const std::string& password, int64_t now);
    EmailStatus request_reset(const std::string& email, int64_t now);
    EmailStatus request_recovery_email(int64_t player_id, const std::string& email,
                                       const std::string& password, int64_t now);
    EmailStatus confirm_email(const std::string& token, int64_t now, const std::string& password = "");
    EmailStatus reset_password(const std::string& token, const std::string& password, int64_t now);
private:
    EmailStatus issue(const std::string& purpose, const std::string& email,
        int64_t player_id, int epoch, const std::string& username,
        int64_t now);
    storage::Database& db_;
    EmailSender send_;
    std::string public_url_;
};
} // namespace chess::auth
