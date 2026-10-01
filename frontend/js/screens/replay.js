/**
 * replay.js — live persisted-game replay screens.
 *
 * Two entries:
 *   /replay             → ReplayListScreen (recent games)
 *   /replay/:gameId     → ReplayDetailScreen (move-by-move viewer)
 *
 * Expected backend contract:
 *   → { type: 'get_game', game_id }
 *   ← { type: 'game', game_id, white, black, white_elo, black_elo, result, reason,
 *         time_control, played_at,
 *         positions: [{ fen, san, think_ms, white_ms, black_ms,
 *                       from?, to?, eval_cp? }]  // one entry per ply INCLUDING starting pos at [0]
 *     }
 *
 *   → { type: 'analyze_position', fen, depth }
 *   ← { type: 'analysis', fen, eval_cp, best_move }
 */

import { Screen } from '../ui/screen.js';
import { h, clear, icon } from '../core/dom.js';
import { BoardRenderer } from '../board/renderer.js';
import { THEMES } from '../board/theme.js';
import { START_FEN, parseFen, findKingSquare } from '../board/chess.js';
import { initials, relativeTime, parseAndFormatTimeControl, formatElo, reasonLabel } from '../core/format.js';

/* ─── List ─────────────────────────────────────────────── */

export class ReplayListScreen extends Screen {
    constructor(ctx) {
        super(ctx);
        this._loading = false;
        this._loaded = false;
        this._liveHandlersBound = false;
    }

    render() {
        return h('div', { class: 'screen' },
            this.header('Replays', 'Browse and replay your saved games.'),
            h('div', { class: 'screen__body' },
                h('div', { ref: el => this._rematch = el }),
                h('div', { ref: el => this._active = el }),
                h('div', { class: 'card' },
                    h('div', { class: 'card__header' },
                        icon('replay', 'icon--sm'),
                        h('div', { class: 'card__title' }, 'Your games'),
                    ),
                    h('div', { style: { padding: 0 }, ref: el => this._body = el },
                        h('div', { class: 'empty' }, 'Loading…')
                    )
                )
            )
        );
    }

    onMount() {
        this.sub(this.ctx.session.on('change', () => this._beginLoading()));
        this._beginLoading();
    }

    _bindLiveHandlers() {
        if (this._liveHandlersBound) return;
        this._liveHandlersBound = true;
        this.sub(this.ctx.socket.on('rematch_offered', raw => {
            this._renderPendingRematch({
                gameId: Number(raw.game_id),
                from: String(raw.from || 'Opponent'),
                role: 'recipient',
                expiresInMs: Number(raw.expires_in_ms || 45_000),
            });
        }));
        this.sub(this.ctx.socket.on('rematch_declined', () => this._renderPendingRematch(null)));
        this.sub(this.ctx.socket.on('rematch_offer_resolved', () => this._renderPendingRematch(null)));
        this.sub(this.ctx.socket.on('rematch_started', raw =>
            this._onRematchStarted(this.ctx.Inbound.normalize(raw))));
        this.sub(this.ctx.socket.on('error', raw => {
            if (!this._rematchResponding) return;
            this._rematchResponding = false;
            this._renderPendingRematch(this._pendingRematch);
            const error = this.ctx.Inbound.normalize(raw);
            this.ctx.toast.danger(error.message || 'Could not respond to the rematch offer.');
        }));
    }

    async _beginLoading() {
        if (!this.root || this._loading) return;

        const session = this.ctx.session;
        if (!session || !session.accessToken) {
            this._loaded = false;
            clear(this._active);
            clear(this._rematch);
            if (session && session.hasRefreshToken) {
                clear(this._body);
                this._body.appendChild(h('div', {
                    class: 'empty', role: 'status', 'aria-live': 'polite',
                }, 'Restoring your session…'));
            } else {
                this._renderSignedOut();
            }
            return;
        }
        if (this._loaded) return;

        this._loading = true;
        this._bindLiveHandlers();

        try {
            await this._loadPendingRematch();
            await this._loadActive();
            if (!this.root) return;
            const username = this.ctx.session.username;
            const { data, live, error } = await this.ctx.capability.request(
                this.ctx.Outbound.getHistory(username, 30),
                { expect: 'history', timeout: 5000, demo: () => null, failOnError: true },
            );
            if (!this.root) return;
            if (!live || !data) {
                this._renderUnavailable(error);
                return;
            }
            this._loaded = true;
            this._render(data.games || []);
        } finally {
            this._loading = false;
        }
    }

    async _loadPendingRematch() {
        const token = this.ctx.session.accessToken;
        if (!token) { this._renderPendingRematch(null); return; }
        const { data, live } = await this.ctx.capability.request(
            this.ctx.Outbound.pendingRematch(token),
            { expect: 'pending_rematch', timeout: 3000, demo: () => null, failOnError: true },
        );
        this._renderPendingRematch(live && data ? data.offer : null);
    }

    _renderPendingRematch(offer) {
        if (!this._rematch) return;
        clear(this._rematch);
        this._pendingRematch = offer || null;
        if (!offer || !offer.gameId) return;

        const recipient = offer.role === 'recipient';
        const actions = recipient
            ? h('div', { style: { display: 'flex', gap: '8px', flexWrap: 'wrap' } },
                h('button', {
                    class: 'btn btn--primary',
                    disabled: !!this._rematchResponding,
                    onclick: () => this._respondToRematch(true),
                }, this._rematchResponding ? 'Starting…' : 'Accept rematch'),
                h('button', {
                    class: 'btn',
                    disabled: !!this._rematchResponding,
                    onclick: () => this._respondToRematch(false),
                }, 'Decline'))
            : h('span', { class: 'badge' }, 'Waiting for opponent');

        this._rematch.appendChild(h('div', {
            class: 'card', style: { marginBottom: 'var(--sp-4)' },
        }, h('div', {
            class: 'card__body',
            style: { display: 'flex', alignItems: 'center', gap: 'var(--sp-3)', flexWrap: 'wrap' },
        },
            h('div', { style: { flex: 1, minWidth: '220px' } },
                h('div', { class: 'card__title' }, recipient
                    ? `${offer.from} offered a rematch`
                    : 'Rematch offer pending'),
                h('div', { class: 'field__hint' },
                    'This offer expires after 45 seconds.')),
            actions)));
    }

    _respondToRematch(accept) {
        const token = this.ctx.session.accessToken;
        const offer = this._pendingRematch;
        if (!token || !offer || !offer.gameId || this._rematchResponding) return;
        this._rematchResponding = true;
        this._renderPendingRematch(offer);
        this.ctx.socket.send(this.ctx.Outbound.rematchResponse(
            token, offer.gameId, accept));
        if (!accept) this._renderPendingRematch(null);
    }

    _onRematchStarted(msg) {
        if (!msg || !msg.gameId) return;
        this.ctx.store.setGame({
            gameId: msg.gameId,
            color: msg.color === 'black' ? 'b' : 'w',
            opponent: msg.opponent,
            isAI: false,
            whiteMs: msg.whiteMs,
            blackMs: msg.blackMs,
            timeBaseSec: msg.timeBaseSec,
            timeIncSec: msg.timeIncSec,
            difficulty: null,
        });
        this.ctx.router.go('/game/' + msg.gameId);
    }

    async _loadActive() {
        const token = this.ctx.session.accessToken;
        if (!token) {
            this._renderActive();
            return;
        }
        const { data, live } = await this.ctx.capability.request(
            this.ctx.Outbound.activeGame(token),
            { expect: 'active_game', timeout: 3000, demo: () => null, failOnError: true },
        );
        if (live && data) {
            this.ctx.store.setGame(data.game || null);
        }
        this._renderActive();
    }

    _renderActive() {
        clear(this._active);
        const game = this.ctx.store.game;
        if (!game || !game.gameId) return;
        this._active.appendChild(h('div', { class: 'card', style: { marginBottom: 'var(--sp-4)' } },
            h('div', { class: 'card__body', style: { display: 'flex', alignItems: 'center', gap: 'var(--sp-3)', flexWrap: 'wrap' } },
                h('div', { style: { flex: 1, minWidth: '220px' } },
                    h('div', { class: 'card__title' }, 'Live game in progress'),
                    h('div', { class: 'field__hint', role: 'status', 'aria-atomic': 'true' },
                        `Game #${game.gameId} against ${game.opponent || 'Opponent'} can be resumed.`)),
                h('button', {
                    class: 'btn btn--primary',
                    onclick: () => this.ctx.router.go('/game/' + game.gameId),
                }, 'Resume game'))));
    }

    _renderSignedOut() {
        clear(this._body);
        this._body.appendChild(h('div', { class: 'empty' },
            h('div', {}, 'Sign in to browse your saved games.'),
            h('div', { style: { display: 'flex', gap: '8px', justifyContent: 'center', marginTop: '16px' } },
                h('a', { class: 'btn btn--primary', href: '#/login' }, 'Sign in'),
                h('a', { class: 'btn btn--ghost', href: '#/register' }, 'Create account'))));
    }

    _renderUnavailable(error) {
        clear(this._body);
        const detail = error && error.message ? error.message : 'Check the server and database connection.';
        this._body.appendChild(h('div', { class: 'empty' },
            h('div', {}, 'Saved games could not be loaded.'),
            h('div', { class: 'field__hint', style: { marginTop: '8px' } }, detail)));
    }

    _render(games) {
        clear(this._body);
        if (games.length === 0) {
            this._body.appendChild(h('div', { class: 'empty' }, 'No games saved yet.'));
            return;
        }
        this._body.appendChild(
            h('table', { class: 'table' },
                h('thead', {}, h('tr', {},
                    h('th', {}, 'Result'),
                    h('th', {}, 'Opponent'),
                    h('th', {}, 'Time'),
                    h('th', {}, 'Moves'),
                    h('th', {}, 'Played'),
                    h('th', { style: { textAlign: 'right' } }, ''),
                )),
                h('tbody', {},
                    ...games.map(g => h('tr', {},
                        h('td', {},
                            h('span', { class: 'result-dot result-dot--' + g.result },
                                { w: 'W', l: 'L', d: 'D' }[g.result] || '?')
                        ),
                        h('td', {},
                            h('div', { style: { display: 'flex', alignItems: 'center', gap: '10px' } },
                                h('div', { class: 'avatar avatar--sm' }, initials(g.opponent)),
                                h('div', {},
                                    h('div', {}, g.opponent),
                                    h('div', { style: { fontSize: 'var(--fs-2xs)', color: 'var(--text-muted)' } },
                                        g.rated ? formatElo(g.opponentElo) : 'Unrated'),
                                )
                            )
                        ),
                        h('td', { class: 'mono' }, parseAndFormatTimeControl(g.timeControl)),
                        h('td', { class: 'mono' }, String(g.moveCount || g.moves || '—')),
                        h('td', { style: { color: 'var(--text-muted)' } }, relativeTime(g.playedAt)),
                        h('td', { style: { textAlign: 'right' } },
                            h('button', {
                                class: 'btn btn--sm',
                                onclick: () => this.ctx.router.go('/replay/' + g.gameId),
                            }, 'Replay')
                        ),
                    ))
                )
            )
        );
    }

}

/* ─── Detail (move-by-move) ────────────────────────────── */

export class ReplayDetailScreen extends Screen {
    constructor(ctx, params) {
        super(ctx);
        this._gameId = Number(params.gameId);
        this._ply = 0;
        this._playing = false;
        this._playTimer = null;
    }

    render() {
        return h('div', { class: 'screen' },
            this.header('Replay', null,
                h('button', {
                    class: 'btn btn--ghost btn--sm',
                    onclick: () => this.ctx.router.go('/replay'),
                }, '← All games')),
            h('div', { class: 'screen__body' },
                h('div', { class: 'replay-grid' },

                    // Board + eval + nav
                    h('div', { class: 'replay-main' },
                        h('div', { class: 'replay-stage' },
                            h('div', { class: 'eval-bar', ref: el => this._evalBar = el },
                                h('div', { class: 'eval-bar__white', ref: el => this._evalFill = el }),
                                h('div', { class: 'eval-bar__label', ref: el => this._evalLabel = el }, '0.0')
                            ),
                            h('div', { class: 'board-frame', style: { flex: '0 0 auto' } },
                                h('div', { class: 'board-container', style: { width: '480px', maxWidth: '100%' }, ref: el => this._container = el },
                                    h('canvas', { class: 'board-canvas', ref: el => this._canvas = el, role: 'img', 'aria-label': 'Replay board' })
                                )
                            ),
                        ),
                        h('div', { class: 'replay-controls' },
                            h('button', { class: 'btn btn--sm', title: 'First',    onclick: () => this._goto(0) }, '⏮'),
                            h('button', { class: 'btn btn--sm', title: 'Previous', onclick: () => this._goto(this._ply - 1) }, '◀'),
                            h('button', { class: 'btn btn--sm btn--primary', ref: el => this._playBtn = el, onclick: () => this._togglePlay() }, '▶ Play'),
                            h('button', { class: 'btn btn--sm', title: 'Next',     onclick: () => this._goto(this._ply + 1) }, '▶'),
                            h('button', { class: 'btn btn--sm', title: 'Last',     onclick: () => this._goto(this._maxPly()) }, '⏭'),
                            h('button', { class: 'btn btn--sm btn--ghost', title: 'Flip', onclick: () => { this.renderer.flip(); this.renderer.setPosition(this._currentFen()); } }, 'Flip'),
                            h('button', { class: 'btn btn--sm btn--ghost', title: 'Copy PGN', onclick: () => this._copyPgn() }, 'PGN'),
                        ),
                    ),

                    // Meta + move list
                    h('div', { class: 'replay-side' },
                        h('div', { class: 'card' },
                            h('div', { class: 'card__body', ref: el => this._meta = el },
                                h('div', { class: 'empty' }, 'Loading…')
                            )
                        ),
                        h('div', { class: 'card' },
                            h('div', { class: 'card__header' },
                                icon('replay', 'icon--sm'),
                                h('div', { class: 'card__title' }, 'Moves'),
                                h('span', {
                                    style: { marginLeft: 'auto', fontSize: 'var(--fs-xs)', color: 'var(--text-muted)' },
                                    ref: el => this._plyLabel = el,
                                }, ''),
                            ),
                            h('div', { class: 'move-list', ref: el => this._moveList = el },
                                h('div', { class: 'move-list__empty' }, 'Loading moves…')
                            )
                        )
                    ),
                )
            )
        );
    }

    async onMount() {
        this.renderer = new BoardRenderer(this._canvas,
            (THEMES[this.ctx.store.prefs.boardTheme] || THEMES.classic).factory());
        this.renderer.pieceSetUrl = `assets/pieces/${this.ctx.store.prefs.pieceSet || 'classic'}.svg`;
        this._boot();

        if (!Number.isSafeInteger(this._gameId) || this._gameId <= 0) {
            this._renderLoadError('Invalid game ID.');
            return;
        }

        const { data, live, error } = await this.ctx.capability.request(
            this.ctx.Outbound.getGame(this._gameId),
            { expect: 'game', timeout: 5000, demo: () => null, failOnError: true },
        );
        if (!live || !data) {
            this._renderLoadError(error && error.message
                ? error.message
                : 'Replay could not be loaded. Check the server and database connection.');
            return;
        }
        this._game = data;
        this._renderMeta();
        this._renderMoves();
        this._goto(0);

        // Keyboard nav
        this._keyHandler = (e) => {
            if (e.key === 'ArrowLeft')  this._goto(this._ply - 1);
            else if (e.key === 'ArrowRight') this._goto(this._ply + 1);
            else if (e.key === 'Home')       this._goto(0);
            else if (e.key === 'End')        this._goto(this._maxPly());
            else if (e.key === ' ')          { e.preventDefault(); this._togglePlay(); }
            else return;
            e.stopPropagation();
        };
        window.addEventListener('keydown', this._keyHandler);
    }

    _renderLoadError(message) {
        clear(this._meta);
        this._meta.appendChild(h('div', { class: 'empty' }, message));
        clear(this._moveList);
        this._moveList.appendChild(h('div', { class: 'move-list__empty' }, 'No replay loaded.'));
    }

    onUnmount() {
        if (this._resizeObs) this._resizeObs.disconnect();
        if (this._playTimer) clearInterval(this._playTimer);
        if (this._keyHandler) window.removeEventListener('keydown', this._keyHandler);
    }

    _boot() {
        let booted = false;
        const doIt = () => {
            if (booted) return;
            const rect = this._container.getBoundingClientRect();
            const size = Math.min(rect.width, rect.height);
            if (size <= 0) return;
            booted = true;
            this.renderer.resize(size).then(() => this.renderer.setPosition(START_FEN));
        };
        doIt();
        if (!booted) requestAnimationFrame(doIt);
        this.timeout(doIt, 60);
        this.timeout(doIt, 250);
        this._resizeObs = new ResizeObserver(entries => {
            for (const e of entries) {
                const s = Math.min(e.contentRect.width, e.contentRect.height);
                if (s > 0) this.renderer.resize(s);
            }
        });
        this._resizeObs.observe(this._container);
    }

    _maxPly() { return (this._game && this._game.positions) ? this._game.positions.length - 1 : 0; }

    _goto(ply) {
        if (!this._game) return;
        const max = this._maxPly();
        ply = Math.max(0, Math.min(max, ply));
        this._ply = ply;
        const pos = this._game.positions[ply];
        this.renderer.setPosition(pos.fen);
        this.renderer.clearCheck();
        if (ply > 0 && pos.from && pos.to) this.renderer.setLastMove(pos.from, pos.to);
        else this.renderer.clearLastMove();
        // Check indicator via SAN
        if (pos.san && /[+#]$/.test(pos.san)) {
            const parsed = parseFen(pos.fen);
            const kSq = findKingSquare(parsed, parsed.sideToMove);
            if (kSq) this.renderer.setCheck(kSq);
        }
        this._updateEval(pos.eval_cp);
        this._highlightMove();
        this._plyLabel.textContent = `${ply} / ${max}`;
        if (ply === max) this._pause();
    }

    _updateEval(cp) {
        // 100cp = 1 pawn. Map centipawns to a 0..100% white fill height.
        // Sigmoid so ±800cp saturates roughly to 95%.
        const v = cp == null ? 0 : cp;
        const pct = 50 + (Math.atan(v / 400) / Math.PI) * 100;
        this._evalFill.style.height = `${Math.max(2, Math.min(98, pct))}%`;
        const pawns = (v / 100).toFixed(1);
        this._evalLabel.textContent = v > 0 ? `+${pawns}` : String(pawns);
    }

    _togglePlay() {
        if (this._playing) this._pause();
        else this._play();
    }
    _play() {
        this._playing = true;
        this._playBtn.textContent = '⏸ Pause';
        this._playTimer = setInterval(() => {
            if (this._ply >= this._maxPly()) this._pause();
            else this._goto(this._ply + 1);
        }, 900);
    }
    _pause() {
        this._playing = false;
        this._playBtn.textContent = '▶ Play';
        if (this._playTimer) { clearInterval(this._playTimer); this._playTimer = null; }
    }

    _renderMeta() {
        const g = this._game;
        clear(this._meta);
        this._meta.appendChild(
            h('div', {},
                h('div', { style: { display: 'flex', gap: 'var(--sp-2)', alignItems: 'center', marginBottom: 'var(--sp-3)' } },
                    h('div', { class: 'avatar' }, initials(g.white)),
                    h('div', { style: { flex: 1 } },
                        h('div', {}, g.white, h('span', { style: { color: 'var(--text-muted)', marginLeft: '6px' } }, formatElo(g.white_elo))),
                        h('div', { style: { color: 'var(--text-muted)', fontSize: 'var(--fs-xs)' } }, 'vs'),
                        h('div', {}, g.black, h('span', { style: { color: 'var(--text-muted)', marginLeft: '6px' } },
                            g.rated ? formatElo(g.black_elo) : 'Unrated')),
                    ),
                ),
                h('div', { style: { display: 'flex', gap: 'var(--sp-2)', flexWrap: 'wrap' } },
                    h('span', { class: 'badge' }, parseAndFormatTimeControl(g.time_control)),
                    h('span', { class: 'badge badge--accent' }, g.result || '*'),
                    h('span', { class: 'badge' }, reasonLabel(g.reason)),
                    !g.rated ? h('span', { class: 'badge' }, 'AI game · Unrated') : null,
                ),
            ),
        );
    }

    _renderMoves() {
        const g = this._game;
        clear(this._moveList);
        // Positions[0] is starting — skip it in the SAN column
        const sans = g.positions.slice(1).map(p => p.san);
        if (sans.length === 0) {
            this._moveList.appendChild(h('div', { class: 'move-list__empty' }, 'No moves recorded.'));
            return;
        }
        for (let i = 0; i < sans.length; i += 2) {
            this._moveList.appendChild(h('div', { class: 'move-list__num' }, (Math.floor(i / 2) + 1) + '.'));
            this._moveList.appendChild(this._moveEl(i + 1, sans[i]));
            this._moveList.appendChild(sans[i + 1]
                ? this._moveEl(i + 2, sans[i + 1])
                : h('span', {}, ''));
        }
    }
    _moveEl(ply, san) {
        return h('span', {
            class: 'move-list__san',
            dataset: { ply: String(ply) },
            onclick: () => this._goto(ply),
        }, san || '');
    }

    _highlightMove() {
        const all = this._moveList.querySelectorAll('.move-list__san');
        all.forEach(el => el.classList.remove('is-current'));
        const el = this._moveList.querySelector(`.move-list__san[data-ply="${this._ply}"]`);
        if (el) {
            el.classList.add('is-current');
            el.scrollIntoView({ block: 'nearest' });
        }
    }

    _currentFen() {
        return this._game && this._game.positions[this._ply] && this._game.positions[this._ply].fen;
    }

    _copyPgn() {
        if (!this._game) return;
        const g = this._game;
        const tags = [
            `[Event "${g.event || 'Casual Game'}"]`,
            `[White "${g.white}"]`,
            `[Black "${g.black}"]`,
            `[Result "${g.result || '*'}"]`,
            `[TimeControl "${g.time_control || '-'}"]`,
        ].join('\n');
        const moves = [];
        const sans = g.positions.slice(1).map(p => p.san);
        for (let i = 0; i < sans.length; i += 2) {
            moves.push(`${Math.floor(i / 2) + 1}.`, sans[i], sans[i + 1] || '');
        }
        const pgn = `${tags}\n\n${moves.join(' ').trim()} ${g.result || '*'}`;
        navigator.clipboard.writeText(pgn).then(
            () => this.ctx.toast.success('PGN copied to clipboard.', { duration: 2000 }),
            () => this.ctx.toast.danger('Copy failed.', { duration: 2000 }),
        );
    }

}
