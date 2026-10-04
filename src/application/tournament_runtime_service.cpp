#include "application/tournament_runtime_service.h"

#include <chrono>
#include <nlohmann/json.hpp>
#include <vector>

#include "core/logger.h"
#include "tournament/tournament_manager.h"
#include "tournament/tournament_repo.h"

namespace chess::application {

using nlohmann::json;

TournamentRuntimeService::TournamentRuntimeService(
        storage::Database* db, game::RoomManager& rooms,
        TournamentReadySender ready_sender, ports::Clock& clock)
    : db_(db), rooms_(rooms), ready_sender_(std::move(ready_sender)), clock_(clock) {
    if (db_) rooms_.ensure_next_id_above(tournament::max_pairing_game_id(*db_));
}

ports::TournamentCheckInResult TournamentRuntimeService::check_in_and_bind(
        int64_t tournament_id, int round,
        const AuthenticatedIdentity& actor, int connection_fd, uint64_t generation) {
    ports::TournamentCheckInResult out;
    if (!db_) { out.error = "Tournaments require a database"; return out; }
    tournament::TournamentManager manager(*db_, clock_);
    auto checked = manager.check_in(tournament_id, round, actor.player_id);
    if (!checked.ok) { out.error = checked.error; return out; }

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto& seat = pending_seats_[{tournament_id, round, actor.player_id}];
        if (generation < seat.generation) { out.error = "connection_replaced"; return out; }
        seat = PendingSeat{actor.player_id, connection_fd, generation};
    }
    rooms_.reserve_tournament_player(actor.player_id);

    auto pairing_rows = tournament::get_pairings_for_round(*db_, tournament_id, round);
    // Early check-in records attendance only. Pairing visibility does not
    // authorize entering a game or starting its clocks before the start.
    bool due = false;
    for (const auto& scheduled : tournament::get_rounds(*db_, tournament_id)) {
        if (scheduled.round == round && scheduled.status == "live"
            && clock_.unix_seconds() >= scheduled.earliest_start_at_unix) due = true;
    }
    if (!due) { out.ok = true; return out; }
    for (const auto& pairing : pairing_rows) {
        if (pairing.result != "pending" || !pairing.black_player_id) continue;
        if (pairing.white_player_id != actor.player_id
            && *pairing.black_player_id != actor.player_id) continue;
        auto room = rooms_.find_room_by_pairing(pairing.id);
        if (!room) {
            ensure_live_rooms();
            room = rooms_.find_room_by_pairing(pairing.id);
        }
        if (!room) break;
        (void)room->allow_reserved_player(actor.player_id);
        auto bound = room->bind_reserved_player(actor.player_id,
            static_cast<chess::PlayerId>(actor.player_id), connection_fd, true, generation);
        if (!bound.ok) { out.error = bound.error; return out; }
        out.room_ready = true;
        out.game_started = room->get_state() == game::RoomState::IN_PROGRESS;
        out.game_id = room->get_id();
        out.color = bound.color;
        notify_room(room);
        break;
    }
    out.ok = true;
    return out;
}

void TournamentRuntimeService::bind_pending(
        const std::shared_ptr<game::GameRoom>& room,
        int64_t tournament_id, int round, int64_t white_id, int64_t black_id) {
    std::vector<PendingSeat> seats;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (int64_t id : {white_id, black_id}) {
            auto it = pending_seats_.find({tournament_id, round, id});
            if (it != pending_seats_.end()) seats.push_back(it->second);
        }
    }
    for (const auto& seat : seats) {
        (void)room->bind_reserved_player(seat.player_id,
            static_cast<chess::PlayerId>(seat.player_id), seat.fd, false, seat.generation);
    }
}

void TournamentRuntimeService::notify_room(
        const std::shared_ptr<game::GameRoom>& room) {
    if (!room || !ready_sender_) return;
    const bool started = room->get_state() == game::RoomState::IN_PROGRESS;
    for (chess::Color color : {chess::Color::WHITE, chess::Color::BLACK}) {
        const int fd = room->get_player_fd(color);
        if (fd < 0 || !room->is_connected(color)) continue;
        const std::string state = started ? "in_progress" : "waiting";
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            const auto key = std::make_pair(room->pairing_id(), fd);
            auto it = notified_states_.find(key);
            if (it != notified_states_.end() && it->second == state) continue;
            notified_states_[key] = state;
        }
        json message{{"type", "tournament_game_ready"},
                     {"tournament_id", room->tournament_id()},
                     {"pairing_id", room->pairing_id()},
                     {"game_id", room->get_id()},
                     {"color", color == chess::Color::WHITE ? "white" : "black"},
                     {"state", state}};
        ready_sender_(fd, message.dump());
    }
    if (started && db_) {
        const auto tid = room->tournament_id();
        const auto white = room->get_db_player_id(chess::Color::WHITE);
        const auto black = room->get_db_player_id(chess::Color::BLACK);
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (auto it = pending_seats_.begin(); it != pending_seats_.end();) {
            const auto [tournament_id, round, player_id] = it->first;
            (void)round;
            if (tournament_id == tid && (player_id == white || player_id == black)) it = pending_seats_.erase(it);
            else ++it;
        }
    }
}

void TournamentRuntimeService::ensure_live_rooms() {
    if (!db_) return;
    std::lock_guard<std::mutex> materialize(materialization_mutex_);
    rooms_.ensure_next_id_above(tournament::max_pairing_game_id(*db_));
    for (const auto& pairing : tournament::get_live_pending_pairings(*db_)) {
        auto room = rooms_.find_room_by_pairing(pairing.id);
        // Seats/names/check-in eligibility are already in memory once play
        // starts. Do not poll SQL or rebind a live game on every scheduler pass.
        if (room && room->get_state() == game::RoomState::IN_PROGRESS) continue;
        if (!room) {
            auto tournament_row = tournament::find_tournament(*db_, pairing.tournament_id);
            if (!tournament_row || !pairing.black_player_id) continue;
            auto players = tournament::get_participants(*db_, pairing.tournament_id);
            auto names = tournament::get_participant_usernames(*db_, pairing.tournament_id);
            int white_elo = 1200, black_elo = 1200;
            for (const auto& p : players) {
                if (p.player_id == pairing.white_player_id) white_elo = p.initial_elo;
                if (p.player_id == *pairing.black_player_id) black_elo = p.initial_elo;
            }
            room = rooms_.create_reserved_tournament_room(
                pairing.tournament_id, pairing.id,
                pairing.white_player_id, names[pairing.white_player_id], white_elo,
                *pairing.black_player_id, names[*pairing.black_player_id], black_elo,
                game::TimeControl(tournament_row->time_control_initial_ms,
                                  tournament_row->time_control_increment_ms));
            // Persist before any ready notification. On failure the room is
            // removed, so no client can enter an untracked tournament game.
            if (!tournament::set_pairing_game_id(*db_, pairing.id, room->get_id())) {
                rooms_.remove_room(room->get_id());
                continue;
            }
        }
        const auto checked_in = tournament::get_round_checkins(
            *db_, pairing.tournament_id, pairing.round);
        for (int64_t player_id : checked_in) {
            (void)room->allow_reserved_player(player_id);
        }
        bind_pending(room, pairing.tournament_id, pairing.round,
                     pairing.white_player_id, *pairing.black_player_id);
        room->open_reserved();
        notify_room(room);
    }
}

void TournamentRuntimeService::maintenance_tick() {
    if (!db_) return;
    // TcpServer invokes its lightweight maintenance callback after every
    // epoll cycle (as often as every packet, not merely every 250 ms timeout).
    // Durable tournament maintenance performs several SQL queries, so running
    // it on every callback can monopolize the one serialized PG connection and
    // starve auth, directory, and game-state requests. One durable pass per
    // second is precise enough for human-visible tournament deadlines.
    std::unique_lock<std::mutex> lock(maintenance_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    const auto now = clock_.steady_now();
    if (maintenance_scheduled_ && now < next_maintenance_at_) return;
    next_maintenance_at_ = now + std::chrono::seconds(1);
    maintenance_scheduled_ = true;
    run_maintenance_locked();
}

void TournamentRuntimeService::run_maintenance_locked() {
    tournament::TournamentManager manager(*db_, clock_);
    auto result = manager.maintenance_tick();
    if (!result.ok) {
        if (!maintenance_error_logged_.exchange(true)) {
            core::Logger::error("tournament", "TournamentRuntimeService",
                                "maintenance failed: " + result.error);
        }
        return;
    }
    if (maintenance_error_logged_.exchange(false)) {
        core::Logger::info("tournament", "TournamentRuntimeService",
                           "maintenance recovered");
    }
    ensure_live_rooms();
    // A no-show may resolve a pairing without ever starting its reserved
    // room. Remove that WAITING shell immediately so neither identity remains
    // blocked by a stale active-room lookup.
    for (const auto& room : rooms_.rooms_snapshot()) {
        if (!room || !room->is_tournament_game()
            || room->get_state() != game::RoomState::WAITING) continue;
        auto pairing = tournament::find_pairing(*db_, room->pairing_id());
        if (!pairing || pairing->result != "pending") {
            if (pairing) {
                rooms_.release_tournament_player(pairing->white_player_id);
                if (pairing->black_player_id)
                    rooms_.release_tournament_player(*pairing->black_player_id);
            }
            rooms_.remove_room(room->get_id());
        }
    }
    // Covers delayed ticks where the no-show decision happened before a room
    // could be materialized.
    std::map<CheckInKey, PendingSeat> pending;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending = pending_seats_;
    }
    // Never hold the seat mutex across remote SQL: concurrent check-ins and
    // disconnect callbacks need it, even while storage is slow.
    for (const auto& [key, seat] : pending) {
        const auto [tournament_id, round, player_id] = key;
        bool terminal = false;
        for (const auto& pairing : tournament::get_pairings_for_round(*db_, tournament_id, round)) {
            if (pairing.white_player_id == player_id
                || (pairing.black_player_id && *pairing.black_player_id == player_id)) {
                terminal = pairing.result != "pending";
                break;
            }
        }
        if (terminal) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_seats_.find(key);
            if (it != pending_seats_.end() && it->second.fd == seat.fd) {
                rooms_.release_tournament_player(player_id);
                pending_seats_.erase(it);
            }
        }
    }
}

void TournamentRuntimeService::on_disconnect(int connection_fd) {
    std::vector<CheckInKey> abandoned_waits;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (auto it = pending_seats_.begin(); it != pending_seats_.end();) {
            if (it->second.fd == connection_fd) {
                abandoned_waits.push_back(it->first);
                it = pending_seats_.erase(it);
            } else ++it;
        }
        for (auto it = notified_states_.begin(); it != notified_states_.end();) {
            if (it->first.second == connection_fd) it = notified_states_.erase(it);
            else ++it;
        }
    }
    if (!db_) return;
    for (const auto& [tournament_id, round, player_id] : abandoned_waits) {
        (void)db_->exec(
            "DELETE FROM tournament_round_checkins WHERE tournament_id=$1 AND round=$2 AND player_id=$3",
            {storage::Param::int64(tournament_id), storage::Param::int64(round),
             storage::Param::int64(player_id)});
        rooms_.release_tournament_player(player_id);
    }
}

void TournamentRuntimeService::on_tournament_game_persisted(
        int64_t pairing_id, const std::string& result, int64_t persisted_game_id) {
    if (!db_ || pairing_id <= 0) return;
    if (!tournament::set_pairing_replay_game_id(*db_, pairing_id, persisted_game_id)) {
        core::Logger::error("tournament", "TournamentRuntimeService",
            "could not link pairing " + std::to_string(pairing_id)
            + " to persisted replay " + std::to_string(persisted_game_id));
        return;
    }
    tournament::TournamentManager manager(*db_, clock_);
    auto recorded = manager.record_game_result(pairing_id, result);
    if (!recorded.ok) {
        core::Logger::error("tournament", "TournamentRuntimeService",
            "could not resolve pairing " + std::to_string(pairing_id) + ": " + recorded.error);
        return;
    }
    auto pairing = tournament::find_pairing(*db_, pairing_id);
    if (pairing) {
        rooms_.release_tournament_player(pairing->white_player_id);
        if (pairing->black_player_id)
            rooms_.release_tournament_player(*pairing->black_player_id);
    }
    // Completion must advance standings/rounds immediately rather than wait
    // for the next throttled transport tick.
    std::lock_guard<std::mutex> lock(maintenance_mutex_);
    next_maintenance_at_ = clock_.steady_now() + std::chrono::seconds(1);
    maintenance_scheduled_ = true;
    run_maintenance_locked();
}

} // namespace chess::application
