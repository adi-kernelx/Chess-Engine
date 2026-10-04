#include "crypto/identity_file.h"
#include "crypto/sha512.h"
#include <openssl/evp.h>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace chess::crypto;

namespace {
int failures = 0;
void check(const char* label, bool passed) {
    std::cout << (passed ? "PASS " : "FAIL ") << label << '\n';
    if (!passed) ++failures;
}
MlDsa65KeyPair parse(const SecureBuffer& bytes) {
    return parse_identity_key(bytes.data(), bytes.size());
}
bool same_signing_identity(const MlDsa65KeyPair& key, const std::vector<uint8_t>& public_key) {
    const uint8_t message[] = {1, 2, 3, 4};
    const auto signature = key.sign(message, sizeof(message));
    return key.valid() && key.public_key() == public_key &&
        MlDsa65::verify(public_key.data(), public_key.size(), message, sizeof(message),
                       signature.data(), signature.size());
}
SecureBuffer text_key(const SecureBuffer& raw, bool crlf, bool trailing = true) {
    const std::string header = std::string(IDENTITY_TEXT_HEADER) + (crlf ? "\r\n" : "\n");
    const size_t encoded_size = 4 * (raw.size() / 3);
    const size_t tail = trailing ? (crlf ? 2 : 1) : 0;
    // Extra byte for EVP_EncodeBlock's terminating NUL; wipe it on resize.
    SecureBuffer text(header.size() + encoded_size + tail + 1);
    std::memcpy(text.data(), header.data(), header.size());
    EVP_EncodeBlock(text.data() + header.size(), raw.data(), static_cast<int>(raw.size()));
    if (trailing) {
        if (crlf) text[header.size() + encoded_size] = '\r';
        text[header.size() + encoded_size + tail - 1] = '\n';
    }
    text.resize(header.size() + encoded_size + tail);
    return text;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--verify-files") {
        // Read real files internally; print only pass/fail, never private bytes.
        auto raw = load_identity_key_file(argv[2]);
        auto text = load_identity_key_file(argv[3]);
        const auto public_key = raw.public_key();
        check("converted identity has the same public key and signs correctly",
              raw.valid() && same_signing_identity(text, public_key));
        const auto digest = Sha384::hash(public_key.data(), public_key.size());
        unsigned char encoded[65] = {};
        EVP_EncodeBlock(encoded, digest.data(), static_cast<int>(digest.size()));
        check("identity matches configured public fingerprint",
              std::string(reinterpret_cast<char*>(encoded)) == argv[4]);
        return failures ? 1 : 0;
    }
    if (argc != 1) return 2;
    auto original = MlDsa65KeyPair::generate();
    auto raw = original.private_key();
    const auto public_key = original.public_key();
    check("generated fixture valid", original.valid() && raw.size() == mldsa65::PRIVATE_KEY_SIZE);
    check("original binary key preserved", same_signing_identity(parse(raw), public_key));
    auto text = text_key(raw, false);
    check("text format preserves signing identity", same_signing_identity(parse(text), public_key));
    check("CRLF upload preserves identity", same_signing_identity(parse(text_key(raw, true)), public_key));
    check("no final newline accepted", same_signing_identity(parse(text_key(raw, false, false)), public_key));
    check("empty rejected", !parse_identity_key(nullptr, 0).valid());
    check("null rejected", !parse_identity_key(nullptr, 4032).valid());
    check("truncated raw rejected", !parse_identity_key(raw.data(), raw.size() - 1).valid());
    auto bad = text.clone(); bad[0] = '!';
    check("wrong marker rejected", !parse(bad).valid());
    bad = text.clone(); bad[sizeof(IDENTITY_TEXT_HEADER)] = '!';
    check("invalid alphabet rejected", !parse(bad).valid());
    bad = text.clone(); bad[sizeof(IDENTITY_TEXT_HEADER)] = '=';
    check("padding rejected", !parse(bad).valid());
    bad = text.clone(); bad[sizeof(IDENTITY_TEXT_HEADER)] = '-';
    check("URL alphabet rejected", !parse(bad).valid());
    bad = text.clone(); bad[sizeof(IDENTITY_TEXT_HEADER)] = ' ';
    check("embedded whitespace rejected", !parse(bad).valid());
    bad = text.clone(); bad[sizeof(IDENTITY_TEXT_HEADER)] = 0;
    check("embedded NUL rejected", !parse(bad).valid());
    check("truncated text rejected", !parse_identity_key(text.data(), text.size() - 2).valid());
    bad = text.clone(); bad.resize(bad.size() + 1); bad[bad.size() - 1] = 'X';
    check("trailing payload rejected", !parse(bad).valid());
    bad[bad.size() - 1] = '\n';
    check("extra line rejected", !parse(bad).valid());
    check("missing path rejected", !load_identity_key_file(nullptr).valid());
    check("unreadable file rejected", !load_identity_key_file("/chess-test-nonexistent-key").valid());
    char path[] = "/tmp/chess-identity-format-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) return 2;
    check("write fixture", write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    check("file loader round trip", same_signing_identity(load_identity_key_file(path), public_key));
    SecureBuffer oversized(IDENTITY_FILE_MAX_BYTES + 2);
    check("oversized buffer rejected", !parse(oversized).valid());
    if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0) { close(fd); unlink(path); return 2; }
    check("oversized fixture write", write(fd, oversized.data(), oversized.size()) == static_cast<ssize_t>(oversized.size()));
    check("oversized file rejected", !load_identity_key_file(path).valid());
    close(fd);
    unlink(path); // Only this suite's freshly created fixture.
    return failures ? 1 : 0;
}
