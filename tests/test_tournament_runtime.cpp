#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "application/game_completion_service.h"
#include "application/tournament_runtime_service.h"
#include "core/uuid.h"
#include "game/game_events.h"
#include "storage/database.h"
#include "tournament/tournament_manager.h"
#include "tournament/tournament_repo.h"

using namespace chess;
using namespace chess::application;
using namespace chess::storage;
using namespace chess::tournament;

namespace {
int passed = 0, failed = 0;
void test(const std::string& name, const std::function<bool()>& fn) {
    std::cout << "  [TEST] " << name << "... ";
    try {
        if (fn()) { ++passed; std::cout << "PASS\n"; }
        else { ++failed; std::cout << "FAIL\n"; }
    } catch (const std::exception& e) {
        ++failed; std::cout << "FAIL (" << e.what() << ")\n";
    }
}

struct RecordingStore final : ports::GameStore {
    explicit RecordingStore(Database* database = nullptr) : db(database) {}
    Database* db = nullptr;
    int calls = 0;
    bool capable() const override { return true; }
    ports::SaveGameOutcome save_completed_game(const CompletedGame& game) override {
        ++calls;
        ports::SaveGameOutcome out;
        if (!db) { out.game_id = calls; return out; }
        auto inserted = db->exec(
            "INSERT INTO games(white_id,black_id,moves,result,termination,white_elo,black_elo,"
            "time_control,started_at,ended_at,move_count,completion_uuid,rated) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9::timestamptz,$10::timestamptz,$11,$12,TRUE) "
            "RETURNING id",
            {Param::int64(game.white_id), Param::int64(game.black_id),
             Param::text(game.moves), Param::text(game.result), Param::text(game.termination),
             Param::int64(game.white_elo), Param::int64(game.black_elo),
             Param::text(game.time_control), Param::text(game.started_at),
             Param::text(game.ended_at), Param::int64(game.move_count),
             Param::text(game.completion_uuid)});
        if (!inserted.ok || inserted.empty()) {
            out.code = classify(inserted);
            out.error = inserted.error;
            return out;
        }
        out.game_id = std::stoll(inserted.first().at(0));
        return out;
    }
    ports::SaveCheatReportOutcome save_cheat_report(
        int64_t, int64_t, const std::string&,
        const chess::analysis::AnalysisReport&) override { return {}; }
};

struct RecordingTournamentSink final : ports::TournamentCompletionSink {
    std::vector<std::pair<int64_t,std::string>> results;
    void on_tournament_game_persisted(int64_t pairing_id,
        const std::string& result, int64_t) override {
        results.push_back({pairing_id,result});
    }
};

struct Capture final : game::GameEventListener {
    std::vector<game::GameCompleted> completed;
    void on_game_completed(const game::GameCompleted& ev) override {
        completed.push_back(ev);
    }
};

std::string source_path(const std::string& rel) {
#ifdef CHESS_SOURCE_DIR
    return std::string(CHESS_SOURCE_DIR) + "/" + rel;
#else
    return rel;
#endif
}
std::string read_file(const std::string& path) {
    std::ifstream f(path); std::stringstream s; s << f.rdbuf(); return s.str();
}

bool prepare_schema(Database& db) {
    std::string err;
    if (!db.run_script(
        "DROP TABLE IF EXISTS tournament_result_overrides;"
        "DROP TABLE IF EXISTS tournament_round_checkins;"
        "DROP TABLE IF EXISTS tournament_rounds;"
        "DROP TABLE IF EXISTS tournament_pairings;"
        "DROP TABLE IF EXISTS tournament_players;"
        "DROP TABLE IF EXISTS tournaments;"
        "DROP TABLE IF EXISTS cheat_reports;"
        "DROP TABLE IF EXISTS move_times;"
        "DROP TABLE IF EXISTS games;"
        "DROP TABLE IF EXISTS sessions;"
        "DROP TABLE IF EXISTS schema_migrations;"
        "DROP TABLE IF EXISTS players;"
        "DROP FUNCTION IF EXISTS assert_username_ci_matches();", err)) return false;
    if (!db.run_script(read_file(source_path("src/storage/schema_phase7.sql")), err)) return false;
    bool applied = false;
    const std::vector<std::pair<std::string,std::string>> migrations = {
        {"0001_phase8_game_persistence", "0001_phase8_game_persistence.sql"},
        {"0002_phase9_3_cheat_reports", "0002_phase9_3_cheat_reports.sql"},
        {"0003_phase9_4_tournaments", "0003_phase9_4_tournaments.sql"},
        {"0004_lld4_completion_uuid", "0004_lld4_completion_uuid.sql"},
        {"0009_persist_unrated_ai_games", "0009_persist_unrated_ai_games.sql"},
        {"0010_live_tournament_runtime", "0010_live_tournament_runtime.sql"},
        {"0012_tournament_replay_identity", "0012_tournament_replay_identity.sql"},
        {"0013_winners_advance_tournaments", "0013_winners_advance_tournaments.sql"},
    };
    for (const auto& [version,file] : migrations) {
        if (!db.apply_migration(version,
            read_file(source_path("src/storage/migrations/" + file)), applied, err)) return false;
    }
    return true;
}

int64_t add_player(Database& db, const std::string& username, int elo) {
    std::string lower = username;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto r = db.exec("INSERT INTO players(username,username_ci,password_hash,elo_rating) "
                     "VALUES($1,$2,'hash',$3) RETURNING id",
                     {Param::text(username),Param::text(lower),Param::int64(elo)});
    return (!r.ok || r.empty()) ? 0 : std::stoll(r.first().at(0));
}
}

int main() {
    std::cout << "=== Tournament runtime Phase 2 ===\n";
    test("reserved seats reject strangers and start exactly once", [] {
        game::RoomManager rooms;
        auto room = rooms.create_reserved_tournament_room(
            7, 9, 101, "White", 1500, 202, "Black", 1450,
            game::TimeControl(60000, 0));
        if (room->bind_reserved_player(999, 999, 30).ok) return false;
        if (!room->allow_reserved_player(101) || !room->allow_reserved_player(202)) return false;
        auto white = room->bind_reserved_player(101, 1, 10);
        if (!white.ok || white.color != Color::WHITE || white.started) return false;
        if (!room->open_reserved() || room->get_state() != game::RoomState::WAITING) return false;
        auto black = room->bind_reserved_player(202, 2, 20);
        if (!black.ok || black.color != Color::BLACK || !black.started) return false;
        if (!room->add_spectator(30) || room->spectator_count() != 1) return false;
        auto spectator_move = room->submit_move(
            30, chess::Squares::E2, chess::make_square(3, 4));
        if (spectator_move.success || spectator_move.error != "You are not a player in this game") {
            return false;
        }
        return room->get_state() == game::RoomState::IN_PROGRESS
            && room->open_reserved() && room->get_state() == game::RoomState::IN_PROGRESS;
    });

    test("reserved reconnect restores color and snapshot identity", [] {
        game::RoomManager rooms;
        auto capture = std::make_shared<Capture>();
        rooms.set_default_listener(capture);
        auto room = rooms.create_reserved_tournament_room(
            8, 10, 111, "W", 1500, 222, "B", 1500,
            game::TimeControl(60000, 0));
        room->allow_reserved_player(111); room->allow_reserved_player(222);
        room->bind_reserved_player(111, 1, 11);
        room->bind_reserved_player(222, 2, 22);
        room->open_reserved();
        room->on_disconnect(22);
        if (!room->on_reconnect_db_player(222, 23)
            || room->get_player_fd(Color::BLACK) != 23) return false;
        if (!room->resign(11) || capture->completed.size() != 1) return false;
        return capture->completed[0].snapshot.tournament_id == 8
            && capture->completed[0].snapshot.pairing_id == 10
            && capture->completed[0].snapshot.result == "0-1";
    });

    test("every persisted terminal reason forwards immutable tournament result", [] {
        RecordingStore store;
        RecordingTournamentSink sink;
        GameCompletionService completion(store, &sink);
        const std::vector<std::pair<GameStatus,std::string>> endings = {
            {GameStatus::CHECKMATE,"1-0"}, {GameStatus::TIMEOUT,"0-1"},
            {GameStatus::RESIGNATION,"1-0"}, {GameStatus::DRAW_AGREEMENT,"1/2-1/2"},
            {GameStatus::ABANDONMENT,"0-1"},
        };
        int64_t id = 1;
        for (const auto& [status,result] : endings) {
            game::GameCompleted event;
            auto& s = event.snapshot;
            s.room_id=id; s.pairing_id=id; s.tournament_id=1;
            s.completion_uuid=core::generate_uuid_v4(); s.status=status; s.result=result;
            s.termination_reason="test"; s.white.db_player_id=1; s.black.db_player_id=2;
            s.white.elo=1500; s.black.elo=1500; s.started_at_iso="2026-01-01T00:00:00Z";
            s.ended_at_iso="2026-01-01T00:01:00Z";
            completion.on_game_completed(event); ++id;
        }
        return store.calls==5 && sink.results.size()==5
            && sink.results.front().second=="1-0"
            && sink.results.back().second=="0-1";
    });

    test("tournament checkmate returns before queued persistence and round updates", [] {
        RecordingStore store;
        RecordingTournamentSink sink;
        std::vector<std::function<void()>> tasks;
        auto completion = std::make_shared<GameCompletionService>(store, &sink,
            [&](std::function<void()> task) { tasks.push_back(std::move(task)); });
        game::RoomManager rooms;
        rooms.set_default_listener(completion);
        auto room = rooms.create_reserved_tournament_room(
            7, 9, 101, "White", 1500, 202, "Black", 1450, game::TimeControl(60000, 0));
        room->allow_reserved_player(101); room->allow_reserved_player(202);
        room->bind_reserved_player(101, 1, 10); room->bind_reserved_player(202, 2, 20);
        room->open_reserved();
        // Fool's mate through precisely the same submit_move used in normal play.
        if (!room->submit_move(10, make_square(1, 5), make_square(2, 5)).success
            || !room->submit_move(20, make_square(6, 4), make_square(4, 4)).success
            || !room->submit_move(10, make_square(1, 6), make_square(3, 6)).success) return false;
        const auto mate = room->submit_move(20, make_square(7, 3), make_square(3, 7));
        if (!mate.success || mate.game_status != GameStatus::CHECKMATE
            || store.calls != 0 || !sink.results.empty() || tasks.size() != 1) return false;
        tasks.front()();
        return store.calls == 1 && sink.results.size() == 1 && sink.results[0].second == "0-1";
    });

    if (!std::getenv("DATABASE_URL")) {
        std::cout << "DATABASE_URL not set — DB runtime integration skipped\n";
        std::cout << "Results: " << passed << " passed, " << failed << " failed\n";
        return failed ? 1 : 0;
    }

    Database db; std::string error;
    if (!db.connect_from_env(error) || !prepare_schema(db)) return 1;
    const int64_t white = add_player(db, "RuntimeWhite", 1500);
    const int64_t black = add_player(db, "RuntimeBlack", 1450);
    const int64_t third = add_player(db, "RuntimeThird", 1400);
    const int64_t fourth = add_player(db, "RuntimeFourth", 1350);

    test("early check-ins bind one persisted room and completion resolves pairing", [&] {
        ports::FakeClock clock;
        game::RoomManager rooms;
        std::vector<std::pair<int,std::string>> notifications;
        TournamentRuntimeService runtime(&db, rooms,
            [&](int fd, const std::string& frame) { notifications.push_back({fd,frame}); }, clock);
        RecordingStore store(&db);
        std::vector<std::function<void()>> tasks;
        auto completion = std::make_shared<GameCompletionService>(store, &runtime,
            [&](std::function<void()> task) { tasks.push_back(std::move(task)); });
        rooms.set_default_listener(completion);

        auto created = create_tournament(db, "Runtime Cup", 1, 60000, 0, white,
                                         100, 200, 60);
        TournamentManager manager(db, clock);
        if (!created.ok || !manager.join(created.id, white, 1500).ok
            || !manager.join(created.id, black, 1450).ok
            || !manager.start(created.id, white).ok) return false;
        AuthenticatedIdentity w{white,"RuntimeWhite",1500};
        AuthenticatedIdentity b{black,"RuntimeBlack",1450};
        if (!runtime.check_in_and_bind(created.id,1,w,10).ok
            || !runtime.check_in_and_bind(created.id,1,b,20).ok
            || rooms.room_count() != 0) return false;
        clock.advance(std::chrono::seconds(200));
        runtime.maintenance_tick();
        auto pairings = get_pairings_for_round(db, created.id, 1);
        if (pairings.size()!=1 || !pairings[0].game_id) return false;
        auto room = rooms.find_room_by_pairing(pairings[0].id);
        if (!room || room->get_state()!=game::RoomState::IN_PROGRESS
            || room->get_player_fd(Color::WHITE)!=10
            || room->get_player_fd(Color::BLACK)!=20) return false;
        if (!room->resign(10) || store.calls != 0 || tasks.size() != 1) return false;
        auto before_save = find_pairing(db, pairings[0].id);
        if (!before_save || before_save->result != "pending") return false;
        tasks.front()();
        if (store.calls != 1) return false;
        auto state = manager.get_state(created.id);
        if (!state || state->tournament.status!="completed"
            || state->all_pairings[0].result!="0-1"
            || state->all_pairings[0].result_source!="game"
            || state->all_pairings[0].replay_game_id != 1) return false;
        runtime.on_tournament_game_persisted(pairings[0].id,"0-1",1);
        state = manager.get_state(created.id);
        return state && state->standings[0].player_id==black
            && state->standings[0].score==1.0 && notifications.size()>=2;
    });

    test("unstarted room mapping recovers after process-local room loss", [&] {
        ports::FakeClock clock;
        auto created = create_tournament(db, "Recovery Cup", 1, 60000, 0, white,
                                         100, 200, 60);
        TournamentManager manager(db,clock);
        if (!created.ok || !manager.join(created.id,white,1500).ok
            || !manager.join(created.id,black,1450).ok
            || !manager.start(created.id,white).ok) return false;
        AuthenticatedIdentity w{white,"RuntimeWhite",1500};
        AuthenticatedIdentity b{black,"RuntimeBlack",1450};
        GameId first_room_id=0; int64_t pairing_id=0;
        {
            game::RoomManager first_rooms;
            TournamentRuntimeService first(&db,first_rooms,[](int,const std::string&){},clock);
            if (!first.check_in_and_bind(created.id,1,w,31).ok) return false;
            clock.advance(std::chrono::seconds(200)); first.maintenance_tick();
            auto pairings=get_pairings_for_round(db,created.id,1);
            if (pairings.size()!=1 || !pairings[0].game_id) return false;
            pairing_id=pairings[0].id; first_room_id=*pairings[0].game_id;
        }
        game::RoomManager recovered_rooms;
        TournamentRuntimeService recovered(&db,recovered_rooms,[](int,const std::string&){},clock);
        recovered.maintenance_tick();
        auto recovered_room=recovered_rooms.find_room_by_pairing(pairing_id);
        auto pairing=find_pairing(db,pairing_id);
        if (!recovered_room || !pairing || !pairing->game_id
            || *pairing->game_id==first_room_id) return false;
        if (!recovered_room->on_reconnect_db_player(white,41)) return false;
        auto black_join = recovered.check_in_and_bind(created.id,1,b,42);
        if (!black_join.ok || !black_join.room_ready) return false;
        return recovered_room->get_state()==game::RoomState::IN_PROGRESS
            && recovered_room->get_player_fd(Color::WHITE)==41
            && recovered_room->get_player_fd(Color::BLACK)==42;
    });

    test("two rounds advance entirely from played game completions", [&] {
        ports::FakeClock clock;
        game::RoomManager rooms;
        TournamentRuntimeService runtime(&db,rooms,[](int,const std::string&){},clock);
        RecordingStore store(&db);
        auto completion=std::make_shared<GameCompletionService>(store,&runtime);
        rooms.set_default_listener(completion);
        auto created=create_tournament(db,"Two Round Cup",2,60000,0,white,100,200,60);
        TournamentManager manager(db,clock);
        const std::vector<std::tuple<int64_t,std::string,int,int>> players={
            {white,"RuntimeWhite",1500,51},{black,"RuntimeBlack",1450,52},
            {third,"RuntimeThird",1400,53},{fourth,"RuntimeFourth",1350,54}};
        if (!created.ok) return false;
        for (const auto& [id,name,elo,fd] : players)
            if (!manager.join(created.id,id,elo).ok) return false;
        if (!manager.start(created.id,white).ok) return false;
        for (const auto& [id,name,elo,fd] : players) {
            AuthenticatedIdentity actor{id,name,elo};
            if (!runtime.check_in_and_bind(created.id,1,actor,fd).ok) return false;
        }
        clock.advance(std::chrono::seconds(200)); runtime.maintenance_tick();
        auto round1=get_pairings_for_round(db,created.id,1);
        if (round1.size()!=2) return false;
        for (const auto& pairing : round1) {
            auto room=rooms.find_room_by_pairing(pairing.id);
            if (!room || room->get_state()!=game::RoomState::IN_PROGRESS
                || !room->resign(room->get_player_fd(Color::WHITE))) return false;
        }
        auto mid=manager.get_state(created.id);
        if (!mid || mid->tournament.current_round!=2 || mid->tournament.status!="in_progress") return false;
        for (const auto& [id,name,elo,fd] : players) {
            AuthenticatedIdentity actor{id,name,elo};
            if (!runtime.check_in_and_bind(created.id,2,actor,fd+10).ok) return false;
        }
        clock.advance(std::chrono::seconds(60)); runtime.maintenance_tick();
        auto round2=get_pairings_for_round(db,created.id,2);
        if (round2.size()!=2) return false;
        for (const auto& pairing : round2) {
            auto room=rooms.find_room_by_pairing(pairing.id);
            if (!room || room->get_state()!=game::RoomState::IN_PROGRESS
                || !room->resign(room->get_player_fd(Color::WHITE))) return false;
        }
        auto final=manager.get_state(created.id);
        if (!final || final->tournament.status!="completed" || store.calls!=4) return false;
        for (const auto& pairing : final->all_pairings)
            if (pairing.result=="pending" || pairing.result_source!="game") return false;
        return true;
    });

    test("pre-start disconnect becomes no-show and leaves no stale room lock", [&] {
        ports::FakeClock clock;
        game::RoomManager rooms;
        TournamentRuntimeService runtime(&db,rooms,[](int,const std::string&){},clock);
        auto created=create_tournament(db,"Disconnect No-show Cup",1,60000,0,
                                       white,100,200,60);
        TournamentManager manager(db,clock);
        if (!created.ok || !manager.join(created.id,white,1500).ok
            || !manager.join(created.id,black,1450).ok
            || !manager.start(created.id,white).ok) return false;
        AuthenticatedIdentity w{white,"RuntimeWhite",1500};
        AuthenticatedIdentity b{black,"RuntimeBlack",1450};
        if (!runtime.check_in_and_bind(created.id,1,w,61).ok) return false;
        runtime.on_disconnect(61);
        if (!runtime.check_in_and_bind(created.id,1,b,62).ok) return false;
        clock.advance(std::chrono::seconds(260)); runtime.maintenance_tick();
        auto state=manager.get_state(created.id);
        const bool ok = state && state->tournament.status=="completed"
            && state->all_pairings.size()==1 && state->all_pairings[0].result=="0-1"
            && state->all_pairings[0].result_source=="forfeit"
            && !rooms.find_room_by_pairing(state->all_pairings[0].id)
            && !rooms.is_tournament_player_reserved(white)
            && !rooms.is_tournament_player_reserved(black);
        return ok;
    });

    test("winners advance: shared draw controls persist two replays and joint final", [&] {
        ports::FakeClock clock;
        game::RoomManager rooms;
        TournamentRuntimeService runtime(&db, rooms, [](int, const std::string&) {}, clock);
        RecordingStore store(&db);
        auto completion = std::make_shared<GameCompletionService>(store, &runtime);
        rooms.set_default_listener(completion);
        auto created = create_tournament(db, "Draw Final", 0, 60000, 0, white, 100, 200, 60, "winners_advance");
        TournamentManager manager(db, clock);
        if (!created.ok || !manager.join(created.id, white, 1500).ok
            || !manager.join(created.id, black, 1450).ok || !manager.start(created.id, white).ok) return false;
        for (int round = 1; round <= 2; ++round) {
            AuthenticatedIdentity w{white, "RuntimeWhite", 1500}, b{black, "RuntimeBlack", 1450};
            if (!runtime.check_in_and_bind(created.id, round, w, 70 + round * 2).ok
                || !runtime.check_in_and_bind(created.id, round, b, 71 + round * 2).ok) return false;
            clock.advance(std::chrono::seconds(round == 1 ? 200 : 60));
            runtime.maintenance_tick();
            const auto pairings = get_pairings_for_round(db, created.id, round);
            if (pairings.size() != 1) return false;
            auto room = rooms.find_room_by_pairing(pairings[0].id);
            std::string draw_error;
            if (!room || room->get_state() != game::RoomState::IN_PROGRESS
                || !room->offer_draw(room->get_player_fd(Color::WHITE), draw_error)
                || !room->respond_to_draw(room->get_player_fd(Color::BLACK), true, draw_error)) return false;
            const auto stored = find_pairing(db, pairings[0].id);
            if (!stored || stored->result != "1/2-1/2" || !stored->replay_game_id) return false;
        }
        return store.calls == 2 && find_tournament(db, created.id)->status == "completed";
    });

    std::cout << "Results: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
