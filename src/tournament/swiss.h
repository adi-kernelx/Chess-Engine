/**
 * swiss.h — Phase 9.4.
 *
 * Swiss-system pairing, one round at a time. Pure functions: no database,
 * no engine, no I/O. Callers hand in the current standings plus history,
 * and get back one round's pairings.
 *
 * PROJECT PAIRING POLICY (RATING-FIRST, NOT FIDE DUTCH)
 *
 * Requested policy: minimize the sum of absolute rating differences over
 * the whole round, after minimizing rematches. Score differences are the
 * next tiebreak. This intentionally differs from standard score-first Swiss
 * grouping. Ratings are frozen at registration, so results changing global
 * ELO do not change the event's seeds mid-tournament.
 *
 * An exact memoized branch-and-bound search is appropriate for the small
 * local fields this project targets. Its worst case is exponential; larger
 * events should use a polynomial minimum-weight general matching solver.
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
    int     elo            = 1200;///< Initial tournament rating (matching cost)
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
/// where possible. If a full fresh round is impossible, the fewest repeated
/// pairings are used. Within that constraint the total rating gap is minimal.
///
/// The returned vector may include one bye pairing (last, by
/// convention). Ordering and colour assignment are deterministic given the
/// inputs; the highest-seeded remaining player is emitted first.
std::vector<Pairing> pair_swiss_round(const std::vector<PlayerStanding>& standings,
                                      const std::set<PlayerPair>& played,
                                      const std::set<PlayerPair>& forbidden = {});
// With hard forbidden edges (custom draw cap), maximize playable pairs first;
// unmatched players advance by bye. Swiss callers pass no forbidden edges.

/// True if `a` and `b` have already met (helper for tests).
inline bool have_played(const std::set<PlayerPair>& played,
                        int64_t a, int64_t b) {
    return played.count(PlayerPair(a, b)) > 0;
}

} // namespace tournament
} // namespace chess
