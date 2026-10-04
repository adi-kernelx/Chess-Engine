#include "auth/email_recovery.h"
#include "auth/password.h"
#include "auth/service.h"
#include "auth/username.h"
#include "crypto/base64.h"
#include "crypto/random.h"
#include "storage/transaction.h"
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <cctype>

namespace chess::auth {
using storage::Param;
std::string canonical_email(const std::string& input) {
    if (input.empty() || input.size()>254) return {};
    const auto at=input.find('@');
    if (at==std::string::npos || at==0 || at>64 || at!=input.rfind('@')) return {};
    std::string email=input;
    for (char& ch:email) {
        auto c=static_cast<unsigned char>(ch);
        if (c<=32 || c>=127) return {};
        ch=static_cast<char>(std::tolower(c));
    }
    const std::string local=email.substr(0,at), domain=email.substr(at+1);
    if (local.front()=='.' || local.back()=='.' || local.find("..")!=std::string::npos) return {};
    for (char c:local) if (!std::isalnum(static_cast<unsigned char>(c)) &&
        std::string(".!#$%&'*+-/=?^_`{|}~").find(c)==std::string::npos) return {};
    if (domain.find('.')==std::string::npos || domain.empty()) return {};
    size_t start=0;
    while (start<domain.size()) {
        auto end=domain.find('.',start); if(end==std::string::npos) end=domain.size();
        auto label=domain.substr(start,end-start);
        if(label.empty() || label.size()>63 || label.front()=='-' || label.back()=='-') return {};
        for(char c:label) if(!std::isalnum(static_cast<unsigned char>(c)) && c!='-') return {};
        start=end+1;
    }
    if(domain.back()=='.') return {};
    return email;
}
std::string email_token_hash(const std::string& token) {
    if(token.size()!=43 || !std::all_of(token.begin(),token.end(),[](unsigned char c){
        return std::isalnum(c) || c=='_' || c=='-'; })) return {};
    std::array<unsigned char,SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(token.data()),token.size(),digest.data());
    return crypto::encode_base64url(digest.data(),digest.size());
}

EmailStatus EmailRecovery::issue(const std::string& purpose, const std::string& email,
    int64_t player_id, int epoch, const std::string& username, int64_t now) {
    if(!enabled()) return EmailStatus::Unavailable;
    std::string token, token_hash;
    {
        storage::Transaction tx(db_); if(!tx.ok()) return EmailStatus::DatabaseError;
        // Serializes issuance across DB connections/instances as well as threads.
        if(!db_.exec("SELECT pg_advisory_xact_lock(hashtext($1))",{Param::text(email)}).ok)
            return EmailStatus::DatabaseError;
        if(!db_.exec("DELETE FROM email_challenges WHERE expires_at<=to_timestamp($1) AND created_at<to_timestamp($1)-interval '1 hour'",
            {Param::int64(now)}).ok) return EmailStatus::DatabaseError;
        auto count=db_.exec("SELECT count(*) FROM email_challenges WHERE email=$1 AND created_at>to_timestamp($2)-interval '1 hour'",
            {Param::text(email),Param::int64(now)});
        if(!count.ok) return EmailStatus::DatabaseError;
        if(std::stoi(count.first().at(0))>=3) return EmailStatus::Ok; // no enumeration
        auto random=crypto::secure_random_buffer(32);
        token=crypto::encode_base64url(random.data(),random.size()); token_hash=email_token_hash(token);
        auto result=db_.exec("INSERT INTO email_challenges(token_hash,purpose,email,player_id,token_epoch,username,created_at,expires_at)"
            " VALUES($1,$2,$3,$4,$5,$6,to_timestamp($7),to_timestamp($8))",
            {Param::text(token_hash),Param::text(purpose),Param::text(email),
             player_id ? Param::int64(player_id):Param::null(), player_id ? Param::int64(epoch):Param::null(),
             username.empty()?Param::null():Param::text(username),
             Param::int64(now),Param::int64(now+(purpose=="reset"?900:1800))});
        if(!result.ok || !tx.commit()) return EmailStatus::DatabaseError;
    } // Never hold the database transaction while dispatching mail.
    const auto route=purpose=="reset"?"reset-password":purpose=="register"?"activate-account":"verify-email";
    std::string body="A request was made for your Multiplayer Chess account.\r\n\r\n";
    body += purpose=="reset"?"Choose a new password using this link (expires in 15 minutes):\r\n":"Confirm your email using this link (expires in 30 minutes):\r\n";
    body += public_url_+"/#/"+route+"/"+token+"\r\n\r\nIf you did not request this, ignore this email. Do not share this link.\r\n";
    if(purpose=="reset" && !player_id) body.clear();
    if(!send_(email,"Multiplayer Chess account confirmation",body)) {
        db_.exec("DELETE FROM email_challenges WHERE token_hash=$1",{Param::text(token_hash)});
        return EmailStatus::Unavailable;
    }
    return EmailStatus::Ok;
}
EmailStatus EmailRecovery::register_user(const std::string& username, const std::string& raw_email,
    const std::string& password, int64_t now) {
    if(!enabled()) return EmailStatus::Unavailable;
    const auto email=canonical_email(raw_email); if(email.empty()) return EmailStatus::InvalidEmail;
    if(!valid_username(username)) return EmailStatus::InvalidUsername;
    if(password.size()<PASSWORD_MIN_LEN || password.size()>PASSWORD_MAX_LEN) return EmailStatus::WeakPassword;
    // Always pay the KDF cost, even for an existing-email registration.
    const auto hash=hash_password(password); if(hash.empty()) return EmailStatus::Unavailable;
    auto existing=db_.exec("SELECT id,token_epoch,email_verified FROM players WHERE lower(email)=$1",{Param::text(email)});
    if(!existing.ok) return EmailStatus::DatabaseError;
    if(!existing.empty()) {
        if(existing.first().at(2)!="t") return send_(email,"","")?EmailStatus::Ok:EmailStatus::Unavailable;
        // Never overwrite an existing password from a registration request.
        return issue("reset",email,std::stoll(existing.first().at(0)),std::stoi(existing.first().at(1)),"",now);
    }
    return issue("register",email,0,0,username,now);
}
EmailStatus EmailRecovery::request_reset(const std::string& raw_email, int64_t now) {
    if(!enabled()) return EmailStatus::Unavailable;
    const auto email=canonical_email(raw_email); if(email.empty()) return EmailStatus::InvalidEmail;
    auto row=db_.exec("SELECT id,token_epoch FROM players WHERE lower(email)=$1 AND email_verified=TRUE",{Param::text(email)});
    if(!row.ok) return EmailStatus::DatabaseError;
    // Same bounded persistence/enqueue path for missing and existing accounts.
    if(row.empty()) return issue("reset",email,0,0,"",now);
    return issue("reset",email,std::stoll(row.first().at(0)),std::stoi(row.first().at(1)),"",now);
}
EmailStatus EmailRecovery::request_recovery_email(int64_t player_id,const std::string& raw_email,
    const std::string& password,int64_t now) {
    if(!enabled()) return EmailStatus::Unavailable;
    const auto email=canonical_email(raw_email); if(email.empty()) return EmailStatus::InvalidEmail;
    if(password.size()<PASSWORD_MIN_LEN || password.size()>PASSWORD_MAX_LEN) return EmailStatus::Unauthorized;
    auto row=db_.exec("SELECT password_hash,token_epoch,email FROM players WHERE id=$1",{Param::int64(player_id)});
    if(!row.ok) return EmailStatus::DatabaseError;
    if(row.empty() || row.first().is_null(0) || !verify_password(password,row.first().at(0))) return EmailStatus::Unauthorized;
    // Legacy accounts only: changing an already-bound address is a separate,
    // higher-risk workflow. In particular never change a Google identity email.
    if(!row.first().is_null(2)) return EmailStatus::Conflict;
    return issue("recovery_email",email,player_id,std::stoi(row.first().at(1)),"",now);
}

EmailStatus EmailRecovery::confirm_email(const std::string& token,int64_t now,const std::string& password) {
    const auto hash=email_token_hash(token); if(hash.empty()) return EmailStatus::InvalidToken;
    // Only the mailbox owner may choose the active password. Do not install
    // the password supplied by a possibly hostile, unverified registration.
    std::string phc;
    if(!password.empty()) {
        if(password.size()<PASSWORD_MIN_LEN || password.size()>PASSWORD_MAX_LEN) return EmailStatus::WeakPassword;
        phc=hash_password(password); if(phc.empty()) return EmailStatus::Unavailable;
    }
    storage::Transaction tx(db_); if(!tx.ok()) return EmailStatus::DatabaseError;
    auto row=db_.exec("SELECT purpose,email,player_id,token_epoch,username FROM email_challenges"
        " WHERE token_hash=$1 AND purpose<>'reset' AND expires_at>to_timestamp($2)",
        {Param::text(hash),Param::int64(now)});
    if(!row.ok) return EmailStatus::DatabaseError;
    if(row.empty()) return EmailStatus::InvalidToken;
    if(!row.first().is_null(2)) {
        auto player=db_.exec("SELECT id FROM players WHERE id=$1 FOR UPDATE",{Param::text(row.first().at(2))});
        if(!player.ok) return EmailStatus::DatabaseError;
        if(player.empty()) return EmailStatus::InvalidToken;
    }
    row=db_.exec("SELECT purpose,email,player_id,token_epoch,username FROM email_challenges"
        " WHERE token_hash=$1 AND purpose<>'reset' AND expires_at>to_timestamp($2) FOR UPDATE",
        {Param::text(hash),Param::int64(now)});
    if(!row.ok) return EmailStatus::DatabaseError;
    if(row.empty()) return EmailStatus::InvalidToken;
    const auto& r=row.first();
    if(r.at(0)=="register") {
        if(phc.empty()) return EmailStatus::WeakPassword;
        // Conflict with a now-existing Google/email account NEVER installs the
        // pending password. Owner must use the reset flow on that account.
        auto ins=db_.exec("INSERT INTO players(username,username_ci,email,email_verified,password_hash,elo_rating)"
            " VALUES($1,$2,$3,TRUE,$4,$5)",
            {Param::text(r.at(4)),Param::text(to_lower_ascii(r.at(4))),Param::text(r.at(1)),Param::text(phc),Param::int64(INITIAL_ELO)});
        if(!ins.ok) return ins.sqlstate==storage::pg_errors::UNIQUE_VIOLATION?EmailStatus::Conflict:EmailStatus::DatabaseError;
    } else {
        auto up=db_.exec("UPDATE players SET email=$1,email_verified=TRUE WHERE id=$2 AND token_epoch=$3 AND email IS NULL",
            {Param::text(r.at(1)),Param::text(r.at(2)),Param::text(r.at(3))});
        if(!up.ok) return up.sqlstate==storage::pg_errors::UNIQUE_VIOLATION?EmailStatus::Conflict:EmailStatus::DatabaseError;
        if(up.rows_affected!=1) return EmailStatus::InvalidToken;
    }
    if(!db_.exec("DELETE FROM email_challenges WHERE token_hash=$1",{Param::text(hash)}).ok || !tx.commit())
        return EmailStatus::DatabaseError;
    return EmailStatus::Ok;
}
EmailStatus EmailRecovery::reset_password(const std::string& token,const std::string& password,int64_t now) {
    const auto hash=email_token_hash(token); if(hash.empty()) return EmailStatus::InvalidToken;
    if(password.size()<PASSWORD_MIN_LEN || password.size()>PASSWORD_MAX_LEN) return EmailStatus::WeakPassword;
    const auto phc=hash_password(password); if(phc.empty()) return EmailStatus::Unavailable;
    storage::Transaction tx(db_); if(!tx.ok()) return EmailStatus::DatabaseError;
    // Consistent player -> challenge lock order avoids sibling-reset deadlocks.
    auto row=db_.exec("SELECT p.id,p.token_epoch,c.token_epoch FROM players p JOIN email_challenges c ON c.player_id=p.id"
        " WHERE c.token_hash=$1 AND c.purpose='reset' AND c.expires_at>to_timestamp($2)"
        " AND p.email_verified=TRUE AND lower(p.email)=c.email FOR UPDATE OF p",
        {Param::text(hash),Param::int64(now)});
    if(!row.ok) return EmailStatus::DatabaseError;
    if(row.empty() || row.first().at(1)!=row.first().at(2)) return EmailStatus::InvalidToken;
    auto claimed=db_.exec("DELETE FROM email_challenges WHERE token_hash=$1 RETURNING player_id",{Param::text(hash)});
    if(!claimed.ok) return EmailStatus::DatabaseError;
    if(claimed.empty()) return EmailStatus::InvalidToken;
    const auto id=row.first().at(0);
    if(!db_.exec("UPDATE players SET password_hash=$1,token_epoch=token_epoch+1 WHERE id=$2",{Param::text(phc),Param::text(id)}).ok ||
       !db_.exec("DELETE FROM sessions WHERE player_id=$1",{Param::text(id)}).ok ||
       !db_.exec("DELETE FROM email_challenges WHERE player_id=$1",{Param::text(id)}).ok || !tx.commit())
        return EmailStatus::DatabaseError;
    return EmailStatus::Ok;
}
} // namespace chess::auth
