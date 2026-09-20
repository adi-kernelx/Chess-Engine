/**
 * application/gameplay_service.h — the "what to do" for the gameplay +
 * matchmaking family (LLD-2.1).
 *
 * NINE ROUTES land here — every one that used to live on `GameHandler`:
 *
 *      create_game / join_game / make_move / resign / game_state
 *      quick_play / cancel_queue / list_games / play_ai
 *
 * plus the `on_player_disconnect` callback the transport layer fires when
 * a socket closes.
 *
 * WHAT THIS CLASS OWNS
 *   - The room/matchmaker orchestration logic that used to be inline in
 *     `GameHandler::handle_*`.
 *   - The AI-move follow-up path.
 *   - The `persist_game` step for finished games.
 *
 * WHAT IT DOES NOT OWN
 *   - `nlohmann::json::parse()` on the incoming frame. The `GameplayHandler`
 *     adapter parses; the service takes structured inputs.
 *   - The identity-extraction step. Auth-required routes take an
 *     `AuthenticatedIdentity` as input; the handler proved identity
 *     via `IdentityExtractor::extract` before calling.
 *
 * WHAT IT PRAGMATICALLY STILL DOES
 *   - Emits JSON frames for the six legacy (non-LLD-1-migrated) routes.
 *     The plan doc's ideal boundary is "no JSON in service"; hitting it
 *     for every legacy response means inventing typed DTOs for every
 *     one, which explodes this slice's scope. LLD-3+ will replace those
 *     inline emitters with the codec as their responses move to typed
 *     shapes. LLD-1-migrated routes (make_move / resign / game_state)
 *     already flow through `protocol::codec` and this file keeps them
 *     that way.
 *   - Uses two injected send callables (`ForeignSender` for
 *     opponent-fd delivery, `SpectatorBroadcaster` for the room-wide
 *     fan-out). Both are the same closures `GameHandler` used to hold
 *     inline — just relocated behind an explicit boundary that tests
 *     can substitute.
 *
 * THREADING
 *   Each service method runs on the worker thread that decoded the
 *   frame. `RoomManager` and `Matchmaker` already synchronise
 *   themselves; nothing in this class introduces new global state.
 *   `on_player_disconnect` may run on any thread that the transport
 *   picks; it stays confined to `RoomManager` + `Matchmaker` calls
 *   for exactly that reason.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "application/ports/message_sink.h"
#include "application/request_context.h"
#include "application/result.h"
#include "game/ai_player.h"
#include "game/matchmaker.h"
#include "game/room_manager.h"
#include "protocol/request.h"

namespace chess::application {

/// Send a UTF-8 JSON frame to a specific file descriptor. Bound at
/// construction to the transport's send-and-inline-flush closure —
/// same behaviour `GameHandler::send_json_to_fd` had.
using ForeignSender = std::function<void(int fd, const std::string& frame)>;

/// Broadcast a UTF-8 JSON frame to every spectator of a specific room.
/// Bound at construction to the transport's spectator fan-out.
using SpectatorBroadcaster =
    std::function<void(chess::game::GameRoom& room, const std::string& frame)>;

class GameplayService {
public:
    /// All references must outlive the service. `foreign_sender` and
    /// `spectator_broadcaster` MUST NOT be null; the constructor
    /// asserts otherwise (bad state, not a runtime condition).
    ///
    /// LLD-4.2 changed the ownership shape here: persistence used to
    /// live inline on this service via a `GameStore&` and a private
    /// `persist_game` helper. That was moved to
    /// `GameCompletionService`, which listens on every room via
    /// `RoomManager::set_default_listener`. This service no longer
    /// touches the store.
    GameplayService(chess::game::RoomManager&              rooms,
                    chess::game::Matchmaker&               matchmaker,
                    chess::game::AIPlayer&                 ai,
                    ForeignSender                          foreign_sender,
                    SpectatorBroadcaster                   spectator_broadcaster);

    // ── LLD-1-migrated routes (typed request, MessageSink caller) ──

    void make_move  (const RequestContext&                    ctx,
                     const chess::protocol::MakeMoveRequest&  req,
                     MessageSink&                             caller_sink);

    void resign     (const RequestContext&                    ctx,
                     const chess::protocol::ResignRequest&    req,
                     MessageSink&                             caller_sink);

    // (chess::GameStatus is used by the private persist_game helper;
    // it lives in chess::, not chess::game::, and is pulled in via
    // game_room.h below.)

    void game_state (const RequestContext&                    ctx,
                     const chess::protocol::GameStateRequest& req,
                     MessageSink&                             caller_sink);

    // ── Legacy routes: identity + primitive inputs, JSON caller_sink ──
    //
    // The handler parses JSON, runs auth, and calls the appropriate
    // method. The service builds the response JSON and sends via
    // `caller_sink` (still the shared MessageSink boundary — no raw
    // Connection reaches here).

    void create_game(const RequestContext&        ctx,
                     const AuthenticatedIdentity& actor,
                     int                          time_base_sec,
                     int                          time_inc_sec,
                     MessageSink&                 caller_sink);

    void join_game  (const RequestContext&        ctx,
                     const AuthenticatedIdentity& actor,
                     int64_t                      game_id,
                     MessageSink&                 caller_sink);

    void quick_play (const RequestContext&        ctx,
                     const AuthenticatedIdentity& actor,
                     int                          time_base_sec,
                     int                          time_inc_sec,
                     MessageSink&                 caller_sink);

    void cancel_queue(const RequestContext& ctx,
                      MessageSink&          caller_sink);

    void list_games (const RequestContext& ctx,
                     MessageSink&          caller_sink);

    void play_ai    (const RequestContext&        ctx,
                     const AuthenticatedIdentity& actor,
                     const std::string&           difficulty_str,
                     int                          time_base_sec,
                     int                          time_inc_sec,
                     MessageSink&                 caller_sink);

    /// Called by the transport when a socket closes. Removes the fd
    /// from the matchmaking queue and notifies its room (if any) of
    /// the disconnect. Idempotent.
    void on_player_disconnect(int fd);

private:
    /// After a human makes a move in an AI game, compute + submit the
    /// AI's response. Runs synchronously on the worker thread that
    /// handled the human move — matches pre-refactor behaviour.
    void trigger_ai_move(std::shared_ptr<chess::game::GameRoom> room,
                         int human_fd);

    chess::game::RoomManager&              rooms_;
    chess::game::Matchmaker&               matchmaker_;
    chess::game::AIPlayer&                 ai_;
    ForeignSender                          foreign_sender_;
    SpectatorBroadcaster                   spectator_broadcaster_;

    /// Local monotonically-increasing player id for in-memory bookkeeping.
    /// Distinct from `players.id` (the DB primary key), which is what
    /// `AuthenticatedIdentity` carries. Kept for backward compatibility
    /// with GameRoom / Matchmaker's own uint identifier space.
    std::atomic<chess::PlayerId> next_player_id_{1};
};

} // namespace chess::application
