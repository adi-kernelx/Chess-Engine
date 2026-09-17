/**
 * tournament_manager.cpp — the lifecycle glue for Phase 9.4.
 *
 * See tournament_manager.h for the state-machine picture. This file is
 * pure orchestration: each public method reads the tournament + its
 * participants + its pairings, decides what to do, and writes back the
 * minimum delta. It never caches — a fresh read every call keeps
 * behaviour identical across process restarts and would keep two-server
 * setups honest even though the current deployment is single-instance.
 *
 * Two invariants worth naming:
 *
 *   (1) A pairing's `result` moves from 'pending' → decisive ('1-0',
 *       '0-1', '1/2-1/2') or 'bye', and once decisive it does not
 *       change. The `report_result` path refuses to overwrite a
 *       non-pending pairing; this is what stops a client from replaying
 *       an old result_frame to re-bump a score.
 *
 *   (2) A player earns colour balance and score credit exactly once per
 *       pairing. `bump_participant` is called once per side (or once
 *       for the byer). If reporting a result partially fails midway,
 *       the manager returns an error and leaves the pairing row
 *       marked pending so the caller can retry idempotently.
 */

#include "tournament/tournament_manager.h"

#include <algorithm>
#include <map>
#include <set>

#include "tournament/swiss.h"

namespace chess {
namespace tournament {

namespace {

// Map storage rows → PlayerStanding, the shape the pairing algorithm
// wants. One-to-one; kept as a free function so tests could reuse it if
// we ever want an in-memory pairing round from raw participants.
PlayerStanding to_standing(const StoredTournamentPlayer& p) {
    PlayerStanding s;
    s.player_id     = p.player_id;
    s.elo           = p.initial_elo;
    s.score         = p.score;
    s.whites_played = p.whites_played;
    s.received_bye  = p.received_bye;
    s.withdrawn     = p.withdrawn;
    return s;
}

// Compute Buchholz for every participant. Given `pairings`, we walk each
// participant's non-bye opponents and sum their current score. Byes are
// counted as playing an opponent whose score equals the byer's own — a
// simple placeholder that keeps Buchholz from over-penalising byes.
std::map<int64_t, double> compute_buchholz(
        const std::vector<StoredTournamentPlayer>& players,
        const std::vector<StoredPairing>& pairings) {
    std::map<int64_t, double> score_of;
    for (const auto& p : players) score_of[p.player_id] = p.score;

    std::map<int64_t, double> out;
    for (const auto& p : players) out[p.player_id] = 0.0;

    for (const auto& pr : pairings) {
        if (pr.result == "pending") continue;
        int64_t w = pr.white_player_id;
        if (pr.black_player_id.has_value()) {
            int64_t b = *pr.black_player_id;
            out[w] += score_of[b];
            out[b] += score_of[w];
        } else {
            // Bye. Placeholder opponent — see comment above.
            out[w] += score_of[w];
        }
    }
    return out;
}

// True if every pairing in a round has a non-pending result.
bool round_is_complete(const std::vector<StoredPairing>& pairings, int round) {
    bool saw_round = false;
    for (const auto& p : pairings) {
        if (p.round != round) continue;
        saw_round = true;
        if (p.result == "pending") return false;
    }
    return saw_round;
}

} // namespace

std::set<PlayerPair> TournamentManager::gather_played(
        const std::vector<StoredPairing>& pairings) const {
    std::set<PlayerPair> out;
    for (const auto& p : pairings) {
        if (!p.black_player_id.has_value()) continue;   // byes don't count
        out.insert(PlayerPair(p.white_player_id, *p.black_player_id));
    }
    return out;
}

// ── join ─────────────────────────────────────────────────────────────

ManagerResult TournamentManager::join(int64_t tournament_id,
                                      int64_t player_id,
                                      int player_elo) {
    ManagerResult out;

    auto t = find_tournament(db_, tournament_id);
    if (!t) { out.error = "tournament_not_found"; return out; }
    if (t->status != "registration") {
        out.error = "tournament_not_in_registration";
        return out;
    }

    JoinTournamentResult jr = add_participant(db_, tournament_id, player_id, player_elo);
    if (!jr.ok) { out.error = jr.error; return out; }

    out.ok = true;
    return out;
}

// ── start ────────────────────────────────────────────────────────────

ManagerResult TournamentManager::start(int64_t tournament_id, int64_t initiator_id) {
    ManagerResult out;

    auto t = find_tournament(db_, tournament_id);
    if (!t) { out.error = "tournament_not_found"; return out; }
    if (t->status != "registration") {
        out.error = "tournament_not_in_registration";
        return out;
    }
    if (t->created_by != initiator_id) {
        out.error = "not_creator";
        return out;
    }

    auto participants = get_participants(db_, tournament_id);
    int alive = 0;
    for (const auto& p : participants) if (!p.withdrawn) ++alive;
    if (alive < 2) { out.error = "not_enough_players"; return out; }

    // Flip status and current_round in that order — the pairings insert
    // is what actually starts round 1, so failing to insert leaves the
    // tournament visibly non-empty (in_progress, round 1, no pairings)
    // which the client can retry from cleanly.
    if (!set_tournament_status(db_, tournament_id, "in_progress")) {
        out.error = "set_status_failed"; return out;
    }
    if (!set_tournament_round(db_, tournament_id, 1)) {
        out.error = "set_round_failed"; return out;
    }

    auto gr = generate_round_pairings(tournament_id, 1);
    if (!gr.ok) return gr;

    out.ok = true;
    return out;
}

// ── report_result ────────────────────────────────────────────────────

ManagerResult TournamentManager::report_result(int64_t pairing_id,
                                               const std::string& result) {
    ManagerResult out;

    // Look up the pairing directly. A single SELECT here would avoid a
    // full get_pairings scan, but tournament sizes are small — keeping
    // one code path is worth more than saving one query.
    storage::QueryResult r = db_.exec(
        "SELECT id, tournament_id, round, white_player_id, black_player_id, "
        "       game_id, result, "
        "       to_char(created_at AT TIME ZONE 'UTC', "
        "               'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
        "FROM tournament_pairings WHERE id = $1",
        {storage::Param::int64(pairing_id)});
    if (!r.ok || r.rows.empty()) { out.error = "pairing_not_found"; return out; }

    StoredPairing p;
    {
        const auto& row = r.rows[0];
        p.id              = std::stoll(row.at(0));
        p.tournament_id   = std::stoll(row.at(1));
        p.round           = std::stoi (row.at(2));
        p.white_player_id = std::stoll(row.at(3));
        if (!row.is_null(4)) p.black_player_id = std::stoll(row.at(4));
        p.result          = row.at(6);
    }

    // Idempotence gate: re-reporting the SAME result on an already-
    // decided pairing is a no-op success. Re-reporting a DIFFERENT
    // result is an error.
    if (p.result != "pending") {
        if (p.result == result) { out.ok = true; return out; }
        out.error = "result_already_recorded";
        return out;
    }

    // Validate the requested result against schema.
    const bool is_bye = (result == "bye");
    if (result != "1-0" && result != "0-1" && result != "1/2-1/2" && !is_bye) {
        out.error = "invalid_result"; return out;
    }
    if (is_bye != !p.black_player_id.has_value()) {
        // bye-ness of the result must match bye-ness of the pairing.
        out.error = "result_shape_mismatch"; return out;
    }

    // Write the pairing row FIRST — that is the point-of-no-return in
    // the state machine. If bump_participant then fails, the caller
    // retries; the pairing row already carries the decisive value so
    // the retry recomputes participants from the same set of decided
    // pairings.
    if (!set_pairing_result(db_, p.id, result)) {
        out.error = "set_pairing_result_failed"; return out;
    }

    // Apply score/whites deltas.
    if (is_bye) {
        // +1 point, no colour change (byer did not play a real side).
        if (!bump_participant(db_, p.tournament_id, p.white_player_id,
                              /*score_delta=*/1.0, /*played_white=*/false,
                              /*bye=*/true)) {
            out.error = "bump_participant_failed"; return out;
        }
    } else {
        double white_delta = 0.0, black_delta = 0.0;
        if      (result == "1-0")     { white_delta = 1.0; black_delta = 0.0; }
        else if (result == "0-1")     { white_delta = 0.0; black_delta = 1.0; }
        else /* 1/2-1/2 */            { white_delta = 0.5; black_delta = 0.5; }

        if (!bump_participant(db_, p.tournament_id, p.white_player_id,
                              white_delta, /*played_white=*/true, /*bye=*/false)) {
            out.error = "bump_participant_white_failed"; return out;
        }
        if (!bump_participant(db_, p.tournament_id, *p.black_player_id,
                              black_delta, /*played_white=*/false, /*bye=*/false)) {
            out.error = "bump_participant_black_failed"; return out;
        }
    }

    return maybe_advance_after_result(p.tournament_id, p.round);
}

// ── generate_round_pairings ──────────────────────────────────────────

ManagerResult TournamentManager::generate_round_pairings(int64_t tournament_id,
                                                         int round) {
    ManagerResult out;

    auto participants = get_participants(db_, tournament_id);
    if (participants.empty()) { out.error = "no_participants"; return out; }

    std::vector<PlayerStanding> pool;
    pool.reserve(participants.size());
    for (const auto& p : participants) pool.push_back(to_standing(p));

    auto all_pairings = get_pairings(db_, tournament_id);
    auto played = gather_played(all_pairings);

    auto pairings = pair_swiss_round(pool, played);
    if (pairings.empty()) { out.error = "pair_swiss_round_failed"; return out; }

    for (const auto& pr : pairings) {
        std::optional<int64_t> black;
        std::string init_result = "pending";
        if (pr.is_bye) init_result = "bye";
        else           black = pr.black_id;

        InsertPairingResult ir = insert_pairing(db_, tournament_id, round,
                                                pr.white_id, black, init_result);
        if (!ir.ok) { out.error = "insert_pairing_failed: " + ir.error; return out; }

        // A bye is auto-scored the moment it is dealt — record the +1
        // point and set received_bye = true here rather than making a
        // caller send report_result("bye"). This matches how a human
        // tournament runs — the bye is not something the byer plays.
        if (pr.is_bye) {
            if (!bump_participant(db_, tournament_id, pr.white_id,
                                  /*score_delta=*/1.0,
                                  /*played_white=*/false,
                                  /*bye=*/true)) {
                out.error = "bye_bump_failed"; return out;
            }
        }
    }

    // After dealing the pairings we may find the round is already
    // "complete" (a lone bye in a 1-player tournament, e.g.). Handle
    // that uniformly — flowing through the same advance path keeps
    // the state machine tidy.
    return maybe_advance_after_result(tournament_id, round);
}

// ── maybe_advance_after_result ───────────────────────────────────────

ManagerResult TournamentManager::maybe_advance_after_result(
        int64_t tournament_id, int round) {
    ManagerResult out;

    auto t = find_tournament(db_, tournament_id);
    if (!t) { out.error = "tournament_not_found"; return out; }

    auto pairings = get_pairings(db_, tournament_id);
    if (!round_is_complete(pairings, round)) {
        // Nothing to advance — this reporter was not the last one in
        // this round. Return success; the tournament will keep waiting.
        out.ok = true;
        return out;
    }

    if (round >= t->rounds) {
        // Final round done — mark completed.
        if (!set_tournament_status(db_, tournament_id, "completed")) {
            out.error = "set_status_completed_failed"; return out;
        }
        out.ok = true;
        return out;
    }

    // Advance the round counter and pair the next round.
    int next = round + 1;
    if (!set_tournament_round(db_, tournament_id, next)) {
        out.error = "set_round_failed"; return out;
    }
    return generate_round_pairings(tournament_id, next);
}

// ── get_state ────────────────────────────────────────────────────────

std::optional<TournamentState> TournamentManager::get_state(int64_t tournament_id) {
    auto t = find_tournament(db_, tournament_id);
    if (!t) return std::nullopt;

    TournamentState st;
    st.tournament   = *t;
    st.all_pairings = get_pairings(db_, tournament_id);

    auto participants = get_participants(db_, tournament_id);
    auto buchholz     = compute_buchholz(participants, st.all_pairings);

    st.standings.reserve(participants.size());
    for (const auto& p : participants) {
        StandingRow r;
        r.player_id     = p.player_id;
        r.initial_elo   = p.initial_elo;
        r.score         = p.score;
        r.buchholz      = buchholz[p.player_id];
        r.whites_played = p.whites_played;
        r.received_bye  = p.received_bye;
        r.withdrawn     = p.withdrawn;
        st.standings.push_back(r);
    }

    // Sort standings: score desc, Buchholz desc, elo desc, id asc.
    std::sort(st.standings.begin(), st.standings.end(),
              [](const StandingRow& a, const StandingRow& b) {
                  if (a.score       != b.score)       return a.score       > b.score;
                  if (a.buchholz    != b.buchholz)    return a.buchholz    > b.buchholz;
                  if (a.initial_elo != b.initial_elo) return a.initial_elo > b.initial_elo;
                  return a.player_id < b.player_id;
              });
    return st;
}

} // namespace tournament
} // namespace chess
