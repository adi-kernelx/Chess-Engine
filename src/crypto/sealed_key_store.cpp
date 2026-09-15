#include "crypto/sealed_key_store.h"

namespace chess {
namespace crypto {

namespace {

/// The 16 key-id bytes as a std::string, used only as a map key.
std::string id_key_of(const std::vector<uint8_t>& key_id) {
    return std::string(reinterpret_cast<const char*>(key_id.data()), key_id.size());
}

} // namespace

SealedKeyStore::SealedKeyStore(uint32_t ttl_seconds, size_t max_per_ip,
                               size_t max_total)
    : ttl_seconds_(ttl_seconds),
      max_per_ip_(max_per_ip),
      max_total_(max_total) {}

size_t SealedKeyStore::sweep_locked(Clock::time_point now) {
    size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (now >= it->second.expires_at) {
            auto ip_it = per_ip_.find(it->second.ip);
            if (ip_it != per_ip_.end() && --ip_it->second == 0) {
                per_ip_.erase(ip_it);   // do not accumulate a per-IP map forever
            }
            it = entries_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

void SealedKeyStore::erase_locked(const std::string& id_key) {
    auto it = entries_.find(id_key);
    if (it == entries_.end()) return;
    auto ip_it = per_ip_.find(it->second.ip);
    if (ip_it != per_ip_.end() && --ip_it->second == 0) {
        per_ip_.erase(ip_it);
    }
    entries_.erase(it);
}

bool SealedKeyStore::issue(const MlDsa65KeyPair& identity,
                           const std::string& client_ip,
                           SealKeyOffer& out_offer) {
    // Admission control FIRST. Minting is ML-KEM keygen + X25519 keygen + an
    // ML-DSA signature; doing that before deciding whether we want the request
    // is what turns a rate limit into a rate suggestion.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const Clock::time_point now = Clock::now();
        sweep_locked(now);

        if (entries_.size() >= max_total_) return false;
        auto it = per_ip_.find(client_ip);
        if (it != per_ip_.end() && it->second >= max_per_ip_) return false;
    }

    // Generate outside the lock: three key generations plus a signature is on
    // the order of a hundred microseconds, and holding the mutex across it
    // would serialise every concurrent login.
    SealKeyMaterial material;
    SealKeyOffer    offer;
    if (!SealedEnvelopeService::mint(identity, ttl_seconds_, material, offer)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const Clock::time_point now = Clock::now();

        // Re-check under the lock: the caps were verified before an unlocked
        // gap, so a burst of concurrent requests could otherwise all pass.
        if (entries_.size() >= max_total_) return false;
        auto it = per_ip_.find(client_ip);
        if (it != per_ip_.end() && it->second >= max_per_ip_) return false;

        Entry entry;
        entry.expires_at = now + std::chrono::seconds(ttl_seconds_);
        entry.ip         = client_ip;
        entry.material   = std::move(material);

        entries_.emplace(id_key_of(offer.key_id), std::move(entry));
        per_ip_[client_ip]++;
    }

    out_offer = std::move(offer);
    return true;
}

bool SealedKeyStore::consume(const std::vector<uint8_t>& key_id,
                             SealKeyMaterial& out) {
    if (key_id.size() != seal::KEY_ID_SIZE) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    const std::string id_key = id_key_of(key_id);

    auto it = entries_.find(id_key);
    if (it == entries_.end()) return false;   // unknown, or already consumed

    // Expired is treated exactly like missing, and the entry is removed either
    // way. The caller cannot distinguish "never existed", "already used" and
    // "too late" — three answers an attacker would like to have.
    const bool expired = Clock::now() >= it->second.expires_at;
    SealKeyMaterial material = std::move(it->second.material);
    erase_locked(id_key);

    if (expired) return false;
    out = std::move(material);
    return true;
}

size_t SealedKeyStore::sweep_expired() {
    std::lock_guard<std::mutex> lock(mutex_);
    return sweep_locked(Clock::now());
}

size_t SealedKeyStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

size_t SealedKeyStore::outstanding_for(const std::string& client_ip) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = per_ip_.find(client_ip);
    return (it == per_ip_.end()) ? 0 : it->second;
}

} // namespace crypto
} // namespace chess
