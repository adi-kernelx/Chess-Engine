#pragma once

#include <cstdint>
#include <string>

namespace chess::application::ports {

/// Optional post-persistence observer for tournament games. It is invoked
/// only after normal game persistence succeeds or confirms an idempotent
/// duplicate; therefore tournament standings can never get ahead of replay,
/// profile, and rating persistence.
class TournamentCompletionSink {
public:
    virtual ~TournamentCompletionSink() = default;
    virtual void on_tournament_game_persisted(int64_t pairing_id,
                                               const std::string& result,
                                               int64_t persisted_game_id) = 0;
};

} // namespace chess::application::ports
