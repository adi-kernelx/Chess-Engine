/**
 * board.cpp — Chess board implementation
 *
 * Implements the mailbox (8x8 array) board representation, FEN parsing/
 * serialization, move execution with all special rules (castling, en passant,
 * promotion, double pawn push), and ASCII display for debugging.
 */

#include "chess/board.h"
#include "chess/zobrist.h"
#include <sstream>
#include <cctype>
#include <cassert>

namespace chess {

// ============================================================
// Piece — character conversion (defined in types.h, implemented here)
// ============================================================

char Piece::to_char() const {
    if (type == PieceType::NONE) return '.';

    char c = '.';
    switch (type) {
        case PieceType::PAWN:   c = 'P'; break;
        case PieceType::KNIGHT: c = 'N'; break;
        case PieceType::BISHOP: c = 'B'; break;
        case PieceType::ROOK:   c = 'R'; break;
        case PieceType::QUEEN:  c = 'Q'; break;
        case PieceType::KING:   c = 'K'; break;
        default: return '.';
    }

    // Lowercase for black pieces (standard FEN convention)
    return (color == Color::BLACK) ? static_cast<char>(std::tolower(c)) : c;
}

Piece Piece::from_char(char c) {
    Color col = std::isupper(c) ? Color::WHITE : Color::BLACK;
    switch (std::tolower(c)) {
        case 'p': return Piece(PieceType::PAWN,   col);
        case 'n': return Piece(PieceType::KNIGHT, col);
        case 'b': return Piece(PieceType::BISHOP, col);
        case 'r': return Piece(PieceType::ROOK,   col);
        case 'q': return Piece(PieceType::QUEEN,  col);
        case 'k': return Piece(PieceType::KING,   col);
        default:  return Piece();  // NONE/NONE
    }
}

// ============================================================
// Move — UCI notation conversion (defined in move.h, implemented here)
// ============================================================

std::string Move::to_uci() const {
    std::string result;
    result += static_cast<char>('a' + file_of(from));
    result += static_cast<char>('1' + rank_of(from));
    result += static_cast<char>('a' + file_of(to));
    result += static_cast<char>('1' + rank_of(to));

    if (is_promotion()) {
        switch (promo_type) {
            case PieceType::QUEEN:  result += 'q'; break;
            case PieceType::ROOK:   result += 'r'; break;
            case PieceType::BISHOP: result += 'b'; break;
            case PieceType::KNIGHT: result += 'n'; break;
            default: break;
        }
    }
    return result;
}

Move Move::from_uci(const std::string& uci) {
    if (uci.length() < 4 || uci.length() > 5) return Move();

    int from_file = uci[0] - 'a';
    int from_rank = uci[1] - '1';
    int to_file   = uci[2] - 'a';
    int to_rank   = uci[3] - '1';

    if (from_file < 0 || from_file > 7 || from_rank < 0 || from_rank > 7 ||
        to_file   < 0 || to_file   > 7 || to_rank   < 0 || to_rank   > 7) {
        return Move();
    }

    Square from_sq = make_square(from_rank, from_file);
    Square to_sq   = make_square(to_rank,   to_file);

    uint8_t flags = MoveFlags::NONE;
    PieceType promo = PieceType::NONE;

    if (uci.length() == 5) {
        flags |= MoveFlags::PROMOTION;
        switch (std::tolower(uci[4])) {
            case 'q': promo = PieceType::QUEEN;  break;
            case 'r': promo = PieceType::ROOK;   break;
            case 'b': promo = PieceType::BISHOP; break;
            case 'n': promo = PieceType::KNIGHT; break;
            default:  return Move();  // Invalid promotion character
        }
    }

    return Move(from_sq, to_sq, flags, promo);
}

// ============================================================
// Board — Construction
// ============================================================

Board::Board() {
    clear();
}

Board Board::starting_position() {
    Board b;
    b.set_from_fen(STARTING_FEN);
    return b;
}

void Board::clear() {
    squares_.fill(Piece());
    side_to_move_    = Color::WHITE;
    castling_rights_ = CastlingRights::NONE;
    en_passant_sq_   = NO_SQUARE;
    halfmove_clock_  = 0;
    fullmove_number_ = 1;
    zobrist_key_     = zobrist::compute_hash(*this);
}

void Board::recompute_zobrist_key() {
    zobrist_key_ = zobrist::compute_hash(*this);
}

// ============================================================
// Board — FEN Parsing
//
// FEN format: "<pieces> <side> <castling> <en_passant> <halfmove> <fullmove>"
//
// Piece placement is described rank-by-rank from rank 8 (top) down to rank 1,
// with '/' separating ranks.  Digits represent consecutive empty squares.
//
// Example (starting position):
//   rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1
// ============================================================

bool Board::set_from_fen(const std::string& fen) {
    clear();

    std::istringstream stream(fen);
    std::string piece_placement, side, castling, en_passant;
    int halfmove = 0, fullmove = 1;

    // All 6 fields are required
    if (!(stream >> piece_placement >> side >> castling >> en_passant >> halfmove >> fullmove)) {
        return false;
    }

    // --- Field 1: Piece placement ---
    // We walk the string left-to-right, which gives us rank 8 first.
    // Our internal mapping is a1=0, so rank 8 starts at index 56.
    int rank = 7;  // Start at rank 8 (index 7, zero-based)
    int file = 0;

    for (char c : piece_placement) {
        if (c == '/') {
            if (file != 8) return false;  // Each rank must have exactly 8 squares accounted for
            rank--;
            file = 0;
        } else if (std::isdigit(c)) {
            int empty_count = c - '0';
            if (empty_count < 1 || empty_count > 8) return false;
            file += empty_count;
            if (file > 8) return false;
        } else {
            Piece piece = Piece::from_char(c);
            if (piece.is_none()) return false;  // Invalid character
            if (file >= 8) return false;

            squares_[make_square(rank, file)] = piece;
            file++;
        }
    }
    if (rank != 0 || file != 8) return false;  // Must end on rank 1 with file 8

    // --- Field 2: Side to move ---
    if (side == "w") {
        side_to_move_ = Color::WHITE;
    } else if (side == "b") {
        side_to_move_ = Color::BLACK;
    } else {
        return false;
    }

    // --- Field 3: Castling availability ---
    castling_rights_ = CastlingRights::NONE;
    if (castling != "-") {
        for (char c : castling) {
            switch (c) {
                case 'K': castling_rights_ |= CastlingRights::WHITE_KINGSIDE;  break;
                case 'Q': castling_rights_ |= CastlingRights::WHITE_QUEENSIDE; break;
                case 'k': castling_rights_ |= CastlingRights::BLACK_KINGSIDE;  break;
                case 'q': castling_rights_ |= CastlingRights::BLACK_QUEENSIDE; break;
                default: return false;
            }
        }
    }

    // --- Field 4: En passant target square ---
    if (en_passant == "-") {
        en_passant_sq_ = NO_SQUARE;
    } else {
        en_passant_sq_ = algebraic_to_square(en_passant);
        if (en_passant_sq_ == NO_SQUARE) return false;
    }

    // --- Fields 5 & 6: Clocks ---
    if (halfmove < 0 || fullmove < 1) return false;
    halfmove_clock_  = halfmove;
    fullmove_number_ = fullmove;

    recompute_zobrist_key();
    return true;
}

// ============================================================
// Board — FEN Export
// ============================================================

std::string Board::to_fen() const {
    std::string fen;

    // --- Field 1: Piece placement (rank 8 down to rank 1) ---
    for (int rank = 7; rank >= 0; rank--) {
        int empty = 0;
        for (int file = 0; file < 8; file++) {
            Piece p = squares_[make_square(rank, file)];
            if (p.is_none()) {
                empty++;
            } else {
                if (empty > 0) {
                    fen += std::to_string(empty);
                    empty = 0;
                }
                fen += p.to_char();
            }
        }
        if (empty > 0) fen += std::to_string(empty);
        if (rank > 0) fen += '/';
    }

    // --- Field 2: Side to move ---
    fen += (side_to_move_ == Color::WHITE) ? " w " : " b ";

    // --- Field 3: Castling availability ---
    if (castling_rights_ == CastlingRights::NONE) {
        fen += '-';
    } else {
        if (castling_rights_ & CastlingRights::WHITE_KINGSIDE)  fen += 'K';
        if (castling_rights_ & CastlingRights::WHITE_QUEENSIDE) fen += 'Q';
        if (castling_rights_ & CastlingRights::BLACK_KINGSIDE)  fen += 'k';
        if (castling_rights_ & CastlingRights::BLACK_QUEENSIDE) fen += 'q';
    }

    // --- Field 4: En passant target square ---
    fen += ' ';
    if (en_passant_sq_ == NO_SQUARE) {
        fen += '-';
    } else {
        fen += square_to_algebraic(en_passant_sq_);
    }

    // --- Fields 5 & 6: Clocks ---
    fen += ' ' + std::to_string(halfmove_clock_);
    fen += ' ' + std::to_string(fullmove_number_);

    return fen;
}

// ============================================================
// Board — Piece access
// ============================================================

Piece Board::piece_at(Square sq) const {
    assert(sq < NUM_SQUARES);
    return squares_[sq];
}

void Board::set_piece(Square sq, Piece piece) {
    assert(sq < NUM_SQUARES);
    squares_[sq] = piece;
    recompute_zobrist_key();
}

void Board::clear_square(Square sq) {
    assert(sq < NUM_SQUARES);
    squares_[sq] = Piece();
    recompute_zobrist_key();
}

Square Board::find_king(Color color) const {
    for (int sq = 0; sq < NUM_SQUARES; sq++) {
        if (squares_[sq].type == PieceType::KING && squares_[sq].color == color) {
            return static_cast<Square>(sq);
        }
    }
    return NO_SQUARE;
}

// ============================================================
// Board — Move execution
//
// This method applies a move to the board state.  It handles all the
// special cases that make chess tricky:
//   1. Castling — move the rook alongside the king
//   2. En passant — remove the captured pawn from its actual square
//   3. Promotion — replace the pawn with the promoted piece
//   4. Double pawn push — set the en passant target square
//   5. Castling rights — revoke rights when king/rook moves or is captured
//   6. Halfmove clock — reset on pawn move or capture, increment otherwise
//   7. Fullmove counter — increment after Black moves
// ============================================================

Board::UndoInfo Board::make_move(const Move& move) {
    // Capture undo info BEFORE mutating state
    UndoInfo undo;
    undo.captured        = squares_[move.to];
    undo.castling_rights = castling_rights_;
    undo.en_passant_sq   = en_passant_sq_;
    undo.halfmove_clock  = halfmove_clock_;
    undo.zobrist_key     = zobrist_key_;

    Piece moving_piece = squares_[move.from];
    const auto& k = zobrist::get_keys();

    const int c = static_cast<int>(moving_piece.color);
    const int t = static_cast<int>(moving_piece.type);

    // ── Incremental Zobrist update: remove moving piece from origin square ──
    zobrist_key_ ^= k.pieces[c][t][move.from];

    // --- 1. Handle castling: move the rook ---
    if (move.is_kingside_castle()) {
        // King moves e1->g1 (white) or e8->g8 (black); rook moves h->f
        int rank = rank_of(move.from);
        Square rook_from = make_square(rank, 7);  // h-file
        Square rook_to   = make_square(rank, 5);  // f-file
        squares_[rook_to]   = squares_[rook_from];
        squares_[rook_from] = Piece();

        const int rook_type = static_cast<int>(PieceType::ROOK);
        zobrist_key_ ^= k.pieces[c][rook_type][rook_from]
                      ^ k.pieces[c][rook_type][rook_to];
    } else if (move.is_queenside_castle()) {
        // King moves e1->c1 (white) or e8->c8 (black); rook moves a->d
        int rank = rank_of(move.from);
        Square rook_from = make_square(rank, 0);  // a-file
        Square rook_to   = make_square(rank, 3);  // d-file
        squares_[rook_to]   = squares_[rook_from];
        squares_[rook_from] = Piece();

        const int rook_type = static_cast<int>(PieceType::ROOK);
        zobrist_key_ ^= k.pieces[c][rook_type][rook_from]
                      ^ k.pieces[c][rook_type][rook_to];
    }

    // --- 2. Handle en passant: remove the captured pawn ---
    if (move.is_en_passant()) {
        // The captured pawn is on the same file as the destination, but on the
        // rank the moving pawn came from (one rank behind the destination).
        int direction = (moving_piece.color == Color::WHITE) ? -1 : 1;
        Square captured_sq = make_square(rank_of(move.to) + direction, file_of(move.to));
        undo.captured = squares_[captured_sq];
        squares_[captured_sq] = Piece();

        const int cap_c = static_cast<int>(undo.captured.color);
        const int cap_t = static_cast<int>(undo.captured.type);
        zobrist_key_ ^= k.pieces[cap_c][cap_t][captured_sq];
    } else if (!undo.captured.is_none()) {
        // Normal capture: remove captured piece on 'to' square
        const int cap_c = static_cast<int>(undo.captured.color);
        const int cap_t = static_cast<int>(undo.captured.type);
        zobrist_key_ ^= k.pieces[cap_c][cap_t][move.to];
    }

    // --- 3. Move the piece ---
    squares_[move.to]   = moving_piece;
    squares_[move.from] = Piece();

    // --- 4. Handle promotion: replace pawn with promoted piece ---
    if (move.is_promotion()) {
        squares_[move.to] = Piece(move.promo_type, moving_piece.color);
        zobrist_key_ ^= k.pieces[c][static_cast<int>(move.promo_type)][move.to];
    } else {
        zobrist_key_ ^= k.pieces[c][t][move.to];
    }

    // --- 5. Update en passant square ---
    if (undo.en_passant_sq != NO_SQUARE) {
        zobrist_key_ ^= k.en_passant[file_of(undo.en_passant_sq)];
    }
    if (move.is_double_pawn_push()) {
        // The en passant target is the square the pawn "passed through"
        int direction = (moving_piece.color == Color::WHITE) ? -1 : 1;
        en_passant_sq_ = make_square(rank_of(move.to) + direction, file_of(move.to));
        zobrist_key_ ^= k.en_passant[file_of(en_passant_sq_)];
    } else {
        en_passant_sq_ = NO_SQUARE;
    }

    // --- 6. Update castling rights ---
    update_castling_rights(move.from, move.to);
    zobrist_key_ ^= k.castling[undo.castling_rights & CastlingRights::ALL]
                  ^ k.castling[castling_rights_ & CastlingRights::ALL];

    // --- 7. Update halfmove clock ---
    if (moving_piece.type == PieceType::PAWN || !undo.captured.is_none()) {
        halfmove_clock_ = 0;
    } else {
        halfmove_clock_++;
    }

    // --- 8. Update fullmove counter (increments after Black's move) ---
    if (side_to_move_ == Color::BLACK) {
        fullmove_number_++;
    }

    // --- 9. Toggle side to move ---
    side_to_move_ = opposite_color(side_to_move_);
    zobrist_key_ ^= k.side_to_move;

    return undo;
}

void Board::undo_move(const Move& move, const UndoInfo& undo) {
    // --- 1. Toggle side back ---
    side_to_move_ = opposite_color(side_to_move_);

    // --- 2. Decrement fullmove counter if Black just moved ---
    if (side_to_move_ == Color::BLACK) {
        fullmove_number_--;
    }

    // --- 3. Restore clocks and state ---
    castling_rights_ = undo.castling_rights;
    en_passant_sq_   = undo.en_passant_sq;
    halfmove_clock_  = undo.halfmove_clock;

    // --- 4. Figure out the moving piece ---
    Piece moving_piece = squares_[move.to];

    // If it was a promotion, the moving piece was actually a pawn
    if (move.is_promotion()) {
        moving_piece = Piece(PieceType::PAWN, side_to_move_);
    }

    // --- 5. Move the piece back ---
    squares_[move.from] = moving_piece;
    squares_[move.to]   = Piece();  // Clear 'to', will restore captured piece below

    // --- 6. Restore captured piece ---
    if (move.is_en_passant()) {
        // En passant: captured pawn goes back to its original square (not the 'to' square)
        int direction = (side_to_move_ == Color::WHITE) ? -1 : 1;
        Square captured_sq = make_square(rank_of(move.to) + direction, file_of(move.to));
        squares_[captured_sq] = undo.captured;
        // 'to' square stays empty (it was the empty en passant target)
    } else {
        // Normal move or capture: put captured piece back on 'to'
        squares_[move.to] = undo.captured;
    }

    // --- 7. Undo castling: move the rook back ---
    if (move.is_kingside_castle()) {
        int rank = rank_of(move.from);
        Square rook_from = make_square(rank, 5);  // f-file (where rook ended up)
        Square rook_to   = make_square(rank, 7);  // h-file (original position)
        squares_[rook_to]   = squares_[rook_from];
        squares_[rook_from] = Piece();
    } else if (move.is_queenside_castle()) {
        int rank = rank_of(move.from);
        Square rook_from = make_square(rank, 3);  // d-file (where rook ended up)
        Square rook_to   = make_square(rank, 0);  // a-file (original position)
        squares_[rook_to]   = squares_[rook_from];
        squares_[rook_from] = Piece();
    }

    // --- 8. Restore exact Zobrist hash ---
    zobrist_key_ = undo.zobrist_key;
}

/// Revoke castling rights when a king or rook moves, or when a rook is captured.
void Board::update_castling_rights(Square from, Square to) {
    // If the king moves, revoke both castling rights for that color
    if (from == Squares::E1) {
        castling_rights_ &= ~(CastlingRights::WHITE_KINGSIDE | CastlingRights::WHITE_QUEENSIDE);
    }
    if (from == Squares::E8) {
        castling_rights_ &= ~(CastlingRights::BLACK_KINGSIDE | CastlingRights::BLACK_QUEENSIDE);
    }

    // If a rook moves from its starting square, revoke that side's castling right
    if (from == Squares::H1 || to == Squares::H1) {
        castling_rights_ &= ~CastlingRights::WHITE_KINGSIDE;
    }
    if (from == Squares::A1 || to == Squares::A1) {
        castling_rights_ &= ~CastlingRights::WHITE_QUEENSIDE;
    }
    if (from == Squares::H8 || to == Squares::H8) {
        castling_rights_ &= ~CastlingRights::BLACK_KINGSIDE;
    }
    if (from == Squares::A8 || to == Squares::A8) {
        castling_rights_ &= ~CastlingRights::BLACK_QUEENSIDE;
    }
}

// ============================================================
// Board — Display
// ============================================================

std::string Board::to_string() const {
    std::string result;

    // Print ranks from 8 (top) to 1 (bottom) — White's perspective
    for (int rank = 7; rank >= 0; rank--) {
        result += std::to_string(rank + 1) + " |";
        for (int file = 0; file < 8; file++) {
            result += ' ';
            result += squares_[make_square(rank, file)].to_char();
            result += ' ';
        }
        result += '\n';
    }

    result += "  +------------------------\n";
    result += "    a  b  c  d  e  f  g  h\n";

    return result;
}

std::string Board::square_to_algebraic(Square sq) {
    if (sq >= NUM_SQUARES) return "??";
    std::string result;
    result += static_cast<char>('a' + file_of(sq));
    result += static_cast<char>('1' + rank_of(sq));
    return result;
}

Square Board::algebraic_to_square(const std::string& alg) {
    if (alg.length() != 2) return NO_SQUARE;

    int file = alg[0] - 'a';
    int rank = alg[1] - '1';

    if (file < 0 || file > 7 || rank < 0 || rank > 7) return NO_SQUARE;

    return make_square(rank, file);
}

} // namespace chess
