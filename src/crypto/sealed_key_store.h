/**
 * sealed_key_store.h — the one-time key store behind the sealed envelope.
 *
 * The envelope itself (sealed_envelope.h) is pure cryptography and holds no
 * state. Everything that makes the scheme safe *over time* lives here, and it
 * is all policy rather than mathematics:
 *
 * SINGLE USE — the replay defence.
 *   A key is deleted the moment its id is looked up, whether the open then
 *   succeeds or fails. So a captured envelope cannot be replayed: the only key
 *   that could ever open it no longer exists anywhere.
 *
 *   The "whether or not it succeeds" part is the subtle half. If a failed open
 *   put the key back, an attacker could grind against one key forever — and
 *   worse, the difference between "key still there" and "key gone" would leak
 *   whether their forgery was well-formed. Consume first, decide later.
 *
 * EXPIRY — bounding the harvest window.
 *   Unclaimed keys are swept after a short TTL (120 s by default: long enough
 *   for a human to type a password, short enough that a stolen key is nearly
 *   always already dead).
 *
 * PER-IP CAP — the reason this class exists at all rather than a bare map.
 *   Minting a key is ML-KEM keygen + X25519 keygen + an ML-DSA signature, and
 *   each stored entry pins ~6.5 KB of key material. Unauthenticated clients ask
 *   for these, so without a cap `seal_request` is a CPU-and-memory amplifier:
 *   one small frame, a lot of server work. The cap turns that into a bounded,
 *   per-source cost. This is a denial-of-service control, not a confidentiality
 *   one, and it is deliberately enforced BEFORE any key generation happens.
 *
 * Thread safety: every public method takes the mutex. Workers call issue() and
 * consume() concurrently from the thread pool.
 */

#pragma once

#include "crypto/sealed_envelope.h"
#include "crypto/signature.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace chess {
namespace crypto {

class SealedKeyStore {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr uint32_t DEFAULT_TTL_SECONDS = 120;
    static constexpr size_t   DEFAULT_MAX_PER_IP  = 8;
    static constexpr size_t   DEFAULT_MAX_TOTAL   = 4096;

    explicit SealedKeyStore(uint32_t ttl_seconds = DEFAULT_TTL_SECONDS,
                            size_t max_per_ip    = DEFAULT_MAX_PER_IP,
                            size_t max_total     = DEFAULT_MAX_TOTAL);

    /**
     * Mint a one-time key for `client_ip`, store it, and produce the signed
     * offer to send back.
     *
     * Returns false if the caller is over its cap or the store is full — the
     * check happens before any key generation, so a flooding client is cheap
     * to refuse.
     */
    bool issue(const MlDsa65KeyPair& identity, const std::string& client_ip,
               SealKeyOffer& out_offer);

    /**
     * Look up and REMOVE the key filed under `key_id`.
     * Returns false if it is unknown, already used, or expired — the three
     * cases are indistinguishable to the caller on purpose.
     */
    bool consume(const std::vector<uint8_t>& key_id, SealKeyMaterial& out);

    /// Drop everything past its TTL. Called opportunistically from issue().
    size_t sweep_expired();

    size_t size() const;
    size_t outstanding_for(const std::string& client_ip) const;

private:
    struct Entry {
        SealKeyMaterial   material;
        Clock::time_point expires_at;
        std::string       ip;
    };

    /// Caller must hold mutex_.
    size_t sweep_locked(Clock::time_point now);
    void   erase_locked(const std::string& id_key);

    mutable std::mutex mutex_;
    uint32_t ttl_seconds_;
    size_t   max_per_ip_;
    size_t   max_total_;

    // Keyed by the raw 16 key-id bytes held in a std::string, which is just a
    // byte container here — never printed, never treated as text.
    std::unordered_map<std::string, Entry>  entries_;
    std::unordered_map<std::string, size_t> per_ip_;
};

} // namespace crypto
} // namespace chess
