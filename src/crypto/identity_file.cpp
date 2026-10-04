#include "crypto/identity_file.h"

#include <openssl/evp.h>
#include <cstring>
#include <fstream>

namespace chess::crypto {

MlDsa65KeyPair parse_identity_key(const uint8_t* bytes, size_t size) {
    if (!bytes || size > IDENTITY_FILE_MAX_BYTES) return {};
    if (size == mldsa65::PRIVATE_KEY_SIZE) {
        return MlDsa65KeyPair::from_private_key(bytes, size);
    }
    constexpr size_t header_size = sizeof(IDENTITY_TEXT_HEADER) - 1;
    constexpr size_t encoded_size = (mldsa65::PRIVATE_KEY_SIZE / 3) * 4;
    static_assert(mldsa65::PRIVATE_KEY_SIZE % 3 == 0);
    if (size <= header_size || std::memcmp(bytes, IDENTITY_TEXT_HEADER, header_size) != 0) return {};
    size_t offset = header_size;
    if (bytes[offset] == '\r') ++offset;
    if (offset >= size || bytes[offset++] != '\n') return {};
    // Console/editor line-ending conversion is harmless; other whitespace,
    // multiple lines, padding, URL alphabet and extra payload are rejected.
    if (size > offset && bytes[size - 1] == '\n') {
        --size;
        if (size > offset && bytes[size - 1] == '\r') --size;
    }
    if (size - offset != encoded_size) return {};
    for (size_t i = offset; i < size; ++i) {
        const uint8_t c = bytes[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/')) return {};
    }
    // Validate exact length/alphabet BEFORE EVP_DecodeBlock; no padding here.
    // Decode directly into a wiping buffer, not an ordinary vector/string.
    SecureBuffer raw(mldsa65::PRIVATE_KEY_SIZE);
    const int decoded = EVP_DecodeBlock(raw.data(), bytes + offset, static_cast<int>(encoded_size));
    if (decoded != static_cast<int>(raw.size())) return {};
    return MlDsa65KeyPair::from_private_key(raw.data(), raw.size());
}

MlDsa65KeyPair load_identity_key_file(const char* path) {
    if (!path || !*path) return {};
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    // Bounded read, including one excess byte to detect oversized files.
    SecureBuffer bytes(IDENTITY_FILE_MAX_BYTES + 1);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.bad()) return {};
    const auto size = static_cast<size_t>(input.gcount());
    return parse_identity_key(bytes.data(), size);
}

} // namespace chess::crypto
