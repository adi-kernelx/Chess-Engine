/**
 * transposition_table.cpp — Transposition Table Implementation
 */

#include "chess/transposition_table.h"
#include <algorithm>

namespace chess {
namespace engine {

namespace {
constexpr int TT_MATE_BOUND = 800'000;
}

TranspositionTable::TranspositionTable(size_t size_mb) {
    resize(size_mb);
}

void TranspositionTable::resize(size_t size_mb) {
    if (size_mb == 0) size_mb = 1;
    const size_t target_bytes = size_mb * 1024 * 1024;
    const size_t num_entries = target_bytes / sizeof(TTEntry);

    // Compute largest power of two <= num_entries
    size_t power_of_two = 1;
    while ((power_of_two << 1) <= num_entries && (power_of_two << 1) != 0) {
        power_of_two <<= 1;
    }

    entries_.assign(power_of_two, TTEntry{});
    mask_ = power_of_two - 1;
    age_ = 0;
}

void TranspositionTable::clear() {
    std::fill(entries_.begin(), entries_.end(), TTEntry{});
    age_ = 0;
}

int TranspositionTable::score_to_tt(int score, int ply) {
    if (score >= TT_MATE_BOUND) {
        return score + ply;
    }
    if (score <= -TT_MATE_BOUND) {
        return score - ply;
    }
    return score;
}

int TranspositionTable::score_from_tt(int score, int ply) {
    if (score >= TT_MATE_BOUND) {
        return score - ply;
    }
    if (score <= -TT_MATE_BOUND) {
        return score + ply;
    }
    return score;
}

void TranspositionTable::store(uint64_t key, int depth, int ply, int score, TTNodeType type, const Move& best_move) {
    if (entries_.empty()) return;

    const size_t index = key & mask_;
    TTEntry& entry = entries_[index];

    // Replacement logic:
    // 1. Empty slot: always replace
    // 2. Same position: replace if new depth is greater or equal, or if new entry is EXACT
    // 3. Different position from previous search generation: replace
    // 4. Different position from current generation: replace if new depth >= old depth
    bool replace = false;
    if (entry.key == 0) {
        replace = true;
    } else if (entry.key == key) {
        if (depth >= entry.depth || type == TTNodeType::EXACT) {
            replace = true;
        }
    } else if (entry.age != age_) {
        replace = true;
    } else if (depth >= entry.depth) {
        replace = true;
    }

    if (replace) {
        // If the new move is empty but the existing entry had a valid move for this key, keep the existing move
        Move move_to_store = best_move;
        if (move_to_store.from == move_to_store.to && entry.key == key) {
            move_to_store = entry.best_move;
        }

        entry.key = key;
        entry.best_move = move_to_store;
        entry.score = score_to_tt(score, ply);
        entry.depth = static_cast<int8_t>(depth);
        entry.node_type = type;
        entry.age = age_;
    }
}

bool TranspositionTable::probe(uint64_t key, int depth, int ply, int alpha, int beta, int& out_score, Move& out_move) const {
    if (entries_.empty()) return false;

    const size_t index = key & mask_;
    const TTEntry& entry = entries_[index];

    if (entry.key != key || entry.node_type == TTNodeType::NONE) {
        return false;
    }

    // Always output stored move for move ordering
    out_move = entry.best_move;

    // Check if search depth of entry is sufficient for cutoff
    if (entry.depth >= depth) {
        const int score = score_from_tt(entry.score, ply);

        if (entry.node_type == TTNodeType::EXACT) {
            out_score = score;
            return true;
        }
        if (entry.node_type == TTNodeType::LOWER_BOUND && score >= beta) {
            out_score = score;
            return true;
        }
        if (entry.node_type == TTNodeType::UPPER_BOUND && score <= alpha) {
            out_score = score;
            return true;
        }
    }

    return false;
}

bool TranspositionTable::probe_move(uint64_t key, Move& out_move) const {
    if (entries_.empty()) return false;

    const size_t index = key & mask_;
    const TTEntry& entry = entries_[index];

    if (entry.key == key && entry.node_type != TTNodeType::NONE) {
        out_move = entry.best_move;
        return true;
    }
    return false;
}

int TranspositionTable::hashfull() const {
    if (entries_.empty()) return 0;
    const size_t sample_size = std::min<size_t>(entries_.size(), 1000);
    size_t occupied = 0;
    for (size_t i = 0; i < sample_size; ++i) {
        if (entries_[i].key != 0 && entries_[i].age == age_) {
            ++occupied;
        }
    }
    return static_cast<int>((occupied * 1000) / sample_size);
}

} // namespace engine
} // namespace chess
