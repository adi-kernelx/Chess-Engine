/**
 * game/game_events.h — typed events GameRoom emits on state transitions
 * (LLD-4.1).
 *
 * SCOPE
 *
 * Two events only, in this slice:
 *
 *   GameStarted   — the second player joined and the game is now
 *                   IN_PROGRESS. Consumed today by nobody, wired for
 *                   the eventual match-notification listener in
 *                   LLD-4.2 (it currently arrives via the matchmaker
 *                   callback, which will migrate to a listener).
 *
 *   GameCompleted — a terminal transition landed (checkmate, stalemate,
 *                   any of the four draw reasons, resignation, timeout,
 *                   or a disconnect that finalises the game). Carries
 *                   the immutable `GameSnapshot`. Consumed by:
 *                     - LLD-4.2 GameCompletionService  (persistence)
 *                     - LLD-4.2 MatchNotify listener  (participant + spectator)
 *                     - future observability listener (metrics)
 *
 * WHAT IS DELIBERATELY NOT AN EVENT
 *
 * Individual move / spectate / resign frames are still delivered through
 * the existing per-request response path (MessageSink) and the injected
 * ForeignSender / SpectatorBroadcaster. Turning every per-move fan-out
 * into an event would create a general-purpose event bus, which the
 * plan doc explicitly says not to build: "a small listener list is
 * enough, no general event-bus framework required."
 *
 * LISTENER CONTRACT
 *
 * - Listeners are added / removed via `GameRoom::add_listener` and
 *   `GameRoom::remove_listener`. A shared_ptr<Listener> is used so the
 *   room can drop the reference safely when a listener is removed
 *   while an emit is in flight.
 *
 * - Callbacks fire on the worker thread that observed the transition.
 *   They fire OUTSIDE the room mutex — the room copies the listener
 *   list under lock, releases the lock, then invokes each callback.
 *   A listener that itself needs to touch a room must not deadlock.
 *
 * - One listener throwing does NOT skip the others. The room catches
 *   `std::exception` per callback, logs it, and continues. This is
 *   the plan's "listener failure isolation" requirement.
 *
 * - Listeners are removed by shared_ptr identity. Adding the same
 *   pointer twice results in two callbacks per event (the identity
 *   check happens on the shared_ptr, not the pointee).
 *
 * - Listener lifetime is the caller's responsibility. A common pattern
 *   is to hold the shared_ptr in `main.cpp` for the duration of the
 *   process; short-lived listeners (tests) call `remove_listener`
 *   before destroying the object.
 */

#pragma once

#include <cstdint>
#include <memory>

#include "core/types.h"      // GameId
#include "game/game_snapshot.h"

namespace chess {
namespace game {

/// Fired when a room transitions WAITING → IN_PROGRESS.
struct GameStarted {
    GameId       room_id      = 0;
    int64_t      white_db_id  = 0;
    int64_t      black_db_id  = 0;
    bool         is_ai_game   = false;
};

/// Fired when a room transitions IN_PROGRESS → FINISHED (terminal
/// transition of any cause). Snapshot is immutable and safe to hold
/// past the room's lifetime.
struct GameCompleted {
    GameSnapshot snapshot;
};

/// The listener interface. A listener may implement any subset of the
/// hooks; unimplemented ones inherit no-op defaults.
class GameEventListener {
public:
    virtual ~GameEventListener() = default;

    virtual void on_game_started  (const GameStarted&   /*ev*/) {}
    virtual void on_game_completed(const GameCompleted& /*ev*/) {}
};

using GameEventListenerPtr = std::shared_ptr<GameEventListener>;

} // namespace game
} // namespace chess
