/**
 * test_request_pipeline.cpp — LLD-5.1 pipeline stages.
 *
 * The pipeline is the one place where every incoming route's parse-
 * json + auth checks now live. These tests pin its contract before
 * later slices grow it with seal / rate-limit stages:
 *
 *   1. ParseJson stage
 *      - Malformed JSON short-circuits with the pre-refactor
 *        "Invalid JSON: ..." wire error and the typed route never runs.
 *      - Well-formed JSON passes through untouched.
 *
 *   2. Auth stage
 *      - `AuthRequirement::None` skips extraction and calls the
 *        typed route with `ctx.identity` unset.
 *      - `AuthRequirement::Required` with a null IdentityExtractor
 *        emits `auth_required` with the exact "Authentication is not
 *        configured on this server" message every LLD-2 handler used.
 *      - `AuthRequirement::Required` with an extractor but no/bad
 *        token emits `auth_required` with the extractor's reason
 *        string; the typed route still never runs.
 *
 *   3. Dispatch
 *      - Success path: on `AuthRequirement::None`, the typed route is
 *        invoked exactly once, sees the parsed json, and can write via
 *        the caller sink; the frame reaches the socket peer.
 *
 * We drive the pipeline via `dispatch_for_test`, which bypasses the
 * router but runs the same stage code. Frames land on a socketpair
 * peer, decoded with a tiny local WS reader — the same pattern
 * test_message_sink already established.
 */

#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

#include "application/auth/identity_extractor.h"
#include "application/ports/message_sink.h"
#include "application/request_context.h"
#include "net/connection.h"
#include "protocol/request_pipeline.h"

using chess::application::MessageSink;
using chess::application::RequestContext;
using chess::application::auth::IdentityExtractor;
using chess::net::Connection;
using chess::protocol::AuthRequirement;
using chess::protocol::RequestPipeline;
using chess::protocol::RoutePolicy;
using chess::protocol::SealOpenFn;
using chess::protocol::SealOutcome;
using nlohmann::json;

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

struct Pair { int server_fd = -1; int peer_fd = -1; };

Pair make_socketpair() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) return {-1, -1};
    ::fcntl(fds[1], F_SETFL, O_NONBLOCK);
    return {fds[0], fds[1]};
}

/// Read one WebSocket TEXT frame's payload off the peer end.
std::string read_frame_payload(int peer_fd) {
    uint8_t hdr[4] = {};
    ssize_t n = ::recv(peer_fd, hdr, 2, 0);
    if (n < 2) return "";
    uint64_t len = hdr[1] & 0x7F;
    if (len == 126) {
        if (::recv(peer_fd, hdr + 2, 2, 0) < 2) return "";
        len = (uint64_t(hdr[2]) << 8) | uint64_t(hdr[3]);
    } else if (len == 127) {
        return "";
    }
    std::string payload(len, '\0');
    size_t got = 0;
    while (got < len) {
        ssize_t k = ::recv(peer_fd, payload.data() + got, len - got, 0);
        if (k <= 0) break;
        got += static_cast<size_t>(k);
    }
    payload.resize(got);
    return payload;
}

struct Fixture {
    Pair pair;
    std::unique_ptr<Connection> conn;
    Fixture() {
        pair = make_socketpair();
        conn = std::make_unique<Connection>(pair.server_fd, "127.0.0.1");
        conn->set_upgraded(true);
        conn->set_generation(1);
    }
    ~Fixture() {
        if (pair.peer_fd >= 0) ::close(pair.peer_fd);
    }
    chess::net::ConnectionLookup lookup() {
        Connection* c = conn.get();
        int fd = pair.server_fd;
        return [c, fd](int q) -> Connection* { return q == fd ? c : nullptr; };
    }
    /// Manually drain conn's write buffer to the socket. In production
    /// the epoll loop does this after each handler returns; the test
    /// simulates that step so the peer end can see the frame bytes.
    void drain() {
        while (conn->has_data_to_write()) {
            if (conn->write_to_socket() <= 0) break;
        }
    }
};

} // namespace


int main() {
    std::cout << "========================================\n";
    std::cout << " LLD-5.1 — RequestPipeline stages\n";
    std::cout << "========================================\n";

    // ── ParseJson ───────────────────────────────────────────────────────

    run_test("Malformed JSON → wire error, typed route never runs", []() {
        Fixture fx;
        RequestPipeline pipeline(nullptr, SealOpenFn{}, fx.lookup());
        int invocations = 0;
        RoutePolicy pol{"ping"};
        auto fn = [&](RequestContext&, const json&, MessageSink&) { ++invocations; };
        pipeline.dispatch_for_test(pol, fn, *fx.conn, "{ not valid json");

        fx.drain();
        std::string frame = read_frame_payload(fx.pair.peer_fd);
        auto j = json::parse(frame, nullptr, false);
        return invocations == 0
            && !j.is_discarded()
            && j.value("type", "") == "error"
            && j.value("message", "").rfind("Invalid JSON:", 0) == 0;
    });

    run_test("Well-formed JSON passes through to typed route", []() {
        Fixture fx;
        RequestPipeline pipeline(nullptr, SealOpenFn{}, fx.lookup());
        std::string seen_type;
        auto fn = [&](RequestContext&, const json& m, MessageSink&) {
            seen_type = m.value("type", "");
        };
        pipeline.dispatch_for_test(RoutePolicy{"ping"}, fn, *fx.conn,
                                   R"({"type":"ping","x":1})");
        // No wire frame expected — the fn wrote nothing.
        return seen_type == "ping";
    });

    // ── Auth: None ──────────────────────────────────────────────────────

    run_test("AuthRequirement::None does not touch extractor", []() {
        Fixture fx;
        // Deliberately construct pipeline with a null extractor to
        // prove the None path never dereferences it.
        RequestPipeline pipeline(nullptr, SealOpenFn{}, fx.lookup());
        bool ran = false;
        bool identity_set = true;
        auto fn = [&](RequestContext& ctx, const json&, MessageSink&) {
            ran = true;
            identity_set = ctx.identity.has_value();
        };
        pipeline.dispatch_for_test(
            RoutePolicy{"list_games", AuthRequirement::None},
            fn, *fx.conn, R"({"type":"list_games"})");
        return ran && !identity_set;
    });

    // ── Auth: Required, no extractor ────────────────────────────────────

    run_test("AuthRequirement::Required + null extractor → auth_required", []() {
        Fixture fx;
        RequestPipeline pipeline(nullptr, SealOpenFn{}, fx.lookup());
        int invocations = 0;
        auto fn = [&](RequestContext&, const json&, MessageSink&) { ++invocations; };
        pipeline.dispatch_for_test(
            RoutePolicy{"create_game", AuthRequirement::Required},
            fn, *fx.conn, R"({"type":"create_game"})");

        fx.drain();
        std::string frame = read_frame_payload(fx.pair.peer_fd);
        auto j = json::parse(frame, nullptr, false);
        return invocations == 0
            && !j.is_discarded()
            && j.value("type", "") == "error"
            && j.value("code", "") == "auth_required"
            && j.value("message", "") ==
                 "Authentication is not configured on this server";
    });

    // ── Auth: Required, extractor rejects the token ─────────────────────

    run_test("AuthRequirement::Required + bad token → auth_required + reason", []() {
        Fixture fx;
        // An IdentityExtractor built with both dependencies null fails
        // every extract with reason "Authentication is not available".
        IdentityExtractor extractor(nullptr, nullptr);
        RequestPipeline pipeline(&extractor, SealOpenFn{}, fx.lookup());
        int invocations = 0;
        auto fn = [&](RequestContext&, const json&, MessageSink&) { ++invocations; };
        pipeline.dispatch_for_test(
            RoutePolicy{"create_game", AuthRequirement::Required},
            fn, *fx.conn, R"({"type":"create_game","access_token":"nope"})");

        fx.drain();
        std::string frame = read_frame_payload(fx.pair.peer_fd);
        auto j = json::parse(frame, nullptr, false);
        return invocations == 0
            && !j.is_discarded()
            && j.value("type", "") == "error"
            && j.value("code", "") == "auth_required"
            && !j.value("message", "").empty();
    });

    // ── Dispatch: happy path with caller sink ────────────────────────────

    run_test("Success path invokes typed route exactly once", []() {
        Fixture fx;
        RequestPipeline pipeline(nullptr, SealOpenFn{}, fx.lookup());
        int invocations = 0;
        auto fn = [&](RequestContext&, const json&, MessageSink& sink) {
            ++invocations;
            sink.send(R"({"type":"pong"})");
        };
        pipeline.dispatch_for_test(RoutePolicy{"ping"}, fn, *fx.conn,
                                   R"({"type":"ping"})");
        fx.drain();
        std::string frame = read_frame_payload(fx.pair.peer_fd);
        return invocations == 1 && frame == R"({"type":"pong"})";
    });

    // ── Raw route (LLD-5.3) ─────────────────────────────────────────────
    //
    // Raw routes bypass ParseJson/Auth and receive the raw message
    // string. AuthHandler uses this because the auth family has its
    // own error frame shape and per-surface rate limits that don't fit
    // the generic ParseJson + Auth stages.

    run_test("Raw route receives the raw string, no parse, no auth", []() {
        Fixture fx;
        RequestPipeline pipeline(nullptr, SealOpenFn{}, fx.lookup());
        std::string seen;
        int invocations = 0;
        auto fn = [&](chess::net::Connection&, const std::string& m,
                      MessageSink& sink) {
            ++invocations;
            seen = m;
            sink.send(R"({"type":"raw_ok"})");
        };
        // Deliberately malformed JSON — the raw path never parses it,
        // so no error frame is emitted by the pipeline and the fn sees
        // the string verbatim.
        pipeline.dispatch_raw_for_test(RoutePolicy{"weird"}, fn, *fx.conn,
                                       "not-json-at-all");
        fx.drain();
        std::string frame = read_frame_payload(fx.pair.peer_fd);
        return invocations == 1
            && seen == "not-json-at-all"
            && frame == R"({"type":"raw_ok"})";
    });

    run_test("Raw route + null registry = no SealOpen effect", []() {
        Fixture fx;
        RequestPipeline pipeline(nullptr, SealOpenFn{}, fx.lookup());
        std::string seen;
        auto fn = [&](chess::net::Connection&, const std::string& m,
                      MessageSink&) { seen = m; };
        const std::string payload = R"({"type":"login","username":"x"})";
        pipeline.dispatch_raw_for_test(RoutePolicy{"login"}, fn, *fx.conn,
                                       payload);
        return seen == payload;
    });

    // ── SealOpen stage (LLD-5.3) ────────────────────────────────────────

    run_test("SealOpen Rewritten replaces raw message on typed routes", []() {
        Fixture fx;
        SealOpenFn open = [](const std::string&, const std::string&,
                             std::string& out) {
            out = R"({"type":"unsealed","secret":"42"})";
            return SealOutcome::Rewritten;
        };
        RequestPipeline pipeline(nullptr, std::move(open), fx.lookup());
        std::string seen;
        auto fn = [&](RequestContext&, const json& m, MessageSink&) {
            seen = m.dump();
        };
        pipeline.dispatch_for_test(RoutePolicy{"sealed_route"}, fn, *fx.conn,
                                   R"({"type":"sealed_route","envelope":"..."})");
        auto j = json::parse(seen, nullptr, false);
        return !j.is_discarded()
            && j.value("secret", "") == "42"
            && j.value("type", "") == "unsealed";
    });

    run_test("SealOpen Rejected drops the message silently", []() {
        Fixture fx;
        SealOpenFn open = [](const std::string&, const std::string&,
                             std::string&) { return SealOutcome::Rejected; };
        RequestPipeline pipeline(nullptr, std::move(open), fx.lookup());
        int invocations = 0;
        auto fn = [&](RequestContext&, const json&, MessageSink&) { ++invocations; };
        pipeline.dispatch_for_test(RoutePolicy{"sealed_route"}, fn, *fx.conn,
                                   R"({"type":"sealed_route"})");
        fx.drain();
        // No frame on the wire, no invocations.
        std::string frame = read_frame_payload(fx.pair.peer_fd);
        return invocations == 0 && frame.empty();
    });

    run_test("SealOpen Continue passes original message through", []() {
        Fixture fx;
        SealOpenFn open = [](const std::string&, const std::string&,
                             std::string&) { return SealOutcome::Continue; };
        RequestPipeline pipeline(nullptr, std::move(open), fx.lookup());
        std::string seen;
        auto fn = [&](RequestContext&, const json& m, MessageSink&) {
            seen = m.value("type", "");
        };
        pipeline.dispatch_for_test(RoutePolicy{"public_route"}, fn, *fx.conn,
                                   R"({"type":"public_route"})");
        return seen == "public_route";
    });

    // ── summary ─────────────────────────────────────────────────────────

    std::cout << "\n========================================\n";
    std::cout << " Results: " << g_passed << " passed, "
              << g_failed << " failed\n";
    std::cout << "========================================\n";
    return g_failed == 0 ? 0 : 1;
}
