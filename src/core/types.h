#pragma once

#include <cstdint>
#include <string>

namespace chess {

// Chess board coordinates
using Square = uint8_t;
using Bitboard = uint64_t;

// Standard types
using PlayerId = uint64_t;
using GameId = uint32_t;

enum class PieceType : uint8_t {
    PAWN = 0,
    KNIGHT,
    BISHOP,
    ROOK,
    QUEEN,
    KING,
    NONE
};

enum class Color : uint8_t {
    WHITE = 0,
    BLACK,
    NONE
};

// Result pattern for error handling
template<typename T, typename E = std::string>
struct Result {
    T value;
    E error;
    bool success;

    static Result<T, E> Ok(T val) {
        return {val, "", true};
    }

    static Result<T, E> Err(E err) {
        return {T{}, err, false};
    }
};

} // namespace chess
