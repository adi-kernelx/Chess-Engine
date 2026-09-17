/**
 * tournament_repo.cpp — see header for API contract.
 *
 * Every write goes through Database::exec(). No string-formatting of
 * caller-supplied values into SQL, matching the discipline the rest of
 * the phase-7-9 storage layer follows.
 *
 * ISO 8601 timestamps: every `created_at / started_at / completed_at /
 * registered_at` is rendered with the same `to_char(... AT TIME ZONE
 * 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"')` template used by cheat_report_repo
 * so a downstream consumer that already parses one shape sees the other.
 */

#include "tournament/tournament_repo.h"

#include <sstream>

using chess::storage::Database;
using chess::storage::Param;
using chess::storage::QueryResult;
using chess::storage::Row;

namespace chess {
namespace tournament {

namespace {

constexpr const char* TS_FMT =
    "to_char(%s AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"')";

// Helper — assemble the tournaments SELECT column list. Kept in one
// place so read paths do not drift.
std::string tournaments_columns() {
    // Column order matches row_to_tournament below.
    return std::string(
        "id, name, format, rounds, current_round, "
        "time_control_initial_ms, time_control_increment_ms, "
        "status, created_by, "
        "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
        "COALESCE(to_char(started_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), ''), "
        "COALESCE(to_char(completed_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), '')");
}

StoredTournament row_to_tournament(const Row& r) {
    StoredTournament t;
    t.id                        = std::stoll(r.at(0));
    t.name                      = r.at(1);
    t.format                    = r.at(2);
    t.rounds                    = std::stoi(r.at(3));
    t.current_round             = std::stoi(r.at(4));
    t.time_control_initial_ms   = std::stoi(r.at(5));
    t.time_control_increment_ms = std::stoi(r.at(6));
    t.status                    = r.at(7);
    t.created_by                = std::stoll(r.at(8));
    t.created_at                = r.at(9);
    t.started_at                = r.at(10);
    t.completed_at              = r.at(11);
    return t;
}

StoredTournamentPlayer row_to_participant(const Row& r) {
    StoredTournamentPlayer p;
    p.player_id     = std::stoll(r.at(0));
    p.initial_elo   = std::stoi (r.at(1));
    p.score         = std::stod (r.at(2));
    p.whites_played = std::stoi (r.at(3));
    p.received_bye  = (r.at(4) == "t" || r.at(4) == "true");
    p.withdrawn     = (r.at(5) == "t" || r.at(5) == "true");
    p.registered_at = r.at(6);
    return p;
}

StoredPairing row_to_pairing(const Row& r) {
    StoredPairing p;
    p.id              = std::stoll(r.at(0));
    p.tournament_id   = std::stoll(r.at(1));
    p.round           = std::stoi (r.at(2));
    p.white_player_id = std::stoll(r.at(3));
    if (!r.is_null(4)) p.black_player_id = std::stoll(r.at(4));
    if (!r.is_null(5)) p.game_id         = std::stoll(r.at(5));
    p.result          = r.at(6);
    p.created_at      = r.at(7);
    return p;
}

constexpr const char* PAIRING_COLUMNS =
    "id, tournament_id, round, white_player_id, black_player_id, game_id, result, "
    "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"')";

constexpr const char* PARTICIPANT_COLUMNS =
    "player_id, initial_elo, score, whites_played, received_bye, withdrawn, "
    "to_char(registered_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"')";

// Postgres emits "42.5" for a double via ::text; std::to_string on
// double gives locale-dependent output. Convert explicitly with a
// stream so we know what goes on the wire.
std::string double_to_text(double v) {
    std::ostringstream oss;
    oss.precision(17);
    oss << v;
    return oss.str();
}

} // namespace

// ── create/find/list/update ────────────────────────────────────────

CreateTournamentResult create_tournament(Database& db,
                                         const std::string& name,
                                         int rounds,
                                         int tc_initial,
                                         int tc_increment,
                                         int64_t created_by) {
    CreateTournamentResult out;
    if (rounds <= 0)      { out.error = "rounds must be positive";       return out; }
    if (tc_initial <= 0)  { out.error = "time_base must be positive";    return out; }
    if (tc_increment < 0) { out.error = "time_inc must be non-negative"; return out; }

    QueryResult r = db.exec(
        "INSERT INTO tournaments "
        "(name, format, rounds, current_round, "
        " time_control_initial_ms, time_control_increment_ms, "
        " status, created_by) "
        "VALUES ($1, 'swiss', $2, 0, $3, $4, 'registration', $5) "
        "RETURNING id",
        {Param::text(name),
         Param::int64(rounds),
         Param::int64(tc_initial),
         Param::int64(tc_increment),
         Param::int64(created_by)});
    if (!r.ok)        { out.error = r.error;                            return out; }
    if (r.rows.empty()) { out.error = "INSERT ... RETURNING no row";    return out; }

    out.id = std::stoll(r.rows[0].at(0));
    out.ok = true;
    return out;
}

std::optional<StoredTournament> find_tournament(Database& db, int64_t id) {
    QueryResult r = db.exec(
        "SELECT " + tournaments_columns() +
        " FROM tournaments WHERE id = $1",
        {Param::int64(id)});
    if (!r.ok || r.rows.empty()) return std::nullopt;
    return row_to_tournament(r.rows[0]);
}

std::vector<StoredTournament> list_tournaments(Database& db,
                                               const std::string& status,
                                               int limit) {
    std::vector<StoredTournament> out;
    if (limit <= 0) return out;

    QueryResult r;
    if (status.empty()) {
        r = db.exec(
            "SELECT " + tournaments_columns() +
            " FROM tournaments ORDER BY created_at DESC LIMIT $1",
            {Param::int64(limit)});
    } else {
        r = db.exec(
            "SELECT " + tournaments_columns() +
            " FROM tournaments WHERE status = $1 "
            " ORDER BY created_at DESC LIMIT $2",
            {Param::text(status), Param::int64(limit)});
    }
    if (!r.ok) return out;
    out.reserve(r.rows.size());
    for (const auto& row : r.rows) out.push_back(row_to_tournament(row));
    return out;
}

bool set_tournament_status(Database& db, int64_t id, const std::string& new_status) {
    // Set started_at/completed_at on the transitions to in_progress /
    // completed. Idempotent — flipping back to a status the row already
    // holds is a no-op but succeeds.
    const std::string sql =
        "UPDATE tournaments SET "
        "  status = $1, "
        "  started_at   = CASE WHEN $1 = 'in_progress' AND started_at   IS NULL THEN now() ELSE started_at   END, "
        "  completed_at = CASE WHEN $1 = 'completed'   AND completed_at IS NULL THEN now() ELSE completed_at END "
        "WHERE id = $2";
    QueryResult r = db.exec(sql, {Param::text(new_status), Param::int64(id)});
    return r.ok && r.rows_affected > 0;
}

bool set_tournament_round(Database& db, int64_t id, int new_round) {
    QueryResult r = db.exec(
        "UPDATE tournaments SET current_round = $1 WHERE id = $2",
        {Param::int64(new_round), Param::int64(id)});
    return r.ok && r.rows_affected > 0;
}

// ── participants ───────────────────────────────────────────────────

JoinTournamentResult add_participant(Database& db,
                                     int64_t tournament_id,
                                     int64_t player_id,
                                     int initial_elo) {
    JoinTournamentResult out;

    // Insert with ON CONFLICT DO NOTHING — a duplicate join is a no-op,
    // not an error. Two clients racing on the same join is a normal case.
    QueryResult r = db.exec(
        "INSERT INTO tournament_players "
        "(tournament_id, player_id, initial_elo) "
        "VALUES ($1, $2, $3) "
        "ON CONFLICT (tournament_id, player_id) DO NOTHING",
        {Param::int64(tournament_id),
         Param::int64(player_id),
         Param::int64(initial_elo)});
    if (!r.ok) { out.error = r.error; return out; }
    out.ok = true;
    return out;
}

std::vector<StoredTournamentPlayer> get_participants(Database& db,
                                                     int64_t tournament_id) {
    std::vector<StoredTournamentPlayer> out;
    QueryResult r = db.exec(
        std::string("SELECT ") + PARTICIPANT_COLUMNS +
        " FROM tournament_players WHERE tournament_id = $1 "
        " ORDER BY score DESC, initial_elo DESC, player_id ASC",
        {Param::int64(tournament_id)});
    if (!r.ok) return out;
    out.reserve(r.rows.size());
    for (const auto& row : r.rows) out.push_back(row_to_participant(row));
    return out;
}

bool bump_participant(Database& db,
                      int64_t tournament_id,
                      int64_t player_id,
                      double score_delta,
                      bool played_white,
                      bool bye) {
    QueryResult r = db.exec(
        "UPDATE tournament_players SET "
        "  score         = score + $1::double precision, "
        "  whites_played = whites_played + CASE WHEN $2 THEN 1 ELSE 0 END, "
        "  received_bye  = received_bye OR $3 "
        "WHERE tournament_id = $4 AND player_id = $5",
        {Param::text(double_to_text(score_delta)),
         Param::boolean(played_white),
         Param::boolean(bye),
         Param::int64(tournament_id),
         Param::int64(player_id)});
    return r.ok && r.rows_affected > 0;
}

// ── pairings ───────────────────────────────────────────────────────

InsertPairingResult insert_pairing(Database& db,
                                   int64_t tournament_id,
                                   int round,
                                   int64_t white_player_id,
                                   std::optional<int64_t> black_player_id,
                                   const std::string& initial_result) {
    InsertPairingResult out;
    QueryResult r = db.exec(
        "INSERT INTO tournament_pairings "
        "(tournament_id, round, white_player_id, black_player_id, result) "
        "VALUES ($1, $2, $3, $4, $5) "
        "RETURNING id",
        {Param::int64(tournament_id),
         Param::int64(round),
         Param::int64(white_player_id),
         black_player_id.has_value() ? Param::int64(*black_player_id) : Param::null(),
         Param::text(initial_result)});
    if (!r.ok)          { out.error = r.error;                          return out; }
    if (r.rows.empty()) { out.error = "INSERT ... RETURNING no row";    return out; }
    out.id = std::stoll(r.rows[0].at(0));
    out.ok = true;
    return out;
}

std::vector<StoredPairing> get_pairings(Database& db, int64_t tournament_id) {
    std::vector<StoredPairing> out;
    QueryResult r = db.exec(
        std::string("SELECT ") + PAIRING_COLUMNS +
        " FROM tournament_pairings WHERE tournament_id = $1 "
        " ORDER BY round ASC, id ASC",
        {Param::int64(tournament_id)});
    if (!r.ok) return out;
    out.reserve(r.rows.size());
    for (const auto& row : r.rows) out.push_back(row_to_pairing(row));
    return out;
}

std::vector<StoredPairing> get_pairings_for_round(Database& db,
                                                  int64_t tournament_id,
                                                  int round) {
    std::vector<StoredPairing> out;
    QueryResult r = db.exec(
        std::string("SELECT ") + PAIRING_COLUMNS +
        " FROM tournament_pairings WHERE tournament_id = $1 AND round = $2 "
        " ORDER BY id ASC",
        {Param::int64(tournament_id), Param::int64(round)});
    if (!r.ok) return out;
    out.reserve(r.rows.size());
    for (const auto& row : r.rows) out.push_back(row_to_pairing(row));
    return out;
}

bool set_pairing_result(Database& db,
                        int64_t pairing_id,
                        const std::string& result) {
    QueryResult r = db.exec(
        "UPDATE tournament_pairings SET result = $1 WHERE id = $2",
        {Param::text(result), Param::int64(pairing_id)});
    return r.ok && r.rows_affected > 0;
}

} // namespace tournament
} // namespace chess
