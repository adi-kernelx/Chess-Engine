/**
 * application/analysis_service.h — the "what to do" for the analysis
 * family (LLD-2.3).
 *
 * TWO ROUTES land here:
 *
 *      analyze_position      (public — engine eval of an arbitrary FEN)
 *      analyze_game          (public — statistical anti-cheat report
 *                             for a persisted game)
 *
 * WHAT THIS CLASS OWNS
 *   - Engine-driven analysis of a single FEN (per-request disposable
 *     `engine::Engine` with 16 MB TT).
 *   - Per-ply reconstruction of a stored game, engine best-move
 *     comparison at low depth, statistical `analysis::analyze_side`
 *     roll-up, and upsert-on-conflict persistence via
 *     `analysis::save_cheat_report`.
 *
 * WHAT IT DOES NOT OWN
 *   - `nlohmann::json::parse()` on the incoming frame. The
 *     `AnalysisHandler` adapter parses; the service takes structured
 *     inputs (a FEN string, a game_id).
 *   - Auth. Both routes are public reads (moderators / players
 *     investigating suspected opponents need to open a report without an
 *     active session — Phase 9.3 policy). A future moderator-role gate
 *     would go at the handler.
 *
 * WHAT IT PRAGMATICALLY STILL DOES
 *   - Emits JSON frames inline via `nlohmann::json`. The `analysis` and
 *     `cheat_report` responses are legacy shapes (predate
 *     `protocol::codec`); typed DTOs are LLD-3+ scope. Emissions still
 *     flow through the shared `MessageSink` boundary so the transport
 *     is fake-substitutable in tests.
 *
 * BUDGETS (preserved from the pre-refactor handler)
 *   - `analyze_position`: depth ≤ 15, time ≤ 3 s per request.
 *   - `analyze_game`: depth 6 / 250 ms per ply — the Phase 9.3 tradeoff
 *     between engine strength and finishing a whole game in seconds
 *     rather than minutes.
 *
 * THREADING
 *   Each service method runs on the worker thread that decoded the
 *   frame. The engine instance is per-call (or per-game); no shared
 *   mutable state.
 */

#pragma once

#include <cstdint>
#include <string>

#include "application/ports/game_store.h"
#include "application/ports/message_sink.h"
#include "application/ports/player_queries.h"
#include "application/request_context.h"
#include "application/result.h"

namespace chess::application {

class AnalysisService {
public:
    /// Both port references must outlive the service. In capability-
    /// disabled mode the composition root injects `NullPlayerQueries`
    /// + `NullGameStore`; `analyze_game` then hits the port, gets a
    /// `Disconnected` outcome from the read side, and emits the
    /// pre-refactor "Analysis unavailable — no database" wire error.
    /// `analyze_position` uses neither port and works unconditionally.
    AnalysisService(chess::application::ports::PlayerQueries& queries,
                    chess::application::ports::GameStore&     game_store);

    void analyze_position(const RequestContext& ctx,
                          const std::string&    fen,
                          int                   requested_depth,
                          MessageSink&          caller_sink);

    void analyze_game    (const RequestContext& ctx,
                          int64_t               game_id,
                          MessageSink&          caller_sink);

private:
    chess::application::ports::PlayerQueries& queries_;
    chess::application::ports::GameStore&     game_store_;
};

} // namespace chess::application
