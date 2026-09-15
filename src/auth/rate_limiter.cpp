#include "auth/rate_limiter.h"

namespace chess {
namespace auth {

RateLimiter::RateLimiter(RateLimit limit,
                         std::chrono::seconds ttl,
                         ClockFn clock)
    : limit_(limit), ttl_(ttl), clock_(std::move(clock)) {}

void RateLimiter::refill_locked(Bucket& b, Clock::time_point at) const {
    const auto elapsed =
        std::chrono::duration<double>(at - b.last_touched).count();
    // A monotonically-increasing clock plus a first-created-in-the-past
    // last_touched should keep this non-negative; guard anyway so a broken
    // clock or a rewound test cannot mint tokens.
    if (elapsed > 0.0) {
        b.tokens += elapsed * limit_.refill_per_sec;
        if (b.tokens > static_cast<double>(limit_.capacity)) {
            b.tokens = static_cast<double>(limit_.capacity);
        }
    }
    b.last_touched = at;
}

size_t RateLimiter::sweep_locked(Clock::time_point at) {
    size_t removed = 0;
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        if (at - it->second.last_touched > ttl_) {
            it = buckets_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

bool RateLimiter::try_take(const std::string& key, size_t cost) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Clock::time_point at = now();

    // Sweep lazily. Amortised across normal traffic; no periodic thread and
    // no chance of the map growing forever on a spoofed-IP flood.
    sweep_locked(at);

    auto it = buckets_.find(key);
    if (it == buckets_.end()) {
        // A brand-new key starts full. This is the correct behaviour for a
        // legitimate first-time visitor and does not benefit an attacker any
        // more than the burst allowance already does.
        Bucket b;
        b.tokens       = static_cast<double>(limit_.capacity);
        b.last_touched = at;
        it = buckets_.emplace(key, b).first;
    } else {
        refill_locked(it->second, at);
    }

    Bucket& b = it->second;
    if (b.tokens < static_cast<double>(cost)) {
        // Deny WITHOUT taking any tokens. A failed request costs the caller
        // nothing here, so a client that keeps hammering waits the full
        // refill window regardless of how often they retry.
        return false;
    }
    b.tokens -= static_cast<double>(cost);
    return true;
}

double RateLimiter::current_tokens(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = buckets_.find(key);
    if (it == buckets_.end()) return -1.0;
    refill_locked(it->second, now());
    return it->second.tokens;
}

size_t RateLimiter::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buckets_.size();
}

size_t RateLimiter::sweep_expired() {
    std::lock_guard<std::mutex> lock(mutex_);
    return sweep_locked(now());
}

} // namespace auth
} // namespace chess
