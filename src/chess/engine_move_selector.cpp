#include "chess/engine_move_selector.h"

namespace chess {

EngineMoveSelector::EngineMoveSelector(size_t tt_size_mb)
    : engine_(tt_size_mb) {}

application::ports::MoveChoice
EngineMoveSelector::select(const Board& board,
                            const application::ports::SearchLimits& limits) {
    engine_.set_position(board);
    auto r = engine_.search(limits.time_ms, limits.max_depth);
    application::ports::MoveChoice out;
    out.from       = r.best_move.from;
    out.to         = r.best_move.to;
    out.promotion  = r.best_move.promo_type;
    out.nodes      = r.nodes;
    out.elapsed_ms = r.elapsed_ms;
    return out;
}

} // namespace chess
