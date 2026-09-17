/**
 * swiss.cpp — see header for the algorithm's rationale and invariants.
 *
 * The public function is `pair_swiss_round`. Internally it runs a small
 * pipeline:
 *
 *   1. Filter out withdrawn players.
 *   2. Sort by (score desc, elo desc, player_id asc). player_id is the
 *      final tiebreak so pairings are deterministic across runs — two
 *      players with identical score and rating would otherwise ping-pong
 *      between orderings from one round to the next.
 *   3. If the remaining count is odd, pull the bye player out.
 *   4. Backtracking search for a no-rematch pairing. Small-N brute
 *      force: the search space for 8 players is 8!! = 105 pairings and
 *      the search short-circuits as soon as one valid combination is
 *      found. The naïve greedy — take the top unpaired, pair with the
 *      best remaining unplayed — commits early and hits dead-ends by
 *      round 4 with 8 seats (verified against the test harness), so we
 *      spend the extra branching to keep the "avoid rematches"
 *      guarantee real. If no rematch-free pairing exists (mathematically
 *      unavoidable once every player has faced every other), we fall
 *      back to a plain greedy that accepts a rematch.
 *   5. Assign colours: whoever has held White fewer times gets White;
 *      tie broken by higher seed (better score/elo) → White, matching
 *      the convention that stronger players sit White in tiebreakers.
 */

#include "tournament/swiss.h"

#include <algorithm>
#include <cstddef>
#include <functional>

namespace chess {
namespace tournament {

namespace {

// Comparator for the top-of-list sort. Highest score first; then highest
// elo (initial seed); then lowest player_id so the sort is total.
bool higher_seed(const PlayerStanding& a, const PlayerStanding& b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.elo   != b.elo)   return a.elo   > b.elo;
    return a.player_id < b.player_id;
}

// Given two players who WILL be paired, decide who plays White.
// The rule is: whoever has held White fewer times gets White. On a tie,
// the higher-seeded player (per higher_seed above) gets White.
std::pair<int64_t, int64_t> assign_colors(const PlayerStanding& a,
                                          const PlayerStanding& b) {
    if (a.whites_played < b.whites_played) return {a.player_id, b.player_id};
    if (a.whites_played > b.whites_played) return {b.player_id, a.player_id};
    // Equal whites — higher seed gets White.
    if (higher_seed(a, b))                 return {a.player_id, b.player_id};
    return {b.player_id, a.player_id};
}

} // namespace

std::vector<Pairing> pair_swiss_round(const std::vector<PlayerStanding>& standings_in,
                                      const std::set<PlayerPair>& played) {
    std::vector<Pairing> result;

    // (1) filter out withdrawn players — they receive no pairing at all,
    // not even a bye. A withdrawn player is off the tournament board.
    std::vector<PlayerStanding> pool;
    pool.reserve(standings_in.size());
    for (const auto& p : standings_in) if (!p.withdrawn) pool.push_back(p);

    if (pool.empty()) return result;

    // (2) sort by (score desc, elo desc, id asc). Stable is unnecessary
    // because the comparator is total.
    std::sort(pool.begin(), pool.end(), higher_seed);

    // (3) handle odd count → carve off the bye. The bye is placed LAST
    // in the output so that non-bye pairings occupy indices [0..k) in
    // seeded order, which makes tests easier to read.
    std::vector<Pairing> tail;
    if (pool.size() % 2 == 1) {
        // Search from the end (lowest-scoring) for a player with no bye yet.
        // If every remaining player has already had one, fall back to the
        // very last player (lowest-scoring overall).
        std::size_t bye_idx = pool.size(); // "not found" sentinel
        for (std::size_t i = pool.size(); i-- > 0;) {
            if (!pool[i].received_bye) { bye_idx = i; break; }
        }
        if (bye_idx == pool.size()) bye_idx = pool.size() - 1;

        Pairing bye_p;
        bye_p.white_id = pool[bye_idx].player_id;
        bye_p.black_id = 0;
        bye_p.is_bye   = true;
        tail.push_back(bye_p);

        pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(bye_idx));
    }

    // (4) backtracking search for a no-rematch pairing.
    //
    // At each step, take the smallest-index unpaired player (which is
    // the highest-scoring unpaired, since pool is score-sorted) and try
    // every remaining unplayed opponent in seed order. If a branch
    // fails, undo and try the next. This is exponential in principle
    // but branches are pruned aggressively: on the first branch that
    // covers every seat, we return. For the small pools this system
    // targets (tournaments in the tens of players), the runtime is
    // negligible.
    const std::size_t n = pool.size();
    std::vector<bool> taken(n, false);
    std::vector<std::pair<std::size_t, std::size_t>> chosen; // (idx_a, idx_b)

    std::function<bool()> backtrack = [&]() -> bool {
        std::size_t i = 0;
        while (i < n && taken[i]) ++i;
        if (i == n) return true;   // every seat placed

        for (std::size_t j = i + 1; j < n; ++j) {
            if (taken[j]) continue;
            if (have_played(played, pool[i].player_id, pool[j].player_id)) continue;
            taken[i] = taken[j] = true;
            chosen.push_back({i, j});
            if (backtrack()) return true;
            chosen.pop_back();
            taken[i] = taken[j] = false;
        }
        return false;
    };

    if (!backtrack()) {
        // No rematch-free pairing exists. Fall back to a plain greedy
        // that accepts rematches — the tournament has to keep going.
        std::fill(taken.begin(), taken.end(), false);
        chosen.clear();
        for (std::size_t i = 0; i < n; ++i) {
            if (taken[i]) continue;
            std::size_t match = n;
            for (std::size_t j = i + 1; j < n; ++j) {
                if (!taken[j]) { match = j; break; }
            }
            if (match == n) { result.clear(); return result; }
            taken[i] = taken[match] = true;
            chosen.push_back({i, match});
        }
    }

    // Emit pairings in the order they were chosen — top score first.
    for (const auto& c : chosen) {
        const PlayerStanding& a = pool[c.first];
        const PlayerStanding& b = pool[c.second];
        auto colors = assign_colors(a, b);
        Pairing pr;
        pr.white_id = colors.first;
        pr.black_id = colors.second;
        pr.is_bye   = false;
        result.push_back(pr);
    }

    // Bye pairing (if any) goes at the tail.
    for (const auto& p : tail) result.push_back(p);
    return result;
}

} // namespace tournament
} // namespace chess
