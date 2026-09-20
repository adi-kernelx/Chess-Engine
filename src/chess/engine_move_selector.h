/**
 * chess/engine_move_selector.h — real MoveSelector over the alpha-beta engine.
 *
 * Owns a private `chess::engine::Engine` instance (16 MB TT — same
 * as `AIPlayer`) and adapts the port's `select(board, limits)` call
 * to `engine.set_position(board); engine.search(time, depth)`.
 *
 * Not thread-safe: one instance per parallel AI game. This matches
 * the pre-refactor `AIPlayer` invariant.
 */

#pragma once

#include "application/ports/move_selector.h"
#include "chess/engine.h"

namespace chess {

class EngineMoveSelector final : public application::ports::MoveSelector {
public:
    /// Construct with the default 16 MB TT — matches `AIPlayer`.
    /// Tests that want a smaller footprint can pass their own size.
    explicit EngineMoveSelector(size_t tt_size_mb = 16);

    application::ports::MoveChoice select(
        const Board& board,
        const application::ports::SearchLimits& limits) override;

    /// Expose the underlying engine for future consumers that need
    /// engine-specific knobs (e.g. `clear_tt`). Not part of the
    /// port contract.
    engine::Engine& engine() { return engine_; }

private:
    engine::Engine engine_;
};

} // namespace chess
