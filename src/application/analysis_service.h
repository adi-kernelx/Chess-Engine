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

#include "application/ports/message_sink.h"
#include "application/request_context.h"
#include "application/result.h"
#include "storage/database.h"

namespace chess::application {

class AnalysisService {
public:
    /// `db` may be null when the server was launched without
    /// persistence. `analyze_game` then replies with the pre-refactor
    /// "Analysis unavailable — no database" error string;
    /// `analyze_position` needs no DB and works unconditionally.
    explicit AnalysisService(chess::storage::Database* db);

    void analyze_position(const RequestContext& ctx,
                          const std::string&    fen,
                          int                   requested_depth,
                          MessageSink&          caller_sink);

    void analyze_game    (const RequestContext& ctx,
                          int64_t               game_id,
                          MessageSink&          caller_sink);

private:
    chess::storage::Database*  db_ = nullptr;
};

} // namespace chess::application
