/**
 * join.js — Join-by-link handoff (Part G2).
 *
 * Route: #/join/:gameId
 *
 * Flow:
 *   1. Show a small "Joining game #<id>…" panel with the challenger's link.
 *   2. When the socket is connected, send `join_game`.
 *   3. On `game_start` / `game_joined`, populate store.game and jump to
 *      #/game/<id>. The URL updates once — never on every re-render.
 *   4. On any error, surface it and offer "Back to lobby".
 *
 * Deliberately not a full "lobby with prefilled fields" — the whole point of
 * a challenge link is that the challenger already made every decision.
 */

import { Screen } from '../ui/screen.js';
import { h } from '../core/dom.js';
import { absoluteUrl, copyText } from '../core/share.js';

const JOIN_TIMEOUT_MS = 6000;

export class JoinScreen extends Screen {
    constructor(ctx, params) {
        super(ctx);
        const rawId = String(params.gameId || '');
        const parsedId = /^\d+$/.test(rawId) ? Number(rawId) : NaN;
        this._gameId = Number.isSafeInteger(parsedId) && parsedId > 0 ? parsedId : null;
        this._sent = false;
        this._joinTimer = null;
        this._pending = { kind: 'join' };
    }

    render() {
        const displayId = this._gameId === null ? 'Invalid' : String(this._gameId);
        const link = absoluteUrl('#/join/' + displayId);
        const localInvite = /^(localhost|127(?:\.\d+){3}|\[::1\])$/.test(location.hostname);
        return h('div', { class: 'screen' },
            h('div', { class: 'screen__body' },
                h('div', { class: 'card join-card' },
                    h('div', { class: 'card__body card__body--stack' },
                        h('div', { class: 'join-card__spinner-row' },
                            h('span', { class: 'queue-status__spinner', 'aria-hidden': 'true' }),
                            h('div', {},
                                h('div', { class: 'join-card__title' }, 'Joining game'),
                                h('div', { class: 'join-card__id mono' }, '#' + displayId),
                            ),
                        ),
                        h('div', { class: 'join-card__hint', ref: el => this._hintEl = el },
                            'Connecting to the server…'),
                        h('div', { class: 'field' },
                            h('label', { class: 'field__label' }, 'Invite link'),
                            h('div', { class: 'invite-row' },
                                h('input', {
                                    class: 'input mono invite-row__input',
                                    readonly: true,
                                    value: link,
                                    onclick: (e) => e.target.select(),
                                }),
                                h('button', {
                                    class: 'btn btn--sm',
                                    onclick: () => this._copyLink(link),
                                }, 'Copy'),
                            ),
                        ),
                        h('div', { class: 'field-hint' },
                            localInvite
                                ? 'For local testing, open this link in another browser session on this computer.'
                                : 'Share this link with a friend so a second player can join.'),
                        h('div', { style: { display: 'flex', gap: '8px' } },
                            h('button', {
                                class: 'btn btn--primary',
                                hidden: true,
                                ref: el => this._retryBtn = el,
                                onclick: () => this._retry(),
                            }, 'Try again'),
                            h('button', {
                                class: 'btn',
                                onclick: () => this.ctx.router.go('/play'),
                            }, 'Back to lobby'),
                        )
                    )
                )
            )
        );
    }

    onMount() {
        const { socket } = this.ctx;
        if (this._gameId === null) {
            this._say('This invite link has an invalid game ID.');
            if (this._retryBtn) this._retryBtn.hidden = true;
            return;
        }
        this.sub(socket.on('game_joined',  raw => this._onJoined(raw)));
        this.sub(socket.on('game_start',   raw => this._onJoined(raw)));
        this.sub(socket.on('active_game',  raw => this._onActiveGame(raw)));
        this.sub(socket.on('error',        raw => this._onError(raw)));
        this.sub(socket.on('auth_error',   () => this._onAuthError()));
        this.sub(this.ctx.session.on('change', () => this._trySend()));
        this.sub(socket.onState(state => {
            if (state === 'connected') this._trySend();
            else if (state === 'connecting') this._say('Connecting to the server…');
            else {
                this._sent = false;
                this._clearJoinTimer();
                this._say('Offline — reconnecting…');
            }
        }));
        if (socket.isConnected()) this._trySend();
    }

    onUnmount() { this._clearJoinTimer(); }

    _trySend() {
        if (this._sent) return;
        const token = this.ctx.session && this.ctx.session.accessToken;
        if (!token) {
            if (this.ctx.session && this.ctx.session.hasRefreshToken) {
                this._say('Restoring your session…');
                return;
            }
            this._say('Sign in to join this game.');
            this.ctx.toast.warning('Sign in to play.', { duration: 2800 });
            this.ctx.postAuthPath = '/join/' + this._gameId;
            this.ctx.router.go('/login');
            return;
        }
        this._sent = true;
        if (this._retryBtn) this._retryBtn.hidden = true;
        this._say('Asking to join…');
        this.ctx.socket.send(this.ctx.Outbound.joinGame(token, this._gameId));
        this._joinTimer = setTimeout(() => {
            this._joinTimer = null;
            this._sent = false;
            this._say('The server did not answer the join request. Try again.');
            if (this._retryBtn) this._retryBtn.hidden = false;
            // If the reply alone was lost, authoritative active-game recovery
            // avoids turning a successful seat into an erroneous second join.
            this.ctx.socket.send(this.ctx.Outbound.activeGame(token));
        }, JOIN_TIMEOUT_MS);
    }

    _onJoined(raw) {
        const norm = this.ctx.Inbound.normalize(raw);
        // A pending create_game elsewhere could echo game_start too; only act
        // if this message is about the game we asked for.
        if (norm.gameId && Number(norm.gameId) !== this._gameId) return;

        this._clearJoinTimer();
        this._enterGame(norm);
    }

    _onActiveGame(raw) {
        const norm = this.ctx.Inbound.normalize(raw);
        const game = norm && norm.game;
        if (!game || Number(game.gameId) !== this._gameId) return;
        this._clearJoinTimer();
        this._enterGame({
            ...game,
            color: game.color === 'b' ? 'black' : 'white',
        });
    }

    _enterGame(norm) {

        const preset = {
            base: Math.round((norm.whiteMs || 300_000) / 1000),
            inc:  0,   // server does not echo the increment; will re-anchor from move_made
        };
        this.ctx.store.setGame({
            gameId:   this._gameId,
            color:    norm.color === 'black' ? 'b' : 'w',
            opponent: norm.opponent || 'Opponent',
            isAI:     !!norm.isAI,
            whiteMs:  norm.whiteMs,
            blackMs:  norm.blackMs,
            timeBaseSec: preset.base,
            timeIncSec:  preset.inc,
            difficulty:  null,
        });
        this.ctx.router.go('/game/' + this._gameId);
    }

    _onError(raw) {
        const norm = this.ctx.Inbound.normalize(raw);
        if (norm.isUnknownType) return;
        this._clearJoinTimer();
        this._sent = false;
        this.ctx.toast.danger(norm.message || 'That game could not be joined.', { title: 'Join failed' });
        this._say(norm.message || 'This game is no longer available. Try creating a new one.');
        if (this._retryBtn) this._retryBtn.hidden = false;
    }

    _onAuthError() {
        this._clearJoinTimer();
        this._sent = false;
        this.ctx.postAuthPath = '/join/' + this._gameId;
        this.ctx.toast.warning('Your session expired. Sign in again to join this game.',
            { duration: 3400 });
        this.ctx.router.go('/login');
    }

    _retry() {
        this._clearJoinTimer();
        this._sent = false;
        this._trySend();
    }

    _clearJoinTimer() {
        if (this._joinTimer !== null) {
            clearTimeout(this._joinTimer);
            this._joinTimer = null;
        }
    }

    _say(text) { if (this._hintEl) this._hintEl.textContent = text; }

    async _copyLink(link) {
        const res = await copyText(link);
        if (res.ok) this.ctx.toast.success('Link copied.', { duration: 1800 });
        else this.ctx.toast.warning('Copy blocked by the browser — select and copy the field.', { duration: 3000 });
    }
}
