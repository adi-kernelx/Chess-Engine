/**
 * format.js — Presentation-layer formatting.
 * Pure functions, no side effects.
 */

/** Milliseconds → "MM:SS" or "MM:SS.d" when under 20s. */
export function formatClock(ms) {
    if (ms == null || !isFinite(ms)) return '--:--';
    const total = Math.max(0, ms);
    const totalSec = total / 1000;
    const mins = Math.floor(totalSec / 60);
    const secs = totalSec - mins * 60;

    if (total < 20_000) {
        return `${pad2(mins)}:${secs.toFixed(1).padStart(4, '0')}`;
    }
    return `${pad2(mins)}:${pad2(Math.floor(secs))}`;
}

/** Seconds → "10+5" style time control label. */
export function formatTimeControl(baseSec, incSec) {
    const mins = baseSec >= 60 ? baseSec / 60 : baseSec;
    const label = baseSec >= 60 ? mins.toString() : `${baseSec}s`;
    return `${label}+${incSec}`;
}

/** Backend "600+5" (seconds+seconds) → "10+5" (minutes+seconds). */
export function parseAndFormatTimeControl(str) {
    if (!str || typeof str !== 'string') return '—';
    const parts = str.split('+');
    if (parts.length !== 2) return str;
    const base = parseInt(parts[0], 10);
    const inc  = parseInt(parts[1], 10);
    if (isNaN(base) || isNaN(inc)) return str;
    return formatTimeControl(base, inc);
}

/** Human label for the game-over "reason" enum. */
export function reasonLabel(reason) {
    switch (reason) {
        case 'checkmate':             return 'Checkmate';
        case 'stalemate':             return 'Stalemate';
        case 'fifty_move_rule':       return 'Fifty-move rule';
        case 'insufficient_material': return 'Insufficient material';
        case 'threefold_repetition':  return 'Threefold repetition';
        case 'draw_agreement':        return 'Draw by agreement';
        case 'resignation':           return 'Resignation';
        case 'timeout':               return 'Timeout';
        default:                      return 'Game ended';
    }
}

/** Result string ("1-0" | "0-1" | "1/2-1/2" | "*") → phrase for a given color. */
export function resultLabel(result, forColor /* 'w'|'b'|null */) {
    if (result === '1-0') return forColor === 'w' ? 'You won' : forColor === 'b' ? 'You lost' : 'White won';
    if (result === '0-1') return forColor === 'b' ? 'You won' : forColor === 'w' ? 'You lost' : 'Black won';
    if (result === '1/2-1/2') return 'Draw';
    return 'Game over';
}

/** Category label for a time control ("10+5" or seconds). */
export function timeCategory(baseSec, incSec = 0) {
    const est = baseSec + 40 * incSec;
    if (est < 180) return 'Bullet';
    if (est < 480) return 'Blitz';
    if (est < 1500) return 'Rapid';
    return 'Classical';
}

/** ELO like 1547 → "1547" (kept for future locale hooks). */
export function formatElo(elo) {
    if (elo == null) return '—';
    return String(elo);
}

/** Initials for an avatar circle. */
export function initials(name) {
    if (!name) return '?';
    const parts = name.trim().split(/\s+/);
    if (parts.length === 1) return parts[0].slice(0, 2).toUpperCase();
    return (parts[0][0] + parts[parts.length - 1][0]).toUpperCase();
}

/** "3 minutes ago" style. */
export function relativeTime(ms) {
    const diff = Date.now() - ms;
    if (diff < 60_000)      return 'just now';
    if (diff < 3_600_000)   return `${Math.floor(diff / 60_000)}m ago`;
    if (diff < 86_400_000)  return `${Math.floor(diff / 3_600_000)}h ago`;
    if (diff < 604_800_000) return `${Math.floor(diff / 86_400_000)}d ago`;
    return new Date(ms).toLocaleDateString();
}

function pad2(n) { return String(n).padStart(2, '0'); }
