/**
 * spectate.js — LIVE screens (Phase 9.1).
 *
 * Two entry points registered in main.js:
 *   /spectate           → SpectateListScreen  (live games table)
 *   /spectate/:gameId   → SpectateWatchScreen (board + move stream)
 *
 * Wire contract, as implemented by GameHandler (see src/game/game_handler.h):
 *   → { type: 'list_live_games' }
 *   ← { type: 'live_game_list', games: [{
 *         game_id, white, black, time_control, move_count, spectator_count
 *     }] }  — normalized in protocol.js to {gameId, white, black,
 *             timeControl, moveCount, spectatorCount}.
 *
 *   → { type: 'spectate', access_token, game_id }        // Auth-mandatory.
 *   ← { type: 'spectate_start', game_id, white, black,
 *         fen, white_time, black_time, moves: [{san, think_ms}],
 *         spectator_count }
 *   ← push { type: 'move_made', from, to, san, white_time, black_time }
 *   ← push { type: 'game_over', result, reason }
 *
 *   → { type: 'stop_spectating', game_id }               // No auth needed.
 *   ← { type: 'spectate_end',    game_id }
 *
 * Fallback: SpectateListScreen still ships a demo dataset via capability so
 * the UI stays populated when the server has no live games. The individual
 * SpectateWatchScreen also keeps its Scholar's-Mate demo as a fallback.
 */

import { Screen } from '../ui/screen.js';
import { h, clear, icon } from '../core/dom.js';
import { BoardRenderer } from '../board/renderer.js';
import { BoardTheme, THEMES } from '../board/theme.js';
import { parseFen, findKingSquare, capturedPieces, materialBalance, START_FEN } from '../board/chess.js';
import { formatClock, parseAndFormatTimeControl, initials } from '../core/format.js';

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
        // Phase 9.1: the server actually implements list_live_games now.
        // capability.request still keeps the demo path — a browser opened
        // against a dead server can still populate the table for design work.
        const { data, live } = await this.ctx.capability.request(
            this.ctx.Outbound.listLiveGames(),
            { expect: 'live_game_list', timeout: 1500, demo: () => this._demo() },
        );
        // When live, this.preview should stay false so users don't see the
        // "preview" badge on a real feature.
        if (live) this.preview = false;
        const normalized = this.ctx.Inbound.normalize(data);
        this._render(normalized.games || []);

        // Re-poll every 5 seconds while the screen is mounted so spectator
        // counts and move counts stay fresh — same cadence as the lobby.
        if (live) {
            this.interval(() => {
                if (!this.ctx.socket.isConnected()) return;
                this.ctx.socket.send(this.ctx.Outbound.listLiveGames());
            }, 5000);
            this.sub(this.ctx.socket.on('live_game_list',
                raw => this._render(this.ctx.Inbound.normalize(raw).games || [])));
        }
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
                        h('td', {}, this._playerCell(g.white)),
                        h('td', {}, this._playerCell(g.black)),
                        h('td', { class: 'mono' }, parseAndFormatTimeControl(g.timeControl)),
                        h('td', { class: 'mono' }, String(g.moveCount || 0)),
                        h('td', { class: 'mono', style: { color: 'var(--text-muted)' } }, String(g.spectatorCount || 0)),
                        h('td', { style: { textAlign: 'right' } },
                            h('button', {
                                class: 'btn btn--sm btn--primary',
                                onclick: () => this.ctx.router.go('/spectate/' + g.gameId),
                            }, 'Watch')
                        )
                    ))
                )
            )
        );
    }

    _playerCell(name) {
        return h('div', { style: { display: 'flex', alignItems: 'center', gap: '10px' } },
            h('div', { class: 'avatar avatar--sm' }, initials(name)),
            h('div', {}, name)
        );
    }

    // Demo fallback — used only when the socket can't confirm the live wire
    // type within the capability probe window. Kept for offline UI work.
    _demo() {
        return {
            type: 'live_game_list',
            games: [
                { game_id: 501, white: 'Marta',   black: 'Kai',    time_control: '600+5',  move_count: 24, spectator_count: 12 },
                { game_id: 502, white: 'Aria',    black: 'Nikko',  time_control: '300+3',  move_count: 18, spectator_count: 4  },
                { game_id: 503, white: 'Vex',     black: 'Rue',    time_control: '180+2',  move_count: 32, spectator_count: 27 },
                { game_id: 504, white: 'Sable',   black: 'Corvin', time_control: '600+5',  move_count: 6,  spectator_count: 1  },
                { game_id: 505, white: 'Wren',    black: 'Yuki',   time_control: '900+10', move_count: 45, spectator_count: 8  },
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

        // Auth is mandatory on spectate (see Phase 9 pre-work migration).
        // If we don't have a token, we can still show the demo, but the real
        // server will refuse; nudge them once so it's not a mystery.
        const token = this.ctx.session && this.ctx.session.accessToken;
        if (!token) {
            this.ctx.toast.info('Sign in to spectate live games.', { duration: 2800 });
        }

        const { data, live } = await this.ctx.capability.request(
            this.ctx.Outbound.spectate(token || '', this._gameId),
            { expect: 'spectate_start', timeout: 1500, demo: () => this._demoSnapshot() },
        );
        if (live) this.preview = false;

        // Normalize once — the rest of the screen deals in camelCase already.
        const snap = this.ctx.Inbound.normalize(data);
        this._applySnapshot(snap);

        if (!live) {
            this.interval(() => this._demoAdvance(), 1400);
        } else {
            // Live path: spectators receive the SAME move_made / game_over
            // frames as the seated players. No dedicated spectate_move
            // opcode — that keeps the fan-out on one code path server-side.
            this.sub(this.ctx.socket.on('move_made', raw => this._onLiveMove(raw)));
            this.sub(this.ctx.socket.on('game_over', raw => this._onLiveEnd(raw)));
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
        // Normalized snapshot: {white, black, fen, whiteMs, blackMs,
        //                      spectatorCount, moves: [{san, thinkMs}]}.
        // The wire has no ELO for spectator rows — display names only. If a
        // future phase adds elo to the room-info payload, plumb it here.
        this._whiteName = snap.white; this._blackName = snap.black;
        this._whiteAvatar.textContent = initials(snap.white);
        this._blackAvatar.textContent = initials(snap.black);
        const bars = document.querySelectorAll('.player-bar__name');
        if (bars.length >= 2) {
            bars[0].textContent = snap.black;   // top bar (opponent-side layout)
            bars[1].textContent = snap.white;
        }
        this._fen     = snap.fen || START_FEN;
        this._moves   = snap.moves ? snap.moves.slice() : [];
        this._whiteMs = snap.whiteMs || 0;
        this._blackMs = snap.blackMs || 0;
        this._specCount.textContent = String(snap.spectatorCount || 0);
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
        // The server frame is snake_case (from/to/san/white_time/black_time/fen).
        // The FEN comes in the same frame as of Phase 9.1 — no follow-up
        // game_state round-trip is needed.
        const norm = this.ctx.Inbound.normalize(raw);
        this._fen = norm.fen || this._fen;
        this._whiteMs = norm.whiteMs;
        this._blackMs = norm.blackMs;
        this._moves.push({ san: norm.san, thinkMs: 0 });
        this.renderer.animateMove(norm.from, norm.to, () => {
            this.renderer.setPosition(this._fen);
            this.renderer.setLastMove(norm.from, norm.to);
            this._renderMoves();
            this._paintClocks();
            this._updateStatus();
            this._renderCaptured();
        });
    }
    _onLiveEnd(raw) {
        // Spectators receive the same `game_over` frame the seated players do.
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

    // Demo fallback. Matches the wire shape of `spectate_start` so the
    // Inbound.normalize path handles it the same way as live data.
    _demoSnapshot() {
        return {
            type: 'spectate_start',
            game_id: this._gameId,
            white: 'Marta', black: 'Kai',
            fen: START_FEN,
            white_time: 600000, black_time: 600000,
            time_control: '600+5',
            moves: [],
            spectator_count: 12,
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
