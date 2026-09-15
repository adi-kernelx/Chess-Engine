/**
 * streak.js — Client-side daily streak & goal tracking (Part G3).
 *
 * Runs entirely from localStorage until backend §7 B7 lands. Semantics:
 *
 *   - `current`  — consecutive days the user hit at least one goal.
 *   - `longest`  — best streak ever.
 *   - `lastActive` — YYYY-MM-DD of the last day a goal was hit.
 *
 * Rules:
 *   - Same day as `lastActive` → increment goal counter, `current` unchanged.
 *   - Exactly one day after `lastActive` → `current += 1`.
 *   - Any other gap → `current = 1` (fresh streak).
 *
 * A day is the user's *local* midnight — anything else is confusing.
 * Server-side auth eventually owns this; the schema will migrate cleanly.
 */

import { storage } from './storage.js';

const KEY = 'streak';

const DEFAULT_STATE = {
    current: 0,
    longest: 0,
    lastActive: null,                 // 'YYYY-MM-DD'
    goals: {
        gamesPlayed: 0, gamesTarget: 1,
        puzzlesSolved: 0, puzzlesTarget: 1,
    },
};

export function readStreak() {
    const s = storage.get(KEY) || {};
    return {
        ...DEFAULT_STATE,
        ...s,
        goals: { ...DEFAULT_STATE.goals, ...(s.goals || {}) },
    };
}

export function todayKey(now = new Date()) {
    const y = now.getFullYear();
    const m = String(now.getMonth() + 1).padStart(2, '0');
    const d = String(now.getDate()).padStart(2, '0');
    return `${y}-${m}-${d}`;
}

function daysBetween(a, b) {
    // Both are 'YYYY-MM-DD'. Compute integer day delta in the local calendar.
    const [ay, am, ad] = a.split('-').map(Number);
    const [by, bm, bd] = b.split('-').map(Number);
    const A = Date.UTC(ay, am - 1, ad);
    const B = Date.UTC(by, bm - 1, bd);
    return Math.round((B - A) / 86_400_000);
}

/**
 * Record that the user hit a goal today.
 *  @param kind  'game' | 'puzzle'
 *  @returns the updated state, plus `bumped` = true if the streak advanced.
 */
export function recordGoal(kind) {
    const s = readStreak();
    const today = todayKey();
    let bumped = false;

    if (kind === 'game')   s.goals.gamesPlayed  = (s.goals.gamesPlayed  || 0) + 1;
    if (kind === 'puzzle') s.goals.puzzlesSolved = (s.goals.puzzlesSolved || 0) + 1;

    if (s.lastActive !== today) {
        const gap = s.lastActive ? daysBetween(s.lastActive, today) : Infinity;
        s.current = gap === 1 ? (s.current + 1) : 1;
        s.longest = Math.max(s.longest || 0, s.current);
        s.lastActive = today;
        bumped = true;
    }

    storage.set(KEY, s);
    return { state: s, bumped };
}

/**
 * If the user has skipped one or more days since `lastActive`, snap the
 * current streak down to 0. Call this from any surface that displays the
 * streak, so the number is always honest.
 */
export function reconcileStreak() {
    const s = readStreak();
    if (!s.lastActive) return s;
    const gap = daysBetween(s.lastActive, todayKey());
    if (gap > 1) {
        s.current = 0;
        storage.set(KEY, s);
    }
    return s;
}
