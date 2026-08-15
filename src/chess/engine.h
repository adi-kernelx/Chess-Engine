/**
 * engine.h — Chess AI search engine interface
 *
 * Implements iterative deepening alpha-beta search with:
 *   - Alpha-beta pruning (eliminates provably irrelevant branches)
 *   - MVV-LVA move ordering (Most Valuable Victim / Least Valuable Attacker)
 *   - Time-based search (search until a deadline, not a fixed depth)
 *
 * Public API:
 *   Engine engine;
 *   engine.set_position(board);
 *   SearchResult result = engine.search(2000);  // search for 2 seconds
 *   Move best = result.best_move;
 *
 * Future phases will add:
 *   - Transposition table (Phase 6.3)
 *   - Quiescence search (Phase 6.4)
 */

#pragma once

#include "chess/board.h"
#include "chess/move.h"
#include "chess/transposition_table.h"
#include <chrono>
#include <atomic>
#include <vector>

namespace chess {
namespace engine {

// Sentinel score values used during search
constexpr int SCORE_INFINITY  =  1'000'000;
constexpr int SCORE_MATE      =    900'000;  // Winning by checkmate
constexpr int SCORE_DRAW      =          0;

/**
 * The result returned by Engine::search().
 */
struct SearchResult {
    Move best_move;          ///< Best move found (default-constructed = no move)
    int  score        = 0;   ///< Score in centipawns from side-to-move's perspective
    int  depth        = 0;   ///< Depth actually reached
    long nodes        = 0;   ///< Total nodes searched
    int  elapsed_ms   = 0;   ///< Wall-clock time used (ms)
};

/**
 * Engine — alpha-beta search over a chess board.
 *
 * Not thread-safe: create one instance per AI player.
 */
class Engine {
public:
    explicit Engine(size_t tt_size_mb = 32);

    /**
     * Set the position to search from.
     * @param board  The current board position (copied internally)
     */
    void set_position(const Board& board);

    /**
     * Search for the best move using iterative deepening alpha-beta with TT.
     *
     * Searches to increasing depths (1, 2, 3, ...) until the time limit
     * is exceeded. Returns the best move found at the deepest complete depth.
     *
     * @param time_limit_ms  Maximum search time in milliseconds
     * @param max_depth      Hard depth ceiling (default = 64)
     * @return               SearchResult with best move, score, and stats
     */
    SearchResult search(int time_limit_ms, int max_depth = 64);

    /**
     * Stop a running search (thread-safe, uses atomic flag).
     * Called when time runs out or the operator wants to abort.
     */
    void stop();

    /// Transposition table accessors
    TranspositionTable& tt() { return tt_; }
    const TranspositionTable& tt() const { return tt_; }
    void clear_tt() { tt_.clear(); }
    void set_tt_size(size_t mb) { tt_.resize(mb); }

private:
    Board board_;
    TranspositionTable tt_;
    std::atomic<bool> stop_flag_;

    // Search statistics (reset on each search() call)
    long nodes_searched_;

    // Deadline for the current search
    std::chrono::steady_clock::time_point deadline_;

    /**
     * Alpha-beta negamax search with Transposition Table cutoffs.
     *
     * @param board   Current position (modified in-place, restored after)
     * @param depth   Remaining depth to search
     * @param ply     Distance from search root
     * @param alpha   Current lower bound
     * @param beta    Current upper bound
     * @return        Best score found from this position
     */
    int alpha_beta(Board& board, int depth, int ply, int alpha, int beta);

    /**
     * Order moves for better alpha-beta pruning.
     *
     * Priority:
     *   1. Transposition table move (if present and valid)
     *   2. Captures (sorted by MVV-LVA)
     *   3. Quiet moves
     *
     * @param moves    Move list to sort (sorted in-place)
     * @param board    Board for piece lookup
     * @param tt_move  Move suggested by the transposition table
     */
    void order_moves(std::vector<Move>& moves, const Board& board, const Move& tt_move = Move{});

    /**
     * MVV-LVA score for a capture move.
     * Higher score = search this capture first.
     *
     * @param victim    The piece being captured
     * @param attacker  The piece making the capture
     * @return          MVV-LVA heuristic score
     */
    static int mvv_lva_score(PieceType victim, PieceType attacker);

    /**
     * Check if the time limit has been exceeded.
     * @return true if search should stop
     */
    bool time_up() const;
};

} // namespace engine
} // namespace chess
