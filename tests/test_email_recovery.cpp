// Local-only integration tests: synthetic identities and captured email; never SMTP.
#include "auth/email_recovery.h"
#include "auth/smtp_mailer.h"
#include "auth/oauth_verify.h"
#include "auth/password.h"
#include "auth/service.h"
#include "auth/session.h"
#include "crypto/base64.h"
#include "crypto/hmac.h"
#include "crypto/sha256.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <stdexcept>
#include <unistd.h>

using namespace chess::auth;
using namespace chess::storage;
using namespace chess::crypto;
using nlohmann::json;
static void check(bool value,const char* label){if(!value)throw std::runtime_error(label);std::cout<<"PASS "<<label<<'\n';}
static std::string file(const std::string& path){std::ifstream f(path);std::ostringstream s;s<<f.rdbuf();return s.str();}
static std::string token_of(const std::string& body){auto p=body.find("/#/");if(p==std::string::npos)return {};p=body.find('/',p+3);return body.substr(p+1,43);}
static std::string jwt(const std::string& sub,const std::string& email,int64_t now) {
    auto enc=[](const std::string& s){return encode_base64url(reinterpret_cast<const uint8_t*>(s.data()),s.size());};
    json payload={{"iss","https://synthetic.supabase.co/auth/v1"},{"aud","authenticated"},
        {"sub",sub},{"email",email},{"email_verified",true},{"iat",now},{"exp",now+3600},
        {"app_metadata",{{"provider","google"}}}};
    std::string input=enc(R"({"alg":"HS256","typ":"JWT"})")+'.'+enc(payload.dump());
    const std::string secret(32,'s'); HmacSha256 mac(reinterpret_cast<const uint8_t*>(secret.data()),secret.size());
    mac.update(reinterpret_cast<const uint8_t*>(input.data()),input.size());auto digest=mac.finish();
    return input+'.'+encode_base64url(digest.data(),digest.size());
}
int main(int argc,char** argv) {
    // Fixture consumed privately by the local transport harness; this is the
    // hash of a fixed, public test password, never a supplied credential.
    if(argc==2 && std::string(argv[1])=="--synthetic-password-hash") {
        std::cout<<hash_password("Disposable-Seal-Test-42!");return 0;
    }
    // Exercise configuration/worker lifecycle with synthetic settings only.
    unsetenv("SMTP_PASSWORD");unsetenv("SMTP_USERNAME");unsetenv("AUTH_PUBLIC_URL");
    {SmtpMailer mailer;check(!mailer.enabled(),"SMTP absent is disabled");}
    setenv("SMTP_USERNAME","synthetic@example.com",1);setenv("SMTP_PASSWORD","synthetic-not-a-real-password",1);
    setenv("AUTH_PUBLIC_URL","https://example.invalid\r\nInjected: header",1);
    {SmtpMailer mailer;check(!mailer.enabled(),"SMTP rejects injected callback origin");}
    setenv("AUTH_PUBLIC_URL","https://example.invalid",1);
    {SmtpMailer mailer;check(mailer.enabled() && mailer.sender()("synthetic@example.com","",""),"SMTP no-op queue and worker shutdown without network");}
    unsetenv("SMTP_PASSWORD");unsetenv("SMTP_USERNAME");unsetenv("AUTH_PUBLIC_URL");
    const char* config=std::getenv("DATABASE_URL");
    if(!config || std::string(config)!="postgresql://localhost/chess_test?host=/var/run/postgresql&user=adi") {
        std::cerr<<"Refusing: integration test requires the explicit disposable local database\n";return 1;
    }
    Database db;std::string err;
    if(!db.connect(config,err)){std::cerr<<"Local test database unavailable (details withheld)\n";return 1;}
    const std::string schema="email_recovery_test_"+std::to_string(getpid());
    bool created=false;
    try {
        check(db.exec("CREATE SCHEMA "+schema).ok,"isolated local schema created");created=true;
        check(db.exec("SET search_path TO "+schema).ok,"isolated search path");
        check(db.run_script(file(CHESS_SOURCE_DIR "/src/storage/schema_phase7.sql"),err),"base schema");
        std::string migration=file(CHESS_SOURCE_DIR "/src/storage/migrations/0015_verified_email_recovery.sql");
        for(size_t p=0;(p=migration.find("public.",p))!=std::string::npos;p+=schema.size()+1)migration.replace(p,7,schema+'.');
        check(db.run_script(migration,err),"migration 0015");
        check(db.exec("SELECT relrowsecurity FROM pg_class WHERE oid='email_challenges'::regclass").first().at(0)=="t","challenge table RLS enabled");
        check(db.exec("SELECT count(*) FROM pg_policies WHERE schemaname=$1 AND tablename='email_challenges'",
            {Param::text(schema)}).first().at(0)=="0","no permissive browser policy");
        check(canonical_email("Owner@Example.COM")=="owner@example.com" && canonical_email("x\r\nBcc:x@example.com").empty()
            && canonical_email("a@@example.com").empty() && canonical_email("a@x..com").empty(),"email canonicalization/injection rejection");
        const int64_t now=1700000000;
        std::vector<std::string> mail;
        EmailRecovery service(db,[&](const std::string&,const std::string&,const std::string& body){mail.push_back(body);return true;},"https://example.invalid");
        check(service.register_user("Owner","Owner@Example.com","requester-password",now)==EmailStatus::Ok,"registration request accepted");
        auto first=token_of(mail.back());check(first.size()==43,"opaque email-link token");
        check(db.exec("SELECT count(*) FROM players").first().at(0)=="0","no account created before mailbox verification");
        check(db.exec("SELECT token_hash FROM email_challenges").first().at(0)==email_token_hash(first),"only token digest persisted");
        check(service.confirm_email(first,now+1)==EmailStatus::WeakPassword,"unverified requester password cannot activate account");
        check(service.reset_password(first,"owner-password",now+1)==EmailStatus::InvalidToken,"verification token cannot reset passwords");
        check(service.confirm_email(first,now+2,"owner-password")==EmailStatus::Ok,"mailbox owner chooses active password");
        check(service.confirm_email(first,now+3,"owner-password")==EmailStatus::InvalidToken,"verification token single-use");
        auto owner=authenticate_password(db,"Owner","owner-password");
        check(owner.status==LoginResult::Status::Ok && authenticate_password(db,"Owner","requester-password").status!=LoginResult::Status::Ok,
            "only mailbox-confirmed password authenticates");
        SecureBuffer secret(32);std::fill(secret.data(),secret.data()+secret.size(),'s');
        auto verifier=SupabaseVerifier::make(std::move(secret),"https://synthetic.supabase.co/auth/v1","authenticated","google");
        db.exec("UPDATE players SET elo_rating=1234 WHERE id=$1",{Param::int64(owner.player_id)});
        auto google=google_sign_in(db,verifier,jwt("google-owner","OWNER@example.com",now),now+4);
        check(google.status==GoogleSignInStatus::Ok && google.player_id==owner.player_id && google.username=="Owner" && google.elo_rating==1234
            && !google.created_new_account,"verified Google email reuses account/rating");
        check(google_sign_in(db,verifier,jwt("different-google","owner@example.com",now),now+5).status==GoogleSignInStatus::EmailCollision,
            "different Google identity cannot replace binding");
        check(service.request_reset("missing@example.com",now+6)==EmailStatus::Ok && mail.back().empty(),"unknown reset has same response/no email");
        check(service.request_reset("owner@example.com",now+7)==EmailStatus::Ok,"known reset request");
        const auto reset=token_of(mail.back());
        auto signer=TokenSigner::generate_random();auto session=issue_session(db,signer,owner.player_id,"Owner",0,now+7);
        check(session.ok,"synthetic session issued");
        check(service.confirm_email(reset,now+8,"new-password")==EmailStatus::InvalidToken,"reset token cannot confirm email");
        check(service.reset_password(reset,"short",now+8)==EmailStatus::WeakPassword,"weak password does not consume link");
        check(service.reset_password(reset,"new-owner-password",now+9)==EmailStatus::Ok,"password reset succeeds");
        AccessClaims claims;
        check(authorize_access_token(db,signer,session.access_token,now+10,claims)!=GateOutcome::Ok
            && db.exec("SELECT count(*) FROM sessions").first().at(0)=="0","reset revokes access and refresh sessions");
        check(service.reset_password(reset,"replay-password",now+10)==EmailStatus::InvalidToken,"reset token single-use");
        check(authenticate_password(db,"Owner","new-owner-password").status==LoginResult::Status::Ok
            && authenticate_password(db,"Owner","owner-password").status!=LoginResult::Status::Ok,"old password replaced");
        check(service.request_reset("owner@example.com",now+11)==EmailStatus::Ok,"second reset issued");
        const auto expired=token_of(mail.back());
        check(service.reset_password(expired,"expired-password",now+911)==EmailStatus::InvalidToken,"15-minute reset expiry");
        check(service.request_reset("owner@example.com",now+12)==EmailStatus::Ok,"sibling reset one issued");
        const auto sibling1=token_of(mail.back());
        check(service.request_reset("owner@example.com",now+13)==EmailStatus::Ok,"sibling reset two issued");
        const auto sibling2=token_of(mail.back());
        check(service.reset_password(sibling1,"final-owner-password",now+14)==EmailStatus::Ok &&
            service.reset_password(sibling2,"stale-sibling-password",now+15)==EmailStatus::InvalidToken,"password reset invalidates sibling challenges");
        check(service.register_user("Expired","expired@example.com","registration-password",now)==EmailStatus::Ok,"expiring activation issued");
        check(service.confirm_email(token_of(mail.back()),now+1800,"activation-password")==EmailStatus::InvalidToken,"30-minute activation expiry");
        check(service.register_user("Collision","collision@example.com","requester-password",now)==EmailStatus::Ok,"pending registration before Google login");
        const auto pending=token_of(mail.back());
        auto firstGoogle=google_sign_in(db,verifier,jwt("collision-google","collision@example.com",now),now+1);
        check(firstGoogle.status==GoogleSignInStatus::Ok && service.confirm_email(pending,now+2,"pending-password")==EmailStatus::Conflict,
            "pending activation cannot overwrite newly created Google account");
        check(db.exec("SELECT password_hash FROM players WHERE id=$1",{Param::int64(firstGoogle.player_id)}).first().is_null(0),
            "Google account still has no requester-chosen password");
        auto legacy=register_password_user(db,"Legacy","legacy-password");
        check(service.request_recovery_email(legacy.player_id,"legacy@example.com","wrong-password",now)==EmailStatus::Unauthorized,
            "adding recovery email requires current password");
        check(service.request_recovery_email(legacy.player_id,"legacy@example.com","legacy-password",now)==EmailStatus::Ok,"legacy recovery email requested");
        check(service.confirm_email(token_of(mail.back()),now+1)==EmailStatus::Ok,"legacy email verified without changing password");
        check(authenticate_password(db,"Legacy","legacy-password").status==LoginResult::Status::Ok,"legacy credentials preserved");
        check(service.request_recovery_email(legacy.player_id,"other@example.com","legacy-password",now)==EmailStatus::Conflict,
            "verified recovery address cannot silently change");
        auto googleOnly=google_sign_in(db,verifier,jwt("google-only","only@example.com",now),now+1);
        check(service.register_user("NotTheRealName","only@example.com","requester-password",now+2)==EmailStatus::Ok,"registration with Google email sends set-password link");
        check(service.reset_password(token_of(mail.back()),"google-password",now+3)==EmailStatus::Ok,"Google account can add password via verified mailbox");
        auto reused=authenticate_password(db,googleOnly.username,"google-password");
        check(reused.status==LoginResult::Status::Ok && reused.player_id==googleOnly.player_id,"Google/password share one account");
        // Two independent PostgreSQL connections race the same token.
        check(service.request_reset("legacy@example.com",now+10)==EmailStatus::Ok,"concurrent reset issued");
        const auto raceToken=token_of(mail.back());
        Database other;check(other.connect(config,err) && other.exec("SET search_path TO "+schema).ok,"second local connection");
        EmailRecovery concurrent(other,[](const std::string&,const std::string&,const std::string&){return true;},"https://example.invalid");
        EmailStatus a,b;
        std::thread t1([&]{a=service.reset_password(raceToken,"race-password-one",now+11);});
        std::thread t2([&]{b=concurrent.reset_password(raceToken,"race-password-two",now+11);});t1.join();t2.join();
        check((a==EmailStatus::Ok && b==EmailStatus::InvalidToken)||(b==EmailStatus::Ok && a==EmailStatus::InvalidToken),"simultaneous reset consumes token exactly once");
        EmailRecovery offline(db,{},"https://example.invalid");
        check(offline.register_user("Offline","offline@example.com","some-password",now)==EmailStatus::Unavailable,"SMTP absent fails closed");
        EmailRecovery full(db,[](const std::string&,const std::string&,const std::string&){return false;},"https://example.invalid");
        check(full.request_reset("queue@example.com",now)==EmailStatus::Unavailable &&
            db.exec("SELECT count(*) FROM email_challenges WHERE email='queue@example.com'").first().at(0)=="0","queue rejection removes undeliverable challenge");
        check(db.exec("DROP SCHEMA "+schema+" CASCADE").ok,"isolated test schema removed");return 0;
    } catch(const std::exception& e) {
        std::cerr<<"FAIL "<<e.what()<<" (database/response details withheld)\n";
        if(created) db.exec("DROP SCHEMA "+schema+" CASCADE");
        return 1;
    }
}
