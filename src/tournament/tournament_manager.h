/**
 * tournament_manager.h — Phase 9.4 lifecycle glue.
 *
 * The manager owns the state machine that ties the pure Swiss algorithm
 * (swiss.cpp) to the storage layer (tournament_repo.cpp) and to callers
 * higher up the stack — WebSocket handlers today, and a full frontend
 * screen later.
 *
 * STATE MACHINE
 *
 *     registration ── start_tournament ──► in_progress (round 1)
 *                                                │
 *                       report_result completes each round ──► advances current_round
 *                                                │
 *                       final result recorded ──► completed
 *
 * Every mutating call is stateless in the process — the manager holds
 * no in-memory cache. That means two servers hitting the same Postgres
 * see the same story. The pattern is: read a tournament and its
 * participants + pairings, compute, then write back what changed.
 *
 * WHAT DOES *NOT* LIVE HERE
 *
 *   * GameRoom construction. The plan mentions "auto-create game rooms
 *     for each round's pairings"; the room API assumes a WAITING →
 *     creator + joiner flow, and rewriting it for tournaments cleanly
 *     needs a new constructor variant. That work is deferred; see the
 *     log narrative for the rationale. In this phase, pairings live as
 *     rows with `game_id` NULL, and results are reported via
 *     `report_result` — the callable the game-over hook (or a test)
 *     invokes once a pairing's game is decided.
 *
 *   * Colour balancing across rounds. The Swiss algorithm handles that;
 *     the manager only reads `whites_played` off participants and
 *     bumps the value when it records a result.
 *
 * TIEBREAKS
 *
 * `standings_view` returns a snapshot with Buchholz tiebreaks computed
 * on the fly (sum of opponents' scores, with a bye counted as playing
 * a phantom opponent whose score equals the byer's own current score —
 * the "median" variant, common at amateur level). Buchholz is derived,
 * not stored — the source of truth stays in tournament_players.score.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "tournament/swiss.h"
#include "tournament/tournament_repo.h"
#include "storage/database.h"
#include "application/ports/clock.h"

namespace chess {
namespace tournament {

/// One row of the standings view. Fields are what a scoreboard consumer
/// (WebSocket or UI) needs, no more.
struct StandingRow {
    int64_t player_id      = 0;
    int     initial_elo    = 0;
    double  score          = 0.0;
    double  buchholz       = 0.0;  ///< derived — sum of opponents' scores
    int     whites_played  = 0;
    bool    received_bye   = false;
    bool    withdrawn      = false;
};

/// Full snapshot of a tournament. `tournament` is authoritative; the
/// other two are derived by reading from Postgres inside get_state.
struct TournamentState {
    StoredTournament                    tournament;
    std::vector<StandingRow>            standings;    ///< sorted best-first
    std::vector<StoredPairing>          all_pairings; ///< every round, in DB order
    std::vector<StoredTournamentRound>  rounds;
};

// ── Result / error types ─────────────────────────────────────────────

struct ManagerResult {
    bool        ok = false;
    std::string error;
};

class TournamentManager {
public:
    explicit TournamentManager(storage::Database& db,
        application::ports::Clock& clock = application::ports::default_clock())
        : db_(db), clock_(clock) {}

    static bool valid_schedule(int64_t registration_deadline_unix,
                               int64_t first_round_start_unix,
                               int round_duration_seconds);
    static int64_t earliest_round_start(int64_t first_round_start_unix,
                                        int round_duration_seconds, int round);
    static std::optional<std::string> no_show_result(bool white_checked_in,
                                                     bool black_checked_in);

    /// Register a new player into a tournament. Rejects if the
    /// tournament is not in `registration`, if the tournament does
    /// not exist, or if the player row is not found. Duplicate joins
    /// are a no-op (idempotent).
    ManagerResult join(int64_t tournament_id, int64_t player_id, int player_elo);
    ManagerResult leave(int64_t tournament_id, int64_t player_id);

    /// Transition a tournament from `registration` to `in_progress`.
    /// Requires at least 2 non-withdrawn participants. Pairs the first
    /// round immediately. `initiator_id` must equal the tournament's
    /// created_by — anyone else is refused.
    ManagerResult start(int64_t tournament_id, int64_t initiator_id);

    ManagerResult set_registration(int64_t tournament_id, int64_t initiator_id,
                                   bool open);
    ManagerResult check_in(int64_t tournament_id, int round, int64_t player_id);
    ManagerResult override_result(int64_t pairing_id, int64_t initiator_id,
                                  const std::string& result,
                                  const std::string& reason);
    ManagerResult record_game_result(int64_t pairing_id,
                                     const std::string& result);
    ManagerResult maintenance_tick();

    /// Record the outcome of one pairing. `result` is one of
    /// "1-0", "0-1", "1/2-1/2", or "bye" (byes are auto-generated but
    /// callers can pass 'bye' to record they were awarded correctly).
    /// Updates the pairing row, bumps participants' scores/colours,
    /// and if every pairing in the current round is decided, advances
    /// to the next round — either by generating new pairings or, if
    /// this was the last round, by flipping status to `completed`.
    ManagerResult report_result(int64_t pairing_id, const std::string& result);

    /// Return a full snapshot suitable for a status endpoint.
    std::optional<TournamentState> get_state(int64_t tournament_id);

private:
    /// Generate pairings for `round`, insert them into the pairings
    /// table, and auto-record any bye as a 1-point score for the byer.
    /// Returns ok=false with `error` set on any pairing insertion failure.
    ManagerResult generate_round_pairings(int64_t tournament_id, int round);

    /// If every pairing in `round` has a non-pending result, advance
    /// the tournament (either to the next round or to completed).
    ManagerResult maybe_advance_after_result(int64_t tournament_id,
                                             int round);
    ManagerResult start_due_round(int64_t tournament_id, int round, int64_t now_unix);
    ManagerResult adjudicate_no_shows(int64_t tournament_id, int round);

    /// Read the set of pairs {white, black} already played, for the
    /// rematch-avoidance input to the Swiss algorithm.
    std::set<PlayerPair> gather_played(const std::vector<StoredPairing>& pairings) const;

    storage::Database& db_;
    application::ports::Clock& clock_;
};

} // namespace tournament
} // namespace chess
