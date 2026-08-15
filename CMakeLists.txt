cmake_minimum_required(VERSION 3.16)
project(MultiplayerChessPlatform VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Treat warnings as errors, standard warnings
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Werror")

# Threading
find_package(Threads REQUIRED)

# OpenSSL (for SHA1 in WebSocket handshake, HMAC for JWT later)
find_package(OpenSSL REQUIRED)

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
    src/game/matchmaker.cpp
    src/game/ai_player.cpp
)

# Executable
add_executable(chess_server ${SOURCES})
target_link_libraries(chess_server PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

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
