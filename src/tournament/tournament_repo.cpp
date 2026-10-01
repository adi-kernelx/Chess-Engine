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
        "EXTRACT(EPOCH FROM registration_deadline)::bigint, "
        "EXTRACT(EPOCH FROM first_round_starts_at)::bigint, "
        "round_duration_seconds, registration_open, "
        "COALESCE(EXTRACT(EPOCH FROM registration_closed_at)::bigint, 0), "
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
    t.registration_deadline_unix = std::stoll(r.at(9));
    t.first_round_starts_at_unix = std::stoll(r.at(10));
    t.round_duration_seconds     = std::stoi(r.at(11));
    t.registration_open          = (r.at(12) == "t" || r.at(12) == "true");
    t.registration_closed_at_unix = std::stoll(r.at(13));
    t.created_at                = r.at(14);
    t.started_at                = r.at(15);
    t.completed_at              = r.at(16);
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
    p.result_source   = r.at(7);
    p.result_recorded_at_unix = std::stoll(r.at(8));
    p.created_at      = r.at(9);
    return p;
}

constexpr const char* PAIRING_COLUMNS =
    "id, tournament_id, round, white_player_id, black_player_id, game_id, result, "
    "COALESCE(result_source, ''), "
    "COALESCE(EXTRACT(EPOCH FROM result_recorded_at)::bigint, 0), "
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
                                         int64_t created_by,
                                         int64_t registration_deadline_unix,
                                         int64_t first_round_starts_at_unix,
                                         int round_duration_seconds) {
    CreateTournamentResult out;
    if (rounds <= 0)      { out.error = "rounds must be positive";       return out; }
    if (tc_initial <= 0)  { out.error = "time_base must be positive";    return out; }
    if (tc_increment < 0) { out.error = "time_inc must be non-negative"; return out; }
    if (registration_deadline_unix <= 0) registration_deadline_unix = 4102444800LL;
    if (first_round_starts_at_unix <= 0) first_round_starts_at_unix = registration_deadline_unix + 300;
    if (first_round_starts_at_unix < registration_deadline_unix + 30) {
        out.error = "first round must start at least 30 seconds after registration closes";
        return out;
    }
    if (round_duration_seconds < 60) {
        out.error = "round duration must be at least 60 seconds";
        return out;
    }

    QueryResult r = db.exec(
        "WITH made AS (INSERT INTO tournaments "
        "(name, format, rounds, current_round, "
        " time_control_initial_ms, time_control_increment_ms, "
        " status, created_by, registration_deadline, first_round_starts_at, "
        " round_duration_seconds, registration_open) "
        "VALUES ($1, 'swiss', $2, 0, $3, $4, 'registration', $5, "
        " to_timestamp($6), to_timestamp($7), $8, TRUE) RETURNING id) "
        "INSERT INTO tournament_rounds "
        " (tournament_id, round, earliest_start_at, check_in_closes_at) "
        "SELECT made.id, n, to_timestamp($7) + (n-1) * $8 * INTERVAL '1 second', "
        " to_timestamp($7) + n * $8 * INTERVAL '1 second' "
        "FROM made CROSS JOIN generate_series(1, $2::integer) n "
        "RETURNING tournament_id",
        {Param::text(name),
         Param::int64(rounds),
         Param::int64(tc_initial),
         Param::int64(tc_increment),
         Param::int64(created_by),
         Param::int64(registration_deadline_unix),
         Param::int64(first_round_starts_at_unix),
         Param::int64(round_duration_seconds)});
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
                                     int initial_elo,
                                     int64_t now_unix) {
    JoinTournamentResult out;
    if (now_unix <= 0) now_unix = 0;

    // Insert with ON CONFLICT DO NOTHING — a duplicate join is a no-op,
    // not an error. Two clients racing on the same join is a normal case.
    QueryResult r = db.exec(
        "INSERT INTO tournament_players "
        "(tournament_id, player_id, initial_elo) "
        "SELECT $1, $2, $3 FROM tournaments t "
        "WHERE t.id=$1 AND t.status='registration' AND t.registration_open "
        "  AND ($4=0 OR t.registration_deadline > to_timestamp($4)) "
        "ON CONFLICT (tournament_id, player_id) DO NOTHING",
        {Param::int64(tournament_id),
         Param::int64(player_id),
         Param::int64(initial_elo),
         Param::int64(now_unix)});
    if (!r.ok) { out.error = r.error; return out; }
    if (r.rows_affected == 0) {
        auto existing = db.exec(
            "SELECT 1 FROM tournament_players WHERE tournament_id=$1 AND player_id=$2",
            {Param::int64(tournament_id), Param::int64(player_id)});
        if (!existing.ok || existing.empty()) {
            out.error = "registration_closed";
            return out;
        }
    }
    out.ok = true;
    return out;
}

JoinTournamentResult remove_participant(Database& db,
                                         int64_t tournament_id,
                                         int64_t player_id,
                                         int64_t now_unix) {
    JoinTournamentResult out;
    QueryResult r = db.exec(
        "DELETE FROM tournament_players tp USING tournaments t "
        "WHERE tp.tournament_id=$1 AND tp.player_id=$2 AND t.id=tp.tournament_id "
        " AND t.status='registration' AND t.registration_open "
        " AND t.registration_deadline>to_timestamp($3)",
        {Param::int64(tournament_id), Param::int64(player_id), Param::int64(now_unix)});
    if (!r.ok) { out.error = r.error; return out; }
    if (r.rows_affected == 0) {
        auto participant = db.exec(
            "SELECT 1 FROM tournament_players WHERE tournament_id=$1 AND player_id=$2",
            {Param::int64(tournament_id), Param::int64(player_id)});
        out.error = participant.ok && participant.empty()
            ? "not_registered" : "registration_closed";
        return out;
    }
    out.ok = true;
    return out;
}

bool set_registration_open(Database& db, int64_t tournament_id,
                           bool open, int64_t now_unix) {
    QueryResult r = db.exec(
        "UPDATE tournaments SET registration_open=$1, "
        " registration_closed_at=CASE WHEN $1 THEN NULL ELSE to_timestamp($2) END, "
        " status=CASE WHEN $1 THEN 'registration' ELSE 'scheduled' END "
        "WHERE id=$3 AND status IN ('registration','scheduled') "
        "  AND ($1 OR registration_open)",
        {Param::boolean(open), Param::int64(now_unix), Param::int64(tournament_id)});
    return r.ok && r.rows_affected > 0;
}

std::vector<StoredTournamentRound> get_rounds(Database& db, int64_t tournament_id) {
    std::vector<StoredTournamentRound> out;
    QueryResult r = db.exec(
        "SELECT tournament_id, round, EXTRACT(EPOCH FROM earliest_start_at)::bigint, "
        " COALESCE(EXTRACT(EPOCH FROM actual_start_at)::bigint,0), "
        " EXTRACT(EPOCH FROM check_in_closes_at)::bigint, "
        " COALESCE(EXTRACT(EPOCH FROM completed_at)::bigint,0), status "
        "FROM tournament_rounds WHERE tournament_id=$1 ORDER BY round",
        {Param::int64(tournament_id)});
    if (!r.ok) return out;
    for (const auto& row : r.rows) {
        StoredTournamentRound x;
        x.tournament_id = std::stoll(row.at(0)); x.round = std::stoi(row.at(1));
        x.earliest_start_at_unix = std::stoll(row.at(2));
        x.actual_start_at_unix = std::stoll(row.at(3));
        x.check_in_closes_at_unix = std::stoll(row.at(4));
        x.completed_at_unix = std::stoll(row.at(5)); x.status = row.at(6);
        out.push_back(x);
    }
    return out;
}

bool check_in_player(Database& db, int64_t tournament_id, int round,
                     int64_t player_id, int64_t now_unix, std::string& error) {
    QueryResult r = db.exec(
        "INSERT INTO tournament_round_checkins(tournament_id,round,player_id,checked_in_at) "
        "SELECT $1,$2,$3,to_timestamp($4) FROM tournament_rounds tr "
        "JOIN tournament_players tp ON tp.tournament_id=tr.tournament_id AND tp.player_id=$3 "
        "WHERE tr.tournament_id=$1 AND tr.round=$2 AND tr.status IN ('scheduled','live') "
        " AND tr.check_in_closes_at >= to_timestamp($4) AND NOT tp.withdrawn "
        "ON CONFLICT DO NOTHING",
        {Param::int64(tournament_id), Param::int64(round), Param::int64(player_id), Param::int64(now_unix)});
    if (!r.ok) { error = r.error; return false; }
    if (r.rows_affected == 0) {
        auto q = db.exec("SELECT 1 FROM tournament_round_checkins WHERE tournament_id=$1 AND round=$2 AND player_id=$3",
            {Param::int64(tournament_id), Param::int64(round), Param::int64(player_id)});
        if (!q.ok || q.empty()) { error = "check_in_closed_or_not_registered"; return false; }
    }
    return true;
}

std::set<int64_t> get_round_checkins(Database& db, int64_t tournament_id, int round) {
    std::set<int64_t> out;
    auto r = db.exec("SELECT player_id FROM tournament_round_checkins WHERE tournament_id=$1 AND round=$2",
        {Param::int64(tournament_id), Param::int64(round)});
    if (r.ok) for (const auto& row : r.rows) out.insert(std::stoll(row.at(0)));
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

std::map<int64_t, std::string> get_participant_usernames(
        Database& db, int64_t tournament_id) {
    std::map<int64_t, std::string> out;
    QueryResult r = db.exec(
        "SELECT p.id, p.username FROM players p WHERE p.id IN ("
        "  SELECT tp.player_id FROM tournament_players tp "
        "  WHERE tp.tournament_id = $1 "
        "  UNION "
        "  SELECT t.created_by FROM tournaments t WHERE t.id = $1"
        ")",
        {Param::int64(tournament_id)});
    if (!r.ok) return out;
    for (const auto& row : r.rows) {
        out.emplace(std::stoll(row.at(0)), row.at(1));
    }
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
        "(tournament_id, round, white_player_id, black_player_id, result, result_source, result_recorded_at) "
        "VALUES ($1, $2, $3, $4, $5, CASE WHEN $5='bye' THEN 'bye' END, "
        " CASE WHEN $5='bye' THEN now() END) "
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

std::optional<StoredPairing> find_pairing(Database& db, int64_t pairing_id) {
    QueryResult r = db.exec(
        std::string("SELECT ") + PAIRING_COLUMNS +
        " FROM tournament_pairings WHERE id=$1",
        {Param::int64(pairing_id)});
    if (!r.ok || r.empty()) return std::nullopt;
    return row_to_pairing(r.first());
}

std::vector<StoredPairing> get_live_pending_pairings(Database& db) {
    std::vector<StoredPairing> out;
    QueryResult r = db.exec(
        std::string("SELECT p.") +
        "id, p.tournament_id, p.round, p.white_player_id, p.black_player_id, "
        "p.game_id, p.result, COALESCE(p.result_source,''), "
        "COALESCE(EXTRACT(EPOCH FROM p.result_recorded_at)::bigint,0), "
        "to_char(p.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
        "FROM tournament_pairings p JOIN tournament_rounds tr "
        " ON tr.tournament_id=p.tournament_id AND tr.round=p.round "
        "WHERE tr.status='live' AND p.result='pending' AND p.black_player_id IS NOT NULL "
        "ORDER BY p.tournament_id,p.round,p.id");
    if (!r.ok) return out;
    for (const auto& row : r.rows) out.push_back(row_to_pairing(row));
    return out;
}

int64_t max_pairing_game_id(Database& db) {
    auto r = db.exec("SELECT COALESCE(MAX(game_id),0) FROM tournament_pairings");
    return (!r.ok || r.empty()) ? 0 : std::stoll(r.first().at(0));
}

bool set_pairing_game_id(Database& db, int64_t pairing_id, int64_t game_id) {
    auto r = db.exec(
        "UPDATE tournament_pairings SET game_id=$1 WHERE id=$2 AND result='pending'",
        {Param::int64(game_id), Param::int64(pairing_id)});
    return r.ok && r.rows_affected > 0;
}

bool set_pairing_result(Database& db,
                        int64_t pairing_id,
                        const std::string& result,
                        const std::string& source) {
    QueryResult r = db.exec(
        "WITH changed AS (UPDATE tournament_pairings SET result=$1, result_source=$2, "
        " result_recorded_at=now() WHERE id=$3 AND (result='pending' OR result=$1) "
        " RETURNING tournament_id) "
        "UPDATE tournament_players tp SET "
        " score=COALESCE((SELECT SUM(CASE "
        "  WHEN (CASE WHEN p.id=$3 THEN $1 ELSE p.result END)='bye' AND p.white_player_id=tp.player_id THEN 1.0 "
        "  WHEN (CASE WHEN p.id=$3 THEN $1 ELSE p.result END)='1-0' AND p.white_player_id=tp.player_id THEN 1.0 "
        "  WHEN (CASE WHEN p.id=$3 THEN $1 ELSE p.result END)='0-1' AND p.black_player_id=tp.player_id THEN 1.0 "
        "  WHEN (CASE WHEN p.id=$3 THEN $1 ELSE p.result END)='1/2-1/2' "
        "   AND (p.white_player_id=tp.player_id OR p.black_player_id=tp.player_id) THEN 0.5 ELSE 0.0 END) "
        "  FROM tournament_pairings p WHERE p.tournament_id=tp.tournament_id "
        "   AND (p.white_player_id=tp.player_id OR p.black_player_id=tp.player_id)),0), "
        " whites_played=(SELECT COUNT(*) FROM tournament_pairings p WHERE p.tournament_id=tp.tournament_id "
        "  AND p.white_player_id=tp.player_id AND p.black_player_id IS NOT NULL "
        "  AND (CASE WHEN p.id=$3 THEN $1 ELSE p.result END)<>'pending'), "
        " received_bye=EXISTS(SELECT 1 FROM tournament_pairings p WHERE p.tournament_id=tp.tournament_id "
        "  AND p.white_player_id=tp.player_id AND (CASE WHEN p.id=$3 THEN $1 ELSE p.result END)='bye') "
        "FROM changed WHERE tp.tournament_id=changed.tournament_id",
        {Param::text(result), Param::text(source), Param::int64(pairing_id)});
    return r.ok && r.rows_affected > 0;
}

bool recompute_participant_totals(Database& db, int64_t tournament_id) {
    QueryResult r = db.exec(
        "UPDATE tournament_players tp SET "
        " score=COALESCE((SELECT SUM(CASE "
        "   WHEN p.result='bye' AND p.white_player_id=tp.player_id THEN 1.0 "
        "   WHEN p.result='1-0' AND p.white_player_id=tp.player_id THEN 1.0 "
        "   WHEN p.result='0-1' AND p.black_player_id=tp.player_id THEN 1.0 "
        "   WHEN p.result='1/2-1/2' AND (p.white_player_id=tp.player_id OR p.black_player_id=tp.player_id) THEN 0.5 "
        "   ELSE 0.0 END) FROM tournament_pairings p WHERE p.tournament_id=tp.tournament_id "
        "   AND (p.white_player_id=tp.player_id OR p.black_player_id=tp.player_id)),0), "
        " whites_played=(SELECT COUNT(*) FROM tournament_pairings p WHERE p.tournament_id=tp.tournament_id "
        "   AND p.white_player_id=tp.player_id AND p.black_player_id IS NOT NULL AND p.result<>'pending'), "
        " received_bye=EXISTS(SELECT 1 FROM tournament_pairings p WHERE p.tournament_id=tp.tournament_id "
        "   AND p.white_player_id=tp.player_id AND p.result='bye') "
        "WHERE tp.tournament_id=$1",
        {Param::int64(tournament_id)});
    return r.ok;
}

bool audit_and_override_result(Database& db, int64_t pairing_id,
                               int64_t actor_player_id,
                               const std::string& new_result,
                               const std::string& reason) {
    QueryResult r = db.exec(
        "WITH old AS (SELECT id,result FROM tournament_pairings WHERE id=$1 FOR UPDATE), "
        "audit AS (INSERT INTO tournament_result_overrides "
        " (pairing_id,actor_player_id,old_result,new_result,reason) "
        " SELECT id,$2,result,$3,$4 FROM old RETURNING pairing_id) "
        ", changed AS (UPDATE tournament_pairings p SET result=$3,result_source='override',result_recorded_at=now() "
        " FROM audit WHERE p.id=audit.pairing_id RETURNING p.tournament_id) "
        "UPDATE tournament_players tp SET "
        " score=COALESCE((SELECT SUM(CASE "
        "  WHEN (CASE WHEN p.id=$1 THEN $3 ELSE p.result END)='bye' AND p.white_player_id=tp.player_id THEN 1.0 "
        "  WHEN (CASE WHEN p.id=$1 THEN $3 ELSE p.result END)='1-0' AND p.white_player_id=tp.player_id THEN 1.0 "
        "  WHEN (CASE WHEN p.id=$1 THEN $3 ELSE p.result END)='0-1' AND p.black_player_id=tp.player_id THEN 1.0 "
        "  WHEN (CASE WHEN p.id=$1 THEN $3 ELSE p.result END)='1/2-1/2' "
        "   AND (p.white_player_id=tp.player_id OR p.black_player_id=tp.player_id) THEN 0.5 ELSE 0.0 END) "
        "  FROM tournament_pairings p WHERE p.tournament_id=tp.tournament_id "
        "   AND (p.white_player_id=tp.player_id OR p.black_player_id=tp.player_id)),0), "
        " whites_played=(SELECT COUNT(*) FROM tournament_pairings p WHERE p.tournament_id=tp.tournament_id "
        "  AND p.white_player_id=tp.player_id AND p.black_player_id IS NOT NULL "
        "  AND (CASE WHEN p.id=$1 THEN $3 ELSE p.result END)<>'pending'), "
        " received_bye=EXISTS(SELECT 1 FROM tournament_pairings p WHERE p.tournament_id=tp.tournament_id "
        "  AND p.white_player_id=tp.player_id AND (CASE WHEN p.id=$1 THEN $3 ELSE p.result END)='bye') "
        "FROM changed WHERE tp.tournament_id=changed.tournament_id",
        {Param::int64(pairing_id), Param::int64(actor_player_id),
         Param::text(new_result), Param::text(reason)});
    return r.ok && r.rows_affected > 0;
}

} // namespace tournament
} // namespace chess
