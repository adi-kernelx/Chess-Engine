#include "core/uuid.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <random>

namespace chess {
namespace core {

namespace {

/// Thread-local generator so the callers never contend on a shared
/// mutex. Seeded once per thread from `std::random_device`. If the
/// device is deterministic on some platform (older MinGW quirk), we
/// still get per-thread divergence because each thread's std::random_device
/// instance draws from the OS entropy pool independently.
std::mt19937_64& tls_engine() {
    thread_local std::mt19937_64 engine{std::random_device{}()};
    return engine;
}

} // namespace

std::string generate_uuid_v4() {
    auto& rng = tls_engine();
    // Two 64-bit draws give us the 128 random bits we need.
    uint64_t hi = rng();
    uint64_t lo = rng();

    // Version: top nibble of byte 6 = 0x4.
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    // Variant: top two bits of byte 8 = 10 (RFC 4122).
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    // Layout: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    // Bytes are laid out big-endian from the two 64-bit halves.
    std::array<uint8_t, 16> b{};
    for (int i = 0; i < 8; ++i) b[i]     = static_cast<uint8_t>((hi >> (56 - 8 * i)) & 0xFF);
    for (int i = 0; i < 8; ++i) b[8 + i] = static_cast<uint8_t>((lo >> (56 - 8 * i)) & 0xFF);

    char out[37];
    std::snprintf(out, sizeof(out),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string(out, 36);
}

} // namespace core
} // namespace chess
