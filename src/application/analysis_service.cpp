/**
 * application/analysis_service.cpp — see header for design.
 *
 * Every method here is a direct move from the corresponding
 * `GameHandler::handle_*` method in `src/game/game_handler.cpp`. Wire
 * bytes preserved bit-for-bit: same DB calls, same engine budgets, same
 * per-side JSON block, same "return the report even when persistence
 * fails" fallthrough. What changes is only where the code lives —
 * inside a class that can be constructed with a null `db_` and a fake
 * `MessageSink`.
 */

#include "application/analysis_service.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "analysis/anti_cheat.h"
#include "analysis/cheat_report_repo.h"
#include "chess/board.h"
#include "chess/engine.h"
#include "chess/move.h"
#include "chess/move_gen.h"
#include "core/logger.h"
#include "storage/game_repo.h"

using nlohmann::json;

namespace chess::application {

namespace {

// Same helper the pre-refactor handler used, relocated here so the
// two legacy responses keep their exact wire shape.
std::string make_error_frame(const std::string& message) {
    json err;
    err["type"]    = "error";
    err["message"] = message;
    return err.dump();
}

// Match a UCI (from,to,promo?) triple against the current legal-move
// list to recover a fully-flagged Move. See game_query_service.cpp for
// the twin copy that serves the replay path.
chess::Move resolve_legal(const chess::Board& board, const chess::Move& uci_probe) {
    const auto legal = chess::move_gen::generate_legal_moves(board);
    for (const auto& m : legal) {
        if (m.from != uci_probe.from) continue;
        if (m.to   != uci_probe.to)   continue;
        if (uci_probe.flags & chess::MoveFlags::PROMOTION) {
            if (m.promo_type != uci_probe.promo_type) continue;
        }
        return m;
    }
    return chess::Move{};
}

// Split "e2e4 e7e5 g1f3" into ["e2e4", "e7e5", "g1f3"].
std::vector<std::string> split_uci_moves(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

// Engine budget for the interactive analyze_position route. See header.
constexpr int ANALYZE_MAX_DEPTH = 15;
constexpr int ANALYZE_TIME_MS   = 3000;

// Per-ply budget for the whole-game anti-cheat sweep. See header.
constexpr int ANTI_CHEAT_DEPTH   = 6;
constexpr int ANTI_CHEAT_TIME_MS = 250;

// nlohmann::json refuses to serialize NaN — every double we emit has to
// pass through this to become `null` in the payload. Real values pass
// through unchanged.
json double_or_null(double d) {
    if (std::isnan(d) || std::isinf(d)) return nullptr;
    return d;
}

// Build the per-side JSON block. Kept out of the analyze_game body so
// both sides emit exactly the same shape.
json report_to_json(const chess::analysis::AnalysisReport& r) {
    json j;
    j["plies_analyzed"]       = r.plies_analyzed;
    j["plies_matched_engine"] = r.plies_matched_engine;
    j["engine_agreement_pct"] = double_or_null(r.engine_agreement_pct);
    j["time_cv"]              = double_or_null(r.time_cv);
    j["complexity_corr"]      = double_or_null(r.complexity_corr);
    j["flagged"]              = r.flagged;
    j["reasons"]              = r.reasons;
    return j;
}

} // namespace

AnalysisService::AnalysisService(chess::application::ports::PlayerQueries& queries,
                                 chess::application::ports::GameStore&     game_store)
    : queries_(queries), game_store_(game_store) {}

// ── analyze_position ────────────────────────────────────────────────

void AnalysisService::analyze_position(const RequestContext& /*ctx*/,
                                       const std::string&    fen,
                                       int                   requested_depth,
                                       MessageSink&          caller_sink) {
    if (fen.empty()) {
        caller_sink.send(make_error_frame("Missing 'fen' field"));
        return;
    }
    int req_depth = std::clamp(requested_depth, 1, ANALYZE_MAX_DEPTH);

    chess::Board board;
    if (!board.set_from_fen(fen)) {
        caller_sink.send(make_error_frame("Invalid FEN"));
        return;
    }

    // Terminal positions have no best move; return a plain payload so the
    // frontend doesn't spin on "the engine will pick one" forever.
    auto legal = chess::move_gen::generate_legal_moves(board);
    if (legal.empty()) {
        json response;
        response["type"]      = "analysis";
        response["fen"]       = fen;
        response["depth"]     = 0;
        response["nodes"]     = 0;
        response["eval_cp"]   = 0;
        response["best_move"] = "";
        response["terminal"]  = true;
        caller_sink.send(response.dump());
        return;
    }

    // Engine::search takes (time_ms, max_depth). Whichever hits first
    // bounds the request — a shallow position finishes in a few ms
    // long before ANALYZE_TIME_MS.
    chess::engine::Engine eng(16);   // 16 MB TT; disposable per-request
    eng.set_position(board);
    const auto res = eng.search(ANALYZE_TIME_MS, req_depth);

    json response;
    response["type"]      = "analysis";
    response["fen"]       = fen;
    response["depth"]     = res.depth;
    response["nodes"]     = static_cast<int64_t>(res.nodes);
    response["eval_cp"]   = res.score;
    response["best_move"] = res.best_move.to_uci();

    caller_sink.send(response.dump());
}

// ── analyze_game ────────────────────────────────────────────────────

void AnalysisService::analyze_game(const RequestContext& /*ctx*/,
                                   int64_t               game_id,
                                   MessageSink&          caller_sink) {
    if (game_id <= 0) {
        caller_sink.send(make_error_frame("Missing or invalid game_id"));
        return;
    }

    auto stored_res = queries_.find_game_by_id(game_id);
    if (!stored_res.ok) {
        // Storage-level failure (typically Disconnected — the
        // capability-disabled case surfaces exactly here). Same wire
        // string the pre-LLD-3 null-db check used to emit.
        caller_sink.send(make_error_frame("Analysis unavailable \xE2\x80\x94 no database"));
        return;
    }
    if (!stored_res.value.has_value()) {
        caller_sink.send(make_error_frame("Game " + std::to_string(game_id) + " not found"));
        return;
    }
    const auto& stored = stored_res.value;

    // Load per-ply think times keyed by ply_number. Read failure here
    // is soft — we log implicitly by falling back to an empty timeline
    // (same behaviour the pre-refactor inline query had when its
    // QueryResult failed).
    auto times_res = queries_.get_move_times_by_ply(game_id);
    std::vector<int> think_by_ply = times_res.ok ? std::move(times_res.value)
                                                 : std::vector<int>{};

    // Reconstruct positions; for each ply, capture the pre-move
    // complexity, engine's best move, and whether the actual move
    // matched. This is the payoff: two per-side PlyData vectors that
    // the pure analyzer consumes.
    std::vector<chess::analysis::PlyData> white_plies, black_plies;
    chess::Board board = chess::Board::starting_position();
    // One engine instance for the whole game — the TT stays hot across
    // consecutive positions and cuts total search time noticeably.
    chess::engine::Engine eng(16);

    const auto tokens = split_uci_moves(stored->moves);
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto pre_legal = chess::move_gen::generate_legal_moves(board);
        const int  complexity = static_cast<int>(pre_legal.size());

        const chess::Move probe = chess::Move::from_uci(tokens[i]);
        const chess::Move legal = resolve_legal(board, probe);
        if (legal.from == 0 && legal.to == 0) {
            caller_sink.send(make_error_frame("Corrupt move at ply " +
                      std::to_string(i + 1) + ": " + tokens[i]));
            return;
        }

        // Run the engine on the pre-move position. Skip when the
        // position is terminal (no legal moves) — cannot happen here
        // because we just resolved a legal move, but defensive.
        chess::Move engine_best;
        if (!pre_legal.empty()) {
            eng.set_position(board);
            const auto res = eng.search(ANTI_CHEAT_TIME_MS, ANTI_CHEAT_DEPTH);
            engine_best = res.best_move;
        }

        board.make_move(legal);

        // Terminal-after = mate/stalemate landed after this ply.
        const bool terminal_after =
            chess::move_gen::generate_legal_moves(board).empty();

        chess::analysis::PlyData pd;
        pd.think_time_ms    = (i + 1 < think_by_ply.size())
                                ? think_by_ply[i + 1] : 0;
        pd.legal_move_count = complexity;
        pd.matched_engine   = (engine_best.from == legal.from
                               && engine_best.to   == legal.to
                               && engine_best.promo_type == legal.promo_type);
        pd.terminal_after   = terminal_after;

        // Ply 1 = White's first move, so even i (0,2,4,…) → White.
        if ((i % 2) == 0) white_plies.push_back(pd);
        else              black_plies.push_back(pd);
    }

    chess::analysis::AnalysisReport white_report =
        chess::analysis::analyze_side(white_plies);
    chess::analysis::AnalysisReport black_report =
        chess::analysis::analyze_side(black_plies);

    // Persist both sides through the GameStore port. save_cheat_report
    // is upsert-on-conflict, so re-running analyze_game on the same
    // game refreshes rather than duplicates. Skip when the store is
    // capability-disabled; the verdict is still returned to the caller.
    if (game_store_.capable()) {
        auto save_w = game_store_.save_cheat_report(stored->game_id,
                                                    stored->white_id, "w",
                                                    white_report);
        auto save_b = game_store_.save_cheat_report(stored->game_id,
                                                    stored->black_id, "b",
                                                    black_report);
        if (!save_w.ok() || !save_b.ok()) {
            const auto& failed = save_w.ok() ? save_b : save_w;
            chess::core::Logger::warn("game", "AntiCheat",
                "Failed to persist cheat report for game "
                + std::to_string(stored->game_id) + " ("
                + chess::storage::to_string(failed.code) + "): " + failed.error);
            // Fall through — return the report to the caller even if
            // persistence failed. The verdict is already computed and
            // useful; a DB blip should not swallow it.
        }
    }

    json response;
    response["type"]    = "cheat_report";
    response["game_id"] = stored->game_id;
    response["white"]   = report_to_json(white_report);
    response["black"]   = report_to_json(black_report);
    caller_sink.send(response.dump());
}

} // namespace chess::application
