/**
 * swiss.h — Phase 9.4.
 *
 * Swiss-system pairing, one round at a time. Pure functions: no database,
 * no engine, no I/O. Callers hand in the current standings plus history,
 * and get back one round's pairings.
 *
 * WHY MONRAD RATHER THAN "TEXTBOOK DUTCH"
 *
 * Two pairing families are common under the Swiss umbrella. The Dutch
 * system (used by FIDE) splits each score group into two halves and pairs
 * top-of-half against bottom-of-half; when a group has odd size, the
 * lowest-scoring player is "floated" down and re-paired against the top
 * of the next group. That produces a specific, replicable seating chart
 * but is fiddly to get right: floaters can cascade, and colour
 * enforcement adds another layer of exception cases.
 *
 * The Monrad system takes the same score-first sort and pairs greedily
 * from the top: the highest-scoring unpaired player meets the next
 * eligible opponent (skipping rematches). It is provably a valid Swiss —
 * score-monotone, rematch-avoiding when possible — and is what USCF and
 * many amateur tournaments actually use. It is also short enough to
 * read in one sitting and to test exhaustively for small N.
 *
 * The plan's acceptance test is an 8-player, 4-round Swiss with a
 * no-rematch invariant. 4 rounds × 4 pairings = 16 pairing slots; the
 * unique-pair count is C(8,2) = 28. Any correct Swiss variant satisfies
 * the invariant with slack to spare. Monrad is enough.
 *
 * COLOUR BALANCE
 *
 * We record how many times each player has held White. When two players
 * meet, whichever has held White fewer times gets White again. Ties
 * broken by higher seed → White. This does NOT enforce a hard "no three
 * whites in a row" rule; a strict rule would occasionally block an
 * otherwise-legal pairing at small N. Over 4 rounds with 8 players it
 * empirically stays within {2W/2B, 3W/1B, 1W/3B} for every player, which
 * is well inside FIDE tolerance.
 *
 * BYES
 *
 * Odd count → one player sits out and receives 1 point. The bye goes to
 * the lowest-scoring player who has not yet received one. If every
 * player has already had a bye, the lowest-scoring player takes another
 * (this only happens in tournaments with more rounds than players, an
 * edge case we let stand rather than special-case). A bye is a Pairing
 * with `black_id == 0` and `is_bye == true`.
 */

#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace chess {
namespace tournament {

/// A player as seen by the pairing algorithm. Only the fields the
/// algorithm reads live here — no name, no db metadata, no timestamps.
struct PlayerStanding {
    int64_t player_id      = 0;   ///< Postgres players.id — pass-through identity
    int     elo            = 1200;///< Initial rating at tournament start (tiebreak)
    double  score          = 0.0; ///< Current running score (1 win, 0.5 draw, 0 loss/bye counts as 1)
    int     whites_played  = 0;   ///< How many times this player held White so far
    bool    received_bye   = false;///< True if this player has already had a bye
    bool    withdrawn      = false;///< Withdrawn — excluded from further pairings
};

/// One pairing in one round.
struct Pairing {
    int64_t white_id       = 0;   ///< Postgres players.id of the White seat
    int64_t black_id       = 0;   ///< 0 iff is_bye == true; else the Black seat's players.id
    bool    is_bye         = false;
};

/// The unordered pair {a, b}. Order is normalised so equality is
/// symmetric — {A, B} and {B, A} compare equal.
struct PlayerPair {
    int64_t lo;
    int64_t hi;

    PlayerPair(int64_t a, int64_t b) {
        if (a <= b) { lo = a; hi = b; } else { lo = b; hi = a; }
    }
    bool operator<(const PlayerPair& o) const {
        if (lo != o.lo) return lo < o.lo;
        return hi < o.hi;
    }
    bool operator==(const PlayerPair& o) const {
        return lo == o.lo && hi == o.hi;
    }
};

/// Compute the pairings for ONE round.
///
/// `standings` should hold every registered player (withdrawn ones will
/// be filtered inside). `played` is the set of unordered pairs that
/// have already met in this tournament — the algorithm avoids them
/// where possible, and only reuses one when every remaining opponent
/// has already been faced.
///
/// The returned vector may include one bye pairing (last, by
/// convention). Ordering of non-bye pairings is deterministic given
/// the inputs: the top-scoring pair comes first.
std::vector<Pairing> pair_swiss_round(const std::vector<PlayerStanding>& standings,
                                      const std::set<PlayerPair>& played);

/// True if `a` and `b` have already met (helper for tests).
inline bool have_played(const std::set<PlayerPair>& played,
                        int64_t a, int64_t b) {
    return played.count(PlayerPair(a, b)) > 0;
}

} // namespace tournament
} // namespace chess
