cmake_minimum_required(VERSION 3.16)
project(MultiplayerChessPlatform VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Treat warnings as errors, standard warnings
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Werror")

# Threading
find_package(Threads REQUIRED)

# Directories
include_directories(src)
include_directories(third_party)

# Source files
set(SOURCES
    src/main.cpp
    src/core/logger.cpp
    src/net/connection.cpp
    src/net/tcp_server.cpp
    src/concurrent/thread_pool.cpp
)

# Executable
add_executable(chess_server ${SOURCES})
target_link_libraries(chess_server PRIVATE Threads::Threads)
