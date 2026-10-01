/**
 * application/tournament_service.h — the "what to do" for the
 * tournaments family (LLD-2.4).
 *
 * SIX ROUTES land here:
 *
 *      create_tournament / join_tournament / start_tournament       (auth)
 *      report_tournament_result                                     (auth, creator-only)
 *      tournament_state / list_tournaments                          (public)
 *
 * WHAT THIS CLASS OWNS
 *   - Input validation (name length, rounds/time-control bounds,
 *     tournament_id integer-shape).
 *   - Coordination with `tournament::TournamentManager` and the
 *     stateless `tournament::*` repo functions.
 *   - Creator-check for `report_tournament_result` — an out-of-band
 *     admin path invokable via WebSocket for the frontend and tests.
 *
 * WHAT IT DOES NOT OWN
 *   - `nlohmann::json::parse()` on the incoming frame. The
 *     `TournamentHandler` adapter parses; the service takes structured
 *     inputs.
 *   - Auth. The handler runs `IdentityExtractor::extract` up-front for
 *     the four mutating routes and passes the snapshot in; the two
 *     public routes take no identity.
 *
 * WHAT IT PRAGMATICALLY STILL DOES
 *   - Emits JSON frames inline via `nlohmann::json`. All six responses
 *     are legacy shapes (predate `protocol::codec`); typed DTOs are
 *     LLD-3+ scope. Emissions still flow through the shared
 *     `MessageSink` boundary so the transport is fake-substitutable in
 *     tests.
 *
 * THREADING
 *   Each service method runs on the worker thread that decoded the
 *   frame. `TournamentManager` is stateless in-process (a per-call
 *   instance carries no cost); the underlying `Database` synchronises
 *   itself.
 */

#pragma once

#include <cstdint>
#include <string>

#include "application/ports/message_sink.h"
#include "application/request_context.h"
#include "application/result.h"
#include "storage/database.h"
#include "application/ports/clock.h"
#include "application/ports/tournament_runtime.h"

namespace chess::application {

class TournamentService {
public:
    /// `db` may be null when the server was launched without
    /// persistence. Every route then replies with the pre-refactor
    /// "Tournaments require a database" error string.
    explicit TournamentService(chess::storage::Database* db,
        ports::Clock& clock = ports::default_clock(),
        ports::TournamentRuntime* runtime = nullptr);

    // ── Mutating routes (handler runs auth first) ─────────────────

    /// `actor_db_player_id` comes from the handler-verified
    /// `AuthenticatedIdentity::player_id`. The service does not
    /// re-verify identity — it trusts the caller.
    void create_tournament       (const RequestContext& ctx,
                                  int64_t               actor_db_player_id,
                                  const std::string&    name,
                                  int                   rounds,
                                  int                   time_base_sec,
                                  int                   time_inc_sec,
                                  int64_t               registration_deadline_unix,
                                  int64_t               first_round_starts_at_unix,
                                  int                   round_duration_seconds,
                                  MessageSink&          caller_sink);

    void join_tournament         (const RequestContext& ctx,
                                  int64_t               actor_db_player_id,
                                  int                   actor_elo,
                                  bool                  has_tournament_id,
                                  int64_t               tournament_id,
                                  MessageSink&          caller_sink);

    void leave_tournament        (const RequestContext& ctx,
                                  int64_t               actor_db_player_id,
                                  bool                  has_tournament_id,
                                  int64_t               tournament_id,
                                  MessageSink&          caller_sink);

    void set_registration(const RequestContext& ctx, int64_t actor_db_player_id,
                          bool has_tournament_id, int64_t tournament_id,
                          bool open, MessageSink& caller_sink);
    void check_in_round(const RequestContext& ctx, int64_t actor_db_player_id,
                        bool has_tournament_id, int64_t tournament_id,
                        int round, MessageSink& caller_sink);

    void start_tournament        (const RequestContext& ctx,
                                  int64_t               actor_db_player_id,
                                  bool                  has_tournament_id,
                                  int64_t               tournament_id,
                                  MessageSink&          caller_sink);

    void report_tournament_result(const RequestContext& ctx,
                                  int64_t               actor_db_player_id,
                                  bool                  has_pairing_id,
                                  int64_t               pairing_id,
                                  const std::string&    result,
                                  const std::string&    reason,
                                  MessageSink&          caller_sink);

    void maintenance_tick();

    // ── Public routes (no auth) ───────────────────────────────────

    void tournament_state (const RequestContext& ctx,
                           bool                  has_tournament_id,
                           int64_t               tournament_id,
                           MessageSink&          caller_sink);

    void list_tournaments (const RequestContext& ctx,
                           const std::string&    status,
                           int                   limit,
                           MessageSink&          caller_sink);

private:
    chess::storage::Database*  db_ = nullptr;
    ports::Clock&              clock_;
    ports::TournamentRuntime* runtime_ = nullptr;
};

} // namespace chess::application
