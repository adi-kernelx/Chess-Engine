/**
 * test_hash.cpp — Phase 7.1 verification for the hand-written SHA-256,
 * SHA-384/512, HMAC and HKDF.
 *
 * Two independent kinds of evidence, because they catch different bugs:
 *
 *   1. PUBLISHED TEST VECTORS (FIPS 180-4, RFC 4231, RFC 5869).
 *      These prove the implementation agrees with the standard on inputs the
 *      standard's authors chose. They are necessary but not sufficient —
 *      they exercise a handful of lengths.
 *
 *   2. DIFFERENTIAL TESTING against OpenSSL over 10,000 random inputs of
 *      random lengths (0..4096 bytes).
 *      This is what actually catches padding and length-encoding bugs, which
 *      only appear at specific message lengths — typically those straddling a
 *      block boundary. A hand-written hash can pass every published vector and
 *      still be wrong at, say, 119 bytes. Deliberately included: lengths 0,
 *      55, 56, 63, 64, 65, 111, 112, 119, 120, 127, 128, 129.
 */

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <cstring>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "crypto/hkdf.h"
#include "crypto/hmac.h"
#include "crypto/sha256.h"
#include "crypto/sha512.h"

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

// ============================================================
// Helpers
// ============================================================

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

static std::vector<uint8_t> repeat_byte(uint8_t b, size_t n) {
    return std::vector<uint8_t>(n, b);
}

/// OpenSSL reference digest, used only by the differential tests.
static std::vector<uint8_t> openssl_digest(const EVP_MD* md,
                                           const uint8_t* data, size_t len) {
    std::vector<uint8_t> out(static_cast<size_t>(EVP_MD_get_size(md)));
    unsigned int out_len = 0;
    EVP_Digest(data, len, out.data(), &out_len, md, nullptr);
    out.resize(out_len);
    return out;
}

static std::vector<uint8_t> openssl_hmac(const EVP_MD* md,
                                         const uint8_t* key, size_t key_len,
                                         const uint8_t* data, size_t data_len) {
    std::vector<uint8_t> out(EVP_MAX_MD_SIZE);
    unsigned int out_len = 0;
    HMAC(md, key, static_cast<int>(key_len), data, data_len, out.data(), &out_len);
    out.resize(out_len);
    return out;
}

// ============================================================
// SHA-256 — FIPS 180-4 vectors
// ============================================================

static void test_sha256_vectors() {
    std::cout << "\n=== SHA-256: published vectors (FIPS 180-4) ===" << std::endl;

    struct Case { std::string input; std::string expect; std::string name; };
    const std::vector<Case> cases = {
        {"",    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty string"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "\"abc\""},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "56-byte (2-block padding)"},
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
         "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
               "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1", "112-byte"},
    };

    for (const auto& c : cases) {
        run_test("SHA-256 " + c.name, [&c]() {
            return Sha256::to_hex(Sha256::hash(c.input)) == c.expect;
        });
    }

    run_test("SHA-256 one million 'a'", []() {
        Sha256 h;
        const std::string chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i) h.update(chunk);
        return Sha256::to_hex(h.finish())
               == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
    });

    run_test("SHA-256 streaming equals one-shot", []() {
        const std::string msg = "the quick brown fox jumps over the lazy dog";
        Sha256 h;
        for (char ch : msg) h.update(reinterpret_cast<const uint8_t*>(&ch), 1);
        return h.finish() == Sha256::hash(msg);
    });

    run_test("SHA-256 reset() restores initial state", []() {
        Sha256 h;
        h.update(std::string("garbage"));
        h.finish();
        h.reset();
        h.update(std::string("abc"));
        return Sha256::to_hex(h.finish())
               == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    });
}

// ============================================================
// SHA-512 / SHA-384 — FIPS 180-4 vectors
// ============================================================

static void test_sha512_vectors() {
    std::cout << "\n=== SHA-512 / SHA-384: published vectors (FIPS 180-4) ===" << std::endl;

    run_test("SHA-512 empty string", []() {
        return Sha512::to_hex(Sha512::hash(std::string("")))
            == "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
               "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e";
    });

    run_test("SHA-512 \"abc\"", []() {
        return Sha512::to_hex(Sha512::hash(std::string("abc")))
            == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
               "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f";
    });

    run_test("SHA-384 empty string", []() {
        return Sha384::to_hex(Sha384::hash(std::string("")))
            == "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf"
               "63f6e1da274edebfe76f65fbd51ad2f14898b95b";
    });

    run_test("SHA-384 \"abc\"", []() {
        return Sha384::to_hex(Sha384::hash(std::string("abc")))
            == "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a"
               "43ff5bed8086072ba1e7cc2358baeca134c825a7";
    });

    run_test("SHA-384 112-byte message (2-block padding)", []() {
        const std::string m =
            "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
            "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
        return Sha384::to_hex(Sha384::hash(m))
            == "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086"
               "e3b0f712fcc7c71a557e2db966c3e9fa91746039";
    });

    run_test("SHA-384 differs from truncated SHA-512 (different IV)", []() {
        const Sha384::Digest d384 = Sha384::hash(std::string("abc"));
        const Sha512::Digest d512 = Sha512::hash(std::string("abc"));
        return std::memcmp(d384.data(), d512.data(), 48) != 0;
    });
}

// ============================================================
// HMAC — RFC 4231 vectors
// ============================================================

static void test_hmac_vectors() {
    std::cout << "\n=== HMAC: published vectors (RFC 4231) ===" << std::endl;

    // Case 1: 20-byte key, short data
    run_test("HMAC-SHA-256 RFC4231 case 1", []() {
        const auto key  = repeat_byte(0x0b, 20);
        const std::string data = "Hi There";
        const auto d = HmacSha256::mac(key.data(), key.size(),
                                       reinterpret_cast<const uint8_t*>(data.data()), data.size());
        return to_hex(d.data(), d.size())
            == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7";
    });

    run_test("HMAC-SHA-384 RFC4231 case 1", []() {
        const auto key  = repeat_byte(0x0b, 20);
        const std::string data = "Hi There";
        const auto d = HmacSha384::mac(key.data(), key.size(),
                                       reinterpret_cast<const uint8_t*>(data.data()), data.size());
        return to_hex(d.data(), d.size())
            == "afd03944d84895626b0825f4ab46907f15f9dadbe4101ec682aa034c7cebc59c"
               "faea9ea9076ede7f4af152e8b2fa9cb6";
    });

    // Case 2: short ASCII key — exercises the zero-padding branch
    run_test("HMAC-SHA-256 RFC4231 case 2 (short key)", []() {
        const auto d = HmacSha256::mac(std::string("Jefe"),
                                       std::string("what do ya want for nothing?"));
        return to_hex(d.data(), d.size())
            == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843";
    });

    run_test("HMAC-SHA-384 RFC4231 case 2 (short key)", []() {
        const auto d = HmacSha384::mac(std::string("Jefe"),
                                       std::string("what do ya want for nothing?"));
        return to_hex(d.data(), d.size())
            == "af45d2e376484031617f78d2b58a6b1b9c7ef464f5a01b47e42ec3736322445e"
               "8e2240ca5e69e2c78b3239ecfab21649";
    });

    // Case 3: 50 bytes of data
    run_test("HMAC-SHA-256 RFC4231 case 3", []() {
        const auto key  = repeat_byte(0xaa, 20);
        const auto data = repeat_byte(0xdd, 50);
        const auto d = HmacSha256::mac(key.data(), key.size(), data.data(), data.size());
        return to_hex(d.data(), d.size())
            == "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe";
    });

    // Case 4: 25-byte incrementing key
    run_test("HMAC-SHA-256 RFC4231 case 4", []() {
        const auto key  = from_hex("0102030405060708090a0b0c0d0e0f10111213141516171819");
        const auto data = repeat_byte(0xcd, 50);
        const auto d = HmacSha256::mac(key.data(), key.size(), data.data(), data.size());
        return to_hex(d.data(), d.size())
            == "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b";
    });

    // Case 6: 131-byte key — THIS is the branch where the key gets hashed first
    run_test("HMAC-SHA-256 RFC4231 case 6 (key > block size)", []() {
        const auto key = repeat_byte(0xaa, 131);
        const std::string data = "Test Using Larger Than Block-Size Key - Hash Key First";
        const auto d = HmacSha256::mac(key.data(), key.size(),
                                       reinterpret_cast<const uint8_t*>(data.data()), data.size());
        return to_hex(d.data(), d.size())
            == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54";
    });

    run_test("HMAC-SHA-384 RFC4231 case 6 (key > block size)", []() {
        const auto key = repeat_byte(0xaa, 131);
        const std::string data = "Test Using Larger Than Block-Size Key - Hash Key First";
        const auto d = HmacSha384::mac(key.data(), key.size(),
                                       reinterpret_cast<const uint8_t*>(data.data()), data.size());
        return to_hex(d.data(), d.size())
            == "4ece084485813e9088d2c63a041bc5b44f9ef1012a2b588f3cd11f05033ac4c6"
               "0c2ef6ab4030fe8296248df163f44952";
    });

    // Case 7: long key AND long data
    run_test("HMAC-SHA-256 RFC4231 case 7 (long key, long data)", []() {
        const auto key = repeat_byte(0xaa, 131);
        const std::string data =
            "This is a test using a larger than block-size key and a larger "
            "than block-size data. The key needs to be hashed before being "
            "used by the HMAC algorithm.";
        const auto d = HmacSha256::mac(key.data(), key.size(),
                                       reinterpret_cast<const uint8_t*>(data.data()), data.size());
        return to_hex(d.data(), d.size())
            == "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2";
    });

    std::cout << "\n=== HMAC: verification behaviour ===" << std::endl;

    run_test("verify() accepts a correct MAC", []() {
        const auto key  = repeat_byte(0x42, 32);
        const std::string msg = "authentic message";
        const auto d = HmacSha384::mac(key.data(), key.size(),
                                       reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
        return HmacSha384::verify(key.data(), key.size(),
                                  reinterpret_cast<const uint8_t*>(msg.data()), msg.size(),
                                  d.data(), d.size());
    });

    run_test("verify() rejects a MAC with one flipped bit", []() {
        const auto key  = repeat_byte(0x42, 32);
        const std::string msg = "authentic message";
        auto d = HmacSha384::mac(key.data(), key.size(),
                                 reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
        d[17] ^= 0x01;
        return !HmacSha384::verify(key.data(), key.size(),
                                   reinterpret_cast<const uint8_t*>(msg.data()), msg.size(),
                                   d.data(), d.size());
    });

    run_test("verify() rejects a MAC of a modified message", []() {
        const auto key  = repeat_byte(0x42, 32);
        const std::string msg  = "transfer 10 rupees";
        const std::string evil = "transfer 99 rupees";
        const auto d = HmacSha384::mac(key.data(), key.size(),
                                       reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
        return !HmacSha384::verify(key.data(), key.size(),
                                   reinterpret_cast<const uint8_t*>(evil.data()), evil.size(),
                                   d.data(), d.size());
    });

    run_test("verify() rejects a truncated MAC", []() {
        const auto key  = repeat_byte(0x42, 32);
        const std::string msg = "authentic message";
        const auto d = HmacSha384::mac(key.data(), key.size(),
                                       reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
        return !HmacSha384::verify(key.data(), key.size(),
                                   reinterpret_cast<const uint8_t*>(msg.data()), msg.size(),
                                   d.data(), 16);
    });
}

// ============================================================
// HKDF — RFC 5869 vectors
// ============================================================

static void test_hkdf_vectors() {
    std::cout << "\n=== HKDF: published vectors (RFC 5869) ===" << std::endl;

    run_test("HKDF-SHA-256 case 1: PRK", []() {
        const auto ikm  = repeat_byte(0x0b, 22);
        const auto salt = from_hex("000102030405060708090a0b0c");
        const auto prk  = HkdfSha256::extract(salt.data(), salt.size(), ikm.data(), ikm.size());
        return to_hex(prk.data(), prk.size())
            == "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5";
    });

    run_test("HKDF-SHA-256 case 1: OKM (42 bytes)", []() {
        const auto ikm  = repeat_byte(0x0b, 22);
        const auto salt = from_hex("000102030405060708090a0b0c");
        const auto info = from_hex("f0f1f2f3f4f5f6f7f8f9");
        const auto okm  = HkdfSha256::derive(salt.data(), salt.size(),
                                             ikm.data(), ikm.size(),
                                             info.data(), info.size(), 42);
        return to_hex(okm.data(), okm.size())
            == "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
               "34007208d5b887185865";
    });

    run_test("HKDF-SHA-256 case 2: 80-byte inputs, 82-byte OKM", []() {
        std::vector<uint8_t> ikm(80), salt(80), info(80);
        for (size_t i = 0; i < 80; ++i) {
            ikm[i]  = static_cast<uint8_t>(i);
            salt[i] = static_cast<uint8_t>(0x60 + i);
            info[i] = static_cast<uint8_t>(0xb0 + i);
        }
        const auto okm = HkdfSha256::derive(salt.data(), salt.size(),
                                            ikm.data(), ikm.size(),
                                            info.data(), info.size(), 82);
        return to_hex(okm.data(), okm.size())
            == "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
               "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
               "cc30c58179ec3e87c14c01d5c1f3434f1d87";
    });

    run_test("HKDF-SHA-256 case 3: empty salt and info", []() {
        const auto ikm = repeat_byte(0x0b, 22);
        const auto okm = HkdfSha256::derive(nullptr, 0, ikm.data(), ikm.size(),
                                            nullptr, 0, 42);
        return to_hex(okm.data(), okm.size())
            == "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
               "9d201395faa4b61a96c8";
    });

    std::cout << "\n=== HKDF: label separation (the enc/mac key rule) ===" << std::endl;

    run_test("different labels produce different keys", []() {
        const auto ikm = repeat_byte(0x5a, 32);
        auto prk = HkdfSha384::extract(nullptr, 0, ikm.data(), ikm.size());
        const auto k_enc = HkdfSha384::expand_label(prk, "CHESS-SEAL-1 enc", 32);
        const auto k_mac = HkdfSha384::expand_label(prk, "CHESS-SEAL-1 mac", 32);
        return std::memcmp(k_enc.data(), k_mac.data(), 32) != 0;
    });

    run_test("same label reproduces the same key (determinism)", []() {
        const auto ikm = repeat_byte(0x5a, 32);
        auto prk1 = HkdfSha384::extract(nullptr, 0, ikm.data(), ikm.size());
        auto prk2 = HkdfSha384::extract(nullptr, 0, ikm.data(), ikm.size());
        const auto a = HkdfSha384::expand_label(prk1, "CHESS-SEAL-1 enc", 32);
        const auto b = HkdfSha384::expand_label(prk2, "CHESS-SEAL-1 enc", 32);
        return std::memcmp(a.data(), b.data(), 32) == 0;
    });

    run_test("expand output is a prefix-consistent stream", []() {
        // OKM(64) must begin with OKM(32) — the counter construction guarantees it.
        const auto ikm = repeat_byte(0x77, 32);
        auto prk = HkdfSha256::extract(nullptr, 0, ikm.data(), ikm.size());
        const auto a = HkdfSha256::expand_label(prk, "test", 32);
        const auto b = HkdfSha256::expand_label(prk, "test", 64);
        return std::memcmp(a.data(), b.data(), 32) == 0;
    });

    run_test("expand rejects a request beyond 255*HashLen", []() {
        const auto ikm = repeat_byte(0x77, 32);
        auto prk = HkdfSha256::extract(nullptr, 0, ikm.data(), ikm.size());
        const auto too_big = HkdfSha256::expand_label(prk, "test", 255 * 32 + 1);
        return too_big.size() == 0;
    });
}

// ============================================================
// Differential testing against OpenSSL
// ============================================================

static void test_differential() {
    std::cout << "\n=== Differential vs OpenSSL (the padding-bug catcher) ===" << std::endl;

    // Lengths chosen to straddle every block boundary in both algorithms.
    const std::vector<size_t> boundary_lengths = {
        0, 1, 2, 55, 56, 57, 63, 64, 65, 111, 112, 113, 119, 120, 127, 128, 129, 255, 256
    };

    run_test("SHA-256 matches OpenSSL at every block boundary", [&]() {
        std::mt19937 rng(12345);
        for (size_t len : boundary_lengths) {
            std::vector<uint8_t> buf(len);
            for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(rng() & 0xff);
            const auto ours   = Sha256::hash(buf.data(), buf.size());
            const auto theirs = openssl_digest(EVP_sha256(), buf.data(), buf.size());
            if (theirs.size() != ours.size() ||
                std::memcmp(ours.data(), theirs.data(), ours.size()) != 0) {
                std::cout << "\n         mismatch at len=" << len << " ";
                return false;
            }
        }
        return true;
    });

    run_test("SHA-384 matches OpenSSL at every block boundary", [&]() {
        std::mt19937 rng(23456);
        for (size_t len : boundary_lengths) {
            std::vector<uint8_t> buf(len);
            for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(rng() & 0xff);
            const auto ours   = Sha384::hash(buf.data(), buf.size());
            const auto theirs = openssl_digest(EVP_sha384(), buf.data(), buf.size());
            if (theirs.size() != ours.size() ||
                std::memcmp(ours.data(), theirs.data(), ours.size()) != 0) {
                std::cout << "\n         mismatch at len=" << len << " ";
                return false;
            }
        }
        return true;
    });

    run_test("SHA-512 matches OpenSSL at every block boundary", [&]() {
        std::mt19937 rng(34567);
        for (size_t len : boundary_lengths) {
            std::vector<uint8_t> buf(len);
            for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(rng() & 0xff);
            const auto ours   = Sha512::hash(buf.data(), buf.size());
            const auto theirs = openssl_digest(EVP_sha512(), buf.data(), buf.size());
            if (theirs.size() != ours.size() ||
                std::memcmp(ours.data(), theirs.data(), ours.size()) != 0) {
                std::cout << "\n         mismatch at len=" << len << " ";
                return false;
            }
        }
        return true;
    });

    run_test("SHA-256 matches OpenSSL over 10,000 random inputs", []() {
        std::mt19937 rng(0xC0FFEE);
        std::uniform_int_distribution<size_t> len_dist(0, 4096);
        for (int iter = 0; iter < 10000; ++iter) {
            const size_t len = len_dist(rng);
            std::vector<uint8_t> buf(len);
            for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(rng() & 0xff);
            const auto ours   = Sha256::hash(buf.data(), buf.size());
            const auto theirs = openssl_digest(EVP_sha256(), buf.data(), buf.size());
            if (std::memcmp(ours.data(), theirs.data(), ours.size()) != 0) {
                std::cout << "\n         mismatch at iter=" << iter << " len=" << len << " ";
                return false;
            }
        }
        return true;
    });

    run_test("SHA-384 matches OpenSSL over 10,000 random inputs", []() {
        std::mt19937 rng(0xBADC0DE);
        std::uniform_int_distribution<size_t> len_dist(0, 4096);
        for (int iter = 0; iter < 10000; ++iter) {
            const size_t len = len_dist(rng);
            std::vector<uint8_t> buf(len);
            for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(rng() & 0xff);
            const auto ours   = Sha384::hash(buf.data(), buf.size());
            const auto theirs = openssl_digest(EVP_sha384(), buf.data(), buf.size());
            if (std::memcmp(ours.data(), theirs.data(), ours.size()) != 0) {
                std::cout << "\n         mismatch at iter=" << iter << " len=" << len << " ";
                return false;
            }
        }
        return true;
    });

    run_test("HMAC-SHA-256 matches OpenSSL over 5,000 random key/data pairs", []() {
        std::mt19937 rng(0x5EED);
        std::uniform_int_distribution<size_t> key_dist(0, 200);   // spans the >block-size branch
        std::uniform_int_distribution<size_t> data_dist(0, 2048);
        for (int iter = 0; iter < 5000; ++iter) {
            const size_t klen = key_dist(rng);
            const size_t dlen = data_dist(rng);
            std::vector<uint8_t> key(klen), data(dlen);
            for (size_t i = 0; i < klen; ++i) key[i]  = static_cast<uint8_t>(rng() & 0xff);
            for (size_t i = 0; i < dlen; ++i) data[i] = static_cast<uint8_t>(rng() & 0xff);
            const auto ours   = HmacSha256::mac(key.data(), klen, data.data(), dlen);
            const auto theirs = openssl_hmac(EVP_sha256(), key.data(), klen, data.data(), dlen);
            if (theirs.size() != ours.size() ||
                std::memcmp(ours.data(), theirs.data(), ours.size()) != 0) {
                std::cout << "\n         mismatch at iter=" << iter
                          << " klen=" << klen << " dlen=" << dlen << " ";
                return false;
            }
        }
        return true;
    });

    run_test("HMAC-SHA-384 matches OpenSSL over 5,000 random key/data pairs", []() {
        std::mt19937 rng(0xF00D);
        std::uniform_int_distribution<size_t> key_dist(0, 300);   // block size is 128 here
        std::uniform_int_distribution<size_t> data_dist(0, 2048);
        for (int iter = 0; iter < 5000; ++iter) {
            const size_t klen = key_dist(rng);
            const size_t dlen = data_dist(rng);
            std::vector<uint8_t> key(klen), data(dlen);
            for (size_t i = 0; i < klen; ++i) key[i]  = static_cast<uint8_t>(rng() & 0xff);
            for (size_t i = 0; i < dlen; ++i) data[i] = static_cast<uint8_t>(rng() & 0xff);
            const auto ours   = HmacSha384::mac(key.data(), klen, data.data(), dlen);
            const auto theirs = openssl_hmac(EVP_sha384(), key.data(), klen, data.data(), dlen);
            if (theirs.size() != ours.size() ||
                std::memcmp(ours.data(), theirs.data(), ours.size()) != 0) {
                std::cout << "\n         mismatch at iter=" << iter
                          << " klen=" << klen << " dlen=" << dlen << " ";
                return false;
            }
        }
        return true;
    });

    run_test("streaming in random chunks equals one-shot (SHA-256)", []() {
        std::mt19937 rng(0xABCDEF);
        std::uniform_int_distribution<size_t> chunk_dist(1, 200);
        for (int iter = 0; iter < 500; ++iter) {
            const size_t len = 1 + (rng() % 3000);
            std::vector<uint8_t> buf(len);
            for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(rng() & 0xff);

            Sha256 h;
            size_t off = 0;
            while (off < len) {
                const size_t chunk = std::min(chunk_dist(rng), len - off);
                h.update(buf.data() + off, chunk);
                off += chunk;
            }
            if (h.finish() != Sha256::hash(buf.data(), buf.size())) return false;
        }
        return true;
    });
}

// ============================================================
// main
// ============================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Hand-written Hash Primitives - Phase 7.1" << std::endl;
    std::cout << "========================================" << std::endl;

    test_sha256_vectors();
    test_sha512_vectors();
    test_hmac_vectors();
    test_hkdf_vectors();
    test_differential();

    std::cout << "\n========================================" << std::endl;
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_failed > 0) ? 1 : 0;
}
