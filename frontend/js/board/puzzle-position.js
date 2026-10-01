/**
 * Pure helpers for the offline puzzle screen.
 *
 * Legal moves are generated and verified by the C++ rules engine when the
 * bundled puzzle data is authored. The browser only groups those targets and
 * applies a chosen legal UCI move for visual feedback.
 */

import { algebraicToIndex, parseFen } from './chess.js';

export function groupLegalTargets(uciMoves = []) {
    const grouped = new Map();
    for (const move of uciMoves) {
        if (!/^[a-h][1-8][a-h][1-8][qrbn]?$/.test(move)) continue;
        const from = move.slice(0, 2);
        const to = move.slice(2, 4);
        const targets = grouped.get(from) || [];
        if (!targets.includes(to)) targets.push(to);
        grouped.set(from, targets);
    }
    return grouped;
}

export function applyUciToFen(fen, uci) {
    if (!/^[a-h][1-8][a-h][1-8][qrbn]?$/.test(uci || '')) return null;
    const parsed = parseFen(fen);
    if (!parsed) return null;

    const from = uci.slice(0, 2);
    const to = uci.slice(2, 4);
    const fromIdx = algebraicToIndex(from);
    const toIdx = algebraicToIndex(to);
    const squares = parsed.squares.map(piece => piece ? { ...piece } : null);
    const piece = squares[fromIdx];
    if (!piece || piece.color !== parsed.sideToMove) return null;

    const captured = squares[toIdx];
    const fromFile = from.charCodeAt(0) - 97;
    const toFile = to.charCodeAt(0) - 97;
    const fromRank = Number(from[1]);
    const toRank = Number(to[1]);

    // En passant: diagonal pawn move into the FEN en-passant square.
    if (piece.type === 'pawn' && fromFile !== toFile && !captured
        && parsed.epSquare === to) {
        const capturedRank = toRank + (piece.color === 'w' ? -1 : 1);
        squares[algebraicToIndex(`${to[0]}${capturedRank}`)] = null;
    }

    squares[fromIdx] = null;
    const promotion = uci[4];
    squares[toIdx] = promotion
        ? { color: piece.color, type: promotionType(promotion) }
        : piece;

    // Castle: move the rook as part of the same visual position update.
    if (piece.type === 'king' && Math.abs(toFile - fromFile) === 2) {
        const rank = from[1];
        const rookFrom = toFile > fromFile ? `h${rank}` : `a${rank}`;
        const rookTo = toFile > fromFile ? `f${rank}` : `d${rank}`;
        const rookFromIdx = algebraicToIndex(rookFrom);
        const rookToIdx = algebraicToIndex(rookTo);
        squares[rookToIdx] = squares[rookFromIdx];
        squares[rookFromIdx] = null;
    }

    let castling = parsed.castling === '-' ? '' : parsed.castling;
    if (piece.type === 'king') {
        castling = castling.replace(piece.color === 'w' ? /[KQ]/g : /[kq]/g, '');
    }
    const rookRight = { a1: 'Q', h1: 'K', a8: 'q', h8: 'k' };
    if (piece.type === 'rook' && rookRight[from]) castling = castling.replace(rookRight[from], '');
    if (captured && captured.type === 'rook' && rookRight[to]) castling = castling.replace(rookRight[to], '');

    let epSquare = '-';
    if (piece.type === 'pawn' && Math.abs(toRank - fromRank) === 2) {
        epSquare = `${from[0]}${(fromRank + toRank) / 2}`;
    }
    const halfmove = piece.type === 'pawn' || captured ? 0 : parsed.halfmove + 1;
    const fullmove = parsed.fullmove + (parsed.sideToMove === 'b' ? 1 : 0);
    const nextSide = parsed.sideToMove === 'w' ? 'b' : 'w';
    return `${serializePlacement(squares)} ${nextSide} ${castling || '-'} ${epSquare} ${halfmove} ${fullmove}`;
}

function serializePlacement(squares) {
    const pieceChar = {
        pawn: 'p', knight: 'n', bishop: 'b', rook: 'r', queen: 'q', king: 'k',
    };
    const ranks = [];
    for (let rank = 7; rank >= 0; rank--) {
        let row = '';
        let empty = 0;
        for (let file = 0; file < 8; file++) {
            const piece = squares[rank * 8 + file];
            if (!piece) {
                empty++;
                continue;
            }
            if (empty) { row += String(empty); empty = 0; }
            const ch = pieceChar[piece.type];
            row += piece.color === 'w' ? ch.toUpperCase() : ch;
        }
        if (empty) row += String(empty);
        ranks.push(row);
    }
    return ranks.join('/');
}

function promotionType(ch) {
    return ({ q: 'queen', r: 'rook', b: 'bishop', n: 'knight' })[ch] || 'queen';
}
