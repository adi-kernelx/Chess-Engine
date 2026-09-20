/**
 * application/ports/move_selector.h — the "who picks the AI move" port (LLD-6.2).
 *
 * WHY A PORT
 *   `GameplayService` (and `GameCompletionService`, and future
 *   analysis code) currently reach into `AIPlayer` — which owns a
 *   real `chess::engine::Engine` — to compute AI moves. That works
 *   in production and it works in most tests, but a service-level
 *   test that only wants to verify "AI slot is asked for a move
 *   after the human plays" pays a full alpha-beta search worth of
 *   wall-clock time and CPU per test. A port with a fake lets
 *   service tests substitute a `MoveSelector` that returns a canned
 *   move, without linking the engine.
 *
 * WHY NOT A CLASS PER DIFFICULTY
 *   The plan §5 is explicit: "Difficulty alone is a SearchLimits /
 *   config value, not four classes." `SearchLimits` carries the
 *   pair `(max_depth, time_ms)` that used to live in the private
 *   `difficulty_params` switch in `ai_player.cpp`. The port speaks
 *   `SearchLimits`, and any caller that has an `AIDifficulty` can
 *   materialise the limits with `limits_for(difficulty)`.
 *
 * WHY NOT USE `AIMove` AS THE RESPONSE
 *   `AIMove` (`game/ai_player.h`) carries engine-specific stats
 *   (score, depth, nodes, elapsed_ms) that a use-case boundary has
 *   no business demanding. The port returns `MoveChoice` — just the
 *   move fields (`from`, `to`, `promotion`) plus a `nodes` counter
 *   for the one caller (`persist_game`) that still logs it. If a
 *   future feature wants full stats, the concrete `EngineMoveSelector`
 *   can expose them through its own type; the port stays narrow.
 */

#pragma once

#include <cstdint>

#include "chess/board.h"
#include "core/types.h"

namespace chess::application::ports {

/// Configuration for a single search. The two knobs the engine
/// honours today. `max_depth = 0` means "unbounded until time runs
/// out"; `time_ms = 0` means "run to depth". Both zero is a
/// degenerate case the fake returns immediately for.
struct SearchLimits {
    int max_depth = 4;
    int time_ms   = 1000;
};

/// The result of one selection. Deliberately narrower than
/// `game::AIMove` — no score, no depth, no engine internals.
struct MoveChoice {
    Square    from      = NO_SQUARE;
    Square    to        = NO_SQUARE;
    PieceType promotion = PieceType::NONE;
    long      nodes     = 0;   ///< best-effort stat for logging
    int       elapsed_ms = 0;
};

/// The port. One method. Implementations are stateful (they own
/// TT / engine state) but the port itself makes no promise about
/// thread safety — callers must not share a `MoveSelector` across
/// threads without external synchronisation.
class MoveSelector {
public:
    virtual ~MoveSelector() = default;

    /// Compute a move for the side-to-move in `board` under
    /// `limits`. Implementations must return a legal move if any
    /// exists; if `board` is checkmate / stalemate the result's
    /// `from` and `to` are `NO_SQUARE`.
    virtual MoveChoice select(const Board& board, const SearchLimits& limits) = 0;
};

} // namespace chess::application::ports
