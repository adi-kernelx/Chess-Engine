/**
 * notation.cpp — SAN and PGN implementation
 *
 * SAN (Standard Algebraic Notation) is the human-readable format:
 *   "e4", "Nf3", "Bxe5", "O-O", "Qd1+", "e8=Q#"
 *
 * SAN generation requires the board state to:
 *   1. Determine piece type for the prefix letter
 *   2. Disambiguate when multiple pieces can reach the same square
 *   3. Detect captures (an 'x' is inserted)
 *   4. Detect check (+) and checkmate (#) after the move
 *
 * PGN wraps SAN moves with header tags and move numbers.
 */

#include "chess/notation.h"
#include <sstream>
#include <cctype>
#include <algorithm>

namespace chess {
namespace notation {

// ============================================================
// SAN Generation: Move -> "Nf3", "exd5", "O-O", etc.
// ============================================================

static char piece_to_san_char(PieceType pt) {
    switch (pt) {
        case PieceType::KNIGHT: return 'N';
        case PieceType::BISHOP: return 'B';
        case PieceType::ROOK:   return 'R';
        case PieceType::QUEEN:  return 'Q';
        case PieceType::KING:   return 'K';
        default:                return '\0';  // Pawns have no prefix
    }
}

std::string move_to_san(const Board& board, const Move& move) {
    // --- Castling ---
    if (move.is_kingside_castle())  return "O-O";
    if (move.is_queenside_castle()) return "O-O-O";

    std::string san;
    Piece moving = board.piece_at(move.from);
    bool is_capture = !board.piece_at(move.to).is_none() || move.is_en_passant();

    // --- Piece prefix (non-pawns get a letter) ---
    if (moving.type != PieceType::PAWN) {
        san += piece_to_san_char(moving.type);

        // --- Disambiguation ---
        // If multiple pieces of the same type can reach the same square,
        // we need to add file, rank, or both to distinguish them.
        std::vector<Move> legal_moves = move_gen::generate_legal_moves(board);
        bool need_file = false;
        bool need_rank = false;

        for (const Move& other : legal_moves) {
            if (other.from == move.from) continue;           // Same piece
            if (other.to != move.to) continue;               // Different target
            if (board.piece_at(other.from).type != moving.type) continue;  // Different piece type

            // Another piece of the same type can reach the same square
            if (file_of(other.from) == file_of(move.from)) {
                need_rank = true;  // Same file — disambiguate by rank
            } else {
                need_file = true;  // Different file — disambiguate by file
            }
        }

        // If both file and rank are needed (very rare, e.g., 3 rooks via promotion)
        if (need_file) san += static_cast<char>('a' + file_of(move.from));
        if (need_rank) san += static_cast<char>('1' + rank_of(move.from));
    } else if (is_capture) {
        // Pawn captures include the source file: "exd5"
        san += static_cast<char>('a' + file_of(move.from));
    }

    // --- Capture indicator ---
    if (is_capture) san += 'x';

    // --- Destination square ---
    san += Board::square_to_algebraic(move.to);

    // --- Promotion ---
    if (move.is_promotion()) {
        san += '=';
        san += piece_to_san_char(move.promo_type);
    }

    // --- Check / Checkmate indicator ---
    // Apply the move to a copy and check the result
    Board after = board;
    after.make_move(move);
    Color opponent = board.side_to_move() == Color::WHITE ? Color::BLACK : Color::WHITE;
    if (move_gen::is_in_check(after, opponent)) {
        // Is it checkmate?
        std::vector<Move> opponent_moves = move_gen::generate_legal_moves(after);
        san += opponent_moves.empty() ? '#' : '+';
    }

    return san;
}

// ============================================================
// SAN Parsing: "Nf3", "exd5", "O-O" -> Move
// ============================================================

Move san_to_move(const Board& board, const std::string& san) {
    if (san.empty()) return Move();

    // --- Castling ---
    if (san == "O-O" || san == "O-O+" || san == "O-O#") {
        Square king_sq = board.find_king(board.side_to_move());
        int rank = rank_of(king_sq);
        return Move(king_sq, make_square(rank, 6), MoveFlags::KINGSIDE_CASTLE);
    }
    if (san == "O-O-O" || san == "O-O-O+" || san == "O-O-O#") {
        Square king_sq = board.find_king(board.side_to_move());
        int rank = rank_of(king_sq);
        return Move(king_sq, make_square(rank, 2), MoveFlags::QUEENSIDE_CASTLE);
    }

    // Strip check/checkmate indicators from the end
    std::string s = san;
    while (!s.empty() && (s.back() == '+' || s.back() == '#' || s.back() == '!' || s.back() == '?')) {
        s.pop_back();
    }
    if (s.empty()) return Move();

    // Parse from left to right
    size_t pos = 0;

    // --- Piece type (if uppercase letter, it's a piece; otherwise it's a pawn) ---
    PieceType piece_type = PieceType::PAWN;
    if (pos < s.size() && std::isupper(s[pos]) && s[pos] != 'O') {
        switch (s[pos]) {
            case 'N': piece_type = PieceType::KNIGHT; break;
            case 'B': piece_type = PieceType::BISHOP; break;
            case 'R': piece_type = PieceType::ROOK;   break;
            case 'Q': piece_type = PieceType::QUEEN;  break;
            case 'K': piece_type = PieceType::KING;   break;
            default:  return Move();
        }
        pos++;
    }

    // --- Disambiguation and destination ---
    // Remaining characters could be: [file][rank][x]file rank [=piece]
    // We need to find the destination square, which is always the last
    // file-rank pair before an optional promotion suffix.

    // Strip promotion suffix first
    PieceType promo_type = PieceType::NONE;
    if (s.size() >= 2 && s[s.size() - 2] == '=') {
        switch (s.back()) {
            case 'Q': promo_type = PieceType::QUEEN;  break;
            case 'R': promo_type = PieceType::ROOK;   break;
            case 'B': promo_type = PieceType::BISHOP; break;
            case 'N': promo_type = PieceType::KNIGHT; break;
            default:  return Move();
        }
        s = s.substr(0, s.size() - 2);
    }

    // The destination is the last two characters (file + rank)
    if (s.size() < pos + 2) return Move();
    int dest_file = s[s.size() - 2] - 'a';
    int dest_rank = s[s.size() - 1] - '1';
    if (dest_file < 0 || dest_file > 7 || dest_rank < 0 || dest_rank > 7) return Move();
    Square to_sq = make_square(dest_rank, dest_file);

    // Everything between pos and (end - 2) is disambiguation + capture marker
    std::string middle = s.substr(pos, s.size() - 2 - pos);

    // Remove 'x' (capture indicator)
    middle.erase(std::remove(middle.begin(), middle.end(), 'x'), middle.end());

    // Parse disambiguation: could be file, rank, or both
    int disambig_file = -1;
    int disambig_rank = -1;
    for (char c : middle) {
        if (c >= 'a' && c <= 'h') disambig_file = c - 'a';
        else if (c >= '1' && c <= '8') disambig_rank = c - '1';
    }

    // --- Match against legal moves ---
    std::vector<Move> legal_moves = move_gen::generate_legal_moves(board);

    for (const Move& candidate : legal_moves) {
        if (candidate.to != to_sq) continue;

        Piece p = board.piece_at(candidate.from);
        if (p.type != piece_type) continue;

        // Check disambiguation
        if (disambig_file >= 0 && file_of(candidate.from) != disambig_file) continue;
        if (disambig_rank >= 0 && rank_of(candidate.from) != disambig_rank) continue;

        // Check promotion
        if (promo_type != PieceType::NONE) {
            if (!candidate.is_promotion() || candidate.promo_type != promo_type) continue;
        } else {
            if (candidate.is_promotion()) continue;  // SAN didn't specify promotion but move is one
        }

        return candidate;
    }

    // If it's a pawn promotion and we didn't find an exact match,
    // try matching without the promotion filter (for PGNs that use
    // shorthand like "e8Q" without "=")
    if (promo_type == PieceType::NONE && piece_type == PieceType::PAWN) {
        // Check if the destination is the promotion rank
        int promo_rank = (board.side_to_move() == Color::WHITE) ? 7 : 0;
        if (dest_rank == promo_rank) {
            // Default to queen promotion
            for (const Move& candidate : legal_moves) {
                if (candidate.to != to_sq) continue;
                Piece p = board.piece_at(candidate.from);
                if (p.type != PieceType::PAWN) continue;
                if (disambig_file >= 0 && file_of(candidate.from) != disambig_file) continue;
                if (candidate.is_promotion() && candidate.promo_type == PieceType::QUEEN) {
                    return candidate;
                }
            }
        }
    }

    return Move();  // No match found
}

// ============================================================
// PGN Export
// ============================================================

std::string export_pgn(const std::vector<PgnTag>& tags,
                       const std::vector<Move>& moves,
                       const std::string& result) {
    std::string pgn;

    // --- Header tags ---
    for (const auto& tag : tags) {
        pgn += "[" + tag.name + " \"" + tag.value + "\"]\n";
    }
    if (!tags.empty()) pgn += '\n';

    // --- Move text ---
    Board board = Board::starting_position();
    for (size_t i = 0; i < moves.size(); ++i) {
        // Move number (before White's move)
        if (i % 2 == 0) {
            pgn += std::to_string(i / 2 + 1) + ". ";
        }

        // SAN notation
        pgn += move_to_san(board, moves[i]);
        pgn += ' ';

        // Apply the move
        board.make_move(moves[i]);
    }

    // --- Result ---
    pgn += result;
    pgn += '\n';

    return pgn;
}

// ============================================================
// PGN Import
// ============================================================

/// Helper: trim whitespace from both ends of a string
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

PgnGame import_pgn(const std::string& pgn) {
    PgnGame game;
    std::istringstream stream(pgn);
    std::string line;

    // --- Phase 1: Parse tag pairs ---
    // Tags look like: [Event "F/S Return Match"]
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty()) continue;  // Skip blank lines
        if (line[0] != '[') break;   // End of tags

        // Parse [Name "Value"]
        size_t name_start = 1;
        size_t name_end = line.find(' ', name_start);
        if (name_end == std::string::npos) continue;

        size_t val_start = line.find('"', name_end);
        size_t val_end = line.rfind('"');
        if (val_start == std::string::npos || val_end == std::string::npos || val_start == val_end) continue;

        game.tags.push_back({
            line.substr(name_start, name_end - name_start),
            line.substr(val_start + 1, val_end - val_start - 1)
        });
    }

    // --- Check for FEN tag (non-standard starting position) ---
    Board board = Board::starting_position();
    for (const auto& tag : game.tags) {
        if (tag.name == "FEN") {
            board.set_from_fen(tag.value);
            break;
        }
    }

    // --- Phase 2: Parse move text ---
    // Collect all remaining text into a single string
    std::string movetext = line;  // The line that broke out of the tag loop
    while (std::getline(stream, line)) {
        movetext += " " + line;
    }

    // Tokenize the movetext
    std::istringstream move_stream(movetext);
    std::string token;

    while (move_stream >> token) {
        // Skip move numbers ("1.", "1...", "23.")
        if (!token.empty() && std::isdigit(token[0])) {
            // It's a move number if it ends with '.' or is all digits/dots
            bool is_move_number = false;
            for (char c : token) {
                if (c == '.') { is_move_number = true; break; }
            }
            if (is_move_number) continue;
        }

        // Check for result
        if (token == "1-0" || token == "0-1" || token == "1/2-1/2" || token == "*") {
            game.result = token;
            break;
        }

        // Skip comments in braces {like this}
        if (token[0] == '{') {
            // Read until closing brace
            if (token.back() != '}') {
                while (move_stream >> token) {
                    if (token.back() == '}') break;
                }
            }
            continue;
        }

        // Skip NAGs (Numeric Annotation Glyphs: $1, $2, etc.)
        if (token[0] == '$') continue;

        // Skip variations in parentheses (like this)
        if (token[0] == '(') {
            int depth = 1;
            for (char c : token) {
                if (c == '(') depth++;
                if (c == ')') depth--;
            }
            depth--;  // We counted the first '(' twice
            while (depth > 0 && move_stream >> token) {
                for (char c : token) {
                    if (c == '(') depth++;
                    if (c == ')') depth--;
                }
            }
            continue;
        }

        // Parse the SAN token into a Move
        Move move = san_to_move(board, token);
        if (move.from == 0 && move.to == 0 && move.flags == 0) {
            // Parse failure — stop here
            break;
        }

        game.moves.push_back(move);
        board.make_move(move);
    }

    return game;
}

} // namespace notation
} // namespace chess
