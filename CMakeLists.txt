cmake_minimum_required(VERSION 3.16)
project(MultiplayerChessPlatform VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Treat warnings as errors, standard warnings
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Werror")

# Threading
find_package(Threads REQUIRED)

# OpenSSL.
# Phase 7 requires >= 3.5: it is the first release carrying ML-KEM (FIPS 203)
# and ML-DSA (FIPS 204) in the DEFAULT provider, and >= 3.2 for Argon2id.
# Ubuntu 26.04 LTS ships 3.5.5, so no OPENSSL_ROOT_DIR override or source build
# is needed. Ubuntu 22.04's 3.0.2 had none of these — see CLAUDE.md.
find_package(OpenSSL 3.5 REQUIRED)
message(STATUS "OpenSSL ${OPENSSL_VERSION} at ${OPENSSL_INCLUDE_DIR}")

# libpq — Phase 7.5. Postgres via Supabase (Cloud Run's filesystem is ephemeral,
# so SQLite is not an option). CMake's PostgreSQL find module provides the
# imported target `PostgreSQL::PostgreSQL` for libpq (headers + shared library).
find_package(PostgreSQL REQUIRED)
message(STATUS "libpq ${PostgreSQL_VERSION_STRING} at ${PostgreSQL_INCLUDE_DIR}")

# Directories
include_directories(src)
include_directories(third_party)

# Source files
set(SOURCES
    src/main.cpp
    src/core/logger.cpp
    src/net/connection.cpp
    src/net/tcp_server.cpp
    src/net/websocket.cpp
    src/concurrent/thread_pool.cpp
    src/chess/zobrist.cpp
    src/chess/transposition_table.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
    src/chess/notation.cpp
    src/chess/evaluator.cpp
    src/chess/engine.cpp
    src/game/game_room.cpp
    src/game/room_manager.cpp
    src/game/game_handler.cpp
    src/game/handlers/gameplay_handler.cpp
    src/application/gameplay_service.cpp
    src/application/auth/identity_extractor.cpp
    src/game/handlers/query_handler.cpp
    src/application/game_query_service.cpp
    src/game/matchmaker.cpp
    src/game/ai_player.cpp
    # Phase 7 — hand-written cryptographic primitives
    src/crypto/random.cpp
    src/crypto/sha256.cpp
    src/crypto/sha512.cpp
    src/crypto/aes.cpp
    src/crypto/aes_ctr.cpp
    src/crypto/aead.cpp
    src/crypto/kem.cpp
    src/crypto/signature.cpp
    src/crypto/x25519.cpp
    src/crypto/base64.cpp
    src/crypto/sealed_envelope.cpp
    src/crypto/sealed_key_store.cpp
    src/crypto/sealed_registry.cpp
    # Phase 7.5 — storage layer
    src/storage/database.cpp
    # Phase 7.6 — password + username + auth service
    src/auth/username.cpp
    src/auth/password.cpp
    src/auth/service.cpp
    # Phase 7.7 — JWT + sessions
    src/auth/token.cpp
    src/auth/session.cpp
    # Phase 7.8 — Google Sign-In via Supabase Auth
    src/auth/oauth_verify.cpp
    # Phase 7.9 — rate limiter + safe match_found builder
    src/auth/rate_limiter.cpp
    src/game/match_notify.cpp
    # Phase 7.10 — auth surface glued into the router
    src/auth/auth_handler.cpp
    # Phase 8.2 — game persistence repositories
    src/storage/player_repo.cpp
    src/storage/game_repo.cpp
    # Phase 9.3 — statistical anti-cheat + cheat report persistence
    src/analysis/anti_cheat.cpp
    src/analysis/cheat_report_repo.cpp
    # Phase 9.4 — Swiss tournament system
    src/tournament/swiss.cpp
    src/tournament/tournament_repo.cpp
    src/tournament/tournament_manager.cpp
    # LLD-1 — typed protocol boundary
    src/protocol/json_codec.cpp
    src/net/socket_message_sink.cpp
)

# Executable
add_executable(chess_server ${SOURCES})
target_link_libraries(chess_server PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto PostgreSQL::PostgreSQL)

# Test: Board logic (Phase 3.1)
add_executable(test_board
    tests/test_board.cpp
    src/chess/zobrist.cpp
    src/chess/board.cpp
)
target_include_directories(test_board PRIVATE src)
target_link_libraries(test_board PRIVATE Threads::Threads)

# Test: Move Generation (Phase 3.2)
add_executable(test_move_gen
    tests/test_move_gen.cpp
    src/chess/zobrist.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
)
target_include_directories(test_move_gen PRIVATE src)
target_link_libraries(test_move_gen PRIVATE Threads::Threads)

# Test: Notation & PGN (Phase 3.3)
add_executable(test_notation
    tests/test_notation.cpp
    src/chess/zobrist.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
    src/chess/notation.cpp
)
target_include_directories(test_notation PRIVATE src)
target_link_libraries(test_notation PRIVATE Threads::Threads)

# Test: Game Room Manager (Phase 4.1)
add_executable(test_game_room
    tests/test_game_room.cpp
    src/chess/zobrist.cpp
    src/chess/transposition_table.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
    src/chess/notation.cpp
    src/chess/evaluator.cpp
    src/chess/engine.cpp
    src/game/ai_player.cpp
    src/game/game_room.cpp
    src/game/room_manager.cpp
)
target_include_directories(test_game_room PRIVATE src)
target_link_libraries(test_game_room PRIVATE Threads::Threads)

# Test: Matchmaker (Phase 4.3)
add_executable(test_matchmaker
    tests/test_matchmaker.cpp
    src/core/logger.cpp
    src/chess/zobrist.cpp
    src/chess/transposition_table.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
    src/chess/notation.cpp
    src/chess/evaluator.cpp
    src/chess/engine.cpp
    src/game/ai_player.cpp
    src/game/game_room.cpp
    src/game/room_manager.cpp
    src/game/matchmaker.cpp
)
target_include_directories(test_matchmaker PRIVATE src)
target_link_libraries(test_matchmaker PRIVATE Threads::Threads)

# Test: Chess Engine — Minimax + Alpha-Beta (Phase 6.1)
add_executable(test_engine
    tests/test_engine.cpp
    src/chess/zobrist.cpp
    src/chess/transposition_table.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
    src/chess/notation.cpp
    src/chess/evaluator.cpp
    src/chess/engine.cpp
)
target_include_directories(test_engine PRIVATE src)
target_link_libraries(test_engine PRIVATE Threads::Threads)

# Test: Transposition Table & Zobrist (Phase 6.3)
add_executable(test_transposition_table
    tests/test_transposition_table.cpp
    src/chess/zobrist.cpp
    src/chess/transposition_table.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
    src/chess/notation.cpp
    src/chess/evaluator.cpp
    src/chess/engine.cpp
)
target_include_directories(test_transposition_table PRIVATE src)
target_link_libraries(test_transposition_table PRIVATE Threads::Threads)

# Test: Crypto environment gate (Phase 7.0)
# Fails loudly if OpenSSL lacks ML-KEM / ML-DSA / Argon2id, rather than letting
# a missing primitive surface later as a confusing runtime error.
add_executable(test_crypto_env
    tests/test_crypto_env.cpp
    src/crypto/random.cpp
)
target_include_directories(test_crypto_env PRIVATE src)
target_link_libraries(test_crypto_env PRIVATE OpenSSL::Crypto)

# Test: Hand-written SHA-256 / SHA-384 / SHA-512 / HMAC / HKDF (Phase 7.1)
# Links OpenSSL only so the differential tests have a reference implementation
# to compare against — the primitives under test are entirely our own.
add_executable(test_hash
    tests/test_hash.cpp
    src/crypto/sha256.cpp
    src/crypto/sha512.cpp
)
target_include_directories(test_hash PRIVATE src)
target_link_libraries(test_hash PRIVATE OpenSSL::Crypto)

# Test: Hand-written AES-256 / CTR / Encrypt-then-MAC AEAD (Phase 7.2)
# OpenSSL is linked only as a differential reference for the CTR tests.
add_executable(test_aes
    tests/test_aes.cpp
    src/crypto/sha256.cpp
    src/crypto/sha512.cpp
    src/crypto/aes.cpp
    src/crypto/aes_ctr.cpp
    src/crypto/aead.cpp
)
target_include_directories(test_aes PRIVATE src)
target_link_libraries(test_aes PRIVATE OpenSSL::Crypto)

# Test: ML-KEM-768 / ML-DSA-65 / X25519 wrappers (Phase 7.3)
# Unlike test_hash and test_aes, OpenSSL here is the implementation under test,
# not a reference: these are RAII wrappers, so the tests check sizes against
# FIPS 203/204, the raw import/export path, and rejection behaviour.
add_executable(test_pqc
    tests/test_pqc.cpp
    src/crypto/random.cpp
    src/crypto/sha256.cpp
    src/crypto/sha512.cpp
    src/crypto/kem.cpp
    src/crypto/signature.cpp
    src/crypto/x25519.cpp
)
target_include_directories(test_pqc PRIVATE src)
target_link_libraries(test_pqc PRIVATE OpenSSL::Crypto)

# Test: sealed envelope, one-time key store, opt-in registry (Phase 7.4)
add_executable(test_sealed_envelope
    tests/test_sealed_envelope.cpp
    src/crypto/random.cpp
    src/crypto/sha256.cpp
    src/crypto/sha512.cpp
    src/crypto/aes.cpp
    src/crypto/aes_ctr.cpp
    src/crypto/aead.cpp
    src/crypto/kem.cpp
    src/crypto/signature.cpp
    src/crypto/x25519.cpp
    src/crypto/base64.cpp
    src/crypto/sealed_envelope.cpp
    src/crypto/sealed_key_store.cpp
    src/crypto/sealed_registry.cpp
)
target_include_directories(test_sealed_envelope PRIVATE src third_party)
target_link_libraries(test_sealed_envelope PRIVATE Threads::Threads OpenSSL::Crypto)

# Tool: emit cross-language test vectors for the sealed envelope (Phase 7.4).
# The JS client re-derives the same keys from the same inputs; see
# tests/seal_vectors.mjs. A KDF that disagrees across languages is the single
# most likely bug in this phase, and it fails silently as "bad password".
add_executable(gen_seal_vectors
    tools/gen_seal_vectors.cpp
    src/crypto/random.cpp
    src/crypto/sha256.cpp
    src/crypto/sha512.cpp
    src/crypto/aes.cpp
    src/crypto/aes_ctr.cpp
    src/crypto/aead.cpp
    src/crypto/base64.cpp
)
target_include_directories(gen_seal_vectors PRIVATE src)
target_link_libraries(gen_seal_vectors PRIVATE OpenSSL::Crypto)

# Tool: Phase 10.1 engine microbenchmark. Standalone (no server, no DB): loads
# a small position suite, runs Engine::search at 2s/5s/10s each, prints a
# Markdown table for BENCHMARKS.md.
add_executable(bench_engine
    tools/bench_engine.cpp
    src/chess/zobrist.cpp
    src/chess/transposition_table.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
    src/chess/notation.cpp
    src/chess/evaluator.cpp
    src/chess/engine.cpp
)
target_include_directories(bench_engine PRIVATE src)
target_link_libraries(bench_engine PRIVATE Threads::Threads)

# Tool: pre-provision N test accounts + HS384 access tokens for the Phase-10.1
# load test. Load_test.py cannot easily replay the sealed-envelope registration
# path from Python, and sealed-envelope confidentiality only protects the
# password on the wire — nothing on the make_move hot path involves it, so
# bypassing it does not distort the load measurements. Reads DATABASE_URL and
# JWT_SIGNING_KEY from env (the SAME env vars the server reads), inserts N
# players via `ON CONFLICT DO UPDATE`, and emits JSON on stdout.
add_executable(bench_provision
    tools/bench_provision.cpp
    src/auth/token.cpp
    src/crypto/random.cpp
    src/crypto/sha256.cpp
    src/crypto/sha512.cpp
    src/crypto/base64.cpp
    src/storage/database.cpp
)
target_include_directories(bench_provision PRIVATE src third_party)
target_link_libraries(bench_provision PRIVATE OpenSSL::Crypto PostgreSQL::PostgreSQL)

# Tool: generate the server's long-lived ML-DSA-65 identity key (Phase 7.3).
# Run once per deployment; prints the SHA-384 pin for frontend/js/config.js.
add_executable(gen_server_identity
    tools/gen_server_identity.cpp
    src/crypto/sha512.cpp
    src/crypto/signature.cpp
)
target_include_directories(gen_server_identity PRIVATE src)
target_link_libraries(gen_server_identity PRIVATE OpenSSL::Crypto)

# Test: libpq wrapper (Phase 7.5)
# Only runs when $DATABASE_URL is set; regress.sh boots an ephemeral local
# Postgres and points it there. Skipped otherwise — the C++ tests must still
# pass on a machine without Postgres installed.
add_executable(test_database
    tests/test_database.cpp
    src/storage/database.cpp
)
target_include_directories(test_database PRIVATE src)
target_compile_definitions(test_database PRIVATE CHESS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_database PRIVATE PostgreSQL::PostgreSQL)

# Test: Phase 8.1 tracked migration, game-history schema, and index plan.
# Like the Phase-7 database suite, it skips cleanly without DATABASE_URL.
add_executable(test_schema_phase8
    tests/test_schema_phase8.cpp
    src/storage/database.cpp
)
target_include_directories(test_schema_phase8 PRIVATE src)
target_compile_definitions(test_schema_phase8 PRIVATE CHESS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_schema_phase8 PRIVATE PostgreSQL::PostgreSQL)

# Test: Argon2id, username validation, auth service (Phase 7.6)
# Purely-crypto tests always run; service tests require $DATABASE_URL, which
# regress.sh provides via the ephemeral local Postgres from Phase 7.5.
add_executable(test_password
    tests/test_password.cpp
    src/auth/username.cpp
    src/auth/password.cpp
    src/auth/service.cpp
    src/crypto/random.cpp
    src/storage/database.cpp
)
target_include_directories(test_password PRIVATE src)
target_compile_definitions(test_password PRIVATE CHESS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_password PRIVATE OpenSSL::Crypto PostgreSQL::PostgreSQL)

# Test: JWT (HS384) + refresh rotation + logout_all (Phase 7.7)
add_executable(test_token
    tests/test_token.cpp
    src/auth/token.cpp
    src/auth/session.cpp
    src/crypto/random.cpp
    src/crypto/sha256.cpp
    src/crypto/sha512.cpp
    src/crypto/base64.cpp
    src/storage/database.cpp
)
target_include_directories(test_token PRIVATE src third_party)
target_compile_definitions(test_token PRIVATE CHESS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_token PRIVATE OpenSSL::Crypto PostgreSQL::PostgreSQL)

# Test: Google Sign-In via Supabase Auth — HS256 verifier + service (Phase 7.8)
add_executable(test_oauth
    tests/test_oauth.cpp
    src/auth/oauth_verify.cpp
    src/auth/username.cpp
    src/auth/password.cpp
    src/auth/service.cpp
    src/auth/token.cpp
    src/crypto/random.cpp
    src/crypto/sha256.cpp
    src/crypto/sha512.cpp
    src/crypto/base64.cpp
    src/storage/database.cpp
)
target_include_directories(test_oauth PRIVATE src third_party)
target_compile_definitions(test_oauth PRIVATE CHESS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_oauth PRIVATE OpenSSL::Crypto PostgreSQL::PostgreSQL)

# Test: rate limiter + WS frame-size cap (Phase 7.9)
add_executable(test_rate_limiter
    tests/test_rate_limiter.cpp
    src/auth/rate_limiter.cpp
    src/net/connection.cpp
    src/net/websocket.cpp
    src/core/logger.cpp
)
target_include_directories(test_rate_limiter PRIVATE src)
target_link_libraries(test_rate_limiter PRIVATE Threads::Threads OpenSSL::Crypto)

# Test: match_found JSON escaping (Phase 7.9)
add_executable(test_json_escaping
    tests/test_json_escaping.cpp
    src/game/match_notify.cpp
)
target_include_directories(test_json_escaping PRIVATE src third_party)
target_link_libraries(test_json_escaping PRIVATE Threads::Threads)

# Test: AI Player Integration (Phase 6.4)
add_executable(test_ai_integration
    tests/test_ai_integration.cpp
    src/chess/zobrist.cpp
    src/chess/transposition_table.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
    src/chess/notation.cpp
    src/chess/evaluator.cpp
    src/chess/engine.cpp
    src/game/ai_player.cpp
    src/game/game_room.cpp
    src/game/room_manager.cpp
)
target_include_directories(test_ai_integration PRIVATE src)
target_link_libraries(test_ai_integration PRIVATE Threads::Threads)

# Test: ELO calculation (Phase 8.2) — pure math, no DB, runs everywhere.
add_executable(test_elo
    tests/test_elo.cpp
)
target_include_directories(test_elo PRIVATE src)
target_link_libraries(test_elo PRIVATE Threads::Threads)

# Test: PlayerRepository + GameRepository (Phase 8.2)
# Database-backed; skips cleanly without DATABASE_URL.
add_executable(test_repositories
    tests/test_repositories.cpp
    src/storage/database.cpp
    src/storage/player_repo.cpp
    src/storage/game_repo.cpp
)
target_include_directories(test_repositories PRIVATE src)
target_compile_definitions(test_repositories PRIVATE CHESS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_repositories PRIVATE Threads::Threads PostgreSQL::PostgreSQL)

# Test: Game Persistence Integration (Phase 8.3)
# End-to-end: GameRoom getters, persist_game, profile, leaderboard.
# Database-backed tests skip cleanly without DATABASE_URL.
add_executable(test_game_persistence
    tests/test_game_persistence.cpp
    src/chess/zobrist.cpp
    src/chess/transposition_table.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
    src/chess/notation.cpp
    src/chess/evaluator.cpp
    src/chess/engine.cpp
    src/game/ai_player.cpp
    src/game/game_room.cpp
    src/game/room_manager.cpp
    src/storage/database.cpp
    src/storage/player_repo.cpp
    src/storage/game_repo.cpp
)
target_include_directories(test_game_persistence PRIVATE src)
target_compile_definitions(test_game_persistence PRIVATE CHESS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_game_persistence PRIVATE Threads::Threads PostgreSQL::PostgreSQL)

# Test: Auth-mandatory game commands (Phase 9 pre-work).
# Routes real create_game / join_game / quick_play / play_ai JSON messages
# through the actual MessageRouter → GameHandler path and asserts every path
# rejects an unauthenticated caller with {code: "auth_required"}. Database
# backed; skips cleanly without DATABASE_URL, matching Phase 7/8 conventions.
add_executable(test_auth_required
    tests/test_auth_required.cpp
    src/core/logger.cpp
    src/net/connection.cpp
    src/net/websocket.cpp
    src/chess/zobrist.cpp
    src/chess/transposition_table.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
    src/chess/notation.cpp
    src/chess/evaluator.cpp
    src/chess/engine.cpp
    src/game/ai_player.cpp
    src/game/game_room.cpp
    src/game/room_manager.cpp
    src/game/matchmaker.cpp
    src/game/game_handler.cpp
    src/game/handlers/gameplay_handler.cpp
    src/application/gameplay_service.cpp
    src/application/auth/identity_extractor.cpp
    src/game/handlers/query_handler.cpp
    src/application/game_query_service.cpp
    src/protocol/json_codec.cpp
    src/net/socket_message_sink.cpp
    src/storage/database.cpp
    src/storage/player_repo.cpp
    src/storage/game_repo.cpp
    src/analysis/anti_cheat.cpp
    src/analysis/cheat_report_repo.cpp
    src/tournament/swiss.cpp
    src/tournament/tournament_repo.cpp
    src/tournament/tournament_manager.cpp
    src/auth/token.cpp
    src/auth/session.cpp
    src/crypto/random.cpp
    src/crypto/sha256.cpp
    src/crypto/sha512.cpp
    src/crypto/base64.cpp
)
target_include_directories(test_auth_required PRIVATE src third_party)
target_compile_definitions(test_auth_required PRIVATE CHESS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_auth_required PRIVATE
    Threads::Threads OpenSSL::Crypto PostgreSQL::PostgreSQL)

# Test: Spectator Mode (Phase 9.1).
# Two players start a live game, a third connection spectates, and every
# broadcast (moves and game_over) is asserted to reach the spectator alongside
# the two seats. The disconnect sweep and stop_spectating happy paths are
# covered by asserting the socket goes quiet after the fd is removed.
add_executable(test_spectator
    tests/test_spectator.cpp
    src/core/logger.cpp
    src/net/connection.cpp
    src/net/websocket.cpp
    src/chess/zobrist.cpp
    src/chess/transposition_table.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
    src/chess/notation.cpp
    src/chess/evaluator.cpp
    src/chess/engine.cpp
    src/game/ai_player.cpp
    src/game/game_room.cpp
    src/game/room_manager.cpp
    src/game/matchmaker.cpp
    src/game/game_handler.cpp
    src/game/handlers/gameplay_handler.cpp
    src/application/gameplay_service.cpp
    src/application/auth/identity_extractor.cpp
    src/game/handlers/query_handler.cpp
    src/application/game_query_service.cpp
    src/protocol/json_codec.cpp
    src/net/socket_message_sink.cpp
    src/storage/database.cpp
    src/storage/player_repo.cpp
    src/storage/game_repo.cpp
    src/analysis/anti_cheat.cpp
    src/analysis/cheat_report_repo.cpp
    src/tournament/swiss.cpp
    src/tournament/tournament_repo.cpp
    src/tournament/tournament_manager.cpp
    src/auth/token.cpp
    src/auth/session.cpp
    src/crypto/random.cpp
    src/crypto/sha256.cpp
    src/crypto/sha512.cpp
    src/crypto/base64.cpp
)
target_include_directories(test_spectator PRIVATE src third_party)
target_compile_definitions(test_spectator PRIVATE CHESS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_spectator PRIVATE
    Threads::Threads OpenSSL::Crypto PostgreSQL::PostgreSQL)

# Test: Replay + engine analysis (Phase 9.2).
# Persists a full Scholar's Mate through the storage layer, routes get_game
# through the actual handler, and asserts every ply's FEN matches an
# independent replay. Also exercises analyze_position + get_history.
add_executable(test_replay
    tests/test_replay.cpp
    src/core/logger.cpp
    src/net/connection.cpp
    src/net/websocket.cpp
    src/chess/zobrist.cpp
    src/chess/transposition_table.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
    src/chess/notation.cpp
    src/chess/evaluator.cpp
    src/chess/engine.cpp
    src/game/ai_player.cpp
    src/game/game_room.cpp
    src/game/room_manager.cpp
    src/game/matchmaker.cpp
    src/game/game_handler.cpp
    src/game/handlers/gameplay_handler.cpp
    src/application/gameplay_service.cpp
    src/application/auth/identity_extractor.cpp
    src/game/handlers/query_handler.cpp
    src/application/game_query_service.cpp
    src/protocol/json_codec.cpp
    src/net/socket_message_sink.cpp
    src/storage/database.cpp
    src/storage/player_repo.cpp
    src/storage/game_repo.cpp
    src/analysis/anti_cheat.cpp
    src/analysis/cheat_report_repo.cpp
    src/tournament/swiss.cpp
    src/tournament/tournament_repo.cpp
    src/tournament/tournament_manager.cpp
    src/auth/token.cpp
    src/auth/session.cpp
    src/crypto/random.cpp
    src/crypto/sha256.cpp
    src/crypto/sha512.cpp
    src/crypto/base64.cpp
)
target_include_directories(test_replay PRIVATE src third_party)
target_compile_definitions(test_replay PRIVATE CHESS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_replay PRIVATE
    Threads::Threads OpenSSL::Crypto PostgreSQL::PostgreSQL)

# Test: Anti-cheat statistical analyzer (Phase 9.3).
# Three layers: pure analyzer math (no DB, always runs), cheat_report_repo
# round-trip (needs DB), and end-to-end via the router (needs DB). The
# non-DB layer runs first and unconditionally; the DB layers skip cleanly
# without DATABASE_URL, matching test_replay / test_repositories.
add_executable(test_anti_cheat
    tests/test_anti_cheat.cpp
    src/core/logger.cpp
    src/net/connection.cpp
    src/net/websocket.cpp
    src/chess/zobrist.cpp
    src/chess/transposition_table.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
    src/chess/notation.cpp
    src/chess/evaluator.cpp
    src/chess/engine.cpp
    src/game/ai_player.cpp
    src/game/game_room.cpp
    src/game/room_manager.cpp
    src/game/matchmaker.cpp
    src/game/game_handler.cpp
    src/game/handlers/gameplay_handler.cpp
    src/application/gameplay_service.cpp
    src/application/auth/identity_extractor.cpp
    src/game/handlers/query_handler.cpp
    src/application/game_query_service.cpp
    src/protocol/json_codec.cpp
    src/net/socket_message_sink.cpp
    src/storage/database.cpp
    src/storage/player_repo.cpp
    src/storage/game_repo.cpp
    src/analysis/anti_cheat.cpp
    src/analysis/cheat_report_repo.cpp
    src/tournament/swiss.cpp
    src/tournament/tournament_repo.cpp
    src/tournament/tournament_manager.cpp
    src/auth/token.cpp
    src/auth/session.cpp
    src/crypto/random.cpp
    src/crypto/sha256.cpp
    src/crypto/sha512.cpp
    src/crypto/base64.cpp
)
target_include_directories(test_anti_cheat PRIVATE src third_party)
target_compile_definitions(test_anti_cheat PRIVATE CHESS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_anti_cheat PRIVATE
    Threads::Threads OpenSSL::Crypto PostgreSQL::PostgreSQL)

# Test: LLD-1 SocketMessageSink adapter.
# Exercises the delivery boundary: fake sink capture behavior, real
# socket sink over a socketpair, and the generation guard against fd
# reuse.
add_executable(test_message_sink
    tests/test_message_sink.cpp
    src/net/socket_message_sink.cpp
    src/net/connection.cpp
    src/net/websocket.cpp
    src/core/logger.cpp
)
target_include_directories(test_message_sink PRIVATE src third_party)
target_link_libraries(test_message_sink PRIVATE Threads::Threads OpenSSL::Crypto)

# Test: LLD-1 protocol JSON codec.
# Boundary between raw wire JSON and the typed request/response values a
# migrated handler consumes. Pure — no DB, no sockets, no threads — so it
# runs everywhere.
add_executable(test_protocol_codec
    tests/test_protocol_codec.cpp
    src/protocol/json_codec.cpp
)
target_include_directories(test_protocol_codec PRIVATE src third_party)
target_link_libraries(test_protocol_codec PRIVATE Threads::Threads)

# Test: Swiss tournament system (Phase 9.4).
# Three layers: pure Swiss pairing (no DB, always runs), tournament_repo
# round-trip (needs DB), and a full 8-player 4-round tournament run
# through TournamentManager (needs DB). The non-DB layer runs first and
# unconditionally; DB layers skip cleanly without DATABASE_URL.
add_executable(test_tournament
    tests/test_tournament.cpp
    src/storage/database.cpp
    src/tournament/swiss.cpp
    src/tournament/tournament_repo.cpp
    src/tournament/tournament_manager.cpp
)
target_include_directories(test_tournament PRIVATE src)
target_compile_definitions(test_tournament PRIVATE CHESS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_tournament PRIVATE Threads::Threads PostgreSQL::PostgreSQL)

# Test: LLD-2.1 GameplayService.
# Seam-level tests over the extracted service: a fake MessageSink
# captures caller-side frames, two capturing callables stand in for the
# transport foreign-send / spectator-broadcast paths, and a real
# RoomManager + Matchmaker back the state transitions. Pure — no DB,
# no sockets — so it runs everywhere.
add_executable(test_gameplay_service
    tests/test_gameplay_service.cpp
    src/core/logger.cpp
    src/chess/zobrist.cpp
    src/chess/transposition_table.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
    src/chess/notation.cpp
    src/chess/evaluator.cpp
    src/chess/engine.cpp
    src/game/ai_player.cpp
    src/game/game_room.cpp
    src/game/room_manager.cpp
    src/game/matchmaker.cpp
    src/application/gameplay_service.cpp
    src/storage/database.cpp
    src/storage/game_repo.cpp
    src/protocol/json_codec.cpp
)
target_include_directories(test_gameplay_service PRIVATE src third_party)
target_link_libraries(test_gameplay_service PRIVATE Threads::Threads PostgreSQL::PostgreSQL)

# Test: LLD-2.2 GameQueryService.
# Seam-level tests over the extracted service: a fake MessageSink captures
# caller-side frames, a real RoomManager backs spectator + live-game
# routes, and Database* is null for DB-unavailable paths. DB-backed
# routes (get_profile / get_leaderboard / get_history / get_game) are
# covered end-to-end by test_replay / test_repositories; here we test
# only what the seam exposes without a live cluster.
add_executable(test_game_query_service
    tests/test_game_query_service.cpp
    src/core/logger.cpp
    src/chess/zobrist.cpp
    src/chess/transposition_table.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
    src/chess/notation.cpp
    src/chess/evaluator.cpp
    src/chess/engine.cpp
    src/game/ai_player.cpp
    src/game/game_room.cpp
    src/game/room_manager.cpp
    src/game/matchmaker.cpp
    src/application/game_query_service.cpp
    src/storage/database.cpp
    src/storage/game_repo.cpp
    src/storage/player_repo.cpp
)
target_include_directories(test_game_query_service PRIVATE src third_party)
target_link_libraries(test_game_query_service PRIVATE Threads::Threads PostgreSQL::PostgreSQL)
