/**
 * test_tournament_service.cpp — LLD-2.4.
 *
 * Seam-level tests over `application::TournamentService`. A capturing
 * `FakeSink` receives every frame the service emits.
 *
 * Covered here (fast, no DB required):
 *   1. create_tournament with null db → "Tournaments require a database"
 *   2. join_tournament with null db → same
 *   3. start_tournament with null db → same
 *   4. tournament_state with null db → same
 *   5. list_tournaments with null db → same
 *   6. report_tournament_result with null db → same
 *
 * The null-db check runs first in every route, so the non-DB
 * validation branches (name empty, name > 128, rounds out of range,
 * invalid time control, missing tournament_id / pairing_id) are not
 * reachable in these tests without a live cluster. The DB-backed
 * happy paths are already covered end-to-end by test_tournament
 * (27/27) against the real cluster.
 */

#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "application/ports/message_sink.h"
#include "application/request_context.h"
#include "application/tournament_service.h"

using json = nlohmann::json;
using namespace chess;
using namespace chess::application;

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

struct FakeSink final : public MessageSink {
    std::vector<std::string> frames;
    bool send(std::string frame) override {
        frames.push_back(std::move(frame));
        return true;
    }
};

RequestContext make_ctx(int fd) {
    RequestContext ctx;
    ctx.caller.fd         = fd;
    ctx.caller.generation = 1;
    return ctx;
}

bool expect_error(const FakeSink& s, const std::string& want) {
    if (s.frames.size() != 1) return false;
    auto j = json::parse(s.frames[0]);
    return j["type"] == "error" && j["message"] == want;
}

} // namespace

int main() {
    std::cout << "Running TournamentService tests...\n";

    const std::string DB_ERR = "Tournaments require a database";

    run_test("create_tournament with null db → no-database error", [&] {
        TournamentService svc(nullptr);
        FakeSink sink;
        svc.create_tournament(make_ctx(1), 42, "My Cup", 4, 300, 3,
                              2000000000, 2000000060, 3600, sink);
        return expect_error(sink, DB_ERR);
    });

    run_test("join_tournament with null db → no-database error", [&] {
        TournamentService svc(nullptr);
        FakeSink sink;
        svc.join_tournament(make_ctx(1), 42, 1500, true, 7, sink);
        return expect_error(sink, DB_ERR);
    });

    run_test("leave_tournament with null db → no-database error", [&] {
        TournamentService svc(nullptr);
        FakeSink sink;
        svc.leave_tournament(make_ctx(1), 42, true, 7, sink);
        return expect_error(sink, DB_ERR);
    });

    run_test("start_tournament with null db → no-database error", [&] {
        TournamentService svc(nullptr);
        FakeSink sink;
        svc.start_tournament(make_ctx(1), 42, true, 7, sink);
        return expect_error(sink, DB_ERR);
    });

    run_test("tournament_state with null db → no-database error", [&] {
        TournamentService svc(nullptr);
        FakeSink sink;
        svc.tournament_state(make_ctx(1), true, 7, sink);
        return expect_error(sink, DB_ERR);
    });

    run_test("list_tournaments with null db → no-database error", [&] {
        TournamentService svc(nullptr);
        FakeSink sink;
        svc.list_tournaments(make_ctx(1), "", 25, sink);
        return expect_error(sink, DB_ERR);
    });

    run_test("report_tournament_result with null db → no-database error", [&] {
        TournamentService svc(nullptr);
        FakeSink sink;
        svc.report_tournament_result(make_ctx(1), 42, true, 100, "w", "test", sink);
        return expect_error(sink, DB_ERR);
    });

    std::cout << "\nResults: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
