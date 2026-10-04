#pragma once

#include <cstdint>
#include <string>

#include "application/request_context.h"
#include "core/types.h"

namespace chess::application::ports {

struct TournamentCheckInResult {
    bool ok = false;
    std::string error;
    bool room_ready = false;
    bool game_started = false;
    chess::GameId game_id = 0;
    chess::Color color = chess::Color::NONE;
};

class TournamentRuntime {
public:
    virtual ~TournamentRuntime() = default;
    virtual TournamentCheckInResult check_in_and_bind(
        int64_t tournament_id, int round,
        const AuthenticatedIdentity& actor, int connection_fd, uint64_t generation = 0) = 0;
};

} // namespace chess::application::ports
