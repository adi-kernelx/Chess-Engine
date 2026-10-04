#include "auth/oauth_verify.h"
#include "crypto/base64.h"
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>
#include <stdexcept>
#include <cstdlib>
#include <optional>

using json = nlohmann::json;
using chess::auth::SupabaseVerifier;
using chess::auth::Es256JwksVerifier;
using chess::auth::SupabaseIdentity;
namespace {
constexpr const char* ISSUER = "https://abcdefghijklmnopqrst.supabase.co/auth/v1";
constexpr int64_t NOW = 1700000000;
using Key = std::shared_ptr<EVP_PKEY>;
Key generate() { return Key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"), EVP_PKEY_free); }
std::string encoded(const std::string& value) {
    return chess::crypto::encode_base64url(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}
std::string coordinate(EVP_PKEY* key, const char* name) {
    BIGNUM* raw = nullptr;
    if (EVP_PKEY_get_bn_param(key, name, &raw) != 1) throw std::runtime_error("fixture coordinate");
    std::unique_ptr<BIGNUM, decltype(&BN_free)> bn(raw, BN_free);
    uint8_t bytes[32];
    if (BN_bn2binpad(bn.get(), bytes, 32) != 32) throw std::runtime_error("fixture coordinate size");
    return chess::crypto::encode_base64url(bytes, 32);
}
json jwk(const Key& key, std::string kid) {
    return {{"kty","EC"},{"crv","P-256"},{"alg","ES256"},{"use","sig"},{"kid",kid},
        {"x",coordinate(key.get(),OSSL_PKEY_PARAM_EC_PUB_X)},
        {"y",coordinate(key.get(),OSSL_PKEY_PARAM_EC_PUB_Y)}};
}
json claims() {
    return {{"iss",ISSUER},{"aud","authenticated"},{"sub","es256-test-sub"},
        {"email","es256@example.invalid"},{"email_verified",true},
        {"exp",NOW+3600},{"iat",NOW},{"app_metadata",{{"provider","google"}}}};
}
std::string token(const Key& key, json payload=claims(), json header={{"alg","ES256"},{"typ","JWT"},{"kid","key-one"}}) {
    const auto input = encoded(header.dump())+"."+encoded(payload.dump());
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (EVP_DigestSignInit(ctx.get(),nullptr,EVP_sha256(),nullptr,key.get()) != 1) throw std::runtime_error("fixture sign");
    size_t size=0;
    EVP_DigestSign(ctx.get(),nullptr,&size,reinterpret_cast<const uint8_t*>(input.data()),input.size());
    std::vector<uint8_t> der(size);
    if (EVP_DigestSign(ctx.get(),der.data(),&size,reinterpret_cast<const uint8_t*>(input.data()),input.size()) != 1) throw std::runtime_error("fixture signature");
    const auto* cursor=der.data();
    std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> sig(d2i_ECDSA_SIG(nullptr,&cursor,static_cast<long>(size)),ECDSA_SIG_free);
    if (!sig) throw std::runtime_error("fixture DER");
    const BIGNUM *r,*s; ECDSA_SIG_get0(sig.get(),&r,&s);
    uint8_t raw[64]; BN_bn2binpad(r,raw,32); BN_bn2binpad(s,raw+32,32);
    return input+"."+chess::crypto::encode_base64url(raw,64);
}
struct Fixture {
    Key first=generate(), second=generate();
    json document={{"keys",json::array({jwk(first,"key-one")})}};
    std::chrono::steady_clock::time_point time{};
    std::atomic<int> calls{0};
    bool online=true;
    SupabaseVerifier verifier() {
        return SupabaseVerifier::make_es256(ISSUER,"authenticated","google",
            [this](const std::string& url,std::string& out) {
                if (url != std::string(ISSUER)+"/.well-known/jwks.json") return false;
                ++calls; out=document.dump(); return online;
            },[this]{ return time; });
    }
};
}
int test_es256() {
    int failures=0;
    const auto check=[&](const char* name,const std::function<bool()>& test) {
        const bool ok=test(); std::cout<<"  [ES256] "<<name<<": "<<(ok?"PASS":"FAIL")<<std::endl;
        if(!ok) ++failures;
    };
    check("valid signature and cached project keys",[]{
        Fixture f; auto verifier=f.verifier(); SupabaseIdentity id;
        const auto jwt=token(f.first);
        return verifier.verify(jwt,NOW,id) && id.sub=="es256-test-sub" &&
            verifier.verify(jwt,NOW,id) && f.calls==1;
    });
    check("wrong private key and changed signature rejected",[]{
        Fixture f; auto verifier=f.verifier(); SupabaseIdentity id;
        auto changed=token(f.first); changed.back()=changed.back()=='A'?'B':'A';
        return !verifier.verify(token(f.second),NOW,id) && !verifier.verify(changed,NOW,id) && id.sub.empty();
    });
    check("issuer/audience/expiry/nbf/iat/provider/email checked",[]{
        Fixture f; auto verifier=f.verifier(); SupabaseIdentity id;
        for (const auto& change : std::vector<json>{{{"iss","https://evil.invalid"}},{{"aud","anon"}},
            {{"exp",NOW}},{{"nbf",NOW+1}},{{"iat",NOW+61}},{{"iat","not-a-number"}},
            {{"iat",18446744073709551615ULL}},{{"email_verified",false}},
            {{"app_metadata",{{"provider","email"}}}}}) {
            auto payload=claims(); payload.update(change);
            if(verifier.verify(token(f.first,payload),NOW,id)) return false;
        }
        return true;
    });
    check("algorithm confusion/remote header keys/missing kid rejected before fetch",[]{
        Fixture f; auto verifier=f.verifier(); SupabaseIdentity id;
        for (const auto& header : std::vector<json>{{{"alg","HS256"},{"kid","key-one"}},
            {{"alg","none"},{"kid","key-one"}},{{"alg","RS256"},{"kid","key-one"}},
            {{"alg","ES256"}},{{"alg","ES256"},{"kid","key-one"},{"jku","https://evil.invalid"}},
            {{"alg","ES256"},{"kid","key-one"},{"crit",json::array()}},
            {{"alg","ES256"},{"kid","key-one"},{"b64",false}}}) {
            if(verifier.verify(token(f.first,claims(),header),NOW,id)) return false;
        }
        return f.calls==0;
    });
    check("rotation refresh, removed-key expiry and outage fail closed",[]{
        Fixture f; auto verifier=f.verifier(); SupabaseIdentity id;
        if(!verifier.verify(token(f.first),NOW,id)) return false;
        f.document["keys"]=json::array({jwk(f.second,"key-two")});
        const auto newer=token(f.second,claims(),{{"alg","ES256"},{"kid","key-two"}});
        if(verifier.verify(newer,NOW,id)) return false; // Bounded unknown-kid cooldown.
        f.time+=std::chrono::seconds(31);
        if(!verifier.verify(newer,NOW,id) || verifier.verify(token(f.first),NOW,id)) return false;
        f.time+=std::chrono::minutes(6); f.online=false;
        return !verifier.verify(newer,NOW,id) && id.sub.empty();
    });
    check("unknown-kid flood and concurrent cold start fetch once",[]{
        Fixture f; auto verifier=f.verifier(); std::atomic<int> accepted{0};
        const auto jwt=token(f.first); std::vector<std::thread> threads;
        for(int i=0;i<12;i++) threads.emplace_back([&]{SupabaseIdentity id;if(verifier.verify(jwt,NOW,id)) ++accepted;});
        for(auto& thread:threads) thread.join();
        if(accepted!=12 || f.calls!=1) return false;
        for(int i=0;i<50;i++) {
            SupabaseIdentity id;
            if(verifier.verify(token(f.first,claims(),{{"alg","ES256"},{"kid",std::to_string(i)}}),NOW,id)) return false;
        }
        return f.calls==1;
    });
    check("invalid/off-curve/private/duplicate keys and malformed documents rejected",[]{
        for(int mode=0;mode<7;mode++) {
            Fixture f;
            auto& key=f.document["keys"][0];
            if(mode==0) key["crv"]="P-384";
            if(mode==1) key["x"]="bad";
            if(mode==2) key["d"]="private-material";
            if(mode==3) f.document["keys"].push_back(key);
            if(mode==4) key["key_ops"]=json::array({"sign"});
            if(mode==5) key["kid"]=42;
            if(mode==6) { key["x"]=std::string(43,'A'); key["y"]=std::string(43,'A'); }
            auto verifier=f.verifier(); SupabaseIdentity id;
            if(verifier.verify(token(f.first),NOW,id)) return false;
        }
        for(const auto& body:std::vector<std::string>{"not JSON","{}",std::string(65537,'x')}) {
            auto verifier=SupabaseVerifier::make_es256(ISSUER,"authenticated","google",
                [body](const std::string&,std::string& out){out=body;return true;});
            SupabaseIdentity id; const auto key=generate();
            if(verifier.verify(token(key),NOW,id)) return false;
        }
        return true;
    });
    check("HTTP failure never authenticates and is cooldown bounded",[]{
        Fixture f; f.online=false; auto verifier=f.verifier(); SupabaseIdentity id;
        return !verifier.verify(token(f.first),NOW,id) && !verifier.verify(token(f.first),NOW,id) && f.calls==1;
    });
    check("issuer cannot select attacker/local HTTP endpoint",[]{
        return !SupabaseVerifier::make_es256("http://127.0.0.1/auth/v1","authenticated","google").valid() &&
            !SupabaseVerifier::make_es256("https://evil.invalid/auth/v1","authenticated","google").valid();
    });
    check("environment pins ES256 even with a legacy secret present",[]{
        const std::vector<std::string> names={"SUPABASE_ISSUER","SUPABASE_AUDIENCE",
            "SUPABASE_PROVIDER","SUPABASE_JWT_ALGORITHM","SUPABASE_JWT_SECRET"};
        struct Restore {
            std::vector<std::pair<std::string,std::optional<std::string>>> values;
            ~Restore(){for(const auto& [name,value]:values){if(value) setenv(name.c_str(),value->c_str(),1);else unsetenv(name.c_str());}}
        } restore;
        for(const auto& name:names){const auto* value=std::getenv(name.c_str());
            restore.values.emplace_back(name,value?std::optional<std::string>(value):std::nullopt);}
        setenv("SUPABASE_ISSUER",ISSUER,1); setenv("SUPABASE_AUDIENCE","authenticated",1);
        setenv("SUPABASE_PROVIDER","google",1); setenv("SUPABASE_JWT_SECRET","legacy-secret",1);
        setenv("SUPABASE_JWT_ALGORITHM","ES256",1);
        std::string error; auto verifier=SupabaseVerifier::from_env(error); SupabaseIdentity id;
        if(!verifier.valid() || !error.empty() || verifier.verify(encoded("{\"alg\":\"HS256\",\"typ\":\"JWT\"}")+"."+encoded(claims().dump())+".AAAA",NOW,id)) return false;
        unsetenv("SUPABASE_JWT_SECRET");
        if(!SupabaseVerifier::from_env(error).valid()) return false;
        setenv("SUPABASE_JWT_ALGORITHM","HS256",1);
        if(SupabaseVerifier::from_env(error).valid() || error.empty()) return false;
        setenv("SUPABASE_JWT_ALGORITHM","RS256",1);
        return !SupabaseVerifier::from_env(error).valid() && !error.empty();
    });
    return failures;
}

int test_es256_service(chess::storage::Database& db) {
    // test_oauth's disposable fixture schema is already installed by its
    // legacy service tests; do not use a hosted application database here.
    Fixture f; auto verifier=f.verifier(); const auto jwt=token(f.first);
    const auto first=chess::auth::google_sign_in(db,verifier,jwt,NOW);
    const auto second=chess::auth::google_sign_in(db,verifier,jwt,NOW);
    const auto invalid=chess::auth::google_sign_in(db,verifier,token(f.second),NOW);
    const bool ok=first.status==chess::auth::GoogleSignInStatus::Ok &&
        first.created_new_account && second.status==chess::auth::GoogleSignInStatus::Ok &&
        !second.created_new_account && first.player_id==second.player_id &&
        invalid.status==chess::auth::GoogleSignInStatus::InvalidToken;
    std::cout<<"  [ES256] real account service: create/reuse/reject invalid signature: "
             <<(ok?"PASS":"FAIL")<<std::endl;
    return ok?0:1;
}
