/**
 * application/ports/clock.h — time as an injectable dependency (LLD-6.1).
 *
 * The engine, GameRoom's Fischer clock, Matchmaker's ELO-fence widening,
 * and every timestamp we stamp on a request/response currently read
 * `std::chrono::steady_clock::now()` or `std::chrono::system_clock::now()`
 * directly. That's fine in production; it's bad in tests, which have to
 * either sleep (slow, flaky) or install machine-specific wall-clock
 * mocks. A Clock port makes time an injected dependency at the seams
 * where it matters, without paying a virtual call for the engine's
 * per-node hot loop (the engine consults it at most once every 2048
 * nodes; the cost is invisible).
 *
 * TWO CLOCKS
 *   `steady_now()` is a monotonic point suitable for durations. Never
 *   moves backwards; not tied to wall time. Used for time budgets,
 *   Fischer decrement, rate-limit refills, matchmaker fences.
 *
 *   `system_now()` is a wall-clock point suitable for auditable
 *   timestamps that must round-trip through JSON or SQL. Used for
 *   log timestamps, `wall_start_ / wall_end_` on GameRoom, JWT
 *   iat/exp comparisons.
 *
 *   `unix_seconds()` is a convenience for the many call sites that
 *   want `int64_t seconds-since-epoch` (JWT signer, IdentityExtractor,
 *   RequestContext::received_at_unix). It is derived from
 *   `system_now()` — so the two never drift in a FakeClock.
 *
 * PRODUCTION vs TEST
 *   `SystemClock` — reads the real clocks. This is what `main.cpp`
 *   constructs and injects.
 *   `FakeClock` — starts at an operator-supplied instant on BOTH
 *   axes, moves only when `advance(delta)` is called, and keeps
 *   steady/system in lock-step. Tests get deterministic timeouts,
 *   rate-limit refills, and JWT iat/exp comparisons without sleeping.
 *
 * NO VIRTUAL PENALTY WHERE IT MATTERS
 *   The Clock is a small polymorphic port. Every consumer that
 *   dispatches virtually — GameRoom's public methods, the pipeline's
 *   ctx.received_at_unix stamp, IdentityExtractor's now hook — is
 *   already at a coarse granularity relative to the search hot loop.
 *   The Engine, when it migrates, will hold a `const Clock&` and call
 *   `steady_now()` once per 2048-node time-check, so the virtual
 *   dispatch is comfortably below any measurement resolution.
 */

#pragma once

#include <chrono>
#include <cstdint>

namespace chess::application::ports {

class Clock {
public:
    using SteadyPoint = std::chrono::steady_clock::time_point;
    using SystemPoint = std::chrono::system_clock::time_point;

    virtual ~Clock() = default;

    /// Monotonic point for measuring durations.
    virtual SteadyPoint steady_now() const = 0;
    /// Wall-clock point for auditable timestamps.
    virtual SystemPoint system_now() const = 0;
    /// Convenience: seconds since Unix epoch. Derived from system_now(),
    /// so a FakeClock keeps this in lock-step with `system_now()`.
    virtual int64_t unix_seconds() const = 0;
};

/// Production clock. Reads `steady_clock::now()` and
/// `system_clock::now()` directly. Thread-safe (each call reads its
/// own stack).
class SystemClock final : public Clock {
public:
    SteadyPoint steady_now()   const override {
        return std::chrono::steady_clock::now();
    }
    SystemPoint system_now()   const override {
        return std::chrono::system_clock::now();
    }
    int64_t     unix_seconds() const override {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
};

/// Process-wide default `SystemClock`. Long-lived consumers (GameRoom,
/// Matchmaker, RateLimiter) fall back to this when no explicit clock
/// is injected — production behaviour is identical to reading
/// `std::chrono::*::now()` inline. Tests override by calling the
/// consumer's `set_clock(&fake)` setter before touching time.
inline Clock& default_clock() {
    static SystemClock instance;
    return instance;
}

/// Test clock. Starts at a specified `(steady, system)` pair — both
/// axes advance in lock-step when the test calls `advance(delta)`.
/// Not thread-safe; tests that share a FakeClock across threads
/// should serialize access themselves.
class FakeClock final : public Clock {
public:
    /// Default constructor: steady at epoch, system at Unix epoch.
    FakeClock() = default;

    /// Start both axes at the supplied points. Typical test usage:
    ///     FakeClock c(SteadyPoint{}, system_from_iso("2026-01-01T00:00:00Z"));
    FakeClock(SteadyPoint steady, SystemPoint system)
        : steady_(steady), system_(system) {}

    /// Advance both axes by the same duration. Tests use this to
    /// simulate elapsed time deterministically.
    void advance(std::chrono::milliseconds delta) {
        steady_ += delta;
        system_ += delta;
    }
    void advance(std::chrono::seconds delta) {
        advance(std::chrono::duration_cast<std::chrono::milliseconds>(delta));
    }

    /// Set both axes to specific points; useful when a test needs
    /// to jump to an absolute instant (e.g. token exp boundary).
    void set(SteadyPoint steady, SystemPoint system) {
        steady_ = steady;
        system_ = system;
    }

    SteadyPoint steady_now()   const override { return steady_; }
    SystemPoint system_now()   const override { return system_; }
    int64_t     unix_seconds() const override {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   system_.time_since_epoch())
            .count();
    }

private:
    SteadyPoint steady_{};
    SystemPoint system_{};
};

} // namespace chess::application::ports
