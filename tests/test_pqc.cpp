/**
 * test_pqc.cpp — Phase 7.3 verification for the ML-KEM-768 / ML-DSA-65 /
 * X25519 wrappers.
 *
 * These are wrappers, not implementations, so the tests target the things a
 * wrapper can actually get wrong:
 *
 *   1. Sizes. Every constant is checked against FIPS 203/204 rather than
 *      against whatever OpenSSL happened to return, so a parameter-set mix-up
 *      (ML-KEM-512 vs -768) fails loudly instead of silently halving security.
 *   2. Cross-object round-trips — encapsulating to an EXPORTED public key and
 *      decapsulating with the original object. A test that reuses one handle
 *      for both halves proves nothing about the raw import/export path, which
 *      is the path the wire protocol will actually use.
 *   3. Rejection behaviour, including the counter-intuitive one: ML-KEM
 *      implicit rejection means a tampered ciphertext DECAPSULATES FINE and
 *      yields a different secret. Asserting "tampering fails" would encode a
 *      false belief about the primitive into the test suite.
 *   4. Hybrid composition — that the two shared secrets are independent and
 *      that HKDF over their concatenation binds both.
 */

#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "crypto/hkdf.h"
#include "crypto/kem.h"
#include "crypto/random.h"
#include "crypto/sha512.h"
#include "crypto/signature.h"
#include "crypto/x25519.h"

using namespace chess::crypto;

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

static bool same(const SecureBuffer& a, const SecureBuffer& b) {
    return a.size() == b.size() && a.size() > 0 &&
           std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// ============================================================
// ML-KEM-768
// ============================================================

static void test_ml_kem() {
    std::cout << "\n--- ML-KEM-768 (FIPS 203) ---" << std::endl;

    run_test("Key generation succeeds", [] {
        return MlKem768KeyPair::generate().valid();
    });

    run_test("Public key is exactly 1184 bytes (FIPS 203 ek)", [] {
        auto kp = MlKem768KeyPair::generate();
        return kp.public_key().size() == mlkem768::PUBLIC_KEY_SIZE;
    });

    run_test("Two key pairs differ", [] {
        auto a = MlKem768KeyPair::generate().public_key();
        auto b = MlKem768KeyPair::generate().public_key();
        return !a.empty() && a != b;
    });

    run_test("Ciphertext is 1088 bytes, shared secret 32", [] {
        auto kp = MlKem768KeyPair::generate();
        auto pk = kp.public_key();
        auto e  = MlKem768::encapsulate(pk.data(), pk.size());
        return e.ok &&
               e.ciphertext.size()    == mlkem768::CIPHERTEXT_SIZE &&
               e.shared_secret.size() == mlkem768::SHARED_SECRET_SIZE;
    });

    run_test("Round trip through exported public key agrees", [] {
        auto kp = MlKem768KeyPair::generate();
        auto pk = kp.public_key();
        auto e  = MlKem768::encapsulate(pk.data(), pk.size());
        if (!e.ok) return false;
        SecureBuffer ss = kp.decapsulate(e.ciphertext.data(), e.ciphertext.size());
        return same(e.shared_secret, ss);
    });

    run_test("100 round trips all agree", [] {
        for (int i = 0; i < 100; ++i) {
            auto kp = MlKem768KeyPair::generate();
            auto pk = kp.public_key();
            auto e  = MlKem768::encapsulate(pk.data(), pk.size());
            if (!e.ok) return false;
            if (!same(e.shared_secret,
                      kp.decapsulate(e.ciphertext.data(), e.ciphertext.size()))) {
                return false;
            }
        }
        return true;
    });

    run_test("Wrong key pair yields a different secret", [] {
        auto alice   = MlKem768KeyPair::generate();
        auto mallory = MlKem768KeyPair::generate();
        auto pk = alice.public_key();
        auto e  = MlKem768::encapsulate(pk.data(), pk.size());
        SecureBuffer wrong = mallory.decapsulate(e.ciphertext.data(),
                                                 e.ciphertext.size());
        // Implicit rejection: this SUCCEEDS but must not match.
        return wrong.size() == mlkem768::SHARED_SECRET_SIZE &&
               !same(e.shared_secret, wrong);
    });

    run_test("Tampered ciphertext: implicit rejection, not an error", [] {
        auto kp = MlKem768KeyPair::generate();
        auto pk = kp.public_key();
        auto e  = MlKem768::encapsulate(pk.data(), pk.size());
        auto ct = e.ciphertext;
        ct[42] ^= 0x01;
        SecureBuffer ss = kp.decapsulate(ct.data(), ct.size());
        // FIPS 203 section 7.3: decapsulation succeeds and returns a
        // pseudorandom secret. Detection is the AEAD's job, never the KEM's.
        return ss.size() == mlkem768::SHARED_SECRET_SIZE &&
               !same(e.shared_secret, ss);
    });

    run_test("Implicit rejection is deterministic for a given key+ciphertext", [] {
        auto kp = MlKem768KeyPair::generate();
        auto pk = kp.public_key();
        auto e  = MlKem768::encapsulate(pk.data(), pk.size());
        auto ct = e.ciphertext;
        ct[7] ^= 0xFF;
        return same(kp.decapsulate(ct.data(), ct.size()),
                    kp.decapsulate(ct.data(), ct.size()));
    });

    run_test("Wrong-length ciphertext is rejected structurally", [] {
        auto kp = MlKem768KeyPair::generate();
        auto pk = kp.public_key();
        auto e  = MlKem768::encapsulate(pk.data(), pk.size());
        return kp.decapsulate(e.ciphertext.data(), e.ciphertext.size() - 1).empty() &&
               kp.decapsulate(e.ciphertext.data(), 0).empty() &&
               kp.decapsulate(nullptr, mlkem768::CIPHERTEXT_SIZE).empty();
    });

    run_test("Encapsulation to a malformed public key fails", [] {
        std::vector<uint8_t> junk(mlkem768::PUBLIC_KEY_SIZE, 0xAB);
        auto bad_len = MlKem768::encapsulate(junk.data(), junk.size() - 1);
        auto null_pk = MlKem768::encapsulate(nullptr, mlkem768::PUBLIC_KEY_SIZE);
        // A wrong-length or absent key must never produce a usable ciphertext.
        return !bad_len.ok && !null_pk.ok;
    });

    run_test("Default-constructed key pair is inert", [] {
        MlKem768KeyPair kp;
        std::vector<uint8_t> ct(mlkem768::CIPHERTEXT_SIZE, 0);
        return !kp.valid() && kp.public_key().empty() &&
               kp.decapsulate(ct.data(), ct.size()).empty();
    });

    run_test("Shared secret is not all zeroes", [] {
        auto kp = MlKem768KeyPair::generate();
        auto pk = kp.public_key();
        auto e  = MlKem768::encapsulate(pk.data(), pk.size());
        for (size_t i = 0; i < e.shared_secret.size(); ++i) {
            if (e.shared_secret[i] != 0) return true;
        }
        return false;
    });

    run_test("Key pair survives a move", [] {
        auto kp = MlKem768KeyPair::generate();
        auto pk = kp.public_key();
        auto e  = MlKem768::encapsulate(pk.data(), pk.size());
        MlKem768KeyPair moved = std::move(kp);
        return moved.valid() &&
               same(e.shared_secret,
                    moved.decapsulate(e.ciphertext.data(), e.ciphertext.size()));
    });
}

// ============================================================
// ML-DSA-65
// ============================================================

static void test_ml_dsa() {
    std::cout << "\n--- ML-DSA-65 (FIPS 204) ---" << std::endl;

    static const std::string msg = "e2e4 is the first move";
    const uint8_t* m = reinterpret_cast<const uint8_t*>(msg.data());

    run_test("Key generation succeeds", [] {
        return MlDsa65KeyPair::generate().valid();
    });

    run_test("Public key 1952, private key 4032 (FIPS 204)", [] {
        auto kp = MlDsa65KeyPair::generate();
        return kp.public_key().size()  == mldsa65::PUBLIC_KEY_SIZE &&
               kp.private_key().size() == mldsa65::PRIVATE_KEY_SIZE;
    });

    run_test("Signature is 3309 bytes", [m] {
        auto kp = MlDsa65KeyPair::generate();
        return kp.sign(m, msg.size()).size() == mldsa65::SIGNATURE_SIZE;
    });

    run_test("Signature verifies against the exported public key", [m] {
        auto kp  = MlDsa65KeyPair::generate();
        auto pk  = kp.public_key();
        auto sig = kp.sign(m, msg.size());
        return MlDsa65::verify(pk.data(), pk.size(), m, msg.size(),
                               sig.data(), sig.size());
    });

    run_test("Signature fails against a different public key", [m] {
        auto kp    = MlDsa65KeyPair::generate();
        auto other = MlDsa65KeyPair::generate().public_key();
        auto sig   = kp.sign(m, msg.size());
        return !MlDsa65::verify(other.data(), other.size(), m, msg.size(),
                                sig.data(), sig.size());
    });

    run_test("Flipping any single signature byte breaks verification", [m] {
        auto kp  = MlDsa65KeyPair::generate();
        auto pk  = kp.public_key();
        auto sig = kp.sign(m, msg.size());
        // Sample rather than sweep all 3309 — verification is not free.
        const size_t spots[] = {0, 1, 500, 1700, 3000, mldsa65::SIGNATURE_SIZE - 1};
        for (size_t i : spots) {
            auto bad = sig;
            bad[i] ^= 0x01;
            if (MlDsa65::verify(pk.data(), pk.size(), m, msg.size(),
                                bad.data(), bad.size())) {
                return false;
            }
        }
        return true;
    });

    run_test("Modified message breaks verification", [m] {
        auto kp  = MlDsa65KeyPair::generate();
        auto pk  = kp.public_key();
        auto sig = kp.sign(m, msg.size());
        const std::string other = "e2e4 is the first movf";
        return !MlDsa65::verify(pk.data(), pk.size(),
                                reinterpret_cast<const uint8_t*>(other.data()),
                                other.size(), sig.data(), sig.size());
    });

    run_test("Truncated / extended signature is rejected on length", [m] {
        auto kp  = MlDsa65KeyPair::generate();
        auto pk  = kp.public_key();
        auto sig = kp.sign(m, msg.size());
        auto longer = sig;
        longer.push_back(0);
        return !MlDsa65::verify(pk.data(), pk.size(), m, msg.size(),
                                sig.data(), sig.size() - 1) &&
               !MlDsa65::verify(pk.data(), pk.size(), m, msg.size(),
                                longer.data(), longer.size());
    });

    run_test("Malformed public key is rejected", [m] {
        auto kp  = MlDsa65KeyPair::generate();
        auto pk  = kp.public_key();
        auto sig = kp.sign(m, msg.size());
        auto short_pk = pk;
        short_pk.pop_back();
        return !MlDsa65::verify(short_pk.data(), short_pk.size(), m, msg.size(),
                                sig.data(), sig.size()) &&
               !MlDsa65::verify(nullptr, mldsa65::PUBLIC_KEY_SIZE, m, msg.size(),
                                sig.data(), sig.size());
    });

    run_test("Reloaded private key produces verifiable signatures", [m] {
        auto kp = MlDsa65KeyPair::generate();
        auto pk = kp.public_key();
        SecureBuffer sk = kp.private_key();

        auto reloaded = MlDsa65KeyPair::from_private_key(sk.data(), sk.size());
        if (!reloaded.valid()) return false;
        // The reloaded key must be the SAME identity: same public key, and
        // signatures it makes verify under the original's pin.
        if (reloaded.public_key() != pk) return false;
        auto sig = reloaded.sign(m, msg.size());
        return MlDsa65::verify(pk.data(), pk.size(), m, msg.size(),
                               sig.data(), sig.size());
    });

    run_test("Loading a wrong-length private key fails", [] {
        SecureBuffer junk(mldsa65::PRIVATE_KEY_SIZE - 1);
        return !MlDsa65KeyPair::from_private_key(junk.data(), junk.size()).valid() &&
               !MlDsa65KeyPair::from_private_key(nullptr, mldsa65::PRIVATE_KEY_SIZE)
                    .valid();
    });

    run_test("Empty message can be signed and verified", [] {
        auto kp  = MlDsa65KeyPair::generate();
        auto pk  = kp.public_key();
        auto sig = kp.sign(nullptr, 0);
        return sig.size() == mldsa65::SIGNATURE_SIZE &&
               MlDsa65::verify(pk.data(), pk.size(), nullptr, 0,
                               sig.data(), sig.size());
    });

    run_test("Signatures are randomised (hedged), yet all verify", [m] {
        auto kp = MlDsa65KeyPair::generate();
        auto pk = kp.public_key();
        auto s1 = kp.sign(m, msg.size());
        auto s2 = kp.sign(m, msg.size());
        // FIPS 204's default hedged mode adds fresh randomness per signature,
        // so two signatures over the same message differ. Both must verify.
        return s1 != s2 &&
               MlDsa65::verify(pk.data(), pk.size(), m, msg.size(),
                               s1.data(), s1.size()) &&
               MlDsa65::verify(pk.data(), pk.size(), m, msg.size(),
                               s2.data(), s2.size());
    });

    run_test("Message digest is NOT a valid stand-in for the message", [m] {
        // Guards the pre-hashing trap from signature.h: pure ML-DSA hashes
        // internally, so signing the digest is a different statement entirely.
        auto kp  = MlDsa65KeyPair::generate();
        auto pk  = kp.public_key();
        auto sig = kp.sign(m, msg.size());
        auto dg  = Sha384::hash(m, msg.size());
        return !MlDsa65::verify(pk.data(), pk.size(), dg.data(), dg.size(),
                                sig.data(), sig.size());
    });

    run_test("Default-constructed key pair is inert", [m] {
        MlDsa65KeyPair kp;
        return !kp.valid() && kp.public_key().empty() &&
               kp.private_key().empty() && kp.sign(m, msg.size()).empty();
    });

    run_test("Large message (1 MB) signs and verifies", [] {
        std::vector<uint8_t> big(1024 * 1024);
        secure_random_bytes(big.data(), big.size());
        auto kp  = MlDsa65KeyPair::generate();
        auto pk  = kp.public_key();
        auto sig = kp.sign(big.data(), big.size());
        return MlDsa65::verify(pk.data(), pk.size(), big.data(), big.size(),
                               sig.data(), sig.size());
    });
}

// ============================================================
// X25519
// ============================================================

static void test_x25519() {
    std::cout << "\n--- X25519 (RFC 7748) ---" << std::endl;

    run_test("Key generation and 32-byte key sizes", [] {
        auto kp = X25519KeyPair::generate();
        return kp.valid() &&
               kp.public_key().size()  == x25519::PUBLIC_KEY_SIZE &&
               kp.private_key().size() == x25519::PRIVATE_KEY_SIZE;
    });

    run_test("RFC 7748 section 6.1 known-answer vector", [] {
        // Alice's scalar and Bob's public u-coordinate from the RFC, with the
        // shared K it specifies. This is the one true correctness anchor here:
        // everything else in this section is a self-consistency check.
        const uint8_t a_priv[32] = {
            0x77,0x07,0x6d,0x0a,0x73,0x18,0xa5,0x7d,0x3c,0x16,0xc1,0x72,0x51,0xb2,0x66,0x45,
            0xdf,0x4c,0x2f,0x87,0xeb,0xc0,0x99,0x2a,0xb1,0x77,0xfb,0xa5,0x1d,0xb9,0x2c,0x2a};
        const uint8_t b_pub[32] = {
            0xde,0x9e,0xdb,0x7d,0x7b,0x7d,0xc1,0xb4,0xd3,0x5b,0x61,0xc2,0xec,0xe4,0x35,0x37,
            0x3f,0x83,0x43,0xc8,0x5b,0x78,0x67,0x4d,0xad,0xfc,0x7e,0x14,0x6f,0x88,0x2b,0x4f};
        const uint8_t expect[32] = {
            0x4a,0x5d,0x9d,0x5b,0xa4,0xce,0x2d,0xe1,0x72,0x8e,0x3b,0xf4,0x80,0x35,0x0f,0x25,
            0xe0,0x7e,0x21,0xc9,0x47,0xd1,0x9e,0x33,0x76,0xf0,0x9b,0x3c,0x1e,0x16,0x17,0x42};

        auto alice = X25519KeyPair::from_private_key(a_priv, sizeof(a_priv));
        if (!alice.valid()) return false;
        SecureBuffer k = alice.derive_shared(b_pub, sizeof(b_pub));
        return k.size() == 32 && std::memcmp(k.data(), expect, 32) == 0;
    });

    run_test("Both sides derive the same secret", [] {
        auto a = X25519KeyPair::generate();
        auto b = X25519KeyPair::generate();
        auto apk = a.public_key();
        auto bpk = b.public_key();
        return same(a.derive_shared(bpk.data(), bpk.size()),
                    b.derive_shared(apk.data(), apk.size()));
    });

    run_test("A third party derives a different secret", [] {
        auto a = X25519KeyPair::generate();
        auto b = X25519KeyPair::generate();
        auto c = X25519KeyPair::generate();
        auto bpk = b.public_key();
        return !same(a.derive_shared(bpk.data(), bpk.size()),
                     c.derive_shared(bpk.data(), bpk.size()));
    });

    run_test("Small-order peer key is rejected (RFC 7748 section 6.1)", [] {
        auto a = X25519KeyPair::generate();
        // All-zero u is the canonical small-order point; the shared secret
        // would be all zeroes, which OpenSSL refuses to produce.
        const uint8_t zero[32] = {0};
        const uint8_t one[32]  = {1};
        return a.derive_shared(zero, 32).empty() && a.derive_shared(one, 32).empty();
    });

    run_test("Wrong-length peer key is rejected", [] {
        auto a = X25519KeyPair::generate();
        auto b = X25519KeyPair::generate().public_key();
        return a.derive_shared(b.data(), 31).empty() &&
               a.derive_shared(nullptr, 32).empty();
    });

    run_test("Reloaded private key derives identically", [] {
        auto a = X25519KeyPair::generate();
        auto b = X25519KeyPair::generate();
        auto bpk = b.public_key();
        SecureBuffer sk = a.private_key();
        auto reloaded = X25519KeyPair::from_private_key(sk.data(), sk.size());
        return reloaded.valid() && reloaded.public_key() == a.public_key() &&
               same(a.derive_shared(bpk.data(), bpk.size()),
                    reloaded.derive_shared(bpk.data(), bpk.size()));
    });

    run_test("Default-constructed key pair is inert", [] {
        X25519KeyPair kp;
        const uint8_t peer[32] = {0};
        return !kp.valid() && kp.public_key().empty() &&
               kp.private_key().empty() && kp.derive_shared(peer, 32).empty();
    });
}

// ============================================================
// Hybrid composition — the shape 7.4 will build on
// ============================================================

static void test_hybrid() {
    std::cout << "\n--- Hybrid ML-KEM-768 + X25519 ---" << std::endl;

    run_test("Both halves agree end to end", [] {
        // Server publishes both public keys; the client encapsulates and does
        // ECDH in one shot, then both sides hold the same pair of secrets.
        auto s_kem = MlKem768KeyPair::generate();
        auto s_ecc = X25519KeyPair::generate();
        auto kem_pk = s_kem.public_key();
        auto ecc_pk = s_ecc.public_key();

        auto e = MlKem768::encapsulate(kem_pk.data(), kem_pk.size());
        auto c_ecc = X25519KeyPair::generate();
        SecureBuffer c_ss2 = c_ecc.derive_shared(ecc_pk.data(), ecc_pk.size());
        if (!e.ok || c_ss2.empty()) return false;

        auto c_ecc_pk = c_ecc.public_key();
        SecureBuffer s_ss1 = s_kem.decapsulate(e.ciphertext.data(),
                                               e.ciphertext.size());
        SecureBuffer s_ss2 = s_ecc.derive_shared(c_ecc_pk.data(), c_ecc_pk.size());

        return same(e.shared_secret, s_ss1) && same(c_ss2, s_ss2);
    });

    run_test("HKDF over the concatenation binds both secrets", [] {
        SecureBuffer kem_ss = secure_random_buffer(32);
        SecureBuffer ecc_ss = secure_random_buffer(32);

        auto derive = [](const SecureBuffer& a, const SecureBuffer& b) {
            std::vector<uint8_t> master;
            master.insert(master.end(), a.data(), a.data() + a.size());
            master.insert(master.end(), b.data(), b.data() + b.size());
            SecureBuffer prk = HkdfSha384::extract(nullptr, 0,
                                                   master.data(), master.size());
            return HkdfSha384::expand_label(prk, "CHESS-SEAL-1 enc", 32);
        };

        SecureBuffer good = derive(kem_ss, ecc_ss);

        // Breaking only one half must not be enough: substituting either
        // secret changes the derived key completely.
        SecureBuffer other    = secure_random_buffer(32);
        SecureBuffer kem_only = derive(kem_ss, other);
        SecureBuffer ecc_only = derive(other, ecc_ss);

        return good.size() == 32 && !same(good, kem_only) && !same(good, ecc_only);
    });
}

// ============================================================
// main
// ============================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " ML-KEM-768 / ML-DSA-65 / X25519 - Phase 7.3" << std::endl;
    std::cout << "========================================" << std::endl;

    test_ml_kem();
    test_ml_dsa();
    test_x25519();
    test_hybrid();

    std::cout << "\n========================================" << std::endl;
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed"
              << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_failed > 0) ? 1 : 0;
}
