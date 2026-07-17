/**
 * matchmaker.h — ELO-based quick-play matchmaking queue
 *
 * When a player clicks "Quick Play", they are added to a matchmaking queue
 * with their ELO rating. The matchmaker periodically scans the queue and
 * pairs players whose ELO ratings are within a configurable range.
 *
 * Key design decisions:
 *   - Range widening: If a player has been waiting for N seconds, their
 *     acceptable ELO range expands. This prevents high/low-rated players
 *     from waiting forever.
 *   - Thread safety: The queue is protected by a mutex. The matching logic
 *     runs on a worker thread via the thread pool.
 *   - On match: A GameRoom is automatically created via the RoomManager,
 *     and both players receive a "match_found" notification with the game ID.
 */

#pragma once

#include "core/types.h"
#include "game/room_manager.h"
#include <string>
#include <vector>
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
// Matchmaker
// ============================================================

class Matchmaker {
public:
    explicit Matchmaker(RoomManager& room_mgr);

    /// Add a player to the matchmaking queue.
    /// Returns false if the player is already queued.
    bool enqueue(int connection_fd, PlayerId player_id,
                 const std::string& username, int elo = 1200,
                 const TimeControl& tc = TimeControl());

    /// Remove a player from the queue (e.g., they disconnected or cancelled).
    /// Returns true if the player was found and removed.
    bool dequeue(int connection_fd);

    /// Try to match players in the queue. Returns all matches found.
    /// This should be called periodically (e.g., every second).
    std::vector<MatchResult> try_match();

    /// Set the callback invoked when a match is found.
    void set_match_callback(MatchCallback cb) { match_cb_ = std::move(cb); }

    /// Check if a player is already in the queue.
    bool is_queued(int connection_fd) const;

    /// Get the current queue size.
    size_t queue_size() const;

private:
    mutable std::mutex mutex_;
    std::vector<QueueEntry> queue_;
    RoomManager& room_mgr_;
    MatchCallback match_cb_;
};

} // namespace game
} // namespace chess
