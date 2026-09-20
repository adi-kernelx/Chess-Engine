/**
 * application/game_completion_service.h — the single terminal-transition
 * persistence path (LLD-4.2).
 *
 * WHY THIS EXISTS
 *
 * Before this class, `GameplayService::persist_game` was called inline
 * from four sites (checkmate/draw in `make_move`, timeout in
 * `make_move`, `resign`, and the AI-move follow-up in
 * `trigger_ai_move`). Every site had to remember to run the same
 * guards (persistence enabled? AI game? both players authenticated?)
 * before dispatching. Adding a fifth call site — the disconnect-driven
 * finalisation planned for a future slice — would repeat those guards
 * again.
 *
 * The completion service consolidates that logic behind a single
 * `on_game_completed(const GameCompleted&)` callback. `GameRoom` fires
 * the event once per terminal transition (from `finish_game`, which
 * runs on the same worker thread that observed the transition). The
 * service is registered as `RoomManager`'s default listener during
 * composition, so every room the manager creates automatically routes
 * its completion to this one path.
 *
 * IDEMPOTENCY
 *
 * `GameRoom::build_snapshot_locked` stamps every terminal snapshot
 * with a stable v4 UUID (`GameSnapshot::completion_uuid`). This class
 * hands that key to `GameStore::save_completed_game`, which uses it to
 * short-circuit duplicate inserts. That gives us the plan's
 * "duplicate terminal requests save once" + "no double ELO/stat
 * increments" invariants for free — a retried event that fires twice
 * (e.g. because a future slice pipes both `resign` and the socket-
 * close event through the same listener) still results in exactly one
 * games row.
 *
 * WHAT THIS CLASS DOES NOT OWN
 *
 * Notification / broadcast. The wire-facing `game_over` frame is still
 * emitted inline by `GameplayService` — it needs the caller's
 * `MessageSink`, which is per-request state a global listener does
 * not have. The plan-doc "separate participant/spectator notification
 * from mandatory completion persistence" separation lives in the fact
 * that broadcast is now independent of persistence: a persist failure
 * cannot swallow the notification, and vice versa.
 */

#pragma once

#include "application/ports/game_store.h"
#include "game/game_events.h"

namespace chess::application {

class GameCompletionService final : public chess::game::GameEventListener {
public:
    /// `store` must outlive the service and every room it is attached
    /// to. In capability-disabled composition the injected store is a
    /// `NullGameStore` whose `capable()` returns false; the callback
    /// short-circuits before touching it.
    explicit GameCompletionService(chess::application::ports::GameStore& store);

    /// GameRoom's terminal-transition callback. Runs on the worker
    /// thread that observed the transition, outside the room mutex.
    /// Never throws (bubbles nothing back into `GameRoom`'s LockAndDrain);
    /// storage errors are logged with the typed `StorageError` code.
    void on_game_completed(const chess::game::GameCompleted& ev) override;

private:
    chess::application::ports::GameStore& store_;
};

} // namespace chess::application
