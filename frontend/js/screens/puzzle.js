/**
 * puzzle.js — Daily tactic screen (Part G3).
 *
 * Route: #/puzzle
 *
 * Runs entirely on the frontend using the bundled `assets/puzzles.json`
 * (~5 hand-verified positions to start; grows via backend §7 B6).
 *
 * Design:
 *   - The puzzle set is fetched only when this screen mounts. It never
 *     appears in the network trace for any other route.
 *   - "Today's puzzle" is picked by a date-seeded index into the array,
 *     so every visitor sees the same one until midnight local.
 *   - The user is always the side-to-move; the puzzle simply expects the
 *     first UCI move from `solution[]`. Multi-move puzzles play the
 *     opponent's reply from the same array and continue.
 */

import { Screen } from '../ui/screen.js';
import { h, clear, icon } from '../core/dom.js';
import { BoardRenderer } from '../board/renderer.js';
import { BoardInteraction } from '../board/interaction.js';
import { BoardTheme, THEMES } from '../board/theme.js';
import { parseFen, START_FEN } from '../board/chess.js';
import { readStreak, recordGoal, todayKey, reconcileStreak } from '../core/streak.js';
import { copyText, shareUrl, absoluteUrl } from '../core/share.js';
import { storage } from '../core/storage.js';
import { applyUciToFen, groupLegalTargets } from '../board/puzzle-position.js';

const SOLVED_KEY = 'puzzle:solved';   // { 'p001': 'YYYY-MM-DD', ... }

export class PuzzleScreen extends Screen {
    constructor(ctx) {
        super(ctx);
        this._puzzle = null;
        this._userSideToMove = 'w';
        this._solutionIdx = 0;
        this._status = 'loading';       // loading | playing | solved | wrong | error
        this._positionFen = START_FEN;
        this._legalMoves = [];
        this._legalTargets = new Map();
        this._resizeObs = null;
    }

    render() {
        return h('div', { class: 'screen' },
            this.header(
                'Daily puzzle',
                'One position a day. Solve it, keep your streak.',
                h('a', { class: 'btn btn--ghost btn--sm', href: '#/play' }, 'Back to play'),
            ),
            h('div', { class: 'screen__body puzzle-layout' },
                h('div', { class: 'puzzle-board' },
                    h('div', { class: 'board-frame' },
                        h('div', { class: 'board-container', ref: el => this._container = el },
                            h('canvas', {
                                class: 'board-canvas',
                                ref: el => this._canvas = el,
                                role: 'img',
                                'aria-label': 'Puzzle board',
                                tabindex: '0',
                            })
                        )
                    ),
                    h('div', { class: 'puzzle-prompt', ref: el => this._promptEl = el }, 'Loading today’s puzzle…'),
                ),
                h('aside', { class: 'puzzle-side' },
                    h('div', { class: 'card' },
                        h('div', { class: 'card__header' },
                            icon('trophy', 'icon--sm'),
                            h('div', { class: 'card__title' }, 'Today'),
                        ),
                        h('div', { class: 'card__body card__body--stack', ref: el => this._infoEl = el }),
                    ),
                    h('div', { class: 'card' },
                        h('div', { class: 'card__header' },
                            icon('chart', 'icon--sm'),
                            h('div', { class: 'card__title' }, 'Your streak'),
                        ),
                        h('div', { class: 'card__body', ref: el => this._streakEl = el }),
                    ),
                    h('div', { class: 'puzzle-actions', ref: el => this._actionsEl = el }),
                )
            )
        );
    }

    async onMount() {
        // Renderer + interaction. Same renderer the game screen uses.
        const themeName = this.ctx.store.prefs.boardTheme || 'classic';
        const theme = new BoardTheme(THEMES[themeName] || THEMES.classic);
        this.renderer = new BoardRenderer(this._canvas, theme);
        this.interaction = new BoardInteraction(this._canvas, this.renderer);
        this.renderer.pieceSetUrl = `assets/pieces/${this.ctx.store.prefs.pieceSet || 'classic'}.svg`;
        this.interaction.onMoveIntent = (from, to) => this._onMove(from, to);
        this.interaction.legalTargetsProvider = from => this._legalTargets.get(from) || [];

        this._resizeObs = new ResizeObserver(entries => {
            for (const e of entries) {
                const size = Math.min(e.contentRect.width, e.contentRect.height);
                if (size > 0) {
                    // Chain the current position so it re-rasterizes into the
                    // new size — mirrors game.js's resize flow.
                    this.renderer.resize(size).then(() => {
                        if (this._puzzle) this.renderer.setPosition(this._positionFen);
                    });
                }
            }
        });
        this._resizeObs.observe(this._container);

        this._renderStreak();

        try {
            const res = await fetch('assets/puzzles.json', { cache: 'no-cache' });
            const data = await res.json();
            const list = data.puzzles || [];
            if (!list.length) throw new Error('empty puzzle set');
            this._puzzle = list[dailyIndex(list.length)];
            this._loadPuzzle();
        } catch (e) {
            this._promptEl.textContent = 'Could not load the puzzle set.';
            this._status = 'error';
            this._renderActions();
            console.warn('[puzzle] load failed:', e);
        }
    }

    onUnmount() {
        if (this._resizeObs) this._resizeObs.disconnect();
    }

    _loadPuzzle() {
        const p = this._puzzle;
        this._solutionIdx = 0;
        this._status = 'playing';
        this._positionFen = p.fen;
        this._setLegalMoves(p.legal_moves || []);
        const parsed = parseFen(p.fen);
        this._userSideToMove = parsed ? parsed.sideToMove : 'w';

        this.renderer.setOrientation(this._userSideToMove === 'w');
        this.interaction.setPlayerColor(this._userSideToMove);
        this.interaction.setEnabled(true);
        // The ResizeObserver's chained resize().then(setPosition) will paint
        // this correctly once the container is measured. For hot-loads where
        // the observer already ran, set immediately so we don't wait.
        const rect = this._container.getBoundingClientRect();
        const size = Math.min(rect.width, rect.height);
        if (size > 0) {
            this.renderer.resize(size).then(() => this.renderer.setPosition(this._positionFen));
        }

        this._promptEl.textContent = p.prompt || 'Your move.';
        this._renderInfo();
        this._renderActions();
    }

    _renderInfo() {
        if (!this._infoEl || !this._puzzle) return;
        clear(this._infoEl);
        const p = this._puzzle;
        this._infoEl.appendChild(h('div', { class: 'kv' },
            h('span', { class: 'kv__label' }, 'Theme'),
            h('span', { class: 'kv__val' }, p.theme || 'tactic'),
        ));
        this._infoEl.appendChild(h('div', { class: 'kv' },
            h('span', { class: 'kv__label' }, 'Rating'),
            h('span', { class: 'kv__val mono' }, String(p.rating || '—')),
        ));
        this._infoEl.appendChild(h('div', { class: 'kv' },
            h('span', { class: 'kv__label' }, 'Date'),
            h('span', { class: 'kv__val mono' }, todayKey()),
        ));
    }

    _renderStreak() {
        const s = reconcileStreak();
        clear(this._streakEl);
        this._streakEl.appendChild(h('div', { class: 'streak-num' },
            h('span', { class: 'streak-num__val' }, String(s.current)),
            h('span', { class: 'streak-num__unit' }, s.current === 1 ? 'day' : 'days'),
        ));
        this._streakEl.appendChild(h('div', { class: 'streak-sub' },
            s.longest > 0
                ? `Longest: ${s.longest} day${s.longest === 1 ? '' : 's'}`
                : 'Solve today to start your streak.'
        ));
    }

    _renderActions() {
        if (!this._actionsEl) return;
        clear(this._actionsEl);

        if (this._status === 'solved') {
            this._actionsEl.appendChild(h('button', {
                class: 'btn btn--sm',
                onclick: () => this._share(),
            }, 'Share result'));
        }
        if (this._status === 'wrong' || this._status === 'solved') {
            this._actionsEl.appendChild(h('button', {
                class: 'btn btn--sm btn--ghost',
                onclick: () => this._loadPuzzle(),
            }, 'Retry'));
        }
        if (this._status === 'error') {
            this._actionsEl.appendChild(h('button', {
                class: 'btn btn--sm btn--ghost',
                onclick: () => window.location.reload(),
            }, 'Reload puzzle'));
        }
    }

    _onMove(from, to) {
        if (this._status !== 'playing') return;
        const expected = this._puzzle.solution[this._solutionIdx];
        const uci = (from + to).toLowerCase();
        const legal = this._legalMoves.find(move => move.slice(0, 4) === uci);
        if (!legal) {
            this._promptEl.textContent = 'That move is not legal in this position. Try another move.';
            this.ctx.sound.illegal && this.ctx.sound.illegal();
            return;
        }

        const nextFen = applyUciToFen(this._positionFen, legal);
        if (!nextFen) {
            this._promptEl.textContent = 'Could not apply that move. Retry the puzzle.';
            this._onWrong();
            return;
        }
        this._positionFen = nextFen;
        this._setLegalMoves([]);
        this.renderer.setLastMove(from, to);
        // animateMove expects the destination position to be applied first.
        // This keeps both correct and legal-but-wrong moves visible.
        this.renderer.setPosition(nextFen);
        this.interaction.setEnabled(false);
        const isSolution = legal.slice(0, 4) === expected.slice(0, 4);
        this.renderer.animateMove(from, to, () => {
            if (isSolution) this._afterUserMove();
            else this._onWrong();
        });
    }

    _afterUserMove() {
        this._solutionIdx++;
        // Multi-move puzzles: play the opponent's reply automatically.
        if (this._solutionIdx < this._puzzle.solution.length) {
            const reply = this._puzzle.solution[this._solutionIdx];
            const from = reply.slice(0, 2);
            const to   = reply.slice(2, 4);
            this._solutionIdx++;
            setTimeout(() => {
                const nextFen = applyUciToFen(this._positionFen, reply);
                if (!nextFen) {
                    this._promptEl.textContent = 'Puzzle data is invalid. Please reload.';
                    this._status = 'error';
                    this._renderActions();
                    return;
                }
                this._positionFen = nextFen;
                this.renderer.setLastMove(from, to);
                this.renderer.setPosition(nextFen);
                this.renderer.animateMove(from, to, () => {
                    this._setLegalMoves(this._legalMovesForStep());
                    this.interaction.setEnabled(true);
                    this._promptEl.textContent = this._puzzle.prompt || 'Find the next move.';
                });
            }, 320);
            return;
        }
        this._onSolved();
    }

    _onSolved() {
        this._status = 'solved';
        this.interaction.setEnabled(false);
        this._promptEl.textContent = 'Solved. Nice work.';
        this.ctx.sound.gameEnd();
        this._markSolved();
        const { bumped, state } = recordGoal('puzzle');
        this._renderStreak();
        this._renderActions();
        if (bumped) {
            this.ctx.toast.success(
                state.current === 1
                    ? 'Streak started — see you tomorrow.'
                    : `Streak: ${state.current} days in a row.`,
                { duration: 3000 }
            );
        } else {
            this.ctx.toast.success('Already logged for today.', { duration: 2200 });
        }
    }

    _onWrong() {
        this._status = 'wrong';
        this.interaction.setEnabled(false);
        this._promptEl.textContent = 'That move is legal, but it is not the puzzle solution. Retry when ready.';
        this.ctx.sound.check && this.ctx.sound.check();   // fall back gracefully
        this._renderActions();
        // Shake the board a touch.
        if (this._container) {
            this._container.classList.add('is-wrong');
            setTimeout(() => this._container.classList.remove('is-wrong'), 500);
        }
    }

    _setLegalMoves(moves) {
        this._legalMoves = Array.isArray(moves) ? moves : [];
        this._legalTargets = groupLegalTargets(this._legalMoves);
        if (this.renderer) this.renderer.clearSelected();
    }

    _legalMovesForStep() {
        const byStep = this._puzzle && this._puzzle.legal_moves_by_step;
        if (Array.isArray(byStep)) {
            return byStep[Math.floor(this._solutionIdx / 2)] || [];
        }
        return this._solutionIdx === 0 ? (this._puzzle.legal_moves || []) : [];
    }

    _markSolved() {
        const solved = storage.get(SOLVED_KEY) || {};
        solved[this._puzzle.id] = todayKey();
        storage.set(SOLVED_KEY, solved);
    }

    async _share() {
        const url = absoluteUrl('#/puzzle');
        const text = `Solved today's chess puzzle (${this._puzzle.theme}).`;
        const shared = await shareUrl({ title: 'Chess puzzle', text, url });
        if (shared.ok) return;
        const res = await copyText(`${text} ${url}`);
        if (res.ok) this.ctx.toast.success('Copied to clipboard.', { duration: 1800 });
    }
}

/** Same puzzle for every visitor for the whole local day, rotates each day. */
function dailyIndex(len) {
    const [y, m, d] = todayKey().split('-').map(Number);
    // Cheap deterministic hash — good enough for daily rotation over a small set.
    return ((y * 100 + m) * 100 + d) % len;
}
