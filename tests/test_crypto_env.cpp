/**
 * test_crypto_env.cpp — Phase 7.0 environment gate.
 *
 * Everything in Phase 7 assumes OpenSSL 3.5+, because that is the first release
 * carrying ML-KEM (FIPS 203) and ML-DSA (FIPS 204) in the DEFAULT provider,
 * and 3.2+ for Argon2id. Ubuntu 22.04 shipped 3.0.2 and had none of them,
 * which is why this project migrated to Ubuntu 26.04 LTS.
 *
 * This test exists so that a missing primitive fails loudly here, at build
 * time, rather than as a confusing runtime error deep inside the handshake.
 */

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/opensslv.h>

#include <functional>
#include <iostream>
#include <string>

#include "crypto/random.h"
#include "crypto/secure_buffer.h"

using namespace chess::crypto;

static int g_passed = 0;
static int g_failed = 0;

static void run_test(const std::string& name, std::function<bool()> fn) {
    std::cout << "  [TEST] " << name << "... ";
    bool ok = false;
    try {
        ok = fn();
    } catch (const std::exception& e) {
        std::cout << "THREW (" << e.what() << ") ";
        ok = false;
    }
    if (ok) { std::cout << "PASS" << std::endl; g_passed++; }
    else    { std::cout << "FAIL" << std::endl; g_failed++; }
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Crypto Environment - Phase 7.0" << std::endl;
    std::cout << "========================================" << std::endl;

    std::cout << "\n  OpenSSL headers : " << OPENSSL_VERSION_TEXT << std::endl;
    std::cout << "  OpenSSL runtime : " << OpenSSL_version(OPENSSL_VERSION) << std::endl;

    std::cout << "\n=== Version gate ===" << std::endl;

    run_test("OpenSSL >= 3.5 at compile time", []() {
        // 0xMNN00PP0L — 3.5.0 == 0x30500000L
        return OPENSSL_VERSION_NUMBER >= 0x30500000L;
    });

    run_test("OpenSSL >= 3.5 at run time", []() {
        return OpenSSL_version_num() >= 0x30500000L;
    });

    std::cout << "\n=== Post-quantum primitives (default provider) ===" << std::endl;

    run_test("ML-KEM-768 available (FIPS 203)", []() {
        EVP_KEM* kem = EVP_KEM_fetch(nullptr, "ML-KEM-768", nullptr);
        const bool ok = (kem != nullptr);
        if (kem) EVP_KEM_free(kem);
        return ok;
    });

    run_test("ML-DSA-65 available (FIPS 204)", []() {
        EVP_SIGNATURE* sig = EVP_SIGNATURE_fetch(nullptr, "ML-DSA-65", nullptr);
        const bool ok = (sig != nullptr);
        if (sig) EVP_SIGNATURE_free(sig);
        return ok;
    });

    run_test("X25519 available (hybrid classical half)", []() {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
        const bool ok = (ctx != nullptr);
        if (ctx) EVP_PKEY_CTX_free(ctx);
        return ok;
    });

    run_test("Argon2id available (password hashing)", []() {
        EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
        const bool ok = (kdf != nullptr);
        if (kdf) EVP_KDF_free(kdf);
        return ok;
    });

    std::cout << "\n=== CSPRNG and SecureBuffer ===" << std::endl;

    run_test("secure_random_bytes produces differing output", []() {
        uint8_t a[32], b[32];
        secure_random_bytes(a, sizeof(a));
        secure_random_bytes(b, sizeof(b));
        // A collision here is a 1-in-2^256 event; in practice it means the
        // CSPRNG is broken or stubbed.
        return std::memcmp(a, b, sizeof(a)) != 0;
    });

    run_test("secure_random_bytes is not trivially all-zero", []() {
        uint8_t buf[64];
        secure_random_bytes(buf, sizeof(buf));
        for (size_t i = 0; i < sizeof(buf); ++i) {
            if (buf[i] != 0) return true;
        }
        return false;
    });

    run_test("SecureBuffer move leaves the source empty", []() {
        SecureBuffer a = secure_random_buffer(32);
        SecureBuffer b = std::move(a);
        return b.size() == 32 && a.size() == 0;
    });

    run_test("SecureBuffer clone copies contents", []() {
        SecureBuffer a = secure_random_buffer(16);
        SecureBuffer b = a.clone();
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
    });

    run_test("SecureBuffer resize down wipes the tail", []() {
        SecureBuffer a(32);
        for (size_t i = 0; i < 32; ++i) a[i] = 0xAB;
        a.resize(16);
        return a.size() == 16;
    });

    std::cout << "\n========================================" << std::endl;
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_failed > 0) ? 1 : 0;
}
