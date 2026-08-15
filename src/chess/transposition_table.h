/**
 * transposition_table.h — Transposition Table (TT) for Chess AI Search
 *
 * Caches previously evaluated subtrees to:
 *   1. Avoid re-searching identical board states reached via transpositions
 *   2. Provide the best move for high-priority move ordering (TT move)
 *   3. Narrow search windows or trigger immediate alpha-beta cutoffs
 *
 * Implements:
 *   - 64-bit Zobrist key matching
 *   - Depth-preferred replacement with aging
 *   - Mate score normalization relative to search ply
 *   - Configurable memory size with power-of-two mask indexing
 */

#pragma once

#include "core/types.h"
#include "chess/move.h"
#include <vector>
#include <cstdint>
#include <cstddef>

namespace chess {
namespace engine {

/// Type of bound stored in the transposition table entry
enum class TTNodeType : uint8_t {
    NONE = 0,
    EXACT,        ///< PV-Node: exact minimax evaluation score
    LOWER_BOUND,  ///< Cut-Node / Fail-High: beta cutoff (real score >= stored score)
    UPPER_BOUND   ///< All-Node / Fail-Low: alpha cutoff (real score <= stored score)
};

/// A single entry in the transposition table
struct TTEntry {
    uint64_t   key       = 0;                ///< 64-bit Zobrist hash for verification
    Move       best_move = Move{};           ///< Best move found from this position
    int32_t    score     = 0;                ///< Score in centipawns (ply-normalized)
    int8_t     depth     = 0;                ///< Search depth at which entry was stored
    TTNodeType node_type = TTNodeType::NONE; ///< Bound type (EXACT, LOWER_BOUND, UPPER_BOUND)
    uint8_t    age       = 0;                ///< Search generation counter
};

/// Transposition table with fixed memory footprint
class TranspositionTable {
public:
    /// Construct table with given size in megabytes (default 32 MB)
    explicit TranspositionTable(size_t size_mb = 32);

    /// Resize table to specified megabytes (allocates power-of-two number of entries)
    void resize(size_t size_mb);

    /// Clear all entries and reset generation counter
    void clear();

    /// Advance search generation (call at the beginning of each search iteration/move)
    void new_search() { ++age_; }

    /**
     * Store a search result in the table.
     *
     * @param key        64-bit Zobrist hash of the position
     * @param depth      Search depth
     * @param ply        Distance from search root (for mate score normalization)
     * @param score      Evaluated score
     * @param type       Bound type (EXACT, LOWER_BOUND, UPPER_BOUND)
     * @param best_move  Best move found at this node
     */
    void store(uint64_t key, int depth, int ply, int score, TTNodeType type, const Move& best_move);

    /**
     * Probe table for a cutoff or score retrieval.
     *
     * @param key        64-bit Zobrist hash
     * @param depth      Target search depth
     * @param ply        Distance from search root
     * @param alpha      Current alpha bound
     * @param beta       Current beta bound
     * @param out_score  Output score if probe produces a cutoff
     * @param out_move   Output best move if entry exists (even if depth is insufficient for cutoff)
     * @return           True if a valid cutoff score is produced
     */
    bool probe(uint64_t key, int depth, int ply, int alpha, int beta, int& out_score, Move& out_move) const;

    /**
     * Retrieve best move stored for a position (for move ordering).
     *
     * @param key       64-bit Zobrist hash
     * @param out_move  Output best move
     * @return          True if position exists in table and has a valid best move
     */
    bool probe_move(uint64_t key, Move& out_move) const;

    /// Number of entry slots in the table
    size_t capacity() const { return entries_.size(); }

    /// Approximate table occupancy in permille [0..1000]
    int hashfull() const;

    /// Normalize mate scores to be independent of ply before storing
    static int score_to_tt(int score, int ply);

    /// Convert stored score back to current search ply perspective
    static int score_from_tt(int score, int ply);

private:
    std::vector<TTEntry> entries_;
    size_t mask_ = 0;
    uint8_t age_ = 0;
};

} // namespace engine
} // namespace chess
