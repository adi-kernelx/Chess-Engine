#include "application/game_completion_service.h"

#include <string>

#include "core/logger.h"
#include "storage/game_repo.h"
#include "storage/storage_error.h"

namespace chess::application {

GameCompletionService::GameCompletionService(
        chess::application::ports::GameStore& store,
        chess::application::ports::TournamentCompletionSink* tournament_sink)
    : store_(store), tournament_sink_(tournament_sink) {}

void GameCompletionService::on_game_completed(
        const chess::game::GameCompleted& ev) {
    const auto& s = ev.snapshot;

    // Capability-disabled composition: the wire behaviour of the
    // pre-LLD-4 code was a silent no-op when persistence was off. Keep
    // that shape — logging a "Persistence disabled" line per game
    // would flood the log for a legitimately configured deployment.
    if (!store_.capable()) return;

    // A durable human identity is required for every stored game. Human games
    // also require a real opponent identity; AI games deliberately use a
    // nullable computer seat and are persisted as unrated replay history.
    if (s.white.db_player_id <= 0) return;
    if (!s.is_ai_game && s.black.db_player_id <= 0) return;

    // Build the CompletedGame value from the snapshot. All fields
    // come from `s`; no back-reach into GameRoom.
    chess::storage::CompletedGame game;
    game.white_id        = s.white.db_player_id;
    game.black_id        = s.black.db_player_id;
    game.white_elo       = s.white.elo;
    game.black_elo       = s.black.elo;
    game.result          = s.result;
    game.termination     = s.termination_reason;
    game.time_control    = s.time_control.to_string();
    game.started_at      = s.started_at_iso;
    game.ended_at        = s.ended_at_iso;
    game.move_count      = s.move_count;
    game.completion_uuid = s.completion_uuid;
    game.rated           = !s.is_ai_game;
    if (s.is_ai_game) game.black_display_name = s.black.username;

    std::string moves;
    for (std::size_t i = 0; i < s.history.size(); ++i) {
        if (i > 0) moves += ' ';
        moves += s.history[i].move.to_uci();
        game.think_times.push_back({
            static_cast<int>(i + 1),
            (i % 2 == 0) ? s.white.db_player_id
                         : (s.is_ai_game ? 0 : s.black.db_player_id),
            s.history[i].think_time_ms
        });
    }
    game.moves = std::move(moves);

    auto result = store_.save_completed_game(game);

    if (result.ok()) {
        if (result.already_persisted) {
            chess::core::Logger::info("game", "GameCompletionService",
                "Game " + std::to_string(s.room_id) +
                " already persisted (DB id=" + std::to_string(result.game_id) +
                ", completion_uuid=" + s.completion_uuid + ") — retry no-op");
        } else if (game.rated) {
            chess::core::Logger::info("game", "GameCompletionService",
                "Game " + std::to_string(s.room_id) +
                " persisted (DB id=" + std::to_string(result.game_id) +
                ", white ELO " + std::to_string(game.white_elo) + "→" +
                std::to_string(result.elo.white_new) +
                ", black ELO " + std::to_string(game.black_elo) + "→" +
                std::to_string(result.elo.black_new) + ")");
        } else {
            chess::core::Logger::info("game", "GameCompletionService",
                "AI game " + std::to_string(s.room_id) +
                " persisted as unrated replay (DB id=" +
                std::to_string(result.game_id) + ")");
        }
        if (s.pairing_id > 0 && tournament_sink_) {
            tournament_sink_->on_tournament_game_persisted(
                s.pairing_id, s.result, result.game_id);
        }
    } else {
        chess::core::Logger::error("game", "GameCompletionService",
            "Failed to persist game " + std::to_string(s.room_id) +
            " (" + chess::storage::to_string(result.code) + "): " + result.error);
    }
}

} // namespace chess::application
