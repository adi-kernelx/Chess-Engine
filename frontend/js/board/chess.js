/**
 * chess.js — Lightweight client-side helpers.
 *
 * Deliberately NOT a move generator. Just what the UI needs and the
 * server does not send: FEN parsing, king location, captured-piece
 * derivation, material balance.
 *
 * The server remains authoritative. Nothing here validates a move.
 */

/** Standard starting position, for defaults and material diffs. */
export const START_FEN = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';

/**
 * Parse a FEN. Returns:
 *   {
 *     squares: Array(64) of { type, color } | null   — indexed a1=0 … h8=63
 *     sideToMove: 'w' | 'b'
 *     castling: string      — raw, e.g. 'KQkq' or '-'
 *     epSquare: string|null — 'e3' or null
 *     halfmove: number
 *     fullmove: number
 *   }
 */
export function parseFen(fen) {
    if (!fen || typeof fen !== 'string') return null;
    const parts = fen.trim().split(/\s+/);
    if (parts.length < 4) return null;
    const [placement, side, castling, ep, half = '0', full = '1'] = parts;

    const squares = new Array(64).fill(null);
    const rows = placement.split('/');
    if (rows.length !== 8) return null;

    for (let r = 0; r < 8; r++) {
        const rankIdx = 7 - r;
        let fileIdx = 0;
        for (const ch of rows[r]) {
            if (fileIdx > 7) break;
            const n = parseInt(ch, 10);
            if (!isNaN(n)) {
                fileIdx += n;
            } else {
                const color = ch === ch.toUpperCase() ? 'w' : 'b';
                const type = pieceTypeFromChar(ch);
                if (type) squares[rankIdx * 8 + fileIdx] = { type, color };
                fileIdx++;
            }
        }
    }

    return {
        squares,
        sideToMove: side === 'b' ? 'b' : 'w',
        castling: castling || '-',
        epSquare: ep && ep !== '-' ? ep : null,
        halfmove: parseInt(half, 10) || 0,
        fullmove: parseInt(full, 10) || 1,
    };
}

/** Convert a rank-major index (a1=0…h8=63) to algebraic string. */
export function indexToAlgebraic(idx) {
    if (idx < 0 || idx > 63) return null;
    const file = idx % 8;
    const rank = Math.floor(idx / 8);
    return String.fromCharCode(97 + file) + String(rank + 1);
}

/** Convert an algebraic string ('e4') to rank-major index (0…63). */
export function algebraicToIndex(sq) {
    if (!sq || sq.length !== 2) return -1;
    const file = sq.charCodeAt(0) - 97;
    const rank = parseInt(sq[1], 10) - 1;
    if (file < 0 || file > 7 || rank < 0 || rank > 7) return -1;
    return rank * 8 + file;
}

/** Locate a king (or return null if the position has no king of that color). */
export function findKingSquare(parsedOrFen, color /* 'w'|'b' */) {
    const parsed = typeof parsedOrFen === 'string' ? parseFen(parsedOrFen) : parsedOrFen;
    if (!parsed) return null;
    for (let i = 0; i < 64; i++) {
        const p = parsed.squares[i];
        if (p && p.type === 'king' && p.color === color) return indexToAlgebraic(i);
    }
    return null;
}

/** Standard piece point values, used for material balance. */
export const PIECE_VALUE = {
    pawn: 1, knight: 3, bishop: 3, rook: 5, queen: 9, king: 0,
};

/**
 * Count material for each color from a parsed position.
 * Returns { w: {pawn, knight, …}, b: {…} }
 */
export function pieceCounts(parsed) {
    const empty = () => ({ pawn: 0, knight: 0, bishop: 0, rook: 0, queen: 0, king: 0 });
    const out = { w: empty(), b: empty() };
    if (!parsed) return out;
    for (const p of parsed.squares) {
        if (p) out[p.color][p.type]++;
    }
    return out;
}

/**
 * Derive captured pieces (what the opponent has taken from you).
 * Compares against a full initial complement.
 * Returns { w: [type, …], b: [type, …] } — each array is the pieces
 * captured FROM that color (missing from the board), sorted by value asc.
 */
export function capturedPieces(parsed) {
    const INITIAL = { pawn: 8, knight: 2, bishop: 2, rook: 2, queen: 1, king: 1 };
    const counts = pieceCounts(parsed);
    const diff = (color) => {
        const arr = [];
        for (const t of Object.keys(INITIAL)) {
            const missing = Math.max(0, INITIAL[t] - counts[color][t]);
            for (let i = 0; i < missing; i++) arr.push(t);
        }
        arr.sort((a, b) => PIECE_VALUE[a] - PIECE_VALUE[b]);
        return arr;
    };
    return { w: diff('w'), b: diff('b') };
}

/** Net material advantage in points, positive = white ahead. */
export function materialBalance(parsed) {
    const c = pieceCounts(parsed);
    const sum = (color) => Object.keys(PIECE_VALUE).reduce(
        (acc, t) => acc + PIECE_VALUE[t] * c[color][t], 0);
    return sum('w') - sum('b');
}

/** Is a promoting move? (Pawn arriving on rank 8 for white, rank 1 for black.) */
export function isPromotion(fromSq, toSq, parsed) {
    const idx = algebraicToIndex(fromSq);
    if (idx < 0) return false;
    const p = parsed && parsed.squares[idx];
    if (!p || p.type !== 'pawn') return false;
    const toRank = toSq[1];
    return (p.color === 'w' && toRank === '8') || (p.color === 'b' && toRank === '1');
}

function pieceTypeFromChar(ch) {
    switch (ch.toLowerCase()) {
        case 'p': return 'pawn';
        case 'n': return 'knight';
        case 'b': return 'bishop';
        case 'r': return 'rook';
        case 'q': return 'queen';
        case 'k': return 'king';
        default:  return null;
    }
}
