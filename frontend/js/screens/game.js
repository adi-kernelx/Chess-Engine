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
        this._stateReady = false;
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
        this._opponentOffline = false;
        this._legalTargets = new Map();
        this._drawOfferPending = false;
        this._drawOfferSending = false;
        this._incomingDrawOffer = false;
        this._rematchOfferPending = false;
        this._rematchOfferSending = false;
        this._incomingRematchOffer = false;
        this._rematchResponding = false;
        this._aiRematchRequested = false;
        this._resultDialog = null;
        this._activeRecoveryPending = false;
        this._activeRecoveryTimer = null;
    }

    render() {
        const game = this.ctx.store.game;
        if (!game) {
            // Bounce back to the play menu — no active game.
            this.timeout(() => this.ctx.router.go('/play'), 0);
            return h('div', { class: 'screen' },
                this.header('Game', 'No active game — returning to the play menu.'));
        }

        // Keep the completed-game context after Store clears activeGame. The
        // room ID and time control are required for rematch negotiation and
        // PGN export while this finished screen remains mounted.
        this._game = game;

        this._myColor = game.color;             // 'w' | 'b'
        this._opponent = game.opponent || 'Opponent';
        this._isAI = !!game.isAI;
        this._gameId = game.gameId;
        this._tournamentId = Number(game.tournamentId || 0) || null;
        this._pairingId = Number(game.pairingId || 0) || null;
        this._tournamentName = game.tournamentName || '';
        this._tournamentRound = Number(game.tournamentRound || 0) || null;
        this._clock.whiteMs = game.whiteMs;
        this._clock.blackMs = game.blackMs;
        this._clock.anchoredAt = performance.now();
        this._clock.active = 'w'; // Always starts as white

        return h('div', {},
            this._tournamentId ? h('div', { class: 'game-tournament-context', role: 'status' },
                h('div', {},
                    h('strong', {}, this._tournamentName || 'Tournament game'),
                    this._tournamentRound ? ` · Round ${this._tournamentRound}` : ''),
                h('a', { class: 'btn btn--sm btn--ghost', href: `#/tournaments/${this._tournamentId}` },
                    'Tournament')) : null,
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
            h('div', { class: 'avatar', ref: el => { if (!isMe) this._oppAvatarEl = el; } }, initials(displayName)),
            h('div', { class: 'player-bar__name-block' },
                h('div', { class: 'player-bar__name', ref: el => { if (!isMe) this._oppNameEl = el; } }, displayName + (this._isAI && !isMe ? ' (AI)' : '')),
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
        this.renderer.onRastersReady = () => {
            const parsed = parseFen(this._fen);
            if (parsed) this._renderCaptured(parsed);
        };
        this.interaction = new BoardInteraction(this._canvas, this.renderer);
        this.promotion = new PromotionPicker(this._container, this.renderer);

        // Apply piece set from prefs.
        const set = this.ctx.store.prefs.pieceSet || 'classic';
        this.renderer.pieceSetUrl = `assets/pieces/${set}.svg`;

        // Orient so that my color is at the bottom.
        this.renderer.setOrientation(this._myColor === 'w');
        this.interaction.setPlayerColor(this._myColor);
        this.interaction.legalTargetsProvider = from => this._legalTargets.get(from) || [];

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
        this.sub(socket.on('tournament_game_ready', raw => this._onTournamentReady(Inbound.normalize(raw))));
        this.sub(socket.on('active_game', raw => this._onActiveGame(Inbound.normalize(raw))));
        this.sub(socket.on('game_over',  raw => this._onGameOver(Inbound.normalize(raw))));
        this.sub(socket.on('opponent_disconnected', raw => this._onOpponentDisconnected(raw)));
        this.sub(socket.on('opponent_reconnected', () => this._onOpponentReconnected()));
        this.sub(socket.on('draw_offer_sent', () => this._onDrawOfferSent()));
        this.sub(socket.on('draw_offered', raw => this._onDrawOffered(raw)));
        this.sub(socket.on('draw_declined', raw => this._onDrawDeclined(raw)));
        this.sub(socket.on('draw_offer_resolved', raw => this._onDrawOfferResolved(raw)));
        this.sub(socket.on('rematch_offer_sent', () => this._onRematchOfferSent()));
        this.sub(socket.on('rematch_offered', raw => this._onRematchOffered(raw)));
        this.sub(socket.on('rematch_declined', raw => this._onRematchDeclined(raw)));
        this.sub(socket.on('rematch_offer_resolved', raw => this._onRematchOfferResolved(raw)));
        this.sub(socket.on('rematch_started', raw => this._onRematchStarted(Inbound.normalize(raw))));
        this.sub(socket.on('game_start', raw => {
            if (this._aiRematchRequested) this._onAiRematchStarted(Inbound.normalize(raw));
        }));
        this.sub(socket.on('error',      raw => this._onError(Inbound.normalize(raw))));

        // Reconnect-into-game: any new 'connected' → re-request state.
        this.sub(socket.onState(s => {
            if (s === 'connected') this._requestState();
        }));
        if (this.ctx.session) {
            this.sub(this.ctx.session.on('change', () => this._requestState()));
        }

        // Clock ticker.
        this._tickHandle = this.interval(() => this._tickClocks(), 100);
        // A newly materialized tournament room may reach the game route while
        // auth/database work is briefly busy. Retry the idempotent snapshot
        // until the first authoritative state arrives instead of leaving the
        // board inert after one lost or delayed request.
        this.interval(() => {
            if (!this._stateReady && this._state !== 'finished') this._requestState();
        }, 1000);

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
        if (this._activeRecoveryTimer) clearTimeout(this._activeRecoveryTimer);
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
                if (socket.isConnected()) this._requestState();
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
        if (this._incomingDrawOffer && prevSide === this._myColor) {
            this._incomingDrawOffer = false;
        }
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

        // The accepted move already carries the authoritative post-move FEN
        // and the next side's legal moves. Cache the targets before showing
        // the turn, then finish the animation against the old pixels and snap
        // to that FEN. Move submission never waits on this cache.
        if (msg.fen) this._fen = msg.fen;
        this._cacheLegalMoves(msg.legalMoves);
        this.renderer.animateMove(msg.from, msg.to, () => {
            if (msg.fen) {
                this.renderer.setPosition(msg.fen);
                const parsed = parseFen(msg.fen);
                this._sideToMove = parsed.sideToMove;
                this._maybeHighlightCheck(parsed);
                this._renderCaptured(parsed);
                this.interaction.setEnabled(
                    this._state === 'in_progress' && this._sideToMove === this._myColor);
                this._updateStatus();
            }
            // Background reconciliation retains move timings and protects
            // against a dropped/out-of-order frame without blocking input.
            this._requestState();
        });
        this._sideToMove = msg.fen ? parseFen(msg.fen).sideToMove : (prevSide === 'w' ? 'b' : 'w');
        this._updateStatus();

        // Sound + haptics driven by SAN.
        this.ctx.sound.forMove(msg.san);
        void wasCapture;
    }

    _onMoveRejected(msg) {
        this.ctx.sound.illegal();
        this.ctx.toast.danger(msg.reason || 'Illegal move.', { duration: 2400 });
        // Refresh in case the client's view diverged.
        this._requestState();
    }

    _onTournamentReady(msg) {
        if (Number(msg.gameId) !== Number(this._gameId)) return;
        this._requestState();
    }

    _onGameState(msg) {
        if (Number(msg.gameId) !== Number(this._gameId)) return;
        if (this._state === 'finished' && msg.state !== 'finished') return;
        this._stateReady = true;
        const opponent = this._myColor === 'w' ? msg.blackUsername : msg.whiteUsername;
        if (opponent) {
            this._opponent = opponent;
            if (this._oppNameEl) this._oppNameEl.textContent = opponent + (this._isAI ? ' (AI)' : '');
            if (this._oppAvatarEl) this._oppAvatarEl.textContent = initials(opponent);
            this._game = { ...this._game, opponent };
            if (this.ctx.store.game) this.ctx.store.setGame({ ...this.ctx.store.game, opponent });
        }
        // The authoritative snapshot. Rebuilds everything.
        this._fen = msg.fen;
        this._state = msg.state;
        this._result = msg.result || null;
        this._reason = msg.reason || null;
        this._moves = msg.moves.slice();
        this._cacheLegalMoves(msg.legalMoves);
        this._syncDrawOffer(msg.drawOfferFrom);
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
            this.ctx.store.setGame(null);
            this._presentResult();
        }

        this._renderActions();
    }

    _cacheLegalMoves(uciMoves) {
        const grouped = new Map();
        for (const uci of (uciMoves || [])) {
            if (!/^[a-h][1-8][a-h][1-8][qrbn]?$/.test(uci)) continue;
            const from = uci.slice(0, 2);
            const to = uci.slice(2, 4);
            const targets = grouped.get(from) || [];
            if (!targets.includes(to)) targets.push(to);
            grouped.set(from, targets);
        }
        this._legalTargets = grouped;
    }

    _maybeHighlightCheck(parsed) {
        // Check highlighting follows the authoritative SAN suffix.
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
        this.ctx.store.setGame(null);
        this._presentResult();
        this._updateStatus();
        this._renderActions();
    }

    _onError(msg) {
        if (this._drawOfferSending) {
            this._drawOfferSending = false;
            this._renderActions();
        }
        if (this._rematchOfferSending || this._rematchResponding) {
            this._rematchOfferSending = false;
            this._rematchResponding = false;
            this._incomingRematchOffer = false;
            this._renderActions();
        }
        if (msg.isUnknownType) return;
        if (/not in a game/i.test(msg.message || '')) {
            // A queued state request can arrive after the terminal event.
            // Keep the final position until the player chooses to leave.
            if (this._state === 'finished') return;
            // During matchmaking/reconnect, one fd-scoped state request may
            // race the durable seat binding. Verify by authenticated identity
            // before clearing the board or navigating away.
            this._recoverActiveGame();
            return;
        }
        this.ctx.toast.danger(msg.message);
    }

    _recoverActiveGame() {
        if (this._activeRecoveryPending || this._state === 'finished') return;
        const token = this.ctx.session && this.ctx.session.accessToken;
        if (!token || !this.ctx.socket.isConnected()) {
            if (this._statusEl) this._statusEl.textContent = 'Reconnecting to your game…';
            return;
        }
        this._activeRecoveryPending = true;
        if (this._statusEl) {
            this._statusEl.textContent = 'Confirming your active game…';
            this._statusEl.setAttribute('aria-busy', 'true');
        }
        this.ctx.socket.send(this.ctx.Outbound.activeGame(token));
        this._activeRecoveryTimer = setTimeout(() => {
            this._activeRecoveryTimer = null;
            this._activeRecoveryPending = false;
            if (!this.root || this._state === 'finished') return;
            if (this._statusEl) {
                this._statusEl.removeAttribute('aria-busy');
                this._statusEl.textContent = 'Still reconnecting — your game has not been abandoned.';
            }
        }, 3000);
    }

    _onActiveGame(msg) {
        if (!this._activeRecoveryPending) return;
        this._activeRecoveryPending = false;
        if (this._activeRecoveryTimer) {
            clearTimeout(this._activeRecoveryTimer);
            this._activeRecoveryTimer = null;
        }
        if (this._statusEl) this._statusEl.removeAttribute('aria-busy');

        const game = msg && msg.game;
        if (!game || !game.gameId) {
            this.ctx.store.setGame(null);
            this.ctx.toast.warning(
                'This game is no longer active. Its result is available in Replays.',
                { duration: 4200 });
            this.ctx.router.go('/replay');
            return;
        }

        this.ctx.store.setGame({
            gameId: game.gameId,
            color: game.color === 'b' ? 'b' : 'w',
            opponent: game.opponent || 'Opponent',
            isAI: !!game.isAI,
            whiteMs: game.whiteMs,
            blackMs: game.blackMs,
            timeBaseSec: game.timeBaseSec,
            timeIncSec: game.timeIncSec,
            difficulty: this._game && this._game.difficulty || null,
            tournamentId: game.tournamentId || this._game && this._game.tournamentId || null,
            pairingId: game.pairingId || this._game && this._game.pairingId || null,
            tournamentName: this._game && this._game.tournamentName || '',
            tournamentRound: this._game && this._game.tournamentRound || null,
        });

        if (Number(game.gameId) !== Number(this._gameId)) {
            this.ctx.router.go('/game/' + game.gameId);
            return;
        }

        // get_active_game has rebound this socket to the durable seat. Fetch
        // the full authoritative board/moves/clocks now that the binding is
        // guaranteed to exist.
        this._requestState();
    }

    _requestState() {
        if (this._state === 'finished') return;
        const token = this.ctx.session && this.ctx.session.accessToken;
        if (!token || !this.ctx.socket.isConnected()) return;
        this.ctx.socket.send(this.ctx.Outbound.gameState(token));
    }

    _onOpponentDisconnected(raw) {
        this._opponentOffline = true;
        const seconds = Math.max(1, Math.ceil(Number(raw.grace_ms || 120_000) / 1000));
        this.ctx.toast.warning(`${this._opponent} disconnected. They have ${seconds} seconds to return.`,
            { duration: 5000 });
        this._updateStatus();
    }

    _onOpponentReconnected() {
        this._opponentOffline = false;
        this.ctx.toast.success(`${this._opponent} reconnected.`, { duration: 2400 });
        this._updateStatus();
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
        if (this._opponentOffline) return `${this._opponent} disconnected · waiting up to 120 seconds…`;
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
            const thumbnail = this.renderer.createPieceThumbnail(key);
            if (thumbnail) root.appendChild(thumbnail);
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
            if (this._incomingDrawOffer) {
                this._actionsEl.appendChild(h('div', {
                    class: 'game-draw-offer',
                    role: 'status',
                    'aria-live': 'polite',
                }, `${this._opponent} offered a draw. It expires after 45 seconds; respond or keep playing.`));
                this._actionsEl.appendChild(h('button', {
                    class: 'btn btn--primary',
                    onclick: () => this._respondToDraw(true),
                }, 'Accept draw'));
                this._actionsEl.appendChild(h('button', {
                    class: 'btn',
                    onclick: () => this._respondToDraw(false),
                }, 'Decline draw'));
            } else {
                this._actionsEl.appendChild(h('button', {
                    class: 'btn',
                    onclick: () => this._onDrawOffer(),
                    disabled: this._isAI || this._drawOfferPending || this._drawOfferSending,
                    title: this._isAI ? 'Draw offers are available only in human games' : 'Offer your opponent a draw',
                }, this._drawOfferPending ? 'Draw offered' : this._drawOfferSending ? 'Sending…' : 'Offer draw'));
            }
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
        } else if (this._state === 'waiting') {
            this._actionsEl.appendChild(h('div', { role: 'status' }, 'Waiting for both players to join. Game controls appear when play starts.'));
        } else if (this._tournamentId) {
            this._actionsEl.appendChild(h('button', {
                class: 'btn btn--primary',
                onclick: () => this.ctx.router.go(`/tournaments/${this._tournamentId}`),
            }, 'Back to tournament'));
        } else {
            if (this._incomingRematchOffer) {
                this._actionsEl.appendChild(h('div', {
                    class: 'game-draw-offer',
                    role: 'status',
                    'aria-live': 'polite',
                }, `${this._opponent} offered a rematch. It expires after 45 seconds.`));
                this._actionsEl.appendChild(h('button', {
                    class: 'btn btn--primary',
                    onclick: () => this._respondToRematch(true),
                    disabled: this._rematchResponding,
                }, this._rematchResponding ? 'Starting…' : 'Accept rematch'));
                this._actionsEl.appendChild(h('button', {
                    class: 'btn',
                    onclick: () => this._respondToRematch(false),
                    disabled: this._rematchResponding,
                }, 'Decline'));
            } else {
                this._actionsEl.appendChild(h('button', {
                    class: 'btn btn--primary',
                    onclick: () => this._onRematch(),
                    disabled: this._rematchOfferPending || this._rematchOfferSending,
                }, this._rematchOfferPending ? 'Rematch offered'
                    : this._rematchOfferSending ? 'Sending…' : 'Rematch'));
            }
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
        const token = this.ctx.session && this.ctx.session.accessToken;
        if (!token || this._isAI || this._drawOfferPending || this._drawOfferSending) return;
        this._drawOfferSending = true;
        this._renderActions();
        this.ctx.socket.send(this.ctx.Outbound.offerDraw(token));
    }

    _onDrawOfferSent() {
        this._drawOfferSending = false;
        this._drawOfferPending = true;
        this._renderActions();
        this.ctx.toast.info('Draw offer sent. It expires in 45 seconds, or when your opponent moves.',
            { duration: 3200 });
    }

    _onDrawOffered(raw) {
        if (this._state !== 'in_progress') return;
        const firstNotice = !this._incomingDrawOffer;
        this._incomingDrawOffer = true;
        this._renderActions();
        const from = raw && raw.from ? raw.from : this._opponent;
        if (firstNotice) {
            this.ctx.toast.info(`${from} offered a draw. Respond beside the board or keep playing.`,
                { duration: 4200 });
        }
    }

    _respondToDraw(accept) {
        const token = this.ctx.session && this.ctx.session.accessToken;
        if (!token) return;
        this._incomingDrawOffer = false;
        this._renderActions();
        this.ctx.socket.send(this.ctx.Outbound.drawResponse(token, accept));
        if (!accept) this.ctx.toast.info('Draw offer declined.', { duration: 2200 });
    }

    _onDrawDeclined(raw) {
        this._drawOfferPending = false;
        this._drawOfferSending = false;
        this._renderActions();
        const byMove = raw && raw.reason === 'move_made';
        const expired = raw && raw.reason === 'expired';
        this.ctx.toast.info(expired ? 'Your draw offer expired.'
            : byMove ? 'Draw offer declined by your opponent’s move.' : 'Draw offer declined.',
            { duration: 2800 });
    }

    _syncDrawOffer(fromColor) {
        this._drawOfferPending = fromColor === this._myColor;
        if (fromColor && fromColor !== this._myColor) {
            this._onDrawOffered({ from: this._opponent });
        } else if (!fromColor) {
            this._incomingDrawOffer = false;
            this._renderActions();
        }
    }

    _onDrawOfferResolved(raw) {
        this._incomingDrawOffer = false;
        this._renderActions();
        if (raw && raw.reason === 'expired') {
            this.ctx.toast.info('The draw offer expired.', { duration: 2400 });
        }
    }

    _onRematch() {
        const game = this._game;
        if (!game) { this.ctx.router.go('/play'); return; }
        if (this._tournamentId) {
            this.ctx.toast.info('Tournament games do not offer rematches.', { duration: 2600 });
            this.ctx.router.go(`/tournaments/${this._tournamentId}`);
            return;
        }
        const token = this.ctx.session && this.ctx.session.accessToken;
        if (!token) {
            this.ctx.toast.warning('Sign in to play.', { duration: 2800 });
            this.ctx.router.go('/login');
            return;
        }
        if (game.isAI) {
            this._aiRematchRequested = true;
            this._rematchOfferSending = true;
            this._renderActions();
            this.ctx.socket.send(this.ctx.Outbound.playAI(
                token,
                game.difficulty || 'medium',
                game.timeBaseSec, game.timeIncSec,
            ));
        } else {
            if (this._rematchOfferPending || this._rematchOfferSending) return;
            this._rematchOfferSending = true;
            this._renderActions();
            this.ctx.socket.send(this.ctx.Outbound.offerRematch(token, game.gameId));
        }
    }

    _onRematchOfferSent() {
        this._rematchOfferSending = false;
        this._rematchOfferPending = true;
        this._renderActions();
        this.ctx.toast.info('Rematch offer sent. It expires in 45 seconds.', { duration: 3000 });
    }

    _onRematchOffered(raw) {
        if (this._state !== 'finished' || Number(raw.game_id) !== Number(this._gameId)) return;
        this._incomingRematchOffer = true;
        this._rematchResponding = false;
        if (this._resultDialog) this._resultDialog.close();
        this._renderActions();
        const from = raw && raw.from ? raw.from : this._opponent;
        this.ctx.toast.info(`${from} offered a rematch. Accept or decline beside the board.`,
            { duration: 4200 });
    }

    _respondToRematch(accept) {
        const token = this.ctx.session && this.ctx.session.accessToken;
        if (!token || !this._game || this._rematchResponding) return;
        this._rematchResponding = true;
        this._renderActions();
        this.ctx.socket.send(this.ctx.Outbound.rematchResponse(
            token, this._game.gameId, accept));
        if (!accept) this.ctx.toast.info('Rematch declined.', { duration: 2200 });
    }

    _onRematchDeclined(raw) {
        this._rematchOfferPending = false;
        this._rematchOfferSending = false;
        this._renderActions();
        this.ctx.toast.info(raw && raw.reason === 'expired'
            ? 'Your rematch offer expired.' : 'Rematch offer declined.', { duration: 2800 });
    }

    _onRematchOfferResolved(raw) {
        this._incomingRematchOffer = false;
        this._rematchResponding = false;
        this._renderActions();
        if (raw && raw.reason === 'expired') {
            this.ctx.toast.info('The rematch offer expired.', { duration: 2400 });
        }
    }

    _onRematchStarted(msg) {
        if (!msg || !msg.gameId) return;
        if (this._resultDialog) this._resultDialog.close();
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

    _onAiRematchStarted(msg) {
        if (!this._aiRematchRequested || !msg || !msg.gameId) return;
        this._aiRematchRequested = false;
        const previous = this._game;
        this.ctx.store.setGame({
            gameId: msg.gameId,
            color: msg.color === 'black' ? 'b' : 'w',
            opponent: msg.opponent,
            isAI: true,
            whiteMs: msg.whiteMs,
            blackMs: msg.blackMs,
            timeBaseSec: previous.timeBaseSec,
            timeIncSec: previous.timeIncSec,
            difficulty: previous.difficulty || 'medium',
        });
        this.ctx.router.go('/game/' + msg.gameId);
    }

    /* ── Game over modal ─────────────────────────────────── */

    _presentResult() {
        if (this._resultShown) return;
        this._resultShown = true;
        if (this._isAI) {
            this.ctx.toast.info(`${resultLabel(this._result, this._myColor)} · ${reasonLabel(this._reason)}`);
            return;
        }

        const won  = (this._result === '1-0' && this._myColor === 'w') || (this._result === '0-1' && this._myColor === 'b');
        const lost = (this._result === '1-0' && this._myColor === 'b') || (this._result === '0-1' && this._myColor === 'w');
        const draw = this._result === '1/2-1/2';
        const badgeClass = won ? 'result-card__badge--won' : draw ? 'result-card__badge--draw' : 'result-card__badge--lost';
        const badgeText  = won ? 'Victory' : draw ? 'Draw' : lost ? 'Defeat' : 'Ended';

        const me  = (this.ctx.store.session.username || 'Player');
        const white = this._myColor === 'w' ? me : this._opponent;
        const black = this._myColor === 'w' ? this._opponent : me;

        const shareActions = [
            h('button', {
                class: 'btn btn--sm',
                onclick: () => this._copyPgn(white, black),
            }, 'Copy PGN'),
            h('button', {
                class: 'btn btn--sm',
                onclick: () => this._copyBoardImage(),
                title: 'Copy the final board as an image',
            }, 'Copy image'),
        ];
        if (!this._tournamentId) {
            shareActions.push(h('button', {
                class: 'btn btn--sm',
                onclick: () => this._inviteFriend(),
                title: 'Post a fresh game and share the link',
            }, 'Invite a friend'));
        }
        const body = h('div', { class: 'result-card' },
            h('span', { class: 'result-card__badge ' + badgeClass }, badgeText),
            h('div', { class: 'result-card__title' }, resultLabel(this._result, this._myColor)),
            h('div', { class: 'result-card__reason' }, reasonLabel(this._reason)),
            h('div', { class: 'result-card__meta mono' }, `${white} vs ${black} · ${this._moves.length} plies`),
            // G2: share row — copy PGN, copy image, invite a friend.
            h('div', { class: 'result-card__share' }, ...shareActions),
        );
        const footer = this._tournamentId
            ? h('div', { style: { display: 'flex', gap: '8px' } },
                h('button', { class: 'btn', onclick: () => { dlg.close(); this.ctx.router.go(`/replay/${this._gameId}`); } }, 'Open replay'),
                h('button', { class: 'btn btn--primary', onclick: () => { dlg.close(); this.ctx.router.go(`/tournaments/${this._tournamentId}`); } }, 'Back to tournament'))
            : h('div', { style: { display: 'flex', gap: '8px' } },
                h('button', { class: 'btn', onclick: () => { dlg.close(); this.ctx.router.go('/play'); } }, 'Back to lobby'),
                h('button', { class: 'btn btn--primary', onclick: () => { dlg.close(); this._onRematch(); } }, 'Rematch'));
        const dlg = this.ctx.modal.open({
            title: 'Game over',
            body, footer,
            dismissible: true,
        });
        this._resultDialog = dlg;
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
            timeControl: `${(this._game.timeBaseSec || 0)}+${(this._game.timeIncSec || 0)}`,
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
        const token = this.ctx.session && this.ctx.session.accessToken;
        if (!token) {
            this.ctx.toast.warning('Sign in to play.', { duration: 2800 });
            this.ctx.router.go('/login');
            return;
        }
        const game = this.ctx.store.game;
        const base = game && game.timeBaseSec ? game.timeBaseSec : 300;
        const inc  = game && game.timeIncSec  ? game.timeIncSec  : 3;
        this.ctx.store.setGame(null);
        this.ctx.socket.send(this.ctx.Outbound.createGame(token, base, inc));
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
