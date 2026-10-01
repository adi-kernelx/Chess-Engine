#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

#include "application/ports/clock.h"
#include "application/ports/tournament_completion_sink.h"
#include "application/ports/tournament_runtime.h"
#include "application/request_context.h"
#include "game/room_manager.h"
#include "storage/database.h"

namespace chess::application {

using TournamentReadySender = std::function<void(int, const std::string&)>;

/// Facade/Mediator for the only Phase-2 boundary that spans durable tournament
/// state and in-memory rooms. TournamentManager retains rules/SQL; GameRoom
/// retains chess/clocks; this class creates and binds reserved rooms.
class TournamentRuntimeService final
    : public ports::TournamentCompletionSink,
      public ports::TournamentRuntime {
public:
    TournamentRuntimeService(storage::Database* db,
                             game::RoomManager& rooms,
                             TournamentReadySender ready_sender,
                             ports::Clock& clock = ports::default_clock());

    ports::TournamentCheckInResult check_in_and_bind(
        int64_t tournament_id, int round,
        const AuthenticatedIdentity& actor, int connection_fd) override;

    /// Run durable maintenance, materialize every pending live pairing, and
    /// bind any early check-ins remembered by durable player identity.
    void maintenance_tick();
    void on_disconnect(int connection_fd);

    void on_tournament_game_persisted(int64_t pairing_id,
                                       const std::string& result,
                                       int64_t persisted_game_id) override;

private:
    struct PendingSeat {
        int64_t player_id = 0;
        int fd = -1;
    };
    using CheckInKey = std::tuple<int64_t, int, int64_t>;

    void ensure_live_rooms();
    void bind_pending(const std::shared_ptr<game::GameRoom>& room,
                      int64_t tournament_id, int round,
                      int64_t white_id, int64_t black_id);
    void notify_room(const std::shared_ptr<game::GameRoom>& room);

    storage::Database* db_;
    game::RoomManager& rooms_;
    TournamentReadySender ready_sender_;
    ports::Clock& clock_;
    std::mutex pending_mutex_;
    std::map<CheckInKey, PendingSeat> pending_seats_;
    std::map<std::pair<int64_t,int>, std::string> notified_states_;
};

} // namespace chess::application
