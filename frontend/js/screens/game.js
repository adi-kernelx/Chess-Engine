/**
 * game.js — The live game screen.
 *
 * Orchestrates the board renderer, interaction, promotion picker,
 * sound, and the network layer into a playable UI.
 *
 * Design highlights:
 *
 *  ── Anchored clocks (fixes the old drift bug) ──
 *   Every server-sourced message that carries times becomes a NEW anchor:
 *     { whiteMs, blackMs, activeSide, anchoredAt: performance.now() }
 *   The display frame reads
 *     remaining = anchoredMs - (now - anchoredAt)
 *   for the active side, and the anchored value verbatim for the other.
 *   The Fischer increment arrives free — the server's next `move_made`
 *   already includes it.
 *
 *  ── Reconnect-into-game ──
 *   On mount (and on every socket 'connected' state), if we have an active
 *   store.game, fire Outbound.gameState() and rebuild board/clocks/moves
 *   from the reply. Refreshing the tab restores the game.
 *
 *  ── Server-authoritative ──
 *   The client never asserts move legality. The interaction hands us
 *   (from, to); we detect promotion via chess.js and pop the picker,
 *   then submit. Rejected moves reset the visual state.
 */

import { Screen } from '../ui/screen.js';
import { h, clear, icon } from '../core/dom.js';
import { BoardRenderer } from '../board/renderer.js';
import { BoardInteraction } from '../board/interaction.js';
import { BoardTheme, THEMES } from '../board/theme.js';
import { PromotionPicker } from '../board/promotion.js';
import {
    parseFen, findKingSquare, capturedPieces, materialBalance,
    isPromotion, START_FEN,
} from '../board/chess.js';
import { formatClock, reasonLabel, resultLabel, initials } from '../core/format.js';
import { absoluteUrl, copyText, copyImage, shareUrl, buildPGN } from '../core/share.js';

const LOW_TIME_MS = 20_000;

export class GameScreen extends Screen {

    constructor(ctx) {
        super(ctx);
        this._fen = START_FEN;
        this._moves = [];               // [{ san, thinkMs }]
        this._state = 'in_progress';    // waiting | in_progress | finished
        this._result = null;
        this._reason = null;
        this._sideToMove = 'w';         // derived from fen
        this._clock = {
            anchoredAt: performance.now(),
            whiteMs: 0,
            blackMs: 0,
            active: 'w',
        };
        this._lowTicked = null;         // last second we played the low-time tick for
        this._resizeObs = null;
    }

    render() {
        const game = this.ctx.store.game;
        if (!game) {
            // Bounce back to the play menu — no active game.
            this.timeout(() => this.ctx.router.go('/play'), 0);
            return h('div', { class: 'screen' },
                this.header('Game', 'No active game — returning to the play menu.'));
        }

        this._myColor = game.color;             // 'w' | 'b'
        this._opponent = game.opponent || 'Opponent';
        this._isAI = !!game.isAI;
        this._gameId = game.gameId;
        this._clock.whiteMs = game.whiteMs;
        this._clock.blackMs = game.blackMs;
        this._clock.anchoredAt = performance.now();
        this._clock.active = 'w'; // Always starts as white

        return h('div', {},
            h('div', { class: 'game-screen' },
                // ── Board column ──
                h('div', { class: 'game-board-col' },
                    this._playerBar('opponent'),
                    h('div', { class: 'board-frame' },
                        h('div', { class: 'board-container', ref: el => this._container = el },
                            h('canvas', {
                                class: 'board-canvas',
                                ref: el => this._canvas = el,
                                role: 'img',
                                'aria-label': 'Chess board',
                                tabindex: '0',
                            })
                        )
                    ),
                    this._playerBar('me'),
                    h('div', { class: 'status-line', ref: el => this._statusEl = el }, this._statusText())
                ),

                // ── Side panel ──
                h('div', { class: 'game-side' },
                    h('div', { class: 'card' },
                        h('div', { class: 'card__header' },
                            icon('replay', 'icon--sm'),
                            h('div', { class: 'card__title' }, 'Moves'),
                            h('span', { style: { marginLeft: 'auto', fontSize: 'var(--fs-xs)', color: 'var(--text-muted)' }, ref: el => this._moveCountEl = el }, '0')
                        ),
                        h('div', { class: 'move-list', ref: el => this._moveListEl = el },
                            h('div', { class: 'move-list__empty' }, 'No moves yet — good luck!')
                        )
                    ),

                    h('div', { class: 'card' },
                        h('div', { class: 'card__body' },
                            h('div', { class: 'game-actions', ref: el => this._actionsEl = el })
                        )
                    ),
                )
            )
        );
    }

    /* ── Player bars ────────────────────────────────────── */
    _playerBar(which) {
        const isMe = which === 'me';
        const displayName = isMe
            ? (this.ctx.store.session.username || 'You')
            : this._opponent;

        // Which color the bar represents given board orientation.
        // We always show OPPONENT at the top of the screen (which is
        // the top of the board when whiteOnBottom matches myColor='w').
        return h('div', { class: 'player-bar' },
            h('div', { class: 'avatar' }, initials(displayName)),
            h('div', { class: 'player-bar__name-block' },
                h('div', { class: 'player-bar__name' }, displayName + (this._isAI && !isMe ? ' (AI)' : '')),
                h('div', { class: 'player-bar__captured', ref: el => isMe ? (this._myCaptured = el) : (this._oppCaptured = el) })
            ),
            h('div', { class: 'clock', ref: el => isMe ? (this._myClockEl = el) : (this._oppClockEl = el) },
                '--:--')
        );
    }

    /* ── Lifecycle ───────────────────────────────────────── */

    onMount() {
        if (!this.ctx.store.game) return;

        const { socket, capability, Outbound, Inbound, sound } = this.ctx;

        // Renderer, interaction, promotion, sound.
        this.renderer = new BoardRenderer(this._canvas, this._selectedTheme());
        this.interaction = new BoardInteraction(this._canvas, this.renderer);
        this.promotion = new PromotionPicker(this._container, this.renderer);

        // Apply piece set from prefs.
        const set = this.ctx.store.prefs.pieceSet || 'classic';
        this.renderer.pieceSetUrl = `assets/pieces/${set}.svg`;

        // Orient so that my color is at the bottom.
        this.renderer.setOrientation(this._myColor === 'w');
        this.interaction.setPlayerColor(this._myColor);

        // Move intent → server round-trip (with promotion picker).
        this.interaction.onMoveIntent = async (from, to) => {
            const parsed = parseFen(this._fen);
            if (!parsed || parsed.sideToMove !== this._myColor) return;
            let promo = null;
            if (isPromotion(from, to, parsed)) {
                promo = await this.promotion.ask(to, this._myColor);
                if (!promo) return;
            }
            socket.send(Outbound.makeMove(from, to, promo));
        };

        // Board sizing. Try synchronous first — the CSS aspect-ratio
        // rule means the container has real dimensions the moment we
        // mount. Fall back to a rAF + setTimeout race if the initial
        // measurement is 0 (e.g. the parent is still styling).
        this._resizeObs = new ResizeObserver(entries => {
            for (const e of entries) {
                const size = Math.min(e.contentRect.width, e.contentRect.height);
                if (size > 0) this.renderer.resize(size);
            }
        });
        this._resizeObs.observe(this._container);
        this._boot(socket, Outbound);

        // Server subscriptions.
        this.sub(socket.on('move_made',   raw => this._onMoveMade(Inbound.normalize(raw))));
        this.sub(socket.on('move_rejected', raw => this._onMoveRejected(Inbound.normalize(raw))));
        this.sub(socket.on('game_state', raw => this._onGameState(Inbound.normalize(raw))));
        this.sub(socket.on('game_over',  raw => this._onGameOver(Inbound.normalize(raw))));
        this.sub(socket.on('error',      raw => this._onError(Inbound.normalize(raw))));

        // Reconnect-into-game: any new 'connected' → re-request state.
        this.sub(socket.onState(s => {
            if (s === 'connected') socket.send(Outbound.gameState());
        }));

        // Clock ticker.
        this._tickHandle = this.interval(() => this._tickClocks(), 100);

        // G2: make this game's URL shareable. If the route was #/game (no id)
        // but we have a real gameId in the store, rewrite the hash silently
        // — refresh, back-button, and paste all become well-behaved.
        if (this._gameId && !location.hash.startsWith('#/game/')) {
            try { history.replaceState(null, '', '#/game/' + this._gameId); } catch (_) {}
        }

        // Actions.
        this._renderActions();
        this._updateStatus();

        this.ctx.sound.gameStart();
    }

    onUnmount() {
        if (this._resizeObs) this._resizeObs.disconnect();
        if (this.interaction) this.interaction.destroy();
        if (this.promotion) this.promotion.close();
    }

    /** Race between sync/rAF/timeout so the board always sizes even in
     *  environments where rAF is throttled (background tabs, headless). */
    _boot(socket, Outbound) {
        let booted = false;
        const doIt = () => {
            if (booted) return;
            const rect = this._container.getBoundingClientRect();
            const size = Math.min(rect.width, rect.height);
            if (size <= 0) return;
            booted = true;
            this.renderer.resize(size).then(() => {
                this.renderer.setPosition(this._fen);
                if (socket.isConnected()) socket.send(Outbound.gameState());
            });
        };
        doIt();                              // synchronous
        if (!booted) requestAnimationFrame(doIt);
        this.timeout(doIt, 50);              // safety net
        this.timeout(doIt, 250);
    }

    _selectedTheme() {
        const key = this.ctx.store.prefs.boardTheme || 'classic';
        return (THEMES[key] || THEMES.classic).factory();
    }

    /* ── Server events ───────────────────────────────────── */

    _onMoveMade(msg) {
        // Update visual state, animate the piece, then request the FEN so
        // the position is always the server's ground truth (esp. for
        // castling, en passant, promotion).
        const wasCapture = !!(this.renderer.squares[algIdx(msg.to)]);
        const prevSide = this._sideToMove;
        this._moves.push({ san: msg.san, thinkMs: null });
        this._renderMoveList();

        // Re-anchor the clock.
        this._reanchorClock({
            whiteMs: msg.whiteMs,
            blackMs: msg.blackMs,
            active: prevSide === 'w' ? 'b' : 'w',
        });

        this.renderer.setLastMove(msg.from, msg.to);
        this.renderer.clearCheck();
        this.renderer.clearSelected();

        // Animate — we don't yet know the follow-up FEN, but we can move
        // the piece visually. When game_state lands (below), we snap to
        // the authoritative position.
        this.renderer.animateMove(msg.from, msg.to, () => {
            this.ctx.socket.send(this.ctx.Outbound.gameState());
        });
        this._sideToMove = prevSide === 'w' ? 'b' : 'w';
        this._updateStatus();

        // Sound + haptics driven by SAN.
        this.ctx.sound.forMove(msg.san);
        void wasCapture;
    }

    _onMoveRejected(msg) {
        this.ctx.sound.illegal();
        this.ctx.toast.danger(msg.reason || 'Illegal move.', { duration: 2400 });
        // Refresh in case the client's view diverged.
        this.ctx.socket.send(this.ctx.Outbound.gameState());
    }

    _onGameState(msg) {
        // The authoritative snapshot. Rebuilds everything.
        this._fen = msg.fen;
        this._state = msg.state;
        this._result = msg.result || null;
        this._reason = msg.reason || null;
        this._moves = msg.moves.slice();
        this.renderer.setPosition(msg.fen);
        const parsed = parseFen(msg.fen);
        this._sideToMove = parsed.sideToMove;
        this._reanchorClock({
            whiteMs: msg.whiteMs,
            blackMs: msg.blackMs,
            active: this._sideToMove,
        });
        // Interaction: only accept moves when it's my turn and the game is live.
        const canMove = this._state === 'in_progress' && this._sideToMove === this._myColor;
        this.interaction.setEnabled(canMove);

        // Check indicator: king in check?
        this._maybeHighlightCheck(parsed);

        // Captured pieces + material advantage.
        this._renderCaptured(parsed);

        this._renderMoveList();
        this._updateStatus();

        if (this._state === 'finished' && this._result) {
            this._presentResult();
        }

        this._renderActions();
    }

    _maybeHighlightCheck(parsed) {
        // We don't have a legal-move generator client-side; use SAN suffix
        // as a heuristic instead.
        const last = this._moves[this._moves.length - 1];
        this.renderer.clearCheck();
        if (!last || !last.san) return;
        if (!(/[+#]$/.test(last.san))) return;
        const kSq = findKingSquare(parsed, this._sideToMove);
        if (kSq) this.renderer.setCheck(kSq);
    }

    _onGameOver(msg) {
        this._state = 'finished';
        this._result = msg.result;
        this._reason = msg.reason;
        this.ctx.sound.gameEnd();
        this.interaction.setEnabled(false);
        this._presentResult();
        this._updateStatus();
        this._renderActions();
    }

    _onError(msg) {
        if (msg.isUnknownType) return;
        this.ctx.toast.danger(msg.message);
    }

    /* ── Clocks — anchored ───────────────────────────────── */

    _reanchorClock({ whiteMs, blackMs, active }) {
        this._clock.whiteMs = whiteMs;
        this._clock.blackMs = blackMs;
        this._clock.active = active;
        this._clock.anchoredAt = performance.now();
        this._lowTicked = null;
        this._tickClocks();
    }

    _tickClocks() {
        if (this._state !== 'in_progress') {
            this._paintClock(this._myClockEl, this._myClockMs(), false, false);
            this._paintClock(this._oppClockEl, this._oppClockMs(), false, false);
            return;
        }
        const now = performance.now();
        const elapsed = now - this._clock.anchoredAt;
        const white = this._clock.active === 'w' ? Math.max(0, this._clock.whiteMs - elapsed) : this._clock.whiteMs;
        const black = this._clock.active === 'b' ? Math.max(0, this._clock.blackMs - elapsed) : this._clock.blackMs;
        const activeMs = this._clock.active === 'w' ? white : black;

        this._paintClock(this._myClockEl,
            this._myColor === 'w' ? white : black,
            this._clock.active === this._myColor,
            (this._clock.active === this._myColor) && activeMs < LOW_TIME_MS
        );
        this._paintClock(this._oppClockEl,
            this._myColor === 'w' ? black : white,
            this._clock.active !== this._myColor,
            (this._clock.active !== this._myColor) && activeMs < LOW_TIME_MS
        );

        // Low-time tick (once per second in the last 10s of my clock).
        if (this._clock.active === this._myColor && activeMs < 10_000 && activeMs > 0) {
            const sec = Math.floor(activeMs / 1000);
            if (this._lowTicked !== sec) {
                this._lowTicked = sec;
                this.ctx.sound.lowTime();
            }
        }
    }

    _paintClock(el, ms, active, low) {
        if (!el) return;
        el.textContent = formatClock(ms);
        el.classList.toggle('is-active', !!active);
        el.classList.toggle('is-low', !!low);
    }

    _myClockMs() { return this._myColor === 'w' ? this._clock.whiteMs : this._clock.blackMs; }
    _oppClockMs() { return this._myColor === 'w' ? this._clock.blackMs : this._clock.whiteMs; }

    /* ── Status line ─────────────────────────────────────── */

    _statusText() {
        if (this._state === 'finished') return this._resultLine();
        if (this._state === 'waiting') return 'Waiting for opponent…';
        if (this._sideToMove === this._myColor) return 'Your move';
        return `${this._opponent}${this._isAI ? '' : ''} is thinking…`;
    }
    _updateStatus() {
        if (!this._statusEl) return;
        this._statusEl.textContent = this._statusText();
        this._statusEl.classList.remove('is-turn', 'is-check', 'is-over', 'is-waiting');
        if (this._state === 'finished')                 this._statusEl.classList.add('is-over');
        else if (this._state === 'waiting')             this._statusEl.classList.add('is-waiting');
        else if (this._sideToMove === this._myColor)    this._statusEl.classList.add('is-turn');
    }
    _resultLine() {
        if (!this._result) return 'Game over';
        return `${resultLabel(this._result, this._myColor)} · ${reasonLabel(this._reason)}`;
    }

    /* ── Move list ───────────────────────────────────────── */

    _renderMoveList() {
        if (!this._moveListEl) return;
        clear(this._moveListEl);

        if (this._moves.length === 0) {
            this._moveListEl.appendChild(h('div', { class: 'move-list__empty' }, 'No moves yet — good luck!'));
            this._moveCountEl.textContent = '0';
            return;
        }
        this._moveCountEl.textContent = String(this._moves.length);

        for (let i = 0; i < this._moves.length; i += 2) {
            const num = Math.floor(i / 2) + 1;
            const isLastPair = (i + 1 >= this._moves.length - 1) && i + 1 >= this._moves.length - 1;
            const whiteIdx = i;
            const blackIdx = i + 1;
            this._moveListEl.appendChild(h('div', { class: 'move-list__num' }, `${num}.`));
            this._moveListEl.appendChild(h('span', {
                class: 'move-list__san' + (whiteIdx === this._moves.length - 1 ? ' is-current' : ''),
            }, this._moves[whiteIdx] ? this._moves[whiteIdx].san : ''));
            this._moveListEl.appendChild(h('span', {
                class: 'move-list__san' + (blackIdx === this._moves.length - 1 ? ' is-current' : ''),
            }, this._moves[blackIdx] ? this._moves[blackIdx].san : ''));
            void isLastPair;
        }
        this._moveListEl.scrollTop = this._moveListEl.scrollHeight;
    }

    /* ── Captured / material ─────────────────────────────── */

    _renderCaptured(parsed) {
        const cap = capturedPieces(parsed);
        const bal = materialBalance(parsed);
        // Show pieces I've captured (from opponent) in MY bar, and vice versa.
        const opponentColor = this._myColor === 'w' ? 'b' : 'w';
        this._paintCapturedInto(this._myCaptured, cap[opponentColor], opponentColor,
            this._myColor === 'w' ?  bal : -bal);
        this._paintCapturedInto(this._oppCaptured, cap[this._myColor], this._myColor,
            this._myColor === 'w' ? -bal :  bal);
    }
    _paintCapturedInto(root, list, capturedColor, advantage) {
        if (!root) return;
        clear(root);
        for (const t of list) {
            const key = `${capturedColor}_${t}`;
            const img = this.renderer.rasters[key];
            if (!img) continue;
            const clone = new Image();
            clone.src = img.src;
            root.appendChild(clone);
        }
        if (advantage > 0) {
            root.appendChild(h('span', { class: 'player-bar__adv' }, `+${advantage}`));
        }
    }

    /* ── Actions ─────────────────────────────────────────── */

    _renderActions() {
        if (!this._actionsEl) return;
        clear(this._actionsEl);
        if (this._state === 'in_progress') {
            this._actionsEl.appendChild(h('button', {
                class: 'btn btn--danger',
                onclick: () => this._onResign(),
            }, 'Resign'));
            this._actionsEl.appendChild(h('button', {
                class: 'btn',
                onclick: () => this._onDrawOffer(),
                title: 'Offer a draw (backend pending — see log)',
            }, 'Offer draw'));
            // G2: share button on human games only (nothing to share with the AI).
            if (!this._isAI && this._gameId) {
                this._actionsEl.appendChild(h('button', {
                    class: 'btn',
                    onclick: () => this._onShareGame(),
                    title: 'Copy or share the link to this game',
                }, 'Share'));
            }
            this._actionsEl.appendChild(h('button', {
                class: 'btn btn--ghost',
                onclick: () => this.renderer.flip(),
            }, 'Flip board'));
        } else {
            this._actionsEl.appendChild(h('button', {
                class: 'btn btn--primary',
                onclick: () => this._onRematch(),
            }, 'Rematch'));
            this._actionsEl.appendChild(h('button', {
                class: 'btn',
                onclick: () => this.ctx.router.go('/play'),
            }, 'Back to lobby'));
        }
    }

    async _onResign() {
        const ok = await this.ctx.modal.confirm({
            title: 'Resign game',
            message: 'This ends the game and gives your opponent the win.',
            confirmLabel: 'Resign',
            danger: true,
        });
        if (ok) this.ctx.socket.send(this.ctx.Outbound.resign());
    }

    _onDrawOffer() {
        // Backend does not yet have a draw_offer message. capability.js
        // would probe; here we just fire-and-forget with a friendly toast.
        this.ctx.toast.info('Draw offers arrive when Phase 7 authentication ships — logged as backend work.',
            { duration: 3600 });
    }

    _onRematch() {
        const game = this.ctx.store.game;
        if (!game) { this.ctx.router.go('/'); return; }
        // Rematch is not a native server message. Best-effort: create a fresh
        // game with the same time control, or if it was AI, restart against
        // the same difficulty and time control.
        this.ctx.store.setGame(null);
        if (game.isAI) {
            this.ctx.socket.send(this.ctx.Outbound.playAI(
                this.ctx.store.session.username || 'Player',
                game.difficulty || 'medium',
                game.timeBaseSec, game.timeIncSec,
            ));
        } else {
            this.ctx.socket.send(this.ctx.Outbound.createGame(
                this.ctx.store.session.username || 'Player',
                game.timeBaseSec, game.timeIncSec,
            ));
            this.ctx.toast.info('Rematch is not a native server action yet — a new open game was created.',
                { title: 'Rematch', duration: 3600 });
            this.ctx.router.go('/');
        }
    }

    /* ── Game over modal ─────────────────────────────────── */

    _presentResult() {
        if (this._resultShown) return;
        this._resultShown = true;

        const won  = (this._result === '1-0' && this._myColor === 'w') || (this._result === '0-1' && this._myColor === 'b');
        const lost = (this._result === '1-0' && this._myColor === 'b') || (this._result === '0-1' && this._myColor === 'w');
        const draw = this._result === '1/2-1/2';
        const badgeClass = won ? 'result-card__badge--won' : draw ? 'result-card__badge--draw' : 'result-card__badge--lost';
        const badgeText  = won ? 'Victory' : draw ? 'Draw' : lost ? 'Defeat' : 'Ended';

        const me  = (this.ctx.store.session.username || 'Player');
        const white = this._myColor === 'w' ? me : this._opponent;
        const black = this._myColor === 'w' ? this._opponent : me;

        const body = h('div', { class: 'result-card' },
            h('span', { class: 'result-card__badge ' + badgeClass }, badgeText),
            h('div', { class: 'result-card__title' }, resultLabel(this._result, this._myColor)),
            h('div', { class: 'result-card__reason' }, reasonLabel(this._reason)),
            h('div', { class: 'result-card__meta mono' }, `${white} vs ${black} · ${this._moves.length} plies`),
            // G2: share row — copy PGN, copy image, invite a friend.
            h('div', { class: 'result-card__share' },
                h('button', {
                    class: 'btn btn--sm',
                    onclick: () => this._copyPgn(white, black),
                }, 'Copy PGN'),
                h('button', {
                    class: 'btn btn--sm',
                    onclick: () => this._copyBoardImage(),
                    title: 'Copy the final board as an image',
                }, 'Copy image'),
                h('button', {
                    class: 'btn btn--sm',
                    onclick: () => this._inviteFriend(),
                    title: 'Post a fresh game and share the link',
                }, 'Invite a friend'),
            ),
        );
        const footer = h('div', { style: { display: 'flex', gap: '8px' } },
            h('button', { class: 'btn', onclick: () => { dlg.close(); this.ctx.router.go('/play'); } }, 'Back to lobby'),
            h('button', { class: 'btn btn--primary', onclick: () => { dlg.close(); this._onRematch(); } }, 'Rematch'),
        );
        const dlg = this.ctx.modal.open({
            title: 'Game over',
            body, footer,
            dismissible: true,
        });
    }

    /* ── G2 share helpers ─────────────────────────────────── */

    async _onShareGame() {
        if (!this._gameId) return;
        const url = absoluteUrl('#/join/' + this._gameId);
        const shared = await shareUrl({
            title: 'Play me in chess',
            text: 'Join my game — one click, no signup.',
            url,
        });
        if (shared.ok) return;
        const copied = await copyText(url);
        if (copied.ok) this.ctx.toast.success('Invite link copied.', { duration: 2000 });
        else this.ctx.toast.warning('Copy blocked — link: ' + url, { duration: 5000 });
    }

    async _copyPgn(white, black) {
        const pgn = buildPGN({
            white, black,
            result: this._result || '*',
            timeControl: `${(this.ctx.store.game.timeBaseSec || 0)}+${(this.ctx.store.game.timeIncSec || 0)}`,
            moves: this._moves,
        });
        const res = await copyText(pgn);
        if (res.ok) this.ctx.toast.success('PGN copied.', { duration: 1800 });
        else this.ctx.toast.warning('Copy blocked by the browser.', { duration: 2400 });
    }

    _copyBoardImage() {
        // Draw the current canvas into a fresh 600×600 offscreen with a matte
        // background so it looks presentable when pasted into a chat app.
        const src = this.renderer && this.renderer.canvas;
        if (!src) { this.ctx.toast.warning('The board is not ready.', { duration: 2000 }); return; }
        const size = 600;
        const off = document.createElement('canvas');
        off.width = size; off.height = size;
        const g = off.getContext('2d');
        g.fillStyle = getComputedStyle(document.documentElement).getPropertyValue('--surface-1').trim() || '#17191c';
        g.fillRect(0, 0, size, size);
        // The source canvas is a square in device pixels — scale to fit.
        g.drawImage(src, 0, 0, src.width, src.height, 0, 0, size, size);
        off.toBlob(async (blob) => {
            if (!blob) { this.ctx.toast.warning('Could not build the image.', { duration: 2400 }); return; }
            const res = await copyImage(blob);
            if (res.ok) { this.ctx.toast.success('Image copied.', { duration: 1800 }); return; }
            // Fallback: open a data URL in a new tab so the user can save/right-click.
            const url = URL.createObjectURL(blob);
            const win = window.open(url, '_blank', 'noopener');
            if (!win) this.ctx.toast.warning('Image ready — paste is not supported here.', { duration: 3000 });
            else this.ctx.toast.info('Image opened in a new tab — right-click to save.', { duration: 3200 });
            // Revoke on next tick so the new tab keeps the blob alive.
            setTimeout(() => URL.revokeObjectURL(url), 30_000);
        }, 'image/png');
    }

    _inviteFriend() {
        // Post a fresh game and hand the link off to the user. The lobby
        // already handles the toast + copy flow when game_created lands.
        const game = this.ctx.store.game;
        const base = game && game.timeBaseSec ? game.timeBaseSec : 300;
        const inc  = game && game.timeIncSec  ? game.timeIncSec  : 3;
        this.ctx.store.setGame(null);
        this.ctx.socket.send(this.ctx.Outbound.createGame(
            this.ctx.store.session.username || 'Player', base, inc,
        ));
        this.ctx.router.go('/play');
    }
}

/** Local helper: algebraic → 0..63. Duplicates chess.js's version so this
 *  file doesn't have to import both directly, but same behaviour. */
function algIdx(sq) {
    if (!sq || sq.length !== 2) return -1;
    const file = sq.charCodeAt(0) - 97;
    const rank = parseInt(sq[1], 10) - 1;
    return (file >= 0 && file < 8 && rank >= 0 && rank < 8) ? rank * 8 + file : -1;
}
