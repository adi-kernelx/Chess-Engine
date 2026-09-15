/**
 * rate_limiter.h — the hand-written token bucket that lives in front of every
 * expensive operation.
 *
 * TOKEN BUCKET, NOT LEAKY BUCKET
 *
 * A token bucket allows short bursts up to `capacity` and enforces a
 * sustained rate of `refill_per_sec`. That is what you want for login: a
 * legitimate user might mis-type their password three times in five seconds
 * (burst OK) and then walk away (no sustained load). A leaky bucket would
 * pace their retries — bad UX, no security gain.
 *
 * THE ORDERING TRAP
 *
 * Rate limiting must run BEFORE the expensive work it is protecting. If the
 * Argon2id verification runs first and the limiter runs after, the CPU is
 * already spent — the "limit" only rejects the *response*, not the *cost*.
 * Every handler that uses this class MUST call `try_take()` as the first
 * statement.
 *
 * SHARDING
 *
 * The whole map is behind one mutex. That is fine at Phase 7's traffic
 * shape — logins are the busy path and they are already slow (~30 ms of
 * Argon2id each). If a future load pattern makes contention visible, split
 * this into N shards keyed by hash(key) — the API stays the same. Adding
 * complexity now would be premature.
 *
 * CLOCK INJECTION
 *
 * Tests need to advance time without sleeping. The optional `ClockFn`
 * constructor argument lets a test hand-crank the clock; the default is
 * `steady_clock::now`. Steady is critical — a wall-clock jump backwards
 * (NTP correction, container migration) would refund tokens and let a burst
 * through, and a jump forwards would look like a huge burst of legitimate
 * users to the algorithm.
 *
 * STALE ENTRY SWEEP
 *
 * Every `try_take()` opportunistically drops entries older than `ttl`. An
 * attacker who cycles through millions of keys (e.g. spoofed X-Forwarded-For
 * headers on a poorly-configured load balancer) otherwise fills the map.
 * The sweep cost is amortised across normal traffic — no periodic thread.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace chess {
namespace auth {

struct RateLimit {
    size_t capacity;          ///< max burst
    double refill_per_sec;    ///< sustained rate

    /// Convenience: "N per interval". The interval is a duration; the rate is
    /// derived. Both `login` and `register` in the spec are expressed this way,
    /// so the constructor stays readable at call sites.
    static RateLimit per(size_t n, std::chrono::seconds interval) {
        return {n, static_cast<double>(n) / static_cast<double>(interval.count())};
    }
};

class RateLimiter {
public:
    using Clock   = std::chrono::steady_clock;
    using ClockFn = std::function<Clock::time_point()>;

    /**
     * @param ttl   sweep entries untouched for this long. Default: 1 hour —
     *              longer than any per-hour bucket window we operate.
     * @param clock injected for tests; defaults to `Clock::now`.
     */
    RateLimiter(RateLimit limit,
                std::chrono::seconds ttl = std::chrono::seconds(3600),
                ClockFn clock = {});

    /**
     * Attempt to consume `cost` tokens for `key`. Returns true if allowed.
     * A failed take does not roll back — the caller is refused this instant,
     * not the next one.
     */
    bool try_take(const std::string& key, size_t cost = 1);

    /// Read a bucket's current token count (after lazy refill). For tests
    /// and introspection only. -1.0 means "no bucket for this key yet".
    double current_tokens(const std::string& key);

    size_t size() const;

    /// Force a sweep now — the periodic-thread analogue for tests.
    size_t sweep_expired();

private:
    struct Bucket {
        double            tokens;
        Clock::time_point last_touched;
    };

    Clock::time_point now() const { return clock_ ? clock_() : Clock::now(); }
    void refill_locked(Bucket& b, Clock::time_point at) const;
    size_t sweep_locked(Clock::time_point at);

    mutable std::mutex             mutex_;
    RateLimit                      limit_;
    std::chrono::seconds           ttl_;
    ClockFn                        clock_;
    std::unordered_map<std::string, Bucket> buckets_;
};

// ── Curated presets that mirror §7.9's rate table ───────────────────────────
//
// Constants, not code paths — the handlers hold their own RateLimiter
// instances constructed from these. Keeping the numbers in one place makes
// it obvious what the site's policy is; every occurrence would otherwise
// have to be re-checked when a limit needs to change.

namespace rate_presets {
inline RateLimit login_per_ip()        { return RateLimit::per(5,  std::chrono::minutes(1)); }
inline RateLimit login_per_account()   { return RateLimit::per(10, std::chrono::hours(1)); }
inline RateLimit register_per_ip()     { return RateLimit::per(3,  std::chrono::hours(1)); }
inline RateLimit google_auth_per_ip()  { return RateLimit::per(10, std::chrono::minutes(1)); }
inline RateLimit seal_request_per_ip() { return RateLimit::per(20, std::chrono::minutes(1)); }
inline RateLimit move_per_conn()       { return RateLimit::per(10, std::chrono::seconds(1)); }
} // namespace rate_presets

} // namespace auth
} // namespace chess
