/**
 * Verify that bundled puzzle metadata stays aligned with the authoritative
 * C++ rules engine. The browser uses the checked legal_moves list only for
 * offline puzzle highlighting and feedback; live games remain server-driven.
 */

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "chess/board.h"
#include "chess/move_gen.h"

using json = nlohmann::json;

int main() {
    const std::string path = std::string(CHESS_SOURCE_DIR)
        + "/frontend/assets/puzzles.json";
    std::ifstream input(path);
    if (!input) {
        std::cerr << "Could not open " << path << '\n';
        return 1;
    }

    json document;
    input >> document;
    const auto& puzzles = document.at("puzzles");
    int passed = 0;
    int failed = 0;

    for (const auto& puzzle : puzzles) {
        const std::string id = puzzle.value("id", "<missing-id>");
        chess::Board board;
        if (!board.set_from_fen(puzzle.value("fen", ""))) {
            std::cerr << "FAIL " << id << ": invalid FEN\n";
            ++failed;
            continue;
        }

        const auto generated = chess::move_gen::generate_legal_moves(board);
        std::set<std::string> expected;
        for (const auto& move : generated) {
            expected.insert(move.to_uci());
        }

        std::set<std::string> bundled;
        if (puzzle.contains("legal_moves") && puzzle["legal_moves"].is_array()) {
            for (const auto& move : puzzle["legal_moves"]) {
                bundled.insert(move.get<std::string>());
            }
        }

        const std::string solution = puzzle.at("solution").at(0).get<std::string>();
        const bool solution_is_legal = expected.count(solution) > 0;
        bool promised_mate = true;
        if (solution_is_legal
            && puzzle.value("prompt", "").find("mate") != std::string::npos) {
            auto chosen = std::find_if(generated.begin(), generated.end(),
                [&](const chess::Move& move) { return move.to_uci() == solution; });
            chess::Board after = board;
            after.make_move(*chosen);
            promised_mate = chess::move_gen::get_game_status(after)
                == chess::GameStatus::CHECKMATE;
        }
        if (!solution_is_legal || !promised_mate || bundled != expected) {
            std::cerr << "FAIL " << id << ":";
            if (!solution_is_legal) std::cerr << " solution is not legal;";
            if (!promised_mate) std::cerr << " promised mate does not checkmate;";
            if (bundled != expected) {
                std::cerr << " legal_moves should be [";
                bool first = true;
                for (const auto& move : expected) {
                    if (!first) std::cerr << ", ";
                    std::cerr << '"' << move << '"';
                    first = false;
                }
                std::cerr << "]";
            }
            std::cerr << '\n';
            ++failed;
            continue;
        }

        std::cout << "PASS " << id << '\n';
        ++passed;
    }

    std::cout << "Results: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
