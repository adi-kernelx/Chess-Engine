#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include "net/tcp_server.h"

// A deliberately blocked durable scheduler must not prevent a new socket's
// WebSocket handshake. No database/network service or browser is required.
int main() {
    // The subprocess retains default SIGPIPE handling: the old send(..., 0)
    // dies here, independently of any process-wide signal configuration.
    const pid_t child = fork();
    if (child == 0) {
        signal(SIGPIPE, SIG_DFL);
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) _exit(2);
        chess::net::Connection conn(sockets[0], "closed-peer");
        close(sockets[1]);
        conn.append_to_write_buffer("reply");
        _exit(conn.write_to_socket() == -1 && errno == EPIPE ? 0 : 3);
    }
    int child_status = 0;
    if (child < 0 || waitpid(child, &child_status, 0) != child
        || !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) return 1;
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (probe < 0 || bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) return 1;
    socklen_t size = sizeof(address);
    if (getsockname(probe, reinterpret_cast<sockaddr*>(&address), &size) != 0) return 1;
    close(probe);
    chess::concurrent::ThreadPool pool(2);
    chess::net::TcpServer server(ntohs(address.sin_port), pool);
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false, release = false;
    bool handler_started = false, release_handler = false;
    std::atomic<int> calls{0};
    server.set_maintenance_callback([&] {
        ++calls;
        std::unique_lock<std::mutex> lock(mutex);
        started = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    });
    server.get_router().register_handler("slow", [&](chess::net::Connection& conn, const std::string&) {
        std::unique_lock<std::mutex> lock(mutex);
        handler_started = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release_handler; });
        chess::net::WebSocket::write_frame(conn, chess::net::WsOpcode::TEXT, "done");
    });
    if (!server.start()) return 1;
    std::thread loop([&] { server.run(); });
    bool ready;
    {
        std::unique_lock<std::mutex> lock(mutex);
        ready = cv.wait_for(lock, std::chrono::seconds(2), [&] { return started; });
    }
    int client = socket(AF_INET, SOCK_STREAM, 0);
    timeval timeout{1, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    bool connected = connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    const char* request = "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if (connected) send(client, request, std::strlen(request), 0);
    char response[1024]{};
    const auto bytes = connected ? recv(client, response, sizeof(response) - 1, 0) : -1;
    bool passed = ready && bytes > 0 && std::strstr(response, "101 Switching Protocols") && calls == 1;
    const std::string payload = "{\"type\":\"slow\"}";
    const std::string frame = std::string(1, static_cast<char>(0x81))
        + static_cast<char>(payload.size()) + payload;
    send(client, frame.data(), frame.size(), MSG_NOSIGNAL);
    {
        std::unique_lock<std::mutex> lock(mutex);
        passed = cv.wait_for(lock, std::chrono::seconds(2), [&] { return handler_started; }) && passed;
    }
    int other = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(other, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(other, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) passed = false;
    send(other, request, std::strlen(request), MSG_NOSIGNAL);
    std::memset(response, 0, sizeof(response));
    const auto other_bytes = recv(other, response, sizeof(response) - 1, 0);
    passed = passed && other_bytes > 0 && std::strstr(response, "101 Switching Protocols");
    {
        std::lock_guard<std::mutex> lock(mutex);
        release_handler = true;
    }
    cv.notify_all();
    close(other);
    // Stop while the callback is in flight, then release it; run() must join
    // that callback before the server and referenced services can be destroyed.
    server.stop();
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_all();
    loop.join();
    pool.shutdown();
    close(client);
    std::cout << (passed ? "PASS" : "FAIL") << ": closed-peer writes survive SIGPIPE; blocked maintenance/handler do not block another socket; shutdown drains work\n";
    return passed ? 0 : 1;
}
