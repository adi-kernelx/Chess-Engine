/**
 * tournament_repo.h — Postgres persistence for Phase 9.4 tournaments.
 *
 * Pure storage layer. Knows nothing about GameRoom, WebSocket, or the
 * Swiss algorithm itself. Callers assemble the domain types (defined in
 * swiss.h and here) and hand them to these functions; they hand back
 * rows read from disk.
 *
 * The three tables — tournaments, tournament_players, tournament_pairings —
 * are documented in the 0003 migration. This header covers the C++ side
 * of the round-trip.
 *
 * Parameter binding uses `PQexecParams` throughout; no query concatenates
 * caller-supplied text into SQL, matching the rule set in database.h.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "storage/database.h"

namespace chess {
namespace tournament {

// ── Stored row types ─────────────────────────────────────────────────

struct StoredTournament {
    int64_t     id                        = 0;
    std::string name;
    std::string format;                       ///< only "swiss" for now
    int         rounds                    = 0;
    int         current_round             = 0;
    int         time_control_initial_ms   = 0;
    int         time_control_increment_ms = 0;
    std::string status;                       ///< "registration" | "in_progress" | "completed"
    int64_t     created_by                = 0;
    std::string created_at;                   ///< ISO 8601
    std::string started_at;                   ///< ISO 8601, empty if NULL
    std::string completed_at;                 ///< ISO 8601, empty if NULL
};

struct StoredTournamentPlayer {
    int64_t player_id     = 0;
    int     initial_elo   = 0;
    double  score         = 0.0;
    int     whites_played = 0;
    bool    received_bye  = false;
    bool    withdrawn     = false;
    std::string registered_at;
};

struct StoredPairing {
    int64_t                id                = 0;
    int64_t                tournament_id     = 0;
    int                    round             = 0;
    int64_t                white_player_id   = 0;
    std::optional<int64_t> black_player_id;              ///< empty ⇔ bye
    std::optional<int64_t> game_id;                      ///< empty until wired
    std::string            result;                       ///< "pending" | "1-0" | "0-1" | "1/2-1/2" | "bye"
    std::string            created_at;
};

// ── Result types ─────────────────────────────────────────────────────

struct CreateTournamentResult {
    bool        ok = false;
    int64_t     id = 0;
    std::string error;
};

// ── API ──────────────────────────────────────────────────────────────

/// Insert a new tournament row in status='registration'. Returns the new id.
CreateTournamentResult create_tournament(storage::Database& db,
                                         const std::string& name,
                                         int rounds,
                                         int time_control_initial_ms,
                                         int time_control_increment_ms,
                                         int64_t created_by);

/// Load a tournament by id. Returns nullopt if missing.
std::optional<StoredTournament> find_tournament(storage::Database& db, int64_t id);

/// List tournaments in the given status (or "" for all), newest first.
std::vector<StoredTournament> list_tournaments(storage::Database& db,
                                               const std::string& status,
                                               int limit = 50);

/// Update the tournament's status column. On success, sets started_at
/// when transitioning to in_progress and completed_at when transitioning
/// to completed (via now() on the DB side). Returns false if the row does
/// not exist or the DB rejects the update.
bool set_tournament_status(storage::Database& db,
                           int64_t tournament_id,
                           const std::string& new_status);

/// Set the current_round column on a tournament.
bool set_tournament_round(storage::Database& db,
                          int64_t tournament_id,
                          int new_round);

/// Register a player into a tournament in status='registration'. Idempotent
/// on the composite (tournament_id, player_id) key — a repeat call is a
/// no-op and returns ok=true, so a client resend does not error. Returns
/// ok=false with `error` set if the tournament is not in registration or
/// the player row was not found.
struct JoinTournamentResult {
    bool        ok = false;
    std::string error;
};
JoinTournamentResult add_participant(storage::Database& db,
                                     int64_t tournament_id,
                                     int64_t player_id,
                                     int initial_elo);

/// Load all participants of a tournament.
std::vector<StoredTournamentPlayer> get_participants(storage::Database& db,
                                                     int64_t tournament_id);

/// Apply a per-round delta to one participant: adds `score_delta`, sets
/// received_bye if `bye`, and increments whites_played if `played_white`.
/// The three flags are combined into one UPDATE so a round-close writes
/// each participant exactly once.
bool bump_participant(storage::Database& db,
                      int64_t tournament_id,
                      int64_t player_id,
                      double score_delta,
                      bool played_white,
                      bool bye);

/// Insert one pairing row for the current round. `black_player_id` may be
/// nullopt (bye). Result is 'pending' or 'bye'. Returns the row's id.
struct InsertPairingResult {
    bool        ok = false;
    int64_t     id = 0;
    std::string error;
};
InsertPairingResult insert_pairing(storage::Database& db,
                                   int64_t tournament_id,
                                   int round,
                                   int64_t white_player_id,
                                   std::optional<int64_t> black_player_id,
                                   const std::string& initial_result);

/// Load all pairings for a tournament, in (round asc, id asc) order.
std::vector<StoredPairing> get_pairings(storage::Database& db,
                                        int64_t tournament_id);

/// Load the pairings for one round only.
std::vector<StoredPairing> get_pairings_for_round(storage::Database& db,
                                                  int64_t tournament_id,
                                                  int round);

/// Set the result column on one pairing row (identified by id).
/// The allowed values are the schema's CHECK set. Returns false if the
/// row was not found or the DB rejected the update.
bool set_pairing_result(storage::Database& db,
                        int64_t pairing_id,
                        const std::string& result);

} // namespace tournament
} // namespace chess
