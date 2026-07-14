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
    src/chess/board.cpp
    src/chess/move_gen.cpp
    src/chess/notation.cpp
)

# Executable
add_executable(chess_server ${SOURCES})
target_link_libraries(chess_server PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

# Test: Board logic (Phase 3.1)
add_executable(test_board
    tests/test_board.cpp
    src/chess/board.cpp
)
target_include_directories(test_board PRIVATE src)
target_link_libraries(test_board PRIVATE Threads::Threads)

# Test: Move Generation (Phase 3.2)
add_executable(test_move_gen
    tests/test_move_gen.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
)
target_include_directories(test_move_gen PRIVATE src)
target_link_libraries(test_move_gen PRIVATE Threads::Threads)

# Test: Notation & PGN (Phase 3.3)
add_executable(test_notation
    tests/test_notation.cpp
    src/chess/board.cpp
    src/chess/move_gen.cpp
    src/chess/notation.cpp
)
target_include_directories(test_notation PRIVATE src)
target_link_libraries(test_notation PRIVATE Threads::Threads)

