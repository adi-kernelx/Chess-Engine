/**
 * spectate.js — PREVIEW screens (Phase 9).
 *
 * Two entry points registered in main.js:
 *   /spectate           → SpectateListScreen  (live games table)
 *   /spectate/:gameId   → SpectateWatchScreen (board + move stream)
 *
 * Expected backend contract:
 *   → { type: 'list_live_games', limit?: 50 }
 *   ← { type: 'live_games', games: [{
 *         game_id, white, black, white_elo, black_elo,
 *         time_control, move_count, spectators
 *     }] }
 *
 *   → { type: 'spectate', game_id }
 *   ← { type: 'spectate_snapshot',
 *         game_id, white, black, white_elo, black_elo,
 *         fen, white_time, black_time,
 *         moves: [{ san, think_ms }],
 *         spectators
 *     }
 *   ← push { type: 'spectate_move', game_id, from, to, san, fen, white_time, black_time }
 *   ← push { type: 'spectate_end',  game_id, result, reason }
 *
 *   → { type: 'stop_spectating', game_id }
 */

import { Screen } from '../ui/screen.js';
import { h, clear, icon } from '../core/dom.js';
import { BoardRenderer } from '../board/renderer.js';
import { BoardTheme, THEMES } from '../board/theme.js';
import { parseFen, findKingSquare, capturedPieces, materialBalance, START_FEN } from '../board/chess.js';
import { formatClock, parseAndFormatTimeControl, initials, formatElo } from '../core/format.js';

/* ─── List ─────────────────────────────────────────────── */

export class SpectateListScreen extends Screen {
    constructor(ctx) {
        super(ctx);
        this.preview = true;
    }

    render() {
        return h('div', { class: 'screen' },
            this.header('Spectate', 'Watch live games as they happen.'),
            h('div', { class: 'screen__body' },
                h('div', { class: 'card' },
                    h('div', { class: 'card__header' },
                        icon('eye', 'icon--sm'),
                        h('div', { class: 'card__title' }, 'Live games'),
                        h('span', {
                            style: { marginLeft: 'auto', fontSize: 'var(--fs-xs)', color: 'var(--text-muted)' },
                            ref: el => this._count = el,
                        }, '')
                    ),
                    h('div', { style: { padding: 0 }, ref: el => this._body = el },
                        h('div', { class: 'empty' }, 'Loading live games…')
                    )
                )
            )
        );
    }

    async onMount() {
        const { data, live } = await this.ctx.capability.request(
            this.ctx.Outbound.listLiveGames(),
            { expect: 'live_games', timeout: 1500, demo: () => this._demo() },
        );
        void live; // preview badge is already on the base class
        this._render(data.games || []);
    }

    _render(games) {
        clear(this._body);
        this._count.textContent = `${games.length} live`;
        if (games.length === 0) {
            this._body.appendChild(h('div', { class: 'empty' }, 'No live games right now.'));
            return;
        }
        this._body.appendChild(
            h('table', { class: 'table' },
                h('thead', {}, h('tr', {},
                    h('th', {}, 'White'),
                    h('th', {}, 'Black'),
                    h('th', {}, 'Time'),
                    h('th', {}, 'Moves'),
                    h('th', {}, 'Watchers'),
                    h('th', { style: { textAlign: 'right' } }, ''),
                )),
                h('tbody', {},
                    ...games.map(g => h('tr', {},
                        h('td', {}, this._playerCell(g.white, g.white_elo)),
                        h('td', {}, this._playerCell(g.black, g.black_elo)),
                        h('td', { class: 'mono' }, parseAndFormatTimeControl(g.time_control)),
                        h('td', { class: 'mono' }, String(g.move_count || 0)),
                        h('td', { class: 'mono', style: { color: 'var(--text-muted)' } }, String(g.spectators || 0)),
                        h('td', { style: { textAlign: 'right' } },
                            h('button', {
                                class: 'btn btn--sm btn--primary',
                                onclick: () => this.ctx.router.go('/spectate/' + g.game_id),
                            }, 'Watch')
                        )
                    ))
                )
            )
        );
    }

    _playerCell(name, elo) {
        return h('div', { style: { display: 'flex', alignItems: 'center', gap: '10px' } },
            h('div', { class: 'avatar avatar--sm' }, initials(name)),
            h('div', {},
                h('div', {}, name),
                h('div', { style: { fontSize: 'var(--fs-2xs)', color: 'var(--text-muted)' } }, formatElo(elo))
            )
        );
    }

    _demo() {
        return {
            type: 'live_games',
            games: [
                { game_id: 501, white: 'Marta',   black: 'Kai',    white_elo: 2145, black_elo: 2098, time_control: '600+5', move_count: 24, spectators: 12 },
                { game_id: 502, white: 'Aria',    black: 'Nikko',  white_elo: 1876, black_elo: 1902, time_control: '300+3', move_count: 18, spectators: 4 },
                { game_id: 503, white: 'Vex',     black: 'Rue',    white_elo: 2210, black_elo: 2233, time_control: '180+2', move_count: 32, spectators: 27 },
                { game_id: 504, white: 'Sable',   black: 'Corvin', white_elo: 1520, black_elo: 1488, time_control: '600+5', move_count: 6,  spectators: 1 },
                { game_id: 505, white: 'Wren',    black: 'Yuki',   white_elo: 1732, black_elo: 1720, time_control: '900+10', move_count: 45, spectators: 8 },
            ],
        };
    }
}

/* ─── Watch (individual game) ──────────────────────────── */

export class SpectateWatchScreen extends Screen {
    constructor(ctx, params) {
        super(ctx);
        this.preview = true;
        this._gameId = parseInt(params.gameId, 10);
        this._fen = START_FEN;
        this._moves = [];
        this._whiteMs = 0;
        this._blackMs = 0;
        this._demoTimer = null;
        this._demoPly = 0;
    }

    render() {
        return h('div', { class: 'screen' },
            this.header('Game #' + this._gameId, 'Live spectator view.',
                h('button', {
                    class: 'btn btn--ghost btn--sm',
                    onclick: () => this.ctx.router.go('/spectate'),
                }, '← All live games')),
            h('div', { class: 'screen__body' },
                h('div', { class: 'game-screen' },
                    h('div', { class: 'game-board-col' },
                        this._bar('black'),
                        h('div', { class: 'board-frame' },
                            h('div', { class: 'board-container', ref: el => this._container = el },
                                h('canvas', { class: 'board-canvas', ref: el => this._canvas = el, role: 'img', 'aria-label': 'Spectator board' })
                            )
                        ),
                        this._bar('white'),
                        h('div', { class: 'status-line', ref: el => this._status = el }, 'Loading…')
                    ),
                    h('div', { class: 'game-side' },
                        h('div', { class: 'card' },
                            h('div', { class: 'card__header' },
                                icon('eye', 'icon--sm'),
                                h('div', { class: 'card__title' }, 'Spectators'),
                                h('span', { style: { marginLeft: 'auto', fontSize: 'var(--fs-xs)', color: 'var(--text-muted)' }, ref: el => this._specCount = el }, '—')
                            ),
                        ),
                        h('div', { class: 'card' },
                            h('div', { class: 'card__header' },
                                icon('replay', 'icon--sm'),
                                h('div', { class: 'card__title' }, 'Moves')),
                            h('div', { class: 'move-list', ref: el => this._moveList = el },
                                h('div', { class: 'move-list__empty' }, 'Waiting for moves…')
                            )
                        ),
                    )
                )
            )
        );
    }

    _bar(color) {
        return h('div', { class: 'player-bar' },
            h('div', { class: 'avatar', ref: el => this['_' + color + 'Avatar'] = el }, '?'),
            h('div', { class: 'player-bar__name-block' },
                h('div', { class: 'player-bar__name', ref: el => this['_' + color + 'Name'] = el }, color === 'white' ? 'White' : 'Black'),
                h('div', { class: 'player-bar__captured', ref: el => this['_' + color + 'Captured'] = el })
            ),
            h('div', { class: 'clock', ref: el => this['_' + color + 'Clock'] = el }, '--:--')
        );
    }

    async onMount() {
        this.renderer = new BoardRenderer(this._canvas,
            (THEMES[this.ctx.store.prefs.boardTheme] || THEMES.classic).factory());
        this.renderer.pieceSetUrl = `assets/pieces/${this.ctx.store.prefs.pieceSet || 'classic'}.svg`;

        this._boot();

        const { data, live } = await this.ctx.capability.request(
            this.ctx.Outbound.spectate(this._gameId),
            { expect: 'spectate_snapshot', timeout: 1500, demo: () => this._demoSnapshot() },
        );

        this._applySnapshot(data);

        if (!live) {
            this.interval(() => this._demoAdvance(), 1400);
        } else {
            this.sub(this.ctx.socket.on('spectate_move', raw => this._onLiveMove(raw)));
            this.sub(this.ctx.socket.on('spectate_end',  raw => this._onLiveEnd(raw)));
        }
    }

    onUnmount() {
        if (this._resizeObs) this._resizeObs.disconnect();
        if (this.ctx.socket.isConnected()) {
            this.ctx.socket.send(this.ctx.Outbound.stopSpectating(this._gameId));
        }
    }

    _boot() {
        let booted = false;
        const doIt = () => {
            if (booted) return;
            const rect = this._container.getBoundingClientRect();
            const size = Math.min(rect.width, rect.height);
            if (size <= 0) return;
            booted = true;
            this.renderer.resize(size).then(() => this.renderer.setPosition(this._fen));
        };
        doIt();
        if (!booted) requestAnimationFrame(doIt);
        this.timeout(doIt, 60);
        this.timeout(doIt, 250);

        this._resizeObs = new ResizeObserver(entries => {
            for (const e of entries) {
                const size = Math.min(e.contentRect.width, e.contentRect.height);
                if (size > 0) this.renderer.resize(size);
            }
        });
        this._resizeObs.observe(this._container);
    }

    _applySnapshot(snap) {
        this._whiteName = snap.white; this._blackName = snap.black;
        this._whiteAvatar.textContent = initials(snap.white);
        this._blackAvatar.textContent = initials(snap.black);
        this._whiteName_el = this._whiteName;
        this._whiteName_el = this._whiteName;
        // Names + ratings
        this._whiteName_el = null;
        this._whiteName = snap.white;
        this._blackName = snap.black;
        document.querySelectorAll('.player-bar__name')[0].textContent =
            `${snap.black} (${formatElo(snap.black_elo)})`;
        document.querySelectorAll('.player-bar__name')[1].textContent =
            `${snap.white} (${formatElo(snap.white_elo)})`;
        this._fen = snap.fen || START_FEN;
        this._moves = snap.moves ? snap.moves.slice() : [];
        this._whiteMs = snap.white_time || 0;
        this._blackMs = snap.black_time || 0;
        this._specCount.textContent = String(snap.spectators || 0);
        this.renderer.setPosition(this._fen);
        this._renderMoves();
        this._paintClocks();
        this._updateStatus();
        this._renderCaptured();
    }

    _demoAdvance() {
        const next = SCHOLARS_MATE.positions[this._demoPly + 1];
        if (!next) return;
        this._demoPly++;
        this._fen = next.fen;
        this._whiteMs = next.white_time;
        this._blackMs = next.black_time;
        this._moves.push({ san: next.san, think_ms: next.think_ms || 0 });
        this.renderer.animateMove(next.from, next.to, () => {
            this.renderer.setPosition(this._fen);
            this.renderer.setLastMove(next.from, next.to);
            const parsed = parseFen(this._fen);
            const inCheck = /[+#]$/.test(next.san);
            if (inCheck) {
                const kSq = findKingSquare(parsed, parsed.sideToMove);
                if (kSq) this.renderer.setCheck(kSq);
            } else this.renderer.clearCheck();
            this._renderMoves();
            this._paintClocks();
            this._updateStatus();
            this._renderCaptured();
            if (next.san.includes('#')) this._status.textContent = 'Checkmate — White wins';
        });
    }

    _onLiveMove(raw) {
        this._fen = raw.fen;
        this._whiteMs = Number(raw.white_time);
        this._blackMs = Number(raw.black_time);
        this._moves.push({ san: raw.san, think_ms: 0 });
        this.renderer.animateMove(raw.from, raw.to, () => {
            this.renderer.setPosition(this._fen);
            this.renderer.setLastMove(raw.from, raw.to);
            this._renderMoves();
            this._paintClocks();
            this._updateStatus();
            this._renderCaptured();
        });
    }
    _onLiveEnd(raw) {
        this._status.textContent = `Game over — ${raw.result} (${raw.reason})`;
        this._status.classList.add('is-over');
    }

    _renderCaptured() {
        const parsed = parseFen(this._fen);
        const cap = capturedPieces(parsed);
        const bal = materialBalance(parsed);
        this._paintCaptured(this._blackCaptured, cap.w, 'w',  bal < 0 ? -bal : 0);
        this._paintCaptured(this._whiteCaptured, cap.b, 'b',  bal > 0 ?  bal : 0);
    }
    _paintCaptured(root, list, capturedColor, advantage) {
        clear(root);
        for (const t of list) {
            const img = this.renderer.rasters[`${capturedColor}_${t}`];
            if (!img) continue;
            const clone = new Image();
            clone.src = img.src;
            root.appendChild(clone);
        }
        if (advantage > 0) root.appendChild(h('span', { class: 'player-bar__adv' }, `+${advantage}`));
    }

    _paintClocks() {
        this._whiteClock.textContent = formatClock(this._whiteMs);
        this._blackClock.textContent = formatClock(this._blackMs);
        const stm = parseFen(this._fen).sideToMove;
        this._whiteClock.classList.toggle('is-active', stm === 'w');
        this._blackClock.classList.toggle('is-active', stm === 'b');
    }

    _updateStatus() {
        const stm = parseFen(this._fen).sideToMove;
        this._status.textContent = stm === 'w' ? `${this._whiteName} to move` : `${this._blackName} to move`;
    }

    _renderMoves() {
        clear(this._moveList);
        if (this._moves.length === 0) {
            this._moveList.appendChild(h('div', { class: 'move-list__empty' }, 'Waiting for moves…'));
            return;
        }
        for (let i = 0; i < this._moves.length; i += 2) {
            const num = Math.floor(i / 2) + 1;
            this._moveList.appendChild(h('div', { class: 'move-list__num' }, num + '.'));
            this._moveList.appendChild(h('span', { class: 'move-list__san' + (i === this._moves.length - 1 ? ' is-current' : '') }, this._moves[i].san));
            this._moveList.appendChild(h('span', { class: 'move-list__san' + (i + 1 === this._moves.length - 1 ? ' is-current' : '') }, this._moves[i + 1] ? this._moves[i + 1].san : ''));
        }
        this._moveList.scrollTop = this._moveList.scrollHeight;
    }

    _demoSnapshot() {
        return {
            type: 'spectate_snapshot',
            game_id: this._gameId,
            white: 'Marta', black: 'Kai',
            white_elo: 2145, black_elo: 2098,
            fen: START_FEN,
            white_time: 600000, black_time: 600000,
            moves: [],
            spectators: 12,
        };
    }
}

/** Canned demo game (Scholar's Mate) used by the preview watch view. */
export const SCHOLARS_MATE = {
    positions: [
        { fen: 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1', san: null,      from: null,  to: null,  white_time: 600000, black_time: 600000 },
        { fen: 'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1', san: 'e4',   from: 'e2',  to: 'e4',  white_time: 597000, black_time: 600000, think_ms: 3000 },
        { fen: 'rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 2', san: 'e5', from: 'e7',  to: 'e5',  white_time: 597000, black_time: 596500, think_ms: 3500 },
        { fen: 'rnbqkbnr/pppp1ppp/8/4p3/2B1P3/8/PPPP1PPP/RNBQK1NR b KQkq - 1 2', san: 'Bc4', from: 'f1', to: 'c4',  white_time: 593000, black_time: 596500, think_ms: 4000 },
        { fen: 'r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/8/PPPP1PPP/RNBQK1NR w KQkq - 2 3', san: 'Nc6', from: 'b8', to: 'c6', white_time: 593000, black_time: 593000, think_ms: 3500 },
        { fen: 'r1bqkbnr/pppp1ppp/2n5/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR b KQkq - 3 3', san: 'Qh5', from: 'd1', to: 'h5', white_time: 588000, black_time: 593000, think_ms: 5000 },
        { fen: 'r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4', san: 'Nf6', from: 'g8', to: 'f6', white_time: 588000, black_time: 588500, think_ms: 4500 },
        { fen: 'r1bqkb1r/pppp1Qpp/2n2n2/4p3/2B1P3/8/PPPP1PPP/RNB1K1NR b KQkq - 0 4', san: 'Qxf7#', from: 'h5', to: 'f7', white_time: 584000, black_time: 588500, think_ms: 4000 },
    ],
    result: '1-0',
    reason: 'checkmate',
};
