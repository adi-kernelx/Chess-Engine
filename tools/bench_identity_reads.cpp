// Read-only microbenchmark; no fixture setup, provisioning or mutation.
// DATABASE_URL stays in env. Emits only batch timings, never returned profiles.
#include "storage/database_pool.h"
#include <future>
#include <iostream>

using namespace chess::storage;

int main() {
    for (size_t connections : {size_t{1},size_t{2}}) {
        DatabasePool pool;
        std::string error;
        if (!pool.connect_from_env(connections,error)) {
            std::cerr << "Read-only benchmark connection failed\n";
            return 1;
        }
        std::vector<DatabasePool::Lease> initial;
        for (size_t i=0;i<connections;++i) {
            auto lease = pool.acquire();
            if (!lease || !lease->database().exec("SET default_transaction_read_only=on").ok ||
                !lease->database().exec("SET statement_timeout='5s'").ok) return 1;
            initial.push_back(std::move(*lease));
        }
        auto row = initial.front().database().exec("SELECT min(id) FROM players");
        if (!row.ok || row.first().is_null(0)) return 1;
        const auto id = row.first().at(0);
        initial.clear();
        std::promise<void> start;
        const auto ready = start.get_future().share();
        std::vector<std::future<bool>> requests;
        for (int i=0;i<4;++i) requests.push_back(std::async(std::launch::async,[&] {
            ready.wait();
            auto lease = pool.acquire();
            if (!lease) return false;
            return lease->database().exec(
                "SELECT username,elo_rating,token_epoch FROM players WHERE id=$1",
                {Param::text(id)}).ok;
        }));
        const auto began = std::chrono::steady_clock::now();
        start.set_value();
        for (auto& request : requests) if (!request.get()) return 1;
        const auto elapsed = std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-began).count();
        std::cout << "connections=" << connections << " identity_reads=4 batch_ms=" << elapsed << '\n';
    }
}
