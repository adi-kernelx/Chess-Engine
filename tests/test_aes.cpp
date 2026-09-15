/**
 * test_aes.cpp — Phase 7.2 verification for hand-written AES-256, CTR mode,
 * and the Encrypt-then-MAC AEAD.
 *
 * Evidence in three layers:
 *   1. FIPS 197 known-answer vector for the raw AES-256 block cipher.
 *      This is the test that catches an omitted `i % Nk == 4` branch in the
 *      key schedule — a bug that otherwise yields a self-consistent cipher
 *      which simply is not AES.
 *   2. NIST SP 800-38A CTR-AES256 vectors, including counter carry behaviour.
 *   3. Differential testing against OpenSSL over random lengths, plus
 *      tamper-detection tests for every field the AEAD is supposed to protect.
 */

#include <openssl/evp.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "crypto/aead.h"
#include "crypto/aes.h"
#include "crypto/aes_ctr.h"

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

static std::vector<uint8_t> from_hex(const std::string& hex) {
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>((nib(hex[i]) << 4) | nib(hex[i + 1])));
    }
    return out;
}

static std::string to_hex(const uint8_t* d, size_t n) {
    static const char* HEX = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(HEX[d[i] >> 4]);
        s.push_back(HEX[d[i] & 0x0f]);
    }
    return s;
}

/// OpenSSL AES-256-CTR reference, used only by the differential tests.
static std::vector<uint8_t> openssl_aes256_ctr(const std::vector<uint8_t>& key,
                                               const std::vector<uint8_t>& iv,
                                               const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out(in.size());
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) return out;

    int len = 0;
    EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr, key.data(), iv.data());
    if (!in.empty()) {
        EVP_EncryptUpdate(ctx, out.data(), &len, in.data(), static_cast<int>(in.size()));
    }
    int final_len = 0;
    EVP_EncryptFinal_ex(ctx, out.data() + len, &final_len);
    EVP_CIPHER_CTX_free(ctx);
    return out;
}

// ============================================================
// AES-256 block cipher — FIPS 197
// ============================================================

static void test_aes_block() {
    std::cout << "\n=== AES-256 block cipher (FIPS 197 Appendix C.3) ===" << std::endl;

    run_test("FIPS 197 C.3 known-answer vector", []() {
        const auto key = from_hex("000102030405060708090a0b0c0d0e0f"
                                  "101112131415161718191a1b1c1d1e1f");
        const auto pt  = from_hex("00112233445566778899aabbccddeeff");

        Aes256 cipher(key.data());
        uint8_t ct[16];
        cipher.encrypt_block(pt.data(), ct);

        return to_hex(ct, 16) == "8ea2b7ca516745bfeafc49904b496089";
    });

    run_test("all-zero key and block", []() {
        const std::vector<uint8_t> key(32, 0x00);
        const std::vector<uint8_t> pt(16, 0x00);
        Aes256 cipher(key.data());
        uint8_t ct[16];
        cipher.encrypt_block(pt.data(), ct);
        return to_hex(ct, 16) == "dc95c078a2408989ad48a21492842087";
    });

    run_test("in and out may alias", []() {
        const auto key = from_hex("000102030405060708090a0b0c0d0e0f"
                                  "101112131415161718191a1b1c1d1e1f");
        auto buf = from_hex("00112233445566778899aabbccddeeff");
        Aes256 cipher(key.data());
        cipher.encrypt_block(buf.data(), buf.data());   // same pointer
        return to_hex(buf.data(), 16) == "8ea2b7ca516745bfeafc49904b496089";
    });

    run_test("one flipped key bit changes the whole block (avalanche)", []() {
        auto key = from_hex("000102030405060708090a0b0c0d0e0f"
                            "101112131415161718191a1b1c1d1e1f");
        const auto pt = from_hex("00112233445566778899aabbccddeeff");

        uint8_t ct1[16], ct2[16];
        { Aes256 c(key.data()); c.encrypt_block(pt.data(), ct1); }
        key[31] ^= 0x01;
        { Aes256 c(key.data()); c.encrypt_block(pt.data(), ct2); }

        // Expect roughly half the 128 bits to differ; anything under a quarter
        // would indicate the key schedule is not diffusing properly.
        int differing = 0;
        for (int i = 0; i < 16; ++i) {
            uint8_t x = static_cast<uint8_t>(ct1[i] ^ ct2[i]);
            while (x) { differing += (x & 1); x = static_cast<uint8_t>(x >> 1); }
        }
        return differing > 40 && differing < 90;
    });

    run_test("distinct keys give distinct ciphertexts over 256 trials", []() {
        const auto pt = from_hex("00112233445566778899aabbccddeeff");
        std::vector<std::string> seen;
        for (int k = 0; k < 256; ++k) {
            std::vector<uint8_t> key(32, static_cast<uint8_t>(k));
            Aes256 cipher(key.data());
            uint8_t ct[16];
            cipher.encrypt_block(pt.data(), ct);
            seen.push_back(to_hex(ct, 16));
        }
        std::sort(seen.begin(), seen.end());
        return std::adjacent_find(seen.begin(), seen.end()) == seen.end();
    });
}

// ============================================================
// CTR mode — NIST SP 800-38A §F.5.5
// ============================================================

static void test_aes_ctr() {
    std::cout << "\n=== AES-256-CTR (NIST SP 800-38A F.5.5/F.5.6) ===" << std::endl;

    const std::string key_hex = "603deb1015ca71be2b73aef0857d7781"
                                "1f352c073b6108d72d9810a30914dff4";
    const std::string iv_hex  = "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";

    run_test("SP800-38A four-block reference vector", [&]() {
        const auto key = from_hex(key_hex);
        const auto iv  = from_hex(iv_hex);
        const auto pt  = from_hex("6bc1bee22e409f96e93d7e117393172a"
                                  "ae2d8a571e03ac9c9eb76fac45af8e51"
                                  "30c81c46a35ce411e5fbc1191a0a52ef"
                                  "f69f2445df4f9b17ad2b417be66c3710");
        std::vector<uint8_t> ct(pt.size());
        AesCtr::process(key.data(), iv.data(), pt.data(), ct.data(), pt.size());

        return to_hex(ct.data(), ct.size())
            == "601ec313775789a5b7a7f504bbf3d228"
               "f443e3ca4d62b59aca84e990cacaf5c5"
               "2b0930daa23de94ce87017ba2d84988d"
               "dfc9c58db67aada613c2dd08457941a6";
    });

    run_test("decrypt is the same operation as encrypt", [&]() {
        const auto key = from_hex(key_hex);
        const auto iv  = from_hex(iv_hex);
        const std::string msg = "CTR mode is its own inverse.";
        std::vector<uint8_t> pt(msg.begin(), msg.end());

        std::vector<uint8_t> ct(pt.size()), back(pt.size());
        AesCtr::process(key.data(), iv.data(), pt.data(), ct.data(), pt.size());
        AesCtr::process(key.data(), iv.data(), ct.data(), back.data(), ct.size());
        return back == pt;
    });

    run_test("handles a partial trailing block (no padding)", [&]() {
        const auto key = from_hex(key_hex);
        const auto iv  = from_hex(iv_hex);
        // 37 bytes = 2 full blocks + 5 bytes
        std::vector<uint8_t> pt(37);
        for (size_t i = 0; i < pt.size(); ++i) pt[i] = static_cast<uint8_t>(i);

        std::vector<uint8_t> ct(pt.size()), back(pt.size());
        AesCtr::process(key.data(), iv.data(), pt.data(), ct.data(), pt.size());
        AesCtr::process(key.data(), iv.data(), ct.data(), back.data(), ct.size());
        return back == pt && ct.size() == 37;   // length preserved exactly
    });

    run_test("counter carries correctly across 0xFF..FF boundary", [&]() {
        // Starting counter is all-ones in the low 8 bytes, so block 2 forces a
        // carry chain. If increment_counter() got the endianness or the carry
        // wrong, this diverges from OpenSSL while short messages still match.
        const auto key = from_hex(key_hex);
        const auto iv  = from_hex("00000000000000ffffffffffffffffff");
        std::vector<uint8_t> pt(64, 0xAA);

        std::vector<uint8_t> ours(pt.size());
        AesCtr::process(key.data(), iv.data(), pt.data(), ours.data(), pt.size());
        const auto theirs = openssl_aes256_ctr(key, iv, pt);
        return ours == theirs;
    });

    run_test("in and out may alias (in-place encryption)", [&]() {
        const auto key = from_hex(key_hex);
        const auto iv  = from_hex(iv_hex);
        std::vector<uint8_t> buf(100);
        for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<uint8_t>(i * 7);
        const auto original = buf;

        AesCtr::process(key.data(), iv.data(), buf.data(), buf.data(), buf.size());
        if (buf == original) return false;                 // something happened
        AesCtr::process(key.data(), iv.data(), buf.data(), buf.data(), buf.size());
        return buf == original;                            // and it round-trips
    });

    run_test("matches OpenSSL over 2,000 random lengths", []() {
        std::mt19937 rng(0xC7C0DE);
        std::uniform_int_distribution<size_t> len_dist(0, 2000);
        for (int iter = 0; iter < 2000; ++iter) {
            const size_t len = len_dist(rng);
            std::vector<uint8_t> key(32), iv(16), pt(len);
            for (auto& b : key) b = static_cast<uint8_t>(rng() & 0xff);
            for (auto& b : iv)  b = static_cast<uint8_t>(rng() & 0xff);
            for (auto& b : pt)  b = static_cast<uint8_t>(rng() & 0xff);

            std::vector<uint8_t> ours(len);
            if (len > 0) {
                AesCtr::process(key.data(), iv.data(), pt.data(), ours.data(), len);
            }
            const auto theirs = openssl_aes256_ctr(key, iv, pt);
            if (ours != theirs) {
                std::cout << "\n         mismatch at iter=" << iter << " len=" << len << " ";
                return false;
            }
        }
        return true;
    });
}

// ============================================================
// AEAD — Encrypt-then-MAC
// ============================================================

static void test_aead() {
    std::cout << "\n=== AEAD: Encrypt-then-MAC (AES-256-CTR + HMAC-SHA-384) ===" << std::endl;

    auto make_keys = []() {
        const std::vector<uint8_t> master(32, 0x5A);
        return Aead::derive_keys(master.data(), master.size(), "CHESS-SEAL-1");
    };
    const std::vector<uint8_t> nonce(Aead::NONCE_SIZE, 0x11);
    const std::string message = "username=adi&password=hunter2";
    const std::string aad     = "login-v1";

    run_test("derive_keys produces two DIFFERENT keys", [&]() {
        const auto k = make_keys();
        return k.enc.size() == 32 && k.mac.size() == 32 &&
               std::memcmp(k.enc.data(), k.mac.data(), 32) != 0;
    });

    run_test("derive_keys is deterministic for the same master+context", [&]() {
        const auto a = make_keys();
        const auto b = make_keys();
        return std::memcmp(a.enc.data(), b.enc.data(), 32) == 0 &&
               std::memcmp(a.mac.data(), b.mac.data(), 32) == 0;
    });

    run_test("different context yields unrelated keys", [&]() {
        const std::vector<uint8_t> master(32, 0x5A);
        const auto a = Aead::derive_keys(master.data(), master.size(), "CHESS-SEAL-1");
        const auto b = Aead::derive_keys(master.data(), master.size(), "CHESS-CHAT-1");
        return std::memcmp(a.enc.data(), b.enc.data(), 32) != 0;
    });

    run_test("seal then open round-trips", [&]() {
        const auto keys = make_keys();
        const auto sealed = Aead::seal(keys, nonce.data(),
                                       reinterpret_cast<const uint8_t*>(aad.data()), aad.size(),
                                       reinterpret_cast<const uint8_t*>(message.data()),
                                       message.size());
        SecureBuffer out;
        if (!Aead::open(keys, nonce.data(),
                        reinterpret_cast<const uint8_t*>(aad.data()), aad.size(),
                        sealed.data(), sealed.size(), out)) return false;
        return out.size() == message.size() &&
               std::memcmp(out.data(), message.data(), message.size()) == 0;
    });

    run_test("sealed output is plaintext length + 48-byte tag", [&]() {
        const auto keys = make_keys();
        const auto sealed = Aead::seal(keys, nonce.data(), nullptr, 0,
                                       reinterpret_cast<const uint8_t*>(message.data()),
                                       message.size());
        return sealed.size() == message.size() + Aead::TAG_SIZE;
    });

    run_test("ciphertext does not contain the plaintext", [&]() {
        const auto keys = make_keys();
        const auto sealed = Aead::seal(keys, nonce.data(), nullptr, 0,
                                       reinterpret_cast<const uint8_t*>(message.data()),
                                       message.size());
        const std::string blob(sealed.begin(), sealed.end());
        return blob.find("hunter2") == std::string::npos;
    });

    std::cout << "\n=== AEAD: tamper detection (every protected field) ===" << std::endl;

    auto tamper_test = [&](const std::string& name, size_t flip_index) {
        run_test(name, [&]() {
            const auto keys = make_keys();
            auto sealed = Aead::seal(keys, nonce.data(),
                                     reinterpret_cast<const uint8_t*>(aad.data()), aad.size(),
                                     reinterpret_cast<const uint8_t*>(message.data()),
                                     message.size());
            sealed[flip_index] ^= 0x01;
            SecureBuffer out;
            return !Aead::open(keys, nonce.data(),
                               reinterpret_cast<const uint8_t*>(aad.data()), aad.size(),
                               sealed.data(), sealed.size(), out);
        });
    };

    tamper_test("rejects a flipped bit in the first ciphertext byte", 0);
    tamper_test("rejects a flipped bit mid-ciphertext", 10);
    tamper_test("rejects a flipped bit in the tag", message.size() + 5);

    run_test("rejects a modified nonce", [&]() {
        const auto keys = make_keys();
        const auto sealed = Aead::seal(keys, nonce.data(),
                                       reinterpret_cast<const uint8_t*>(aad.data()), aad.size(),
                                       reinterpret_cast<const uint8_t*>(message.data()),
                                       message.size());
        auto bad_nonce = nonce;
        bad_nonce[0] ^= 0x01;
        SecureBuffer out;
        return !Aead::open(keys, bad_nonce.data(),
                           reinterpret_cast<const uint8_t*>(aad.data()), aad.size(),
                           sealed.data(), sealed.size(), out);
    });

    run_test("rejects modified associated data", [&]() {
        const auto keys = make_keys();
        const auto sealed = Aead::seal(keys, nonce.data(),
                                       reinterpret_cast<const uint8_t*>(aad.data()), aad.size(),
                                       reinterpret_cast<const uint8_t*>(message.data()),
                                       message.size());
        const std::string evil_aad = "login-v2";
        SecureBuffer out;
        return !Aead::open(keys, nonce.data(),
                           reinterpret_cast<const uint8_t*>(evil_aad.data()), evil_aad.size(),
                           sealed.data(), sealed.size(), out);
    });

    run_test("rejects a wrong key", [&]() {
        const auto keys = make_keys();
        const auto sealed = Aead::seal(keys, nonce.data(), nullptr, 0,
                                       reinterpret_cast<const uint8_t*>(message.data()),
                                       message.size());
        const std::vector<uint8_t> other_master(32, 0x99);
        const auto other = Aead::derive_keys(other_master.data(), other_master.size(),
                                             "CHESS-SEAL-1");
        SecureBuffer out;
        return !Aead::open(other, nonce.data(), nullptr, 0,
                           sealed.data(), sealed.size(), out);
    });

    run_test("rejects a truncated message", [&]() {
        const auto keys = make_keys();
        auto sealed = Aead::seal(keys, nonce.data(), nullptr, 0,
                                 reinterpret_cast<const uint8_t*>(message.data()),
                                 message.size());
        sealed.resize(sealed.size() - 1);
        SecureBuffer out;
        return !Aead::open(keys, nonce.data(), nullptr, 0,
                           sealed.data(), sealed.size(), out);
    });

    run_test("rejects input shorter than the tag", [&]() {
        const auto keys = make_keys();
        std::vector<uint8_t> junk(10, 0xFF);
        SecureBuffer out;
        return !Aead::open(keys, nonce.data(), nullptr, 0, junk.data(), junk.size(), out);
    });

    run_test("AAD boundary cannot be shifted (length-prefix defence)", [&]() {
        // Without the 8-byte length prefix on the AAD, ("AB","CD") and ("A","BCD")
        // would produce the same MAC input. Sealing with one split must not
        // verify under the other.
        const auto keys = make_keys();
        const std::string aad1 = "AB";
        const std::string pt1  = "CD";
        const auto sealed = Aead::seal(keys, nonce.data(),
                                       reinterpret_cast<const uint8_t*>(aad1.data()), aad1.size(),
                                       reinterpret_cast<const uint8_t*>(pt1.data()), pt1.size());
        const std::string aad2 = "A";
        SecureBuffer out;
        return !Aead::open(keys, nonce.data(),
                           reinterpret_cast<const uint8_t*>(aad2.data()), aad2.size(),
                           sealed.data(), sealed.size(), out);
    });

    run_test("empty plaintext still authenticates", [&]() {
        const auto keys = make_keys();
        const auto sealed = Aead::seal(keys, nonce.data(),
                                       reinterpret_cast<const uint8_t*>(aad.data()), aad.size(),
                                       nullptr, 0);
        if (sealed.size() != Aead::TAG_SIZE) return false;
        SecureBuffer out;
        return Aead::open(keys, nonce.data(),
                          reinterpret_cast<const uint8_t*>(aad.data()), aad.size(),
                          sealed.data(), sealed.size(), out) && out.size() == 0;
    });

    run_test("round-trips 1,000 random messages and rejects each when mutated", []() {
        std::mt19937 rng(0xA34D);
        std::uniform_int_distribution<size_t> len_dist(0, 1500);
        for (int iter = 0; iter < 1000; ++iter) {
            std::vector<uint8_t> master(32), nonce_r(Aead::NONCE_SIZE);
            for (auto& b : master)  b = static_cast<uint8_t>(rng() & 0xff);
            for (auto& b : nonce_r) b = static_cast<uint8_t>(rng() & 0xff);
            const auto keys = Aead::derive_keys(master.data(), master.size(), "CHESS-SEAL-1");

            const size_t len = len_dist(rng);
            std::vector<uint8_t> pt(len);
            for (auto& b : pt) b = static_cast<uint8_t>(rng() & 0xff);

            auto sealed = Aead::seal(keys, nonce_r.data(), nullptr, 0, pt.data(), pt.size());

            SecureBuffer out;
            if (!Aead::open(keys, nonce_r.data(), nullptr, 0,
                            sealed.data(), sealed.size(), out)) return false;
            if (out.size() != len ||
                (len > 0 && std::memcmp(out.data(), pt.data(), len) != 0)) return false;

            // Flip one random bit anywhere in the sealed blob; it must fail.
            const size_t idx = rng() % sealed.size();
            sealed[idx] ^= static_cast<uint8_t>(1u << (rng() % 8));
            SecureBuffer out2;
            if (Aead::open(keys, nonce_r.data(), nullptr, 0,
                           sealed.data(), sealed.size(), out2)) return false;
        }
        return true;
    });
}

// ============================================================
// main
// ============================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " AES-256 / CTR / AEAD - Phase 7.2" << std::endl;
    std::cout << "========================================" << std::endl;

    test_aes_block();
    test_aes_ctr();
    test_aead();

    std::cout << "\n========================================" << std::endl;
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_failed > 0) ? 1 : 0;
}
