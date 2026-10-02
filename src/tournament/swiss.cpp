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
 *   4. Minimum-cost complete matching: avoid rematches, then minimize
 *      the sum of absolute initial-rating differences, then score gaps.
 *      Memoized branch-and-bound searches complete rounds instead of
 *      committing to each player's nearest available opponent greedily.
 *      The exact search has exponential worst-case cost; this is intended
 *      for small local fields, not federation-scale events.
 *   5. Assign colours: whoever has held White fewer times gets White;
 *      tie broken by higher seed (better score/elo) → White, matching
 *      the convention that stronger players sit White in tiebreakers.
 */

#include "tournament/swiss.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <cmath>
#include <tuple>
#include <unordered_map>

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
                                      const std::set<PlayerPair>& played,
                                      const std::set<PlayerPair>& forbidden) {
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
    if (pool.size() % 2 == 1 && forbidden.empty()) {
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

    // (4) Exact minimum-cost matching for the project's small tournament
    // fields. Minimize (rematches, total absolute rating gap, score gap), in
    // that order. A locally nearest opponent is not necessarily the best
    // complete round, especially when prior meetings forbid some edges.
    struct Cost {
        int64_t rematches = 0, rating_gap = 0, score_gap = 0;
        int64_t byes = 0, repeated_byes = 0, bye_rank = 0;
        bool operator<(const Cost& other) const {
            return std::tie(byes, repeated_byes, bye_rank, rematches, rating_gap, score_gap)
                < std::tie(other.byes, other.repeated_byes, other.bye_rank, other.rematches, other.rating_gap, other.score_gap);
        }
        Cost operator+(const Cost& other) const {
            return {rematches + other.rematches, rating_gap + other.rating_gap,
                    score_gap + other.score_gap, byes + other.byes,
                    repeated_byes + other.repeated_byes, bye_rank + other.bye_rank};
        }
    };
    const std::size_t n = pool.size();
    std::vector<bool> taken(n, false);
    std::vector<std::pair<std::size_t, std::size_t>> chosen; // (idx_a, idx_b)
    std::vector<std::pair<std::size_t, std::size_t>> best_chosen;
    Cost best;
    bool have_best = false;
    std::unordered_map<std::string, Cost> prefixes;
    std::vector<std::vector<Cost>> edge(n, std::vector<Cost>(n));
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            edge[i][j] = edge[j][i] = {
                have_played(played, pool[i].player_id, pool[j].player_id) ? 1 : 0,
                std::abs(int64_t(pool[i].elo) - int64_t(pool[j].elo)),
                std::llround(2 * std::abs(pool[i].score - pool[j].score))};
        }
    }
    std::function<void(Cost)> search = [&](Cost cost) {
        std::size_t i = 0;
        while (i < n && taken[i]) ++i;
        if (i == n) {
            if (!have_best || cost < best) {
                have_best = true; best = cost; best_chosen = chosen;
            }
            return;
        }
        // Pairing adjacent sorted values is a lower bound for any complete
        // matching's rating/score distance, ignoring forbidden prior meetings.
        std::vector<int64_t> ratings, scores;
        int64_t forced = 0;
        std::string key(n, '0');
        for (std::size_t a = 0; a < n; ++a) {
            if (taken[a]) { key[a] = '1'; continue; }
            ratings.push_back(pool[a].elo);
            scores.push_back(std::llround(2 * pool[a].score));
            bool fresh = false;
            for (std::size_t b = 0; b < n; ++b) {
                if (a != b && !taken[b] && edge[a][b].rematches == 0) { fresh = true; break; }
            }
            if (!fresh) ++forced;
        }
        Cost lower{(forced + 1) / 2, 0, 0};
        std::sort(ratings.begin(), ratings.end());
        std::sort(scores.begin(), scores.end());
        for (std::size_t a = 0; a + 1 < ratings.size(); a += 2) {
            lower.rating_gap += ratings[a + 1] - ratings[a];
            lower.score_gap += scores[a + 1] - scores[a];
        }
        // Partial matching can leave nodes unmatched, so distance/rematch
        // lower bounds for a complete matching are not admissible here.
        if (!forbidden.empty()) lower = Cost{};
        if (have_best && !(cost + lower < best)) return;
        auto prior = prefixes.find(key);
        if (prior != prefixes.end() && !(cost < prior->second)) return;
        prefixes[key] = cost;
        std::vector<std::size_t> candidates;
        for (std::size_t j = i + 1; j < n; ++j) {
            if (!taken[j] && !forbidden.count(PlayerPair(pool[i].player_id, pool[j].player_id))) candidates.push_back(j);
        }
        std::stable_sort(candidates.begin(), candidates.end(), [&](std::size_t a, std::size_t b) {
            return edge[i][a] < edge[i][b];
        });
        for (std::size_t j : candidates) {
            taken[i] = taken[j] = true;
            chosen.push_back({i, j});
            search(cost + edge[i][j]);
            chosen.pop_back();
            taken[i] = taken[j] = false;
        }
        if (!forbidden.empty()) {
            taken[i] = true;
            chosen.push_back({i, i});
            Cost bye_cost;
            bye_cost.byes = 1;
            bye_cost.repeated_byes = pool[i].received_bye ? 1 : 0;
            bye_cost.bye_rank = static_cast<int64_t>(n - i);
            search(cost + bye_cost);
            chosen.pop_back();
            taken[i] = false;
        }
    };
    search({});
    chosen = std::move(best_chosen);

    // Emit pairings in the order they were chosen — top score first.
    for (const auto& c : chosen) {
        const PlayerStanding& a = pool[c.first];
        if (c.first == c.second) {
            tail.push_back({a.player_id, 0, true});
            continue;
        }
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
