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
 * Spectating is live-only. Network failures render an explicit unavailable
 * state instead of substituting fictional games.
 */

import { Screen } from '../ui/screen.js';
import { h, clear, icon } from '../core/dom.js';
import { storage } from '../core/storage.js';
import { BoardRenderer } from '../board/renderer.js';
import { BoardTheme, THEMES } from '../board/theme.js';
import { parseFen, capturedPieces, materialBalance, START_FEN } from '../board/chess.js';
import { formatClock, parseAndFormatTimeControl, initials, reasonLabel } from '../core/format.js';

const LAST_WATCHED_KEY = 'lastSpectatedGame';

/* ─── List ─────────────────────────────────────────────── */

export class SpectateListScreen extends Screen {
    constructor(ctx) {
        super(ctx);
    }

    render() {
        return h('div', { class: 'screen' },
            this.header('Spectate', 'Watch live games as they happen.'),
            h('div', { class: 'screen__body' },
                h('div', { ref: el => this._recent = el }),
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
            { expect: 'live_game_list', timeout: 5000, demo: () => null },
        );
        if (!live || !data) {
            clear(this._body);
            this._count.textContent = '';
            this._body.appendChild(h('div', { class: 'empty' },
                'Live games are unavailable. Check the server connection and try again.'));
            return;
        }
        // Capability already normalizes successful replies. A second pass
        // erased gameId and produced /spectate/undefined → Game #NaN.
        const games = data.games || [];
        this._render(games);
        this._loadRecentResult(games);

        // Re-poll every 5 seconds while the screen is mounted so spectator
        // counts and move counts stay fresh — same cadence as the lobby.
        if (live) {
            this.interval(() => {
                if (!this.ctx.socket.isConnected()) return;
                this.ctx.socket.send(this.ctx.Outbound.listLiveGames());
            }, 5000);
            this.sub(this.ctx.socket.on('live_game_list', raw => {
                const next = this.ctx.Inbound.normalize(raw).games || [];
                this._render(next);
                this._loadRecentResult(next);
            }));
        }
    }

    async _loadRecentResult(liveGames) {
        if (this._recentLoading || !this.root) return;
        const watched = storage.get(LAST_WATCHED_KEY);
        if (!watched || !Number.isSafeInteger(Number(watched.gameId))
            || Number(watched.gameId) <= 0) return;
        const gameId = Number(watched.gameId);
        if (liveGames.some(g => Number(g.gameId) === gameId)) {
            clear(this._recent);
            return;
        }
        if (watched.result) {
            this._renderRecentResult(watched);
            return;
        }

        this._recentLoading = true;
        try {
            const { data, live } = await this.ctx.capability.request(
                this.ctx.Outbound.getGame(gameId),
                { expect: 'game', timeout: 3000, demo: () => null, failOnError: true },
            );
            if (!this.root || !live || !data || !data.result || data.result === '*') return;
            const completed = {
                gameId,
                white: data.white,
                black: data.black,
                result: data.result,
                reason: data.reason,
            };
            storage.set(LAST_WATCHED_KEY, completed);
            this._renderRecentResult(completed);
        } finally {
            this._recentLoading = false;
        }
    }

    _renderRecentResult(game) {
        if (!this._recent) return;
        clear(this._recent);
        let outcome = 'Draw';
        if (game.result === '1-0') outcome = `${game.white || 'White'} (White) wins`;
        if (game.result === '0-1') outcome = `${game.black || 'Black'} (Black) wins`;
        this._recent.appendChild(h('div', {
            class: 'card', style: { marginBottom: 'var(--sp-4)' },
        }, h('div', {
            class: 'card__body',
            style: { display: 'flex', alignItems: 'center', gap: 'var(--sp-3)', flexWrap: 'wrap' },
        },
            h('div', { style: { flex: 1, minWidth: '220px' } },
                h('div', { class: 'card__title' }, 'Recently watched result'),
                h('div', { class: 'field__hint', role: 'status' },
                    `${game.white || 'White'} vs ${game.black || 'Black'} · ${outcome} · ${reasonLabel(game.reason)} (${game.result})`)),
            h('button', {
                class: 'btn btn--sm',
                onclick: () => this.ctx.router.go('/replay/' + game.gameId),
            }, 'Open replay'))));
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

}

/* ─── Watch (individual game) ──────────────────────────── */

export class SpectateWatchScreen extends Screen {
    constructor(ctx, params) {
        super(ctx);
        const rawGameId = params && String(params.gameId || '');
        this._gameId = /^\d+$/.test(rawGameId) ? Number(rawGameId) : null;
        this._liveJoined = false;
        this._fen = START_FEN;
        this._moves = [];
        this._whiteMs = 0;
        this._blackMs = 0;
        this._clockAnchoredAt = performance.now();
        this._activeSide = 'w';
        this._ended = false;
        this._spectateStarting = false;
    }

    render() {
        return h('div', { class: 'screen' },
            this.header(this._gameId === null ? 'Live game' : 'Game #' + this._gameId, 'Live spectator view.',
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
                        h('div', {
                            class: 'status-line', role: 'status', 'aria-live': 'polite',
                            ref: el => this._status = el,
                        }, 'Loading…')
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
                h('div', { class: 'player-bar__name', ref: el => this['_' + color + 'NameEl'] = el }, color === 'white' ? 'White' : 'Black'),
                h('div', { class: 'player-bar__captured', ref: el => this['_' + color + 'Captured'] = el })
            ),
            h('div', { class: 'clock', ref: el => this['_' + color + 'Clock'] = el }, '--:--')
        );
    }

    onMount() {
        this.renderer = new BoardRenderer(this._canvas,
            (THEMES[this.ctx.store.prefs.boardTheme] || THEMES.classic).factory());
        this.renderer.onRastersReady = () => this._renderCaptured();
        this.renderer.pieceSetUrl = `assets/pieces/${this.ctx.store.prefs.pieceSet || 'classic'}.svg`;

        this._boot();

        if (this._gameId === null) {
            this._status.textContent = 'Invalid game link.';
            this.ctx.toast.error('This spectator link has no valid game ID.', { duration: 3200 });
            return;
        }

        // A hard refresh reconstructs the access token from the persisted
        // refresh token asynchronously. Do not mistake that short hydration
        // window for a signed-out user.
        this.sub(this.ctx.session.on('change', () => this._beginSpectating()));
        this.sub(this.ctx.socket.onState(state => {
            if (state !== 'connected' || this._ended) return;
            if (this._liveJoined) this._sendSpectateSnapshot();
            else this._beginSpectating();
        }));
        this._beginSpectating();
    }

    async _beginSpectating() {
        if (this._liveJoined || this._spectateStarting || this._ended || !this.root) return;

        const session = this.ctx.session;
        const token = session && session.accessToken;
        if (!token) {
            if (session && session.hasRefreshToken) {
                this._status.textContent = 'Restoring your session…';
                return;
            }
            this.ctx.postAuthPath = '/spectate/' + this._gameId;
            this.ctx.toast.warning('Sign in to spectate live games.', { duration: 2800 });
            this.ctx.router.go('/login');
            return;
        }

        if (!this.ctx.socket.isConnected()) {
            this._status.textContent = 'Connecting to the server…';
            return;
        }

        this._spectateStarting = true;
        this._status.textContent = 'Joining live view…';

        const { data, live } = await this.ctx.capability.request(
            this.ctx.Outbound.spectate(token, this._gameId),
            { expect: 'spectate_start', timeout: 5000, demo: () => null, failOnError: true },
        );
        this._spectateStarting = false;
        if (!this.root) return;
        if (!live || !data) {
            this._status.textContent = 'Unable to spectate this game.';
            this.ctx.toast.error('The live spectator request failed.', { duration: 3200 });
            return;
        }

        this._liveJoined = true;
        this._applySnapshot(data);
        this.sub(this.ctx.socket.on('move_made', raw => this._onLiveMove(raw)));
        this.sub(this.ctx.socket.on('game_over', raw => this._onLiveEnd(raw)));
        this.sub(this.ctx.socket.on('spectate_start', raw => {
            const snap = this.ctx.Inbound.normalize(raw);
            if (Number(snap.gameId) === Number(this._gameId)) this._applySnapshot(snap);
        }));
        // Push frames are the fast path. Periodic snapshots repair a dropped
        // frame and re-assert membership after navigation/reconnect.
        this.interval(() => {
            if (!this._ended) this._sendSpectateSnapshot();
        }, 3000);
        this.interval(() => this._paintClocks(), 100);
    }

    _sendSpectateSnapshot() {
        const token = this.ctx.session && this.ctx.session.accessToken;
        if (token && this.ctx.socket.isConnected()) {
            this.ctx.socket.send(this.ctx.Outbound.spectate(token, this._gameId));
        }
    }

    onUnmount() {
        if (this._resizeObs) this._resizeObs.disconnect();
        if (this._liveJoined && this.ctx.socket.isConnected()) {
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
        storage.set(LAST_WATCHED_KEY, {
            gameId: this._gameId, white: snap.white, black: snap.black,
        });
        this._whiteAvatar.textContent = initials(snap.white);
        this._blackAvatar.textContent = initials(snap.black);
        this._blackNameEl.textContent = snap.black;
        this._whiteNameEl.textContent = snap.white;
        this._fen     = snap.fen || START_FEN;
        this._moves   = snap.moves ? snap.moves.slice() : [];
        this._whiteMs = snap.whiteMs || 0;
        this._blackMs = snap.blackMs || 0;
        this._clockAnchoredAt = performance.now();
        this._activeSide = parseFen(this._fen).sideToMove;
        this._specCount.textContent = String(snap.spectatorCount || 0);
        this.renderer.setPosition(this._fen);
        this._renderMoves();
        this._paintClocks();
        this._updateStatus();
        this._renderCaptured();
    }

    _onLiveMove(raw) {
        // The server frame is snake_case (from/to/san/white_time/black_time/fen).
        // The FEN comes in the same frame as of Phase 9.1 — no follow-up
        // game_state round-trip is needed.
        const norm = this.ctx.Inbound.normalize(raw);
        this._fen = norm.fen || this._fen;
        this._whiteMs = norm.whiteMs;
        this._blackMs = norm.blackMs;
        this._clockAnchoredAt = performance.now();
        this._activeSide = parseFen(this._fen).sideToMove;
        this._moves.push({ san: norm.san, thinkMs: 0 });
        this.renderer.animateMove(norm.from, norm.to, () => {
            this.renderer.setPosition(this._fen);
            this.renderer.setLastMove(norm.from, norm.to);
            this._renderMoves();
            this._paintClocks();
            // `game_over` can arrive while the final-move animation is still
            // running. Never let the later animation callback overwrite the
            // winner announcement with "White/Black to move".
            if (!this._ended) this._updateStatus();
            this._renderCaptured();
        });
    }
    _onLiveEnd(raw) {
        // Spectators receive the same `game_over` frame the seated players do.
        let outcome = 'Game over';
        if (raw.result === '1-0') {
            outcome = `${this._whiteName || 'White'} (White) wins`;
        } else if (raw.result === '0-1') {
            outcome = `${this._blackName || 'Black'} (Black) wins`;
        } else if (raw.result === '1/2-1/2') {
            outcome = 'Draw';
        }
        this._status.textContent = `${outcome} · ${reasonLabel(raw.reason)} (${raw.result || '*'})`;
        this._status.classList.add('is-over');
        this._ended = true;
        storage.set(LAST_WATCHED_KEY, {
            gameId: this._gameId,
            white: this._whiteName,
            black: this._blackName,
            result: raw.result,
            reason: raw.reason,
        });
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
            const thumbnail = this.renderer.createPieceThumbnail(`${capturedColor}_${t}`);
            if (thumbnail) root.appendChild(thumbnail);
        }
        if (advantage > 0) root.appendChild(h('span', { class: 'player-bar__adv' }, `+${advantage}`));
    }

    _paintClocks() {
        let white = this._whiteMs;
        let black = this._blackMs;
        if (!this._ended) {
            const elapsed = Math.max(0, performance.now() - this._clockAnchoredAt);
            if (this._activeSide === 'w') white = Math.max(0, white - elapsed);
            else black = Math.max(0, black - elapsed);
        }
        this._whiteClock.textContent = formatClock(white);
        this._blackClock.textContent = formatClock(black);
        this._whiteClock.classList.toggle('is-active', !this._ended && this._activeSide === 'w');
        this._blackClock.classList.toggle('is-active', !this._ended && this._activeSide === 'b');
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
