/**
 * test_message_sink.cpp — LLD-1 delivery-adapter boundary.
 *
 * The MessageSink is what protects an application-layer use case from
 * ever holding a raw Connection. These tests fix its contract in three
 * dimensions:
 *
 *   1. FakeSink — a test-time sink that just captures frames. Every
 *      test in later phases that wants to observe what a use case
 *      sends will lean on something like this, so the shape must be
 *      right from day one.
 *   2. SocketMessageSink happy path — the frame reaches the peer
 *      exactly as WebSocket::write_frame would have written it.
 *   3. SocketMessageSink generation guard — the whole point of
 *      ConnectionHandle. When the fd is reused (same number, higher
 *      generation), a stale sink pointing at the old generation MUST
 *      NOT deliver anything to the new client.
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

#include "application/ports/message_sink.h"
#include "net/connection.h"
#include "net/connection_handle.h"
#include "net/socket_message_sink.h"

using chess::application::MessageSink;
using chess::net::Connection;
using chess::net::ConnectionHandle;
using chess::net::SocketMessageSink;

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

/// Trivial capture-only sink for exercising use-case tests without a
/// socket. Records every frame in order.
struct FakeSink final : MessageSink {
    std::vector<std::string> frames;
    bool return_value = true;
    bool send(std::string frame) override {
        frames.push_back(std::move(frame));
        return return_value;
    }
};

/// Create a Unix-domain socketpair. Returns (server_fd, peer_fd) with
/// the peer set non-blocking. The server side goes into a Connection.
struct Pair {
    int server_fd = -1;
    int peer_fd   = -1;
};

Pair make_socketpair() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) return {-1, -1};
    ::fcntl(fds[1], F_SETFL, O_NONBLOCK);
    return {fds[0], fds[1]};
}

/// Read one WebSocket TEXT frame's payload off the peer end and return
/// it. Supports 7-bit and 16-bit extended length. Returns empty string
/// if the socket has no data / errored / or the frame does not decode.
std::string read_frame_payload(int peer_fd) {
    uint8_t hdr[4] = {};
    ssize_t n = ::recv(peer_fd, hdr, 2, 0);
    if (n < 2) return "";
    uint64_t len = hdr[1] & 0x7F;
    if (len == 126) {
        if (::recv(peer_fd, hdr + 2, 2, 0) < 2) return "";
        len = (uint64_t(hdr[2]) << 8) | uint64_t(hdr[3]);
    } else if (len == 127) {
        return "";  // 64-bit lengths not expected in this suite.
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

} // namespace


int main() {
    std::cout << "========================================\n";
    std::cout << " LLD-1 — MessageSink adapter\n";
    std::cout << "========================================\n";

    // ── FakeSink ────────────────────────────────────────────────────────

    run_test("FakeSink captures frames in order", []() {
        FakeSink sink;
        sink.send("first");
        sink.send("second");
        return sink.frames.size() == 2
            && sink.frames[0] == "first"
            && sink.frames[1] == "second";
    });

    run_test("FakeSink honours return_value=false", []() {
        FakeSink sink;
        sink.return_value = false;
        const bool ok = sink.send("payload");
        // Even when "gone", the frame is captured — this lets tests
        // observe the payload that WOULD have been sent.
        return ok == false && sink.frames.size() == 1;
    });

    // ── SocketMessageSink happy path ────────────────────────────────────

    run_test("SocketMessageSink delivers the frame verbatim", []() {
        auto p = make_socketpair();
        if (p.server_fd < 0) return false;
        Connection conn(p.server_fd, "127.0.0.1");
        conn.set_upgraded(true);
        conn.set_generation(7);

        auto lookup = [&](int fd) -> Connection* {
            return fd == p.server_fd ? &conn : nullptr;
        };
        SocketMessageSink sink(conn.handle(), lookup);
        const bool ok = sink.send(R"({"type":"pong"})");
        const std::string got = read_frame_payload(p.peer_fd);
        ::close(p.peer_fd);
        return ok && got == R"({"type":"pong"})";
    });

    run_test("direct SocketMessageSink flushes before a long handler returns", []() {
        auto p = make_socketpair();
        if (p.server_fd < 0) return false;
        Connection conn(p.server_fd, "127.0.0.1");
        conn.set_upgraded(true);

        // This is the caller-side constructor used by RequestPipeline. The
        // peer must be able to read immediately; no simulated event-loop
        // drain occurs after send().
        SocketMessageSink sink(conn);
        const bool ok = sink.send(R"({"type":"move_made"})");
        const std::string got = read_frame_payload(p.peer_fd);
        ::close(p.peer_fd);
        return ok && got == R"({"type":"move_made"})"
            && !conn.has_data_to_write();
    });

    // ── SocketMessageSink generation guard ──────────────────────────────

    run_test("SocketMessageSink refuses stale generation without writing",
             []() {
        auto p = make_socketpair();
        if (p.server_fd < 0) return false;
        Connection conn(p.server_fd, "127.0.0.1");
        conn.set_upgraded(true);
        conn.set_generation(2);

        // A sink built for generation 1 — as if it were queued for a
        // previous holder of this fd. The socket now has generation 2.
        ConnectionHandle stale_handle{p.server_fd, 1};
        auto lookup = [&](int fd) -> Connection* {
            return fd == p.server_fd ? &conn : nullptr;
        };
        SocketMessageSink sink(stale_handle, lookup);
        const bool ok = sink.send(R"({"type":"pong"})");

        // No bytes on the wire. If the guard failed this recv would
        // return a payload, not empty.
        const std::string got = read_frame_payload(p.peer_fd);
        ::close(p.peer_fd);
        return ok == false && got.empty();
    });

    run_test("SocketMessageSink returns false when lookup finds nothing",
             []() {
        auto lookup = [](int) -> Connection* { return nullptr; };
        ConnectionHandle handle{42, 1};
        SocketMessageSink sink(handle, lookup);
        return sink.send("anything") == false;
    });

    run_test("SocketMessageSink handles invalid handle", []() {
        auto lookup = [](int) -> Connection* {
            return nullptr;
        };
        SocketMessageSink sink({}, lookup);  // fd == -1
        return sink.send("anything") == false;
    });

    // ── summary ─────────────────────────────────────────────────────────

    std::cout << "\n========================================\n";
    std::cout << " Results: " << g_passed << " passed, "
              << g_failed << " failed\n";
    std::cout << "========================================\n";
    return g_failed == 0 ? 0 : 1;
}
