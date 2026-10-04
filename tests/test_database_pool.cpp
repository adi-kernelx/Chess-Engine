#include "storage/database_pool.h"
#include "storage/transaction.h"
#include "support/database_fixture.h"
#include <atomic>
#include <future>
#include <iostream>
#include <set>

using namespace chess::storage;
using namespace std::chrono_literals;

int main() {
    std::string error;
    if (!chess::tests::require_test_database(error)) {
        std::cerr << "REFUSING pool fixture: " << error << '\n';
        return 2;
    }
    Database control;
    if (!control.connect_from_env(error) || !control.run_script(
        "DROP TABLE IF EXISTS pool_fixture; CREATE TABLE pool_fixture(tag int PRIMARY KEY)", error)) return 1;
    DatabasePool pool;
    if (!pool.connect_from_env(2, error)) return 1;
    int failed = 0;
    const auto check = [&failed](const char* name, bool result) {
        std::cout << (result ? "PASS " : "FAIL ") << name << '\n';
        if (!result) ++failed;
    };
    {
        auto a = pool.acquire();
        auto b = pool.acquire();
        auto pid_a = a->database().exec("SELECT pg_backend_pid()");
        auto pid_b = b->database().exec("SELECT pg_backend_pid()");
        check("exclusive independent connections", pid_a.ok && pid_b.ok &&
              pid_a.first().at(0) != pid_b.first().at(0));
        auto began = std::chrono::steady_clock::now();
        auto third = pool.acquire(40ms);
        check("bounded pool and acquisition timeout", !third &&
              std::chrono::steady_clock::now() - began >= 30ms);
        auto moved = std::move(a);
        a.reset();
        check("move does not double-release", !pool.acquire(0ms));
        moved.reset();
        check("return wakes borrower", pool.acquire(40ms).has_value());
    }
    {
        auto lease = pool.acquire();
        auto& db = lease->database();
        auto before = db.exec("SELECT pg_backend_pid()");
        Transaction tx(db);
        auto after = db.exec("SELECT pg_backend_pid()");
        check("transaction pinned to lease", tx.ok() && before.ok && after.ok &&
              before.first().at(0) == after.first().at(0));
        check("commit persists", db.exec("INSERT INTO pool_fixture VALUES(1)").ok && tx.commit());
    }
    {
        auto lease = pool.acquire();
        lease->database().exec("BEGIN");
        lease->database().exec("INSERT INTO pool_fixture VALUES(2)");
        // Deliberately omit ROLLBACK. Return hygiene must undo the write.
    }
    auto count = control.exec("SELECT count(*) FROM pool_fixture");
    check("unfinished transaction rolls back on return", count.ok && count.first().at(0) == "1");
    {
        auto lease = pool.acquire();
        lease->database().exec("BEGIN");
        lease->database().exec("SELECT 'not-a-number'::int");
    }
    {
        auto lease = pool.acquire();
        check("aborted session reusable", lease && lease->database().exec("SELECT 1").ok);
    }
    std::atomic<int> errors{0};
    std::vector<std::future<void>> writers;
    for (int i = 10; i < 30; ++i) {
        writers.push_back(std::async(std::launch::async, [&, i] {
            auto lease = pool.acquire();
            if (!lease) { ++errors; return; }
            Transaction tx(lease->database());
            if (!tx.ok() || !lease->database().exec("INSERT INTO pool_fixture VALUES($1)",
                {Param::int64(i)}).ok || (i % 2 == 0 && !tx.commit())) ++errors;
        }));
    }
    for (auto& writer : writers) writer.get();
    count = control.exec("SELECT count(*) FROM pool_fixture");
    check("concurrent commits/rollbacks isolated", errors == 0 && count.ok && count.first().at(0) == "11");

    {
        DatabasePool recovery;
        if (!recovery.connect_from_env(1,error)) return 1;
        auto lease = recovery.acquire();
        auto pid = lease->database().exec("SELECT pg_backend_pid()");
        auto terminated = control.exec("SELECT pg_terminate_backend($1)", {Param::text(pid.first().at(0))});
        auto broken = lease->database().exec("SELECT 1");
        lease.reset();
        auto repaired = recovery.acquire();
        auto new_pid = repaired ? repaired->database().exec("SELECT pg_backend_pid()") : QueryResult{};
        check("lost connection repaired without replay", terminated.ok && !broken.ok && new_pid.ok &&
              new_pid.first().at(0) != pid.first().at(0));
    }

    // One CPU can overlap waits: emulate the measured 145ms DB round trip.
    const auto benchmark = [&](size_t connections) {
        DatabasePool measured;
        if (!measured.connect_from_env(connections, error)) return -1.0;
        auto began = std::chrono::steady_clock::now();
        std::vector<std::future<bool>> requests;
        for (int i = 0; i < 4; ++i) requests.push_back(std::async(std::launch::async, [&] {
            auto lease = measured.acquire();
            return lease && lease->database().exec("SELECT pg_sleep(0.145)").ok;
        }));
        for (auto& request : requests) if (!request.get()) return -1.0;
        return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
    };
    const auto serial = benchmark(1);
    const auto parallel = benchmark(2);
    std::cout << "SIMULATED 145ms wait, four requests: one connection=" << serial
              << "ms two connections=" << parallel << "ms\n";
    check("delayed queries all complete", serial > 0 && parallel > 0);
    control.exec("DROP TABLE pool_fixture");
    return failed ? 1 : 0;
}
