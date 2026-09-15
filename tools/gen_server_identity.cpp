/**
 * gen_server_identity.cpp — one-off generator for the server's long-lived
 * ML-DSA-65 identity key.
 *
 * The browser cannot be told "trust whatever key the server sends" — that is
 * the whole attack. Instead it ships with SHA-384 of the server's public key
 * baked into config.js (key pinning), and checks every key it is handed
 * against that pin. This tool produces both halves of that arrangement:
 *
 *     secrets/server_identity.key   private, mode 0600, NEVER committed
 *     stdout                        the pin, to paste into config.js
 *
 * Run once per deployment environment. Rotating the key means updating the pin
 * in the frontend, so plan a rotation as a coordinated deploy, not a hot swap.
 *
 * Usage:  ./gen_server_identity [output_path]
 */

#include "crypto/secure_buffer.h"
#include "crypto/sha512.h"
#include "crypto/signature.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

using namespace chess::crypto;

namespace {

std::string to_hex(const uint8_t* d, size_t n) {
    static const char* HEX = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(HEX[d[i] >> 4]);
        s.push_back(HEX[d[i] & 0x0F]);
    }
    return s;
}

/// Standard base64 — the pin is compared as a string in JS, so encoding must match.
std::string to_base64(const uint8_t* d, size_t n) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    s.reserve(((n + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(d[i]) << 16) |
                           (static_cast<uint32_t>(d[i + 1]) << 8) | d[i + 2];
        s.push_back(T[(v >> 18) & 0x3F]);
        s.push_back(T[(v >> 12) & 0x3F]);
        s.push_back(T[(v >> 6) & 0x3F]);
        s.push_back(T[v & 0x3F]);
    }
    if (i < n) {
        uint32_t v = static_cast<uint32_t>(d[i]) << 16;
        const bool two = (i + 1 < n);
        if (two) v |= static_cast<uint32_t>(d[i + 1]) << 8;
        s.push_back(T[(v >> 18) & 0x3F]);
        s.push_back(T[(v >> 12) & 0x3F]);
        s.push_back(two ? T[(v >> 6) & 0x3F] : '=');
        s.push_back('=');
    }
    return s;
}

} // namespace

int main(int argc, char** argv) {
    const std::string path =
        (argc > 1) ? argv[1] : "secrets/server_identity.key";

    std::cout << "Generating ML-DSA-65 server identity key..." << std::endl;

    MlDsa65KeyPair key = MlDsa65KeyPair::generate();
    if (!key.valid()) {
        std::cerr << "ERROR: key generation failed (is this OpenSSL >= 3.5?)"
                  << std::endl;
        return 1;
    }

    const SecureBuffer sk = key.private_key();
    const std::vector<uint8_t> pk = key.public_key();
    if (sk.empty() || pk.empty()) {
        std::cerr << "ERROR: key export failed" << std::endl;
        return 1;
    }

    // Create the parent directory if the caller used the default path.
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        ::mkdir(path.substr(0, slash).c_str(), S_IRWXU);   // EEXIST is fine
    }

    // Create with 0600 from the start. Writing world-readable and chmod-ing
    // afterwards leaves a window in which the key is exposed.
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        std::cerr << "ERROR: cannot create " << path << ": " << std::strerror(errno)
                  << (errno == EEXIST ? "  (refusing to overwrite an existing key)" : "")
                  << std::endl;
        return 1;
    }
    const ssize_t written = ::write(fd, sk.data(), sk.size());
    ::close(fd);
    if (written != static_cast<ssize_t>(sk.size())) {
        std::cerr << "ERROR: short write to " << path << std::endl;
        return 1;
    }

    const Sha384::Digest pin = Sha384::hash(pk.data(), pk.size());

    std::cout << "\nPrivate key written to: " << path << "  (mode 0600)\n"
              << "  -> add to .gitignore, load at start-up, never log it.\n\n"
              << "Public key   (" << pk.size() << " bytes):\n  "
              << to_hex(pk.data(), pk.size()).substr(0, 64) << "...\n\n"
              << "PIN = SHA-384(public key)\n"
              << "  hex    : " << Sha384::to_hex(pin) << "\n"
              << "  base64 : " << to_base64(pin.data(), pin.size()) << "\n\n"
              << "Paste the base64 value into frontend/js/config.js:\n"
              << "  export const PINNED_KEYS = ['"
              << to_base64(pin.data(), pin.size()) << "'];\n"
              << std::endl;
    return 0;
}
