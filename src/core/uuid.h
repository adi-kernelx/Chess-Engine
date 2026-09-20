/**
 * core/uuid.h — RFC-4122 v4 UUID string (LLD-4.2).
 *
 * The one caller today is `GameRoom::finish_game`, which stamps every
 * terminal snapshot with a stable idempotency key so the completion
 * service can retry `save_completed_game` without duplicating rows.
 *
 * We generate v4 UUIDs (128 random bits + the two constant nibbles that
 * pin version=4 and RFC-4122 variant). Randomness comes from a
 * thread-local `std::mt19937_64` seeded ONCE from `std::random_device`.
 * That is enough entropy for an idempotency key: collisions would need
 * two rooms whose termination fell in the same 64-bit random draw, which
 * is astronomically unlikely for the workloads this project targets.
 * Using CSPRNG bytes here would drag OpenSSL into `game/`, which the
 * project rule "external dependencies kept to a minimum" argues
 * against for a non-security use.
 */

#pragma once

#include <string>

namespace chess {
namespace core {

/// Generate a 36-character canonical RFC-4122 v4 UUID string,
/// e.g. `8a3d9c2e-1f7b-4a2e-9c1d-f0b1e2d3c4a5`.
std::string generate_uuid_v4();

} // namespace core
} // namespace chess
