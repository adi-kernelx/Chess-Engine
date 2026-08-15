/**
 * engine.cpp — Alpha-beta minimax search engine
 *
 * ALGORITHM OVERVIEW
 * ──────────────────
 * Minimax: At each node, the side to move picks the move that maximizes
 * their score; at the opponent's node, they minimize. The recursive tree
 * alternates between "maximize" and "minimize" at each ply.
 *
 * Negamax formulation: Instead of tracking min/max separately, we negate
 * the score at each return:
 *
 *   negamax(pos, depth) = max over moves of -negamax(child, depth-1)
 *
 * This works because "good for me = bad for you" — the score is always
 * from the perspective of the player about to move.
 *
 * Alpha-Beta Pruning: We track:
 *   alpha = the best score we can guarantee (our lower bound)
 *   beta  = what the opponent will allow (our upper bound)
 *
 * If we find a move that scores >= beta, we stop searching this branch
 * entirely (beta cutoff). The opponent would never allow us to reach this
 * position because they already have a better option elsewhere.
 *
 * Iterative Deepening: We search depth 1, then 2, then 3, etc.
 * Each completed depth updates the best_move. If time runs out mid-search,
 * we return the best move from the last completed depth.
 *
 * Move Ordering (MVV-LVA): Good move ordering dramatically improves
 * alpha-beta efficiency. Searching good moves first (captures that win
 * material) produces tight alpha/beta windows early, cutting off more branches.
 *
 * With random ordering, alpha-beta examines O(b^d) nodes.
 * With perfect ordering, it examines O(b^(d/2)) — the square root — effectively
 * doubling searchable depth for the same time budget.
 */

#include "chess/engine.h"
#include "chess/evaluator.h"
#include "chess/move_gen.h"
#include "core/types.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace chess {
namespace engine {

// ============================================================
// MVV-LVA lookup table
//
// Table[victim][attacker] → capture score
// Higher = search this capture first.
// Victim values dominate: always prefer capturing the most valuable piece,
// using the least valuable attacker as a tiebreaker.
//
// Victim:   P=100,  N=320,  B=330,  R=500,  Q=900
// Attacker: P=100,  N=320,  B=330,  R=500,  Q=900,  K=large
//
// MVV-LVA score = victim_value * 10 - attacker_value
// (factor of 10 ensures victim dominates attacker in ordering)
// ============================================================

static const int MVV_LVA_VICTIM[7]   = { 100, 320, 330, 500, 900, 20000, 0 }; // by PieceType index
static const int MVV_LVA_ATTACKER[7] = { 100, 320, 330, 500, 900, 20000, 0 };

// ============================================================
// Engine implementation
// ============================================================

Engine::Engine(size_t tt_size_mb)
    : tt_(tt_size_mb), stop_flag_(false), nodes_searched_(0)
{
    board_ = Board::starting_position();
}

void Engine::set_position(const Board& board) {
    board_ = board;
}

void Engine::stop() {
    stop_flag_.store(true, std::memory_order_relaxed);
}

bool Engine::time_up() const {
    if (stop_flag_.load(std::memory_order_relaxed)) return true;
    return std::chrono::steady_clock::now() >= deadline_;
}

// ============================================================
// MVV-LVA score for a capture
// ============================================================

int Engine::mvv_lva_score(PieceType victim, PieceType attacker) {
    const int vi = static_cast<int>(victim);
    const int ai = static_cast<int>(attacker);
    // Multiply victim value by 10 so victim always dominates the sort
    return MVV_LVA_VICTIM[vi] * 10 - MVV_LVA_ATTACKER[ai];
}

// ============================================================
// Move ordering: TT move first, then captures (MVV-LVA), then quiet moves
// ============================================================

void Engine::order_moves(std::vector<Move>& moves, const Board& board, const Move& tt_move) {
    // Score each move for ordering
    std::vector<std::pair<int, int>> scores; // (score, original_index)
    scores.reserve(moves.size());

    const bool has_tt_move = (tt_move.from != tt_move.to || tt_move.flags != 0);

    for (int i = 0; i < static_cast<int>(moves.size()); ++i) {
        const Move& m = moves[i];
        int score = 0;

        // 1. Transposition Table move gets the absolute highest priority
        if (has_tt_move && m.from == tt_move.from && m.to == tt_move.to && m.promo_type == tt_move.promo_type) {
            score = 30'000;
        } else {
            const Piece victim = board.piece_at(m.to);
            if (!victim.is_none()) {
                // 2. Capture: score by MVV-LVA (large positive values)
                const Piece attacker = board.piece_at(m.from);
                score = 10'000 + mvv_lva_score(victim.type, attacker.type);
            } else if (m.is_en_passant()) {
                // En passant is a pawn capture
                score = 10'000 + mvv_lva_score(PieceType::PAWN, PieceType::PAWN);
            }
            // 3. Quiet moves get score 0
        }

        scores.push_back({score, i});
    }

    // Sort by score descending
    std::sort(scores.begin(), scores.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    // Rearrange moves in-place according to sorted order
    std::vector<Move> ordered;
    ordered.reserve(moves.size());
    for (auto& [sc, idx] : scores) {
        ordered.push_back(moves[idx]);
    }
    moves = std::move(ordered);
}

// ============================================================
// Alpha-beta negamax search with Transposition Table
// ============================================================

int Engine::alpha_beta(Board& board, int depth, int ply, int alpha, int beta) {
    ++nodes_searched_;

    // Check time limit every 2048 nodes (avoids syscall overhead on every node)
    if ((nodes_searched_ & 2047) == 0 && time_up()) {
        // Return a neutral score; the caller will discard this result
        return 0;
    }

    // Fifty-move rule draw
    if (board.halfmove_clock() >= 100) {
        return SCORE_DRAW;
    }

    const uint64_t key = board.zobrist_key();
    Move tt_move = Move{};
    int tt_score = 0;

    // Transposition table probe (skip cutoff at root ply, but retrieve best move)
    if (ply > 0) {
        if (tt_.probe(key, depth, ply, alpha, beta, tt_score, tt_move)) {
            return tt_score;
        }
    } else {
        tt_.probe_move(key, tt_move);
    }

    // Base case: evaluate the leaf position
    if (depth == 0) {
        return evaluate(board);
    }

    // Generate all legal moves
    auto moves = move_gen::generate_legal_moves(board);

    // Terminal position detection
    if (moves.empty()) {
        if (move_gen::is_in_check(board, board.side_to_move())) {
            // Checkmate: return large negative score penalized by distance from root
            return -(SCORE_MATE - ply);
        }
        // Stalemate
        return SCORE_DRAW;
    }

    // Order moves (TT move -> MVV-LVA captures -> quiet moves)
    order_moves(moves, board, tt_move);

    const int orig_alpha = alpha;
    int best_score = -SCORE_INFINITY;
    Move best_move = moves[0];

    for (const Move& m : moves) {
        // Make the move
        Board::UndoInfo undo = board.make_move(m);

        // Recurse (negate score because perspective flips)
        const int score = -alpha_beta(board, depth - 1, ply + 1, -beta, -alpha);

        // Undo the move
        board.undo_move(m, undo);

        // Abort if time expired mid-search
        if (time_up() && (nodes_searched_ & 2047) == 0) {
            return best_score; // Return what we have so far
        }

        if (score > best_score) {
            best_score = score;
            best_move = m;
        }

        if (score > alpha) {
            alpha = score;
        }

        // Beta cutoff: opponent would not allow this position
        if (alpha >= beta) {
            break; // Prune remaining moves in this node
        }
    }

    // Store evaluation in Transposition Table if search wasn't aborted
    if (!time_up()) {
        TTNodeType node_type;
        if (best_score >= beta) {
            node_type = TTNodeType::LOWER_BOUND;
        } else if (best_score > orig_alpha) {
            node_type = TTNodeType::EXACT;
        } else {
            node_type = TTNodeType::UPPER_BOUND;
        }
        tt_.store(key, depth, ply, best_score, node_type, best_move);
    }

    return best_score;
}

// ============================================================
// Iterative deepening — the main public search function
// ============================================================

SearchResult Engine::search(int time_limit_ms, int max_depth) {
    stop_flag_.store(false, std::memory_order_relaxed);
    nodes_searched_ = 0;

    const auto start = std::chrono::steady_clock::now();
    deadline_ = start + std::chrono::milliseconds(time_limit_ms);

    // Notify TT of a new search cycle for aging
    tt_.new_search();

    SearchResult result;
    result.best_move = Move{}; // No move yet

    // Get the initial legal moves so we have a fallback
    auto root_moves = move_gen::generate_legal_moves(board_);
    if (root_moves.empty()) {
        return result; // No legal moves (checkmate/stalemate)
    }

    // Start with the first legal move as a safety fallback
    result.best_move = root_moves[0];

    // ── Iterative Deepening Loop ──
    // Search at depth 1, then 2, then 3, ... until time runs out.
    // Each completed depth updates result.best_move, so if we abort
    // mid-depth, we always return the best move from the last COMPLETE depth.

    for (int depth = 1; depth <= max_depth; ++depth) {
        if (time_up()) break;

        Move best_at_this_depth = root_moves[0];
        int  best_score_at_this_depth = -SCORE_INFINITY;
        int  alpha = -SCORE_INFINITY;
        const int beta = SCORE_INFINITY;

        // Try probing TT for best move from previous searches
        Move tt_root_move = result.best_move;
        tt_.probe_move(board_.zobrist_key(), tt_root_move);

        // Order root moves
        order_moves(root_moves, board_, tt_root_move);

        for (const Move& m : root_moves) {
            if (time_up()) break;

            Board::UndoInfo undo = board_.make_move(m);
            const int score = -alpha_beta(board_, depth - 1, 1, -beta, -alpha);
            board_.undo_move(m, undo);

            if (score > best_score_at_this_depth) {
                best_score_at_this_depth = score;
                best_at_this_depth = m;
            }

            if (score > alpha) {
                alpha = score;
            }
        }

        // Only update the result if this depth was fully searched
        if (!time_up()) {
            result.best_move = best_at_this_depth;
            result.score     = best_score_at_this_depth;
            result.depth     = depth;

            // Store root position in TT
            tt_.store(board_.zobrist_key(), depth, 0, best_score_at_this_depth, TTNodeType::EXACT, best_at_this_depth);
        }

        // If we found a forced mate, no need to search deeper
        if (std::abs(result.score) >= SCORE_MATE - 100) {
            break;
        }
    }

    const auto end = std::chrono::steady_clock::now();
    result.nodes      = nodes_searched_;
    result.elapsed_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count()
    );

    return result;
}

} // namespace engine
} // namespace chess
