/**
 * matchmaker.h — ELO-based quick-play matchmaking queue
 *
 * Architecture: Bucketed Spatial Hashing (production-grade)
 *
 * Data Structure:
 *   ┌──────────────────────────────────────────────────────────────────┐
 *   │ unordered_map<TimeControl, multimap<int, QueueEntry>>           │
 *   │   (hash map)               (Red-Black Tree, sorted by ELO)     │
 *   │                                                                 │
 *   │ Bucket "10+5"  →  [ 800:Alice ] → [ 1100:Bob ] → [ 1200:Eve ] │
 *   │ Bucket "3+2"   →  [ 900:Carol ] → [ 1300:Dan ]                │
 *   │ Bucket "1+0"   →  [ 2100:Frank ]                              │
 *   └──────────────────────────────────────────────────────────────────┘
 *
 *   ┌──────────────────────────────────────────────────────────────────┐
 *   │ unordered_map<int, QueueLocation>  (reverse index: fd → entry) │
 *   │                                                                 │
 *   │ fd=10 → { tc="10+5", iterator → Alice }                        │
 *   │ fd=20 → { tc="10+5", iterator → Bob   }                        │
 *   └──────────────────────────────────────────────────────────────────┘
 *
 * Complexity:
 *   - enqueue:   O(log N) — insert into Red-Black Tree
 *   - dequeue:   O(1)     — erase via stored iterator
 *   - is_queued: O(1)     — hash map lookup
 *   - try_match: O(N)     — single linear pass (no sorting needed)
 *   - find nearest match:  O(log N) — lower_bound on the ELO tree
 *
 * Compared to the naive approach (sort + scan):
 *   - enqueue:   O(1) → O(log N)    [slightly slower, but negligible]
 *   - dequeue:   O(N) → O(1)        [massively faster]
 *   - is_queued: O(N) → O(1)        [massively faster]
 *   - try_match: O(N log N) → O(N)  [eliminated the sort entirely]
 *
 * The "fence expansion" metaphor: like a ride-sharing app expanding the
 * search radius, each player's acceptable ELO range widens over time.
 * lower_bound() lets us jump straight to the nearest candidate in O(log N).
 */

#pragma once

#include "core/types.h"
#include "game/room_manager.h"
#include <string>
#include <map>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <functional>

namespace chess {
namespace game {

// ============================================================
// Queue Entry — one player waiting for a match
// ============================================================

struct QueueEntry {
    int         connection_fd;    // WebSocket fd
    PlayerId    player_id;
    std::string username;
    int         elo;              // Current ELO rating
    TimeControl time_control;     // Preferred time control
    std::chrono::steady_clock::time_point enqueue_time;

    /// Calculate the acceptable ELO range based on wait time.
    /// Starts at ±base_range, widens by +50 every 10 seconds.
    int acceptable_range(int base_range = 200) const {
        auto now = std::chrono::steady_clock::now();
        int seconds_waiting = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(now - enqueue_time).count()
        );
        // Every 10 seconds of waiting, widen by 50 ELO points
        int widening = (seconds_waiting / 10) * 50;
        return base_range + widening;
    }
};

// ============================================================
// MatchResult — returned when two players are paired
// ============================================================

struct MatchResult {
    int         white_fd;
    int         black_fd;
    PlayerId    white_id;
    PlayerId    black_id;
    std::string white_name;
    std::string black_name;
    GameId      game_id;
    TimeControl time_control;
};

// Callback type for when a match is found
using MatchCallback = std::function<void(const MatchResult& match)>;

// ============================================================
// TimeControlHash — hash functor for unordered_map keys
// ============================================================
// Combines base_time_ms and increment_ms into a single hash using
// Knuth's multiplicative hash to minimize collisions.

struct TimeControlHash {
    size_t operator()(const TimeControl& tc) const {
        size_t h1 = std::hash<int>()(tc.base_time_ms);
        size_t h2 = std::hash<int>()(tc.increment_ms);
        // Knuth multiplicative hash for mixing
        return h1 ^ (h2 * 2654435761u);
    }
};

// ============================================================
// Matchmaker
// ============================================================

class Matchmaker {
public:
    explicit Matchmaker(RoomManager& room_mgr);

    /// Add a player to the matchmaking queue.
    /// Inserts into the correct TimeControl bucket's ELO tree — O(log N).
    /// Returns false if the player is already queued — O(1) check.
    bool enqueue(int connection_fd, PlayerId player_id,
                 const std::string& username, int elo = 1200,
                 const TimeControl& tc = TimeControl());

    /// Remove a player from the queue using the reverse index — O(1).
    /// Returns true if the player was found and removed.
    bool dequeue(int connection_fd);

    /// Sweep all buckets and match compatible adjacent pairs — O(N).
    /// The ELO tree is already sorted, so no O(N log N) sort needed.
    std::vector<MatchResult> try_match();

    /// Set the callback invoked when a match is found.
    void set_match_callback(MatchCallback cb) { match_cb_ = std::move(cb); }

    /// Check if a player is already in the queue — O(1) via reverse index.
    bool is_queued(int connection_fd) const;

    /// Get the total queue size across all buckets — O(1).
    size_t queue_size() const;

private:
    mutable std::mutex mutex_;
    RoomManager& room_mgr_;
    MatchCallback match_cb_;

    // ── Primary Index ──
    // TimeControl → Red-Black Tree of players sorted by ELO.
    // multimap allows multiple players at the same ELO rating.
    using EloTree = std::multimap<int, QueueEntry>;
    std::unordered_map<TimeControl, EloTree, TimeControlHash> buckets_;

    // ── Reverse Index ──
    // connection_fd → location in the tree (for O(1) dequeue + O(1) duplicate check).
    struct QueueLocation {
        TimeControl tc;
        EloTree::iterator it;
    };
    std::unordered_map<int, QueueLocation> fd_index_;

    // ── Cached Size ──
    // Avoids iterating all buckets to count total players.
    size_t total_size_ = 0;
};

} // namespace game
} // namespace chess
