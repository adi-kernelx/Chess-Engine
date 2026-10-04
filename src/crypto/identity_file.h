#pragma once

#include "crypto/signature.h"
#include <cstddef>
#include <cstdint>

namespace chess::crypto {

// Versioned ASCII wrapper for the same raw private key, NOT encryption.
inline constexpr char IDENTITY_TEXT_HEADER[] = "CHESS-ML-DSA-65-PRIVATE-KEY-V1";
inline constexpr size_t IDENTITY_FILE_MAX_BYTES = 8192;

// Accept exactly 4032 raw bytes, or the header + one strict base64 line.
// Malformed input returns an invalid key; no secret/error payload is logged.
MlDsa65KeyPair parse_identity_key(const uint8_t* bytes, size_t size);
MlDsa65KeyPair load_identity_key_file(const char* path);

} // namespace chess::crypto
