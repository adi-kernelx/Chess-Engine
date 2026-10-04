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
#include <tuple>

#include "tournament/swiss.h"
#include "storage/transaction.h"

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

bool TournamentManager::valid_schedule(int64_t registration_deadline_unix,
                                       int64_t first_round_start_unix,
                                       int round_duration_seconds) {
    return registration_deadline_unix > 0
        && first_round_start_unix >= registration_deadline_unix + 30
        && round_duration_seconds >= 60;
}

int64_t TournamentManager::earliest_round_start(int64_t first_round_start_unix,
                                                int round_duration_seconds,
                                                int round) {
    if (round <= 0) return first_round_start_unix;
    return first_round_start_unix + static_cast<int64_t>(round - 1) * round_duration_seconds;
}

std::optional<std::string> TournamentManager::no_show_result(bool white_checked_in,
                                                             bool black_checked_in) {
    if (white_checked_in && !black_checked_in) return std::string("1-0");
    if (!white_checked_in && black_checked_in) return std::string("0-1");
    if (!white_checked_in && !black_checked_in) return std::string("double_forfeit");
    return std::nullopt;
}

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
    auto operation = db_.acquire_operation();

    auto t = find_tournament(db_, tournament_id);
    if (!t) { out.error = "tournament_not_found"; return out; }
    if (t->status != "registration" || !t->registration_open
        || clock_.unix_seconds() >= std::min(t->registration_deadline_unix, t->first_round_starts_at_unix - 90)) {
        out.error = "registration_closed";
        return out;
    }

    JoinTournamentResult jr = add_participant(db_, tournament_id, player_id, player_elo,
                                              clock_.unix_seconds());
    if (!jr.ok) { out.error = jr.error; return out; }

    out.ok = true;
    return out;
}

ManagerResult TournamentManager::leave(int64_t tournament_id, int64_t player_id) {
    ManagerResult out;
    auto operation = db_.acquire_operation();
    auto t = find_tournament(db_, tournament_id);
    if (!t) { out.error = "tournament_not_found"; return out; }
    if (t->status != "registration" || !t->registration_open
        || clock_.unix_seconds() >= std::min(t->registration_deadline_unix, t->first_round_starts_at_unix - 90)) {
        out.error = "registration_closed";
        return out;
    }
    auto removed = remove_participant(db_, tournament_id, player_id,
                                      clock_.unix_seconds());
    if (!removed.ok) { out.error = removed.error; return out; }
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

    return set_registration(tournament_id, initiator_id, false);
}

ManagerResult TournamentManager::set_registration(int64_t tournament_id,
                                                   int64_t initiator_id,
                                                   bool open) {
    ManagerResult out;
    auto operation = db_.acquire_operation();
    auto t = find_tournament(db_, tournament_id);
    if (!t) { out.error = "tournament_not_found"; return out; }
    if (t->created_by != initiator_id) { out.error = "not_creator"; return out; }
    if (t->status != "registration" && t->status != "scheduled") {
        out.error = "registration_state_locked"; return out;
    }
    if (open && clock_.unix_seconds() >= t->registration_deadline_unix) {
        out.error = "registration_deadline_passed"; return out;
    }
    if (open && (clock_.unix_seconds() >= t->first_round_starts_at_unix - 90
        || !get_pairings_for_round(db_, tournament_id, 1).empty())) {
        out.error = "pairings_locked"; return out;
    }
    const int64_t now = clock_.unix_seconds();
    if (!set_registration_open(db_, tournament_id, open, now)) {
        // The repository repeats the deadline predicate in the UPDATE so a
        // close/reopen request racing the deadline cannot pass a stale read.
        // Preserve the specific public error when that authoritative guard
        // rejects the reopen.
        if (open) {
            auto latest = find_tournament(db_, tournament_id);
            if (latest && now >= latest->registration_deadline_unix) {
                out.error = "registration_deadline_passed";
                return out;
            }
        }
        out.error = "set_registration_failed"; return out;
    }
    out.ok = true;
    return out;
}

ManagerResult TournamentManager::check_in(int64_t tournament_id, int round,
                                          int64_t player_id) {
    ManagerResult out;
    std::string error;
    if (round <= 0) { out.error = "invalid_round"; return out; }
    const auto tournament = find_tournament(db_, tournament_id);
    if (tournament && tournament->format == "winners_advance" && round > 1) {
        bool eligible = false;
        for (const auto& p : get_pairings_for_round(db_, tournament_id, round - 1)) {
            const bool white = p.white_player_id == player_id;
            const bool black = p.black_player_id && *p.black_player_id == player_id;
            if ((white && (p.result == "1-0" || p.result == "bye" || p.result == "1/2-1/2"))
                || (black && (p.result == "0-1" || p.result == "1/2-1/2"))) eligible = true;
        }
        if (!eligible) { out.error = "player_eliminated"; return out; }
    }
    for (const auto& pairing : get_pairings_for_round(db_, tournament_id, round)) {
        if (pairing.white_player_id != player_id
            && (!pairing.black_player_id || *pairing.black_player_id != player_id)) continue;
        if (pairing.result == "bye") {
            out.error = "round_already_resolved_for_player";
            return out;
        }
    }
    if (!check_in_player(db_, tournament_id, round, player_id,
                         clock_.unix_seconds(), error)) {
        out.error = error; return out;
    }
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

    return maybe_advance_after_result(p.tournament_id, p.round);
}

// ── generate_round_pairings ──────────────────────────────────────────

ManagerResult TournamentManager::generate_round_pairings(int64_t tournament_id,
                                                         int round) {
    ManagerResult out;
    const auto t = find_tournament(db_, tournament_id);
    if (!t) { out.error = "tournament_not_found"; return out; }

    auto participants = get_participants(db_, tournament_id);
    if (participants.empty()) { out.error = "no_participants"; return out; }

    std::vector<PlayerStanding> pool;
    pool.reserve(participants.size());
    for (const auto& p : participants) pool.push_back(to_standing(p));

    auto all_pairings = get_pairings(db_, tournament_id);
    auto played = gather_played(all_pairings);
    std::vector<Pairing> forced;
    std::set<PlayerPair> draw_capped;
    if (t->format == "winners_advance" && round > 1) {
        std::set<int64_t> advancing;
        std::set<int64_t> replaying;
        for (const auto& pr : all_pairings) {
            if (pr.round != round - 1) continue;
            if (pr.result == "bye" || pr.result == "1-0") advancing.insert(pr.white_player_id);
            else if (pr.result == "0-1" && pr.black_player_id) advancing.insert(*pr.black_player_id);
            else if (pr.result == "1/2-1/2" && pr.black_player_id) {
                advancing.insert(pr.white_player_id);
                advancing.insert(*pr.black_player_id);
                int draws = 0;
                for (const auto& past : all_pairings) {
                    if (past.round >= round || past.result != "1/2-1/2" || !past.black_player_id) continue;
                    if (PlayerPair(past.white_player_id, *past.black_player_id)
                        == PlayerPair(pr.white_player_id, *pr.black_player_id)) ++draws;
                }
                if (draws < 2) {
                    // Replay the same pair with colors reversed; other winners
                    // may continue in parallel. No alternate gameplay stack.
                    forced.push_back({*pr.black_player_id, pr.white_player_id, false});
                    replaying.insert(pr.white_player_id);
                    replaying.insert(*pr.black_player_id);
                }
            }
        }
        pool.erase(std::remove_if(pool.begin(), pool.end(), [&](const auto& p) {
            return !advancing.count(p.player_id) || replaying.count(p.player_id);
        }), pool.end());
    }
    if (t->format == "winners_advance") {
        std::map<PlayerPair, int> draws;
        for (const auto& p : all_pairings) {
            if (p.result == "1/2-1/2" && p.black_player_id
                && ++draws[PlayerPair(p.white_player_id, *p.black_player_id)] >= 2)
                draw_capped.insert(PlayerPair(p.white_player_id, *p.black_player_id));
        }
    }
    auto pairings = pair_swiss_round(pool, played, draw_capped);
    pairings.insert(pairings.begin(), forced.begin(), forced.end());
    if (pairings.empty()) { out.error = "pair_swiss_round_failed"; return out; }

    for (const auto& pr : pairings) {
        std::optional<int64_t> black;
        std::string init_result = "pending";
        if (pr.is_bye) init_result = "bye";
        else           black = pr.black_id;

        InsertPairingResult ir = insert_pairing(db_, tournament_id, round,
                                                pr.white_id, black, init_result);
        if (!ir.ok) { out.error = "insert_pairing_failed: " + ir.error; return out; }

    }

    if (!recompute_participant_totals(db_, tournament_id)) {
        out.error = "recompute_standings_failed"; return out;
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
    [[maybe_unused]] auto operation = db_.acquire_operation();

    auto t = find_tournament(db_, tournament_id);
    if (!t) { out.error = "tournament_not_found"; return out; }

    auto pairings = get_pairings(db_, tournament_id);
    if (!round_is_complete(pairings, round)) {
        // Nothing to advance — this reporter was not the last one in
        // this round. Return success; the tournament will keep waiting.
        out.ok = true;
        return out;
    }

    auto completed_round = db_.exec(
        "UPDATE tournament_rounds SET status='completed', completed_at=now() "
        "WHERE tournament_id=$1 AND round=$2 AND status<>'completed'",
        {storage::Param::int64(tournament_id), storage::Param::int64(round)});
    if (!completed_round.ok) { out.error = "complete_round_failed"; return out; }

    bool elimination_complete = false;
    if (t->format == "winners_advance") {
        // Ignore historical games: only the latest stage determines survivors.
        std::set<int64_t> survivors;
        for (const auto& p : pairings) {
            if (p.round != round) continue;
            if (p.result == "bye" || p.result == "1-0") survivors.insert(p.white_player_id);
            if (p.result == "0-1" && p.black_player_id) survivors.insert(*p.black_player_id);
            if (p.result == "1/2-1/2" && p.black_player_id) {
                survivors.insert(p.white_player_id); survivors.insert(*p.black_player_id);
            }
        }
        elimination_complete = survivors.size() <= 1;
        if (!elimination_complete) {
            std::map<PlayerPair, int> draws;
            for (const auto& p : pairings) {
                if (p.round <= round && p.result == "1/2-1/2" && p.black_player_id)
                    ++draws[PlayerPair(p.white_player_id, *p.black_player_id)];
            }
            bool any_legal_game = false;
            for (const auto a : survivors) for (const auto b : survivors) {
                if (a < b && draws[PlayerPair(a, b)] < 2) any_legal_game = true;
            }
            elimination_complete = !any_legal_game;
        }
        if (!elimination_complete) {
            const int next = round + 1;
            const int64_t earliest = std::max(clock_.unix_seconds(), earliest_round_start(
                t->first_round_starts_at_unix, t->round_duration_seconds, next));
            auto allocated = db_.exec(
                "WITH extended AS (UPDATE tournaments SET rounds=GREATEST(rounds,$2) WHERE id=$1 RETURNING id) "
                "INSERT INTO tournament_rounds(tournament_id,round,earliest_start_at,check_in_closes_at) "
                "SELECT id,$2,to_timestamp($3),to_timestamp($4) FROM extended ON CONFLICT DO NOTHING",
                {storage::Param::int64(tournament_id), storage::Param::int64(next),
                 storage::Param::int64(earliest), storage::Param::int64(earliest + t->round_duration_seconds)});
            if (!allocated.ok) { out.error = "allocate_round_failed"; return out; }
        }
    }
    if (elimination_complete || (t->format == "swiss" && round >= t->rounds)) {
        // Final round done — mark completed.
        if (!set_tournament_status(db_, tournament_id, "completed")) {
            out.error = "set_status_completed_failed"; return out;
        }
        out.ok = true;
        return out;
    }

    // The next round remains scheduled. The maintenance tick starts it only
    // after both its durable earliest-start timestamp and the prior result set.
    int next = round + 1;
    if (!set_tournament_round(db_, tournament_id, next)) {
        out.error = "set_round_failed"; return out;
    }
    out.ok = true;
    return out;
}

ManagerResult TournamentManager::override_result(int64_t pairing_id,
                                                 int64_t initiator_id,
                                                 const std::string& result,
                                                 const std::string& reason) {
    ManagerResult out;
    if (reason.empty()) { out.error = "override_reason_required"; return out; }
    if (result != "1-0" && result != "0-1" && result != "1/2-1/2"
        && result != "double_forfeit") {
        out.error = "invalid_result"; return out;
    }
    auto q = db_.exec(
        "SELECT p.tournament_id,p.round,t.created_by FROM tournament_pairings p "
        "JOIN tournaments t ON t.id=p.tournament_id WHERE p.id=$1",
        {storage::Param::int64(pairing_id)});
    if (!q.ok || q.empty()) { out.error = "pairing_not_found"; return out; }
    const int64_t tid = std::stoll(q.first().at(0));
    const int round = std::stoi(q.first().at(1));
    if (std::stoll(q.first().at(2)) != initiator_id) {
        out.error = "not_creator"; return out;
    }
    const auto tournament = find_tournament(db_, tid);
    if (tournament && tournament->format == "winners_advance"
        && (tournament->current_round > round || tournament->status == "completed")) {
        out.error = "advancement_already_locked"; return out;
    }
    if (!audit_and_override_result(db_, pairing_id, initiator_id, result, reason)) {
        out.error = "override_failed"; return out;
    }
    return maybe_advance_after_result(tid, round);
}

ManagerResult TournamentManager::record_game_result(int64_t pairing_id,
                                                    const std::string& result) {
    ManagerResult out;
    if (result != "1-0" && result != "0-1" && result != "1/2-1/2") {
        out.error = "invalid_result"; return out;
    }
    auto pairing = find_pairing(db_, pairing_id);
    if (!pairing) { out.error = "pairing_not_found"; return out; }
    if (pairing->result != "pending") {
        if (pairing->result == result) { out.ok = true; return out; }
        out.error = "result_already_recorded"; return out;
    }
    if (!set_pairing_result(db_, pairing_id, result, "game")) {
        // A concurrent completion may have won the conditional update.
        pairing = find_pairing(db_, pairing_id);
        if (pairing && pairing->result == result) { out.ok = true; return out; }
        out.error = "set_pairing_result_failed"; return out;
    }
    return maybe_advance_after_result(pairing->tournament_id, pairing->round);
}

ManagerResult TournamentManager::start_due_round(int64_t tournament_id, int round,
                                                 int64_t now_unix) {
    ManagerResult out;
    auto claimed = db_.exec(
        "UPDATE tournament_rounds tr SET status='live',actual_start_at=to_timestamp($3) "
        "WHERE tr.tournament_id=$1 AND tr.round=$2 AND tr.status='scheduled' "
        " AND tr.earliest_start_at<=to_timestamp($3) "
        " AND ($2=1 OR EXISTS (SELECT 1 FROM tournament_rounds prev "
        "   WHERE prev.tournament_id=$1 AND prev.round=$2-1 AND prev.status='completed')) "
        "RETURNING round",
        {storage::Param::int64(tournament_id), storage::Param::int64(round),
         storage::Param::int64(now_unix)});
    if (!claimed.ok) { out.error = claimed.error; return out; }
    if (claimed.empty()) { out.ok = true; return out; } // another tick claimed it

    if (!set_tournament_status(db_, tournament_id, "in_progress")
        || !set_tournament_round(db_, tournament_id, round)) {
        out.error = "start_round_state_failed"; return out;
    }
    auto existing = get_pairings_for_round(db_, tournament_id, round);
    if (existing.empty()) return generate_round_pairings(tournament_id, round);
    out.ok = true;
    return out;
}

ManagerResult TournamentManager::adjudicate_no_shows(int64_t tournament_id,
                                                     int round) {
    ManagerResult out;
    const auto checked = get_round_checkins(db_, tournament_id, round);
    const auto pairings = get_pairings_for_round(db_, tournament_id, round);
    for (const auto& p : pairings) {
        if (p.result != "pending" || !p.black_player_id) continue;
        auto result = no_show_result(checked.count(p.white_player_id) != 0,
                                     checked.count(*p.black_player_id) != 0);
        if (!result) continue; // both arrived: Phase 2's game result will decide it
        if (!set_pairing_result(db_, p.id, *result, "forfeit")) {
            out.error = "no_show_adjudication_failed"; return out;
        }
    }
    return maybe_advance_after_result(tournament_id, round);
}

ManagerResult TournamentManager::maintenance_tick() {
    ManagerResult out;
    const int64_t now = clock_.unix_seconds();

    auto closed = db_.exec(
        "UPDATE tournaments SET registration_open=FALSE,status='scheduled', "
        " registration_closed_at=COALESCE(registration_closed_at,to_timestamp($1)) "
        "WHERE status='registration' AND LEAST(registration_deadline,first_round_starts_at-INTERVAL '90 seconds')<=to_timestamp($1)",
        {storage::Param::int64(now)});
    if (!closed.ok) { out.error = closed.error; return out; }

    // Freeze round one before play, without opening a room or starting clocks.
    // Row lock + transaction makes concurrent schedulers produce one set and
    // rolls back partially inserted pairings if generation fails.
    auto previews = db_.exec(
        "SELECT t.id FROM tournaments t JOIN tournament_rounds tr ON tr.tournament_id=t.id AND tr.round=1 "
        "WHERE t.status='scheduled' AND tr.status='scheduled' "
        "AND tr.earliest_start_at<=to_timestamp($1)+INTERVAL '90 seconds' "
        "AND NOT EXISTS (SELECT 1 FROM tournament_pairings p WHERE p.tournament_id=t.id AND p.round=1)",
        {storage::Param::int64(now)});
    if (!previews.ok) { out.error = previews.error; return out; }
    for (const auto& row : previews.rows) {
        const auto id = std::stoll(row.at(0));
        storage::Transaction tx(db_);
        if (!tx.ok()) { out.error = tx.error(); return out; }
        auto locked = db_.exec("SELECT id FROM tournaments WHERE id=$1 FOR UPDATE", {storage::Param::int64(id)});
        if (!locked.ok) { out.error = locked.error; return out; }
        if (get_pairings_for_round(db_, id, 1).empty()) {
            int active = 0;
            for (const auto& p : get_participants(db_, id)) if (!p.withdrawn) ++active;
            if (active >= 2) {
                auto generated = generate_round_pairings(id, 1);
                if (!generated.ok) return generated;
            }
        }
        if (!tx.commit()) { out.error = "prepare_pairings_failed"; return out; }
    }

    auto due = db_.exec(
        "SELECT tr.tournament_id,tr.round FROM tournament_rounds tr "
        "JOIN tournaments t ON t.id=tr.tournament_id "
        "WHERE tr.status='scheduled' AND tr.earliest_start_at<=to_timestamp($1) "
        " AND t.status IN ('scheduled','in_progress') "
        " AND (tr.round=1 OR EXISTS (SELECT 1 FROM tournament_rounds prev "
        "  WHERE prev.tournament_id=tr.tournament_id AND prev.round=tr.round-1 AND prev.status='completed')) "
        "ORDER BY tr.earliest_start_at,tr.tournament_id",
        {storage::Param::int64(now)});
    if (!due.ok) { out.error = due.error; return out; }
    for (const auto& row : due.rows) {
        auto r = start_due_round(std::stoll(row.at(0)), std::stoi(row.at(1)), now);
        if (!r.ok) return r;
    }

    auto expired = db_.exec(
        "SELECT tr.tournament_id,tr.round FROM tournament_rounds tr "
        "WHERE tr.status='live' AND tr.check_in_closes_at<=to_timestamp($1) "
        "AND EXISTS (SELECT 1 FROM tournament_pairings p WHERE p.tournament_id=tr.tournament_id AND p.round=tr.round "
        "AND p.result='pending' AND p.black_player_id IS NOT NULL "
        "AND (NOT EXISTS (SELECT 1 FROM tournament_round_checkins c WHERE c.tournament_id=p.tournament_id AND c.round=p.round AND c.player_id=p.white_player_id) "
        "OR NOT EXISTS (SELECT 1 FROM tournament_round_checkins c WHERE c.tournament_id=p.tournament_id AND c.round=p.round AND c.player_id=p.black_player_id)))",
        {storage::Param::int64(now)});
    if (!expired.ok) { out.error = expired.error; return out; }
    for (const auto& row : expired.rows) {
        auto r = adjudicate_no_shows(std::stoll(row.at(0)), std::stoi(row.at(1)));
        if (!r.ok) return r;
    }
    out.ok = true;
    return out;
}

// ── get_state ────────────────────────────────────────────────────────

std::optional<TournamentState> TournamentManager::get_state(int64_t tournament_id) {
    auto snapshot = read_tournament_snapshot(db_, tournament_id);
    if (!snapshot) return std::nullopt;
    const auto* t = &snapshot->tournament;

    TournamentState st;
    st.tournament   = *t;
    st.all_pairings = std::move(snapshot->pairings);
    st.rounds       = std::move(snapshot->rounds);
    st.usernames = std::move(snapshot->usernames);
    st.check_ins = std::move(snapshot->check_ins);

    const auto& participants = snapshot->participants;
    auto buchholz     = compute_buchholz(participants, st.all_pairings);
    std::map<int64_t, int> wins, draws;
    std::map<int64_t, int> reached_round;
    std::set<int64_t> finalists;
    for (const auto& p : st.all_pairings) {
        reached_round[p.white_player_id] = std::max(reached_round[p.white_player_id], p.round);
        if (p.black_player_id) reached_round[*p.black_player_id] = std::max(reached_round[*p.black_player_id], p.round);
        if (t->status == "completed" && p.round == t->current_round) {
            if (p.result == "1-0" || p.result == "bye" || p.result == "1/2-1/2") finalists.insert(p.white_player_id);
            if (p.black_player_id && (p.result == "0-1" || p.result == "1/2-1/2")) finalists.insert(*p.black_player_id);
        }
        if (!p.black_player_id) continue; // a bye is advancement, not a won game
        if (p.result == "1-0") ++wins[p.white_player_id];
        else if (p.result == "0-1") ++wins[*p.black_player_id];
        else if (p.result == "1/2-1/2") { ++draws[p.white_player_id]; ++draws[*p.black_player_id]; }
    }

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
        r.round_wins = wins[p.player_id];
        r.round_draws = draws[p.player_id];
        st.standings.push_back(r);
    }

    // Placement follows advancement; wins only break a final-survivor tie.
    const auto placement = [&](const StandingRow& p) {
        const bool finalist = finalists.count(p.player_id) != 0;
        return std::make_tuple(finalist, finalist ? t->current_round + 1 : reached_round[p.player_id],
                               finalist ? p.round_wins : 0);
    };
    std::sort(st.standings.begin(), st.standings.end(),
              [&](const StandingRow& a, const StandingRow& b) {
                  if (t->format == "winners_advance") {
                      if (placement(a) != placement(b)) return placement(a) > placement(b);
                      return a.player_id < b.player_id; // stable display, not a rank tiebreak
                  }
                  if (a.score       != b.score)       return a.score       > b.score;
                  if (a.buchholz    != b.buchholz)    return a.buchholz    > b.buchholz;
                  if (a.initial_elo != b.initial_elo) return a.initial_elo > b.initial_elo;
                  return a.player_id < b.player_id;
              });
    for (size_t i = 0; i < st.standings.size(); ++i) {
        st.standings[i].rank = static_cast<int>(i + 1);
        if (t->format == "winners_advance" && i > 0
            && placement(st.standings[i]) == placement(st.standings[i - 1]))
            st.standings[i].rank = st.standings[i - 1].rank;
    }
    return st;
}

} // namespace tournament
} // namespace chess
