/**
 * test_clock.cpp — LLD-6.1 Clock port.
 *
 * Contract tests for the two implementations. The FakeClock tests
 * are the ones that matter for downstream consumers: every future
 * deterministic timeout / rate-limit / expiry test leans on this
 * shape.
 */

#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "application/ports/clock.h"

using chess::application::ports::Clock;
using chess::application::ports::FakeClock;
using chess::application::ports::SystemClock;
using std::chrono::milliseconds;
using std::chrono::seconds;
using std::chrono::system_clock;
using std::chrono::steady_clock;

namespace {

int g_passed = 0;
int g_failed = 0;

void run_test(const std::string& name, const std::function<bool()>& fn) {
    std::cout << "  [TEST] " << name << "... ";
    try {
        if (fn()) { std::cout << "PASS\n"; ++g_passed; }
        else      { std::cout << "FAIL\n"; ++g_failed; }
    } catch (const std::exception& e) {
        std::cout << "FAIL (exception: " << e.what() << ")\n"; ++g_failed;
    }
}

} // namespace


int main() {
    std::cout << "========================================\n";
    std::cout << " LLD-6.1 — Clock port\n";
    std::cout << "========================================\n";

    // ── SystemClock ─────────────────────────────────────────────────────

    run_test("SystemClock steady_now is monotonic across a small sleep",
             []() {
        SystemClock c;
        auto a = c.steady_now();
        std::this_thread::sleep_for(milliseconds(1));
        auto b = c.steady_now();
        return b >= a && (b - a) >= milliseconds(1);
    });

    run_test("SystemClock unix_seconds agrees with system_now", []() {
        SystemClock c;
        auto sys = c.system_now();
        auto ux  = c.unix_seconds();
        auto sys_secs = std::chrono::duration_cast<seconds>(
                            sys.time_since_epoch()).count();
        // Allow one-second slack across the two calls.
        return ux == sys_secs || ux == sys_secs + 1 || ux == sys_secs - 1;
    });

    run_test("SystemClock unix_seconds is a plausible 2020s value", []() {
        SystemClock c;
        int64_t ux = c.unix_seconds();
        // 2020-01-01 == 1577836800; 2100-01-01 == 4102444800.
        return ux > 1577836800 && ux < 4102444800;
    });

    // ── FakeClock defaults ──────────────────────────────────────────────

    run_test("FakeClock default construct starts at epoch on both axes",
             []() {
        FakeClock c;
        return c.steady_now().time_since_epoch().count() == 0
            && c.system_now().time_since_epoch().count() == 0
            && c.unix_seconds() == 0;
    });

    // ── FakeClock advance ──────────────────────────────────────────────

    run_test("FakeClock advance(ms) moves both axes in lock-step", []() {
        FakeClock c;
        auto s0  = c.steady_now();
        auto y0  = c.system_now();
        auto u0  = c.unix_seconds();
        c.advance(milliseconds(1500));
        return (c.steady_now() - s0) == milliseconds(1500)
            && (c.system_now() - y0) == milliseconds(1500)
            && c.unix_seconds() == u0 + 1;   // 1500ms crosses one second
    });

    run_test("FakeClock advance(seconds) is equivalent to advance(ms)",
             []() {
        FakeClock c;
        c.advance(seconds(30));
        return c.unix_seconds() == 30
            && c.steady_now().time_since_epoch() == seconds(30);
    });

    run_test("FakeClock does not move when nothing calls advance", []() {
        FakeClock c;
        auto a = c.steady_now();
        std::this_thread::sleep_for(milliseconds(5));
        auto b = c.steady_now();
        return a == b;
    });

    // ── FakeClock set() ─────────────────────────────────────────────────

    run_test("FakeClock set() jumps both axes to specific points", []() {
        FakeClock c;
        auto steady_start = steady_clock::time_point{} + seconds(1000);
        auto sys_start    = system_clock::time_point{} + seconds(1577836800);
        c.set(steady_start, sys_start);
        return c.steady_now() == steady_start
            && c.system_now() == sys_start
            && c.unix_seconds() == 1577836800;
    });

    // ── Polymorphism (the reason the port exists) ───────────────────────

    run_test("Clock& binds to both implementations", []() {
        SystemClock sys;
        FakeClock   fake;
        Clock& a = sys;
        Clock& b = fake;
        // Only structural correctness — no timing assertions here.
        auto sa = a.steady_now();
        auto sb = b.steady_now();
        (void)sa; (void)sb;
        return true;
    });

    // ── summary ─────────────────────────────────────────────────────────

    std::cout << "\n========================================\n";
    std::cout << " Results: " << g_passed << " passed, "
              << g_failed << " failed\n";
    std::cout << "========================================\n";
    return g_failed == 0 ? 0 : 1;
}
