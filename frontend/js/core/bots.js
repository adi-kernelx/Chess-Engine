/**
 * bots.js — The bot ladder (Part G3).
 *
 * Eight named opponents with faces, ELO ratings, and one-line personalities.
 * Every bot maps to one of the four server-side difficulty enums (easy /
 * medium / hard / max). The distinct strengths are still four; the *product*
 * is eight — a beginner can pick "Pawn (700)" instead of guessing what
 * "medium" means. Backend §7 B8 collapses this to a real 1:1 mapping later.
 */

export const BOTS = [
    { id: 'pawn',        name: 'Pawn',        elo:  700, difficulty: 'easy',   blurb: 'Just moves whatever looks nice. Great warmup.', accent: '#6c717a' },
    { id: 'knight',      name: 'Knight',      elo:  950, difficulty: 'easy',   blurb: 'Loves knight jumps. Forgets to defend.',       accent: '#8b6f47' },
    { id: 'bishop',      name: 'Bishop',      elo: 1200, difficulty: 'medium', blurb: 'Fianchettoes everything. Trades bishops for pawns.', accent: '#4b93e6' },
    { id: 'rook',        name: 'Rook',        elo: 1400, difficulty: 'medium', blurb: 'Loves open files. Hangs pieces in time-trouble.', accent: '#48b76a' },
    { id: 'queen',       name: 'Queen',       elo: 1650, difficulty: 'hard',   blurb: 'Aggressive. Punishes loose openings.',          accent: '#a259e6' },
    { id: 'king',        name: 'King',        elo: 1850, difficulty: 'hard',   blurb: 'Slow, positional. Rarely blunders.',            accent: '#e2a03f' },
    { id: 'grandmaster', name: 'Grandmaster', elo: 2100, difficulty: 'max',    blurb: 'Full engine strength. Bring your A-game.',      accent: '#e05555' },
    { id: 'champion',    name: 'Champion',    elo: 2350, difficulty: 'max',    blurb: 'Full strength with more thinking time.',         accent: '#f0c473' },
];

export const DEFAULT_BOT_ID = 'bishop';

export function botById(id) {
    return BOTS.find(b => b.id === id) || BOTS.find(b => b.id === DEFAULT_BOT_ID);
}
