/**
 * test_sealed_envelope.cpp — Phase 7.4.
 *
 * Four layers, because the sealed envelope is four things stacked:
 *
 *   1. base64 — boring, and therefore the most likely place for a silent bug.
 *      Tested strictly: a decoder that accepts junk turns a detectable
 *      protocol error into an undetectable one.
 *   2. The envelope construction — round-trip, and rejection of tampering in
 *      every field. Note that only key_id is explicit AAD; the tests prove the
 *      other fields are bound through key derivation by mutating them and
 *      requiring failure.
 *   3. The key store — single use, expiry, and the per-IP cap. This is where
 *      replay defence actually lives.
 *   4. The registry — including the branch that matters most: a type that must
 *      be sealed, arriving unsealed, is refused rather than processed.
 */

#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "crypto/base64.h"
#include "crypto/sealed_envelope.h"
#include "crypto/sealed_key_store.h"
#include "crypto/sealed_registry.h"

using namespace chess::crypto;
using json = nlohmann::json;

// ============================================================
// Harness
// ============================================================

static int g_passed = 0;
static int g_failed = 0;

static void run_test(const std::string& name, std::function<bool()> fn) {
    std::cout << "  [TEST] " << name << "... ";
    if (fn()) { std::cout << "PASS" << std::endl; g_passed++; }
    else      { std::cout << "FAIL" << std::endl; g_failed++; }
}

static std::vector<uint8_t> bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

static std::string text_of(const SecureBuffer& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

/// A server identity, generated once — ML-DSA keygen is not free.
static const MlDsa65KeyPair& identity() {
    static MlDsa65KeyPair k = MlDsa65KeyPair::generate();
    return k;
}

// ============================================================
// base64
// ============================================================

static void test_base64() {
    std::cout << "\n--- base64 (RFC 4648) ---" << std::endl;

    run_test("RFC 4648 §10 test vectors", [] {
        const std::pair<const char*, const char*> v[] = {
            {"",       ""},        {"f",      "Zg=="},
            {"fo",     "Zm8="},    {"foo",    "Zm9v"},
            {"foob",   "Zm9vYg=="},{"fooba",  "Zm9vYmE="},
            {"foobar", "Zm9vYmFy"},
        };
        for (const auto& p : v) {
            auto in = bytes(p.first);
            if (encode_base64(in) != p.second) return false;
            std::vector<uint8_t> back;
            if (!decode_base64(p.second, back) || back != in) return false;
        }
        return true;
    });

    run_test("Round-trips every length 0..300", [] {
        for (size_t n = 0; n <= 300; ++n) {
            std::vector<uint8_t> in(n);
            for (size_t i = 0; i < n; ++i) in[i] = static_cast<uint8_t>(i * 7 + n);
            std::vector<uint8_t> back;
            if (!decode_base64(encode_base64(in), back) || back != in) return false;
        }
        return true;
    });

    run_test("Rejects characters outside the alphabet", [] {
        std::vector<uint8_t> out;
        // Whitespace and newlines included: every field here has a known exact
        // length, so leniency could only hide a protocol error.
        return !decode_base64("Zm9v YmFy", out) &&
               !decode_base64("Zm9v\nYmFy", out) &&
               !decode_base64("Zm9vYmF-", out) &&
               !decode_base64("Zm9vYmF_", out);
    });

    run_test("Rejects wrong-length and misplaced padding", [] {
        std::vector<uint8_t> out;
        return !decode_base64("Zm9vYmF", out) &&    // not a multiple of 4
               !decode_base64("Zg=a", out) &&       // '=' inside the body
               !decode_base64("Z===", out);
    });

    run_test("Rejects non-zero trailing bits (non-canonical encoding)", [] {
        // "Zh==" and "Zg==" would both decode to 'f' under a lenient decoder,
        // so a signature or key id could be re-encoded into a different string.
        std::vector<uint8_t> out;
        return decode_base64("Zg==", out) && !decode_base64("Zh==", out);
    });

    run_test("expected_len is enforced", [] {
        std::vector<uint8_t> out;
        return decode_base64("Zm9v", out, 3) &&
               !decode_base64("Zm9v", out, 4) &&
               out.empty();
    });
}

// ============================================================
// The envelope
// ============================================================

static bool mint_pair(SealKeyMaterial& material, SealKeyOffer& offer) {
    return SealedEnvelopeService::mint(identity(), 120, material, offer);
}

static void test_envelope() {
    std::cout << "\n--- Sealed envelope construction ---" << std::endl;

    run_test("mint() produces correctly sized, signed offers", [] {
        SealKeyMaterial m; SealKeyOffer o;
        return mint_pair(m, o) && m.valid() &&
               o.key_id.size()    == seal::KEY_ID_SIZE &&
               o.kem_ek.size()    == mlkem768::PUBLIC_KEY_SIZE &&
               o.x25519_pk.size() == x25519::PUBLIC_KEY_SIZE &&
               o.signature.size() == mldsa65::SIGNATURE_SIZE &&
               o.expires_in       == 120;
    });

    run_test("Two mints produce different key ids and keys", [] {
        SealKeyMaterial m1, m2; SealKeyOffer o1, o2;
        return mint_pair(m1, o1) && mint_pair(m2, o2) &&
               o1.key_id != o2.key_id && o1.kem_ek != o2.kem_ek;
    });

    run_test("Offer verifies against the real identity key", [] {
        SealKeyMaterial m; SealKeyOffer o;
        if (!mint_pair(m, o)) return false;
        return SealedEnvelopeService::verify_offer(identity().public_key(), o);
    });

    run_test("Offer FAILS against an impostor identity (the pin check)", [] {
        SealKeyMaterial m; SealKeyOffer o;
        if (!mint_pair(m, o)) return false;
        auto impostor = MlDsa65KeyPair::generate().public_key();
        // This is the whole reason the offer is signed: a man in the middle
        // substituting their own ML-KEM key must not be believed.
        return !SealedEnvelopeService::verify_offer(impostor, o);
    });

    run_test("Every signed field is actually covered by the signature", [] {
        auto pk = identity().public_key();
        SealKeyMaterial m; SealKeyOffer base;
        if (!mint_pair(m, base)) return false;

        SealKeyOffer a = base; a.kem_ek[0]    ^= 0x01;
        SealKeyOffer b = base; b.x25519_pk[0] ^= 0x01;
        SealKeyOffer c = base; c.key_id[0]    ^= 0x01;
        SealKeyOffer d = base; d.expires_in    = 86400;   // a much longer window
        for (const SealKeyOffer* o : {&a, &b, &c, &d}) {
            if (SealedEnvelopeService::verify_offer(pk, *o)) return false;
        }
        return SealedEnvelopeService::verify_offer(pk, base);
    });

    run_test("Round trip: seal then open recovers the payload", [] {
        SealKeyMaterial m; SealKeyOffer o;
        if (!mint_pair(m, o)) return false;
        const std::string payload = R"({"username":"adi","password":"hunter2"})";

        SealedEnvelope env;
        if (!SealedEnvelopeService::seal(o, reinterpret_cast<const uint8_t*>(payload.data()),
                                         payload.size(), env)) return false;
        if (!env.well_formed()) return false;
        if (env.ct.size() != payload.size()) return false;   // CTR preserves length

        SecureBuffer out;
        return SealedEnvelopeService::open(m, env, out) && text_of(out) == payload;
    });

    run_test("Ciphertext does not contain the plaintext", [] {
        SealKeyMaterial m; SealKeyOffer o;
        if (!mint_pair(m, o)) return false;
        const std::string payload(200, 'A');
        SealedEnvelope env;
        if (!SealedEnvelopeService::seal(o, reinterpret_cast<const uint8_t*>(payload.data()),
                                         payload.size(), env)) return false;
        for (uint8_t b : env.ct) if (b == 'A') { /* a byte may coincide */ }
        return std::memcmp(env.ct.data(), payload.data(), payload.size()) != 0;
    });

    run_test("Empty payload seals and opens", [] {
        SealKeyMaterial m; SealKeyOffer o;
        if (!mint_pair(m, o)) return false;
        SealedEnvelope env;
        if (!SealedEnvelopeService::seal(o, nullptr, 0, env)) return false;
        SecureBuffer out;
        return SealedEnvelopeService::open(m, env, out) && out.empty() &&
               env.tag.size() == seal::TAG_SIZE;
    });

    run_test("Tampering with ANY field is detected", [] {
        SealKeyMaterial m; SealKeyOffer o;
        if (!mint_pair(m, o)) return false;
        const std::string payload = R"({"password":"hunter2"})";
        SealedEnvelope base;
        if (!SealedEnvelopeService::seal(o, reinterpret_cast<const uint8_t*>(payload.data()),
                                         payload.size(), base)) return false;

        // ct and tag are covered directly by the MAC; iv likewise. kem_ct and
        // x25519_pk are NOT in the AAD — they are bound because changing them
        // changes the derived MAC key. This test is what proves that claim.
        SealedEnvelope a = base; a.ct[0]        ^= 0x01;
        SealedEnvelope b = base; b.tag[0]       ^= 0x01;
        SealedEnvelope c = base; c.iv[0]        ^= 0x01;
        SealedEnvelope d = base; d.kem_ct[0]    ^= 0x01;
        SealedEnvelope e = base; e.x25519_pk[0] ^= 0x01;
        SealedEnvelope f = base; f.key_id[0]    ^= 0x01;

        for (const SealedEnvelope* env : {&a, &b, &c, &d, &e, &f}) {
            SecureBuffer out;
            if (SealedEnvelopeService::open(m, *env, out)) return false;
            if (!out.empty()) return false;   // nothing leaks on failure
        }
        SecureBuffer ok;
        return SealedEnvelopeService::open(m, base, ok);
    });

    run_test("Truncated or extended ciphertext is rejected", [] {
        SealKeyMaterial m; SealKeyOffer o;
        if (!mint_pair(m, o)) return false;
        const std::string payload = "0123456789";
        SealedEnvelope base;
        if (!SealedEnvelopeService::seal(o, reinterpret_cast<const uint8_t*>(payload.data()),
                                         payload.size(), base)) return false;
        SealedEnvelope shorter = base; shorter.ct.pop_back();
        SealedEnvelope longer  = base; longer.ct.push_back(0x00);
        SecureBuffer out;
        return !SealedEnvelopeService::open(m, shorter, out) &&
               !SealedEnvelopeService::open(m, longer, out);
    });

    run_test("A different key pair cannot open the envelope", [] {
        SealKeyMaterial m1, m2; SealKeyOffer o1, o2;
        if (!mint_pair(m1, o1) || !mint_pair(m2, o2)) return false;
        const std::string payload = "secret";
        SealedEnvelope env;
        if (!SealedEnvelopeService::seal(o1, reinterpret_cast<const uint8_t*>(payload.data()),
                                         payload.size(), env)) return false;
        // Give m2 the matching key id so the structural check passes and the
        // test really exercises the cryptography, not the bookkeeping.
        m2.key_id = env.key_id;
        SecureBuffer out;
        return !SealedEnvelopeService::open(m2, env, out);
    });

    run_test("Structurally malformed envelopes are rejected", [] {
        SealKeyMaterial m; SealKeyOffer o;
        if (!mint_pair(m, o)) return false;
        SealedEnvelope env;
        if (!SealedEnvelopeService::seal(o, nullptr, 0, env)) return false;

        SealedEnvelope a = env; a.iv.pop_back();
        SealedEnvelope b = env; b.tag.pop_back();
        SealedEnvelope c = env; c.kem_ct.pop_back();
        SealedEnvelope d = env; d.x25519_pk.clear();
        SecureBuffer out;
        for (const SealedEnvelope* e : {&a, &b, &c, &d}) {
            if (e->well_formed()) return false;
            if (SealedEnvelopeService::open(m, *e, out)) return false;
        }
        return true;
    });

    run_test("Large payload (64 KB) round-trips", [] {
        SealKeyMaterial m; SealKeyOffer o;
        if (!mint_pair(m, o)) return false;
        std::string payload(65536, '\0');
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(i & 0xFF);
        }
        SealedEnvelope env;
        if (!SealedEnvelopeService::seal(o, reinterpret_cast<const uint8_t*>(payload.data()),
                                         payload.size(), env)) return false;
        SecureBuffer out;
        return SealedEnvelopeService::open(m, env, out) && text_of(out) == payload;
    });

    run_test("signing_input is domain-separated by the context string", [] {
        SealKeyMaterial m; SealKeyOffer o;
        if (!mint_pair(m, o)) return false;
        auto input = SealedEnvelopeService::signing_input(o.key_id, o.kem_ek,
                                                          o.x25519_pk, o.expires_in);
        const std::string prefix(SEAL_CONTEXT);
        return input.size() == prefix.size() + o.kem_ek.size() +
                               o.x25519_pk.size() + o.key_id.size() + 4 &&
               std::memcmp(input.data(), prefix.data(), prefix.size()) == 0;
    });
}

// ============================================================
// The one-time key store
// ============================================================

static void test_key_store() {
    std::cout << "\n--- One-time key store ---" << std::endl;

    run_test("Issue then consume succeeds and the store shrinks", [] {
        SealedKeyStore store;
        SealKeyOffer offer;
        if (!store.issue(identity(), "1.2.3.4", offer)) return false;
        if (store.size() != 1) return false;
        SealKeyMaterial m;
        return store.consume(offer.key_id, m) && m.valid() && store.size() == 0;
    });

    run_test("A key is single use — replay fails", [] {
        SealedKeyStore store;
        SealKeyOffer offer;
        store.issue(identity(), "1.2.3.4", offer);
        SealKeyMaterial a, b;
        // The captured envelope can never be opened again: the only key that
        // could have opened it no longer exists.
        return store.consume(offer.key_id, a) && !store.consume(offer.key_id, b);
    });

    run_test("A failed open still burns the key", [] {
        SealedKeyStore store;
        SealKeyOffer offer;
        store.issue(identity(), "1.2.3.4", offer);

        SealedEnvelope env;
        const std::string payload = "x";
        SealedEnvelopeService::seal(offer, reinterpret_cast<const uint8_t*>(payload.data()),
                                    payload.size(), env);
        env.tag[0] ^= 0x01;                       // forgery

        SealKeyMaterial m;
        if (!store.consume(env.key_id, m)) return false;
        SecureBuffer out;
        if (SealedEnvelopeService::open(m, env, out)) return false;
        // Second attempt with the CORRECT tag must now also fail: no grinding.
        SealKeyMaterial again;
        return !store.consume(offer.key_id, again);
    });

    run_test("Unknown key id fails", [] {
        SealedKeyStore store;
        std::vector<uint8_t> fake(seal::KEY_ID_SIZE, 0xAB);
        SealKeyMaterial m;
        return !store.consume(fake, m) &&
               !store.consume(std::vector<uint8_t>(4, 0), m);   // wrong length
    });

    run_test("Expired keys cannot be consumed", [] {
        SealedKeyStore store(0);   // TTL 0 — expired the moment it is stored
        SealKeyOffer offer;
        if (!store.issue(identity(), "1.2.3.4", offer)) return false;
        SealKeyMaterial m;
        return !store.consume(offer.key_id, m);
    });

    run_test("Expired keys are swept and release their IP slot", [] {
        SealedKeyStore store(0, 2);   // TTL 0
        SealKeyOffer o;
        if (!store.issue(identity(), "9.9.9.9", o)) return false;
        if (store.size() != 1 || store.outstanding_for("9.9.9.9") != 1) return false;
        const size_t removed = store.sweep_expired();
        // The per-IP counter must be released too, or an expired key would
        // permanently consume a slot and lock the client out of logging in.
        return removed == 1 && store.size() == 0 &&
               store.outstanding_for("9.9.9.9") == 0;
    });

    run_test("issue() sweeps first, so a dead key never blocks the cap", [] {
        SealedKeyStore store(0, 1);   // TTL 0, one outstanding key allowed
        SealKeyOffer o;
        // Without the sweep at the top of issue(), the first (already expired)
        // key would occupy the only slot forever and the second call would fail.
        return store.issue(identity(), "8.8.8.8", o) &&
               store.issue(identity(), "8.8.8.8", o) &&
               store.size() == 1;
    });

    run_test("Per-IP cap refuses the (N+1)th outstanding key", [] {
        SealedKeyStore store(120, 3);
        SealKeyOffer o;
        for (int i = 0; i < 3; ++i) {
            if (!store.issue(identity(), "5.5.5.5", o)) return false;
        }
        // Minting is ML-KEM + X25519 keygen + an ML-DSA signature, so an
        // uncapped seal_request is a CPU amplifier for an unauthenticated peer.
        return !store.issue(identity(), "5.5.5.5", o) &&
               store.outstanding_for("5.5.5.5") == 3;
    });

    run_test("The cap is per IP, not global", [] {
        SealedKeyStore store(120, 2);
        SealKeyOffer o;
        store.issue(identity(), "a", o);
        store.issue(identity(), "a", o);
        return !store.issue(identity(), "a", o) && store.issue(identity(), "b", o);
    });

    run_test("Consuming frees a slot for the same IP", [] {
        SealedKeyStore store(120, 1);
        SealKeyOffer first, second;
        if (!store.issue(identity(), "7.7.7.7", first)) return false;
        if (store.issue(identity(), "7.7.7.7", second)) return false;
        SealKeyMaterial m;
        store.consume(first.key_id, m);
        return store.outstanding_for("7.7.7.7") == 0 &&
               store.issue(identity(), "7.7.7.7", second);
    });

    run_test("Global cap is enforced", [] {
        SealedKeyStore store(120, 100, 3);
        SealKeyOffer o;
        int issued = 0;
        for (int i = 0; i < 5; ++i) {
            if (store.issue(identity(), "ip" + std::to_string(i), o)) issued++;
        }
        return issued == 3 && store.size() == 3;
    });

    run_test("Concurrent issue/consume stays consistent", [] {
        SealedKeyStore store(120, 1000, 10000);
        std::vector<std::thread> threads;
        std::atomic<int> opened{0};
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&store, &opened, t] {
                for (int i = 0; i < 10; ++i) {
                    SealKeyOffer offer;
                    if (!store.issue(identity(), "ip" + std::to_string(t), offer)) continue;
                    SealedEnvelope env;
                    const std::string p = "payload";
                    if (!SealedEnvelopeService::seal(
                            offer, reinterpret_cast<const uint8_t*>(p.data()),
                            p.size(), env)) continue;
                    SealKeyMaterial m;
                    if (!store.consume(env.key_id, m)) continue;
                    SecureBuffer out;
                    if (SealedEnvelopeService::open(m, env, out) && text_of(out) == p) {
                        opened++;
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
        return opened == 40 && store.size() == 0;
    });
}

// ============================================================
// The registry — the opt-in gate
// ============================================================

/// Build the JSON a real client would send for a sealed message type.
static std::string sealed_message(const std::string& type,
                                  const SealKeyOffer& offer,
                                  const std::string& payload) {
    SealedEnvelope env;
    if (!SealedEnvelopeService::seal(offer,
                                     reinterpret_cast<const uint8_t*>(payload.data()),
                                     payload.size(), env)) {
        return std::string();
    }
    return "{\"type\":\"" + type + "\",\"sealed\":{"
           "\"key_id\":\""    + encode_base64(env.key_id)    + "\","
           "\"kem_ct\":\""    + encode_base64(env.kem_ct)    + "\","
           "\"x25519_pk\":\"" + encode_base64(env.x25519_pk) + "\","
           "\"iv\":\""        + encode_base64(env.iv)        + "\","
           "\"ct\":\""        + encode_base64(env.ct)        + "\","
           "\"tag\":\""       + encode_base64(env.tag)       + "\"}}";
}

static void test_registry() {
    std::cout << "\n--- Sealed registry (opt-in gate) ---" << std::endl;

    run_test("Unregistered types pass straight through", [] {
        SealedKeyStore store;
        SealedRegistry reg(identity(), store);
        reg.require_sealed("login");
        std::string out;
        const std::string msg = R"({"type":"make_move","from":12,"to":28})";
        return reg.inspect("make_move", msg, out) == SealedRegistry::Outcome::NotSealed &&
               out.empty();
    });

    run_test("seal_request produces a valid, verifiable seal_key reply", [] {
        SealedKeyStore store;
        SealedRegistry reg(identity(), store);
        const std::string reply = reg.handle_seal_request("1.2.3.4");
        if (reply.empty()) return false;
        // "type" must be the first key: websocket.cpp finds it by scanning.
        if (reply.rfind("{\"type\":\"seal_key\"", 0) != 0) return false;

        json j = json::parse(reply, nullptr, false);
        if (j.is_discarded()) return false;

        SealKeyOffer offer;
        if (!decode_base64(j["key_id"], offer.key_id, seal::KEY_ID_SIZE)) return false;
        if (!decode_base64(j["kem_ek"], offer.kem_ek, mlkem768::PUBLIC_KEY_SIZE)) return false;
        if (!decode_base64(j["x25519_pk"], offer.x25519_pk, x25519::PUBLIC_KEY_SIZE)) return false;
        if (!decode_base64(j["signature"], offer.signature, mldsa65::SIGNATURE_SIZE)) return false;
        offer.expires_in = j["expires_in"];
        return SealedEnvelopeService::verify_offer(identity().public_key(), offer);
    });

    run_test("seal_request is refused once the IP cap is reached", [] {
        SealedKeyStore store(120, 2);
        SealedRegistry reg(identity(), store);
        return !reg.handle_seal_request("1.1.1.1").empty() &&
               !reg.handle_seal_request("1.1.1.1").empty() &&
                reg.handle_seal_request("1.1.1.1").empty();
    });

    run_test("A sealed login is opened and rewritten with type first", [] {
        SealedKeyStore store;
        SealedRegistry reg(identity(), store);
        reg.require_sealed("login");

        json offer_json = json::parse(reg.handle_seal_request("1.2.3.4"));
        SealKeyOffer offer;
        decode_base64(offer_json["key_id"], offer.key_id, seal::KEY_ID_SIZE);
        decode_base64(offer_json["kem_ek"], offer.kem_ek, mlkem768::PUBLIC_KEY_SIZE);
        decode_base64(offer_json["x25519_pk"], offer.x25519_pk, x25519::PUBLIC_KEY_SIZE);

        const std::string msg =
            sealed_message("login", offer, R"({"username":"adi","password":"hunter2"})");
        std::string out;
        if (reg.inspect("login", msg, out) != SealedRegistry::Outcome::Opened) return false;

        if (out.rfind("{\"type\":\"login\"", 0) != 0) return false;
        json body = json::parse(out, nullptr, false);
        return !body.is_discarded() && body["username"] == "adi" &&
               body["password"] == "hunter2" && body["type"] == "login";
    });

    run_test("A required type arriving UNSEALED is rejected", [] {
        SealedKeyStore store;
        SealedRegistry reg(identity(), store);
        reg.require_sealed("login");
        std::string out;
        // The downgrade attack. If this were accepted, the envelope would be
        // optional, which is the same as not existing.
        const std::string clear = R"({"type":"login","username":"adi","password":"hunter2"})";
        return reg.inspect("login", clear, out) == SealedRegistry::Outcome::Rejected &&
               out.empty();
    });

    run_test("Replaying a sealed message fails the second time", [] {
        SealedKeyStore store;
        SealedRegistry reg(identity(), store);
        reg.require_sealed("login");

        json oj = json::parse(reg.handle_seal_request("1.2.3.4"));
        SealKeyOffer offer;
        decode_base64(oj["key_id"], offer.key_id, seal::KEY_ID_SIZE);
        decode_base64(oj["kem_ek"], offer.kem_ek, mlkem768::PUBLIC_KEY_SIZE);
        decode_base64(oj["x25519_pk"], offer.x25519_pk, x25519::PUBLIC_KEY_SIZE);

        const std::string msg = sealed_message("login", offer, R"({"username":"adi"})");
        std::string a, b;
        return reg.inspect("login", msg, a) == SealedRegistry::Outcome::Opened &&
               reg.inspect("login", msg, b) == SealedRegistry::Outcome::Rejected;
    });

    run_test("Malformed envelopes are rejected", [] {
        SealedKeyStore store;
        SealedRegistry reg(identity(), store);
        reg.require_sealed("login");
        std::string out;
        const char* bad[] = {
            R"({"type":"login","sealed":"not-an-object"})",
            R"({"type":"login","sealed":{}})",
            R"({"type":"login","sealed":{"key_id":"AAAA","kem_ct":"AAAA","x25519_pk":"AAAA","iv":"AAAA","ct":"","tag":"AAAA"}})",
            R"(not json at all)",
            R"(["type","login"])",
        };
        for (const char* m : bad) {
            if (reg.inspect("login", m, out) != SealedRegistry::Outcome::Rejected) return false;
        }
        return true;
    });

    run_test("An inner 'type' cannot override the routed type", [] {
        SealedKeyStore store;
        SealedRegistry reg(identity(), store);
        reg.require_sealed("login");

        json oj = json::parse(reg.handle_seal_request("1.2.3.4"));
        SealKeyOffer offer;
        decode_base64(oj["key_id"], offer.key_id, seal::KEY_ID_SIZE);
        decode_base64(oj["kem_ek"], offer.kem_ek, mlkem768::PUBLIC_KEY_SIZE);
        decode_base64(oj["x25519_pk"], offer.x25519_pk, x25519::PUBLIC_KEY_SIZE);

        // Routing was already decided from the outer frame; if the inside could
        // override it, one registered type could be dispatched as another.
        const std::string msg =
            sealed_message("login", offer, R"({"type":"resign","username":"adi"})");
        std::string out;
        if (reg.inspect("login", msg, out) != SealedRegistry::Outcome::Opened) return false;
        json body = json::parse(out, nullptr, false);
        return body["type"] == "login" &&
               out.find("resign") == std::string::npos;
    });

    run_test("A non-object payload is rejected", [] {
        SealedKeyStore store;
        SealedRegistry reg(identity(), store);
        reg.require_sealed("login");

        json oj = json::parse(reg.handle_seal_request("1.2.3.4"));
        SealKeyOffer offer;
        decode_base64(oj["key_id"], offer.key_id, seal::KEY_ID_SIZE);
        decode_base64(oj["kem_ek"], offer.kem_ek, mlkem768::PUBLIC_KEY_SIZE);
        decode_base64(oj["x25519_pk"], offer.x25519_pk, x25519::PUBLIC_KEY_SIZE);

        std::string out;
        return reg.inspect("login", sealed_message("login", offer, "\"just a string\""), out)
                   == SealedRegistry::Outcome::Rejected;
    });

    run_test("An empty payload object yields a bare typed message", [] {
        SealedKeyStore store;
        SealedRegistry reg(identity(), store);
        reg.require_sealed("ping_sealed");

        json oj = json::parse(reg.handle_seal_request("1.2.3.4"));
        SealKeyOffer offer;
        decode_base64(oj["key_id"], offer.key_id, seal::KEY_ID_SIZE);
        decode_base64(oj["kem_ek"], offer.kem_ek, mlkem768::PUBLIC_KEY_SIZE);
        decode_base64(oj["x25519_pk"], offer.x25519_pk, x25519::PUBLIC_KEY_SIZE);

        std::string out;
        return reg.inspect("ping_sealed", sealed_message("ping_sealed", offer, "{}"), out)
                   == SealedRegistry::Outcome::Opened &&
               out == R"({"type":"ping_sealed"})";
    });

    run_test("Payload characters that would break JSON are re-escaped", [] {
        SealedKeyStore store;
        SealedRegistry reg(identity(), store);
        reg.require_sealed("login");

        json oj = json::parse(reg.handle_seal_request("1.2.3.4"));
        SealKeyOffer offer;
        decode_base64(oj["key_id"], offer.key_id, seal::KEY_ID_SIZE);
        decode_base64(oj["kem_ek"], offer.kem_ek, mlkem768::PUBLIC_KEY_SIZE);
        decode_base64(oj["x25519_pk"], offer.x25519_pk, x25519::PUBLIC_KEY_SIZE);

        // The plaintext is attacker-controlled up to the moment the tag
        // verified, so the rewritten message is re-serialised, never spliced.
        json p;
        p["password"] = "a\"b\\c\nd\te";
        const std::string msg = sealed_message("login", offer, p.dump());
        std::string out;
        if (reg.inspect("login", msg, out) != SealedRegistry::Outcome::Opened) return false;
        json body = json::parse(out, nullptr, false);
        return !body.is_discarded() && body["password"] == "a\"b\\c\nd\te";
    });

    run_test("Registering a type twice is idempotent", [] {
        SealedKeyStore store;
        SealedRegistry reg(identity(), store);
        reg.require_sealed("login");
        reg.require_sealed("login");
        return reg.is_required("login") && !reg.is_required("register");
    });
}

// ============================================================
// main
// ============================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Sealed envelope - Phase 7.4" << std::endl;
    std::cout << "========================================" << std::endl;

    if (!identity().valid()) {
        std::cerr << "FATAL: could not generate a server identity key" << std::endl;
        return 1;
    }

    test_base64();
    test_envelope();
    test_key_store();
    test_registry();

    std::cout << "\n========================================" << std::endl;
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed"
              << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_failed > 0) ? 1 : 0;
}
