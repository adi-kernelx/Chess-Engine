// tools/bench_engine.cpp — Phase 10.1 engine microbenchmark.
//
// Independent of the WebSocket server: instantiates a fresh Engine per
// position, disables TT reuse across positions to keep runs comparable,
// and reports NPS + depth reached at three fixed time budgets (2s, 5s,
// 10s). Output is a single Markdown table block ready to paste into
// BENCHMARKS.md.
//
// Position suite is deliberately small and diverse: one starting position,
// one Kiwipete (the hairiest common perft position — many captures, both
// castles available, en-passant), and one closed middlegame. Adding more
// exposes more of the search behaviour; three is enough to show the
// opening/middlegame gap without dragging out the benchmark.

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "chess/board.h"
#include "chess/engine.h"

using namespace chess;
using chess::engine::Engine;
using chess::engine::SearchResult;

struct BenchPos {
    const char* name;
    const char* fen;
};

int main() {
    const std::vector<BenchPos> positions = {
        {"startpos",
         "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"},
        {"kiwipete",
         "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"},
        {"middlegame",
         "r1bq1rk1/pp2bppp/2n1pn2/2pp4/3P4/2P1PN2/PPB2PPP/RNBQ1RK1 w - - 0 1"},
    };

    const std::vector<int> budgets_ms = {2000, 5000, 10000};

    std::printf("| Position    | Budget | Depth |     Nodes | Elapsed (ms) |"
                "        NPS |\n");
    std::printf("|-------------|--------|-------|-----------|--------------|"
                "-----------|\n");

    for (const auto& p : positions) {
        for (int budget : budgets_ms) {
            Board b;
            if (!b.set_from_fen(p.fen)) {
                std::cerr << "FEN parse failed for " << p.name << '\n';
                return 1;
            }
            Engine eng(64);          // fresh 64 MB TT per (position, budget)
            eng.set_position(b);

            auto res = eng.search(budget);
            const double sec = res.elapsed_ms / 1000.0;
            const long long nps = sec > 0
                ? static_cast<long long>(res.nodes / sec)
                : 0LL;
            std::printf("| %-11s | %5dms | %5d | %9ld | %12d | %9lld |\n",
                        p.name, budget, res.depth,
                        res.nodes, res.elapsed_ms, nps);
        }
    }

    return 0;
}
