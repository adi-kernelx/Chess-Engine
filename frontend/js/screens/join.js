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
import { h, icon } from '../core/dom.js';
import { absoluteUrl, copyText } from '../core/share.js';

const GUEST_ADJECTIVES = ['Knight', 'Bishop', 'Rook', 'Pawn', 'Castle', 'Gambit', 'Zugzwang', 'Fianchetto'];

export class JoinScreen extends Screen {
    constructor(ctx, params) {
        super(ctx);
        this._gameId = String(params.gameId || '').slice(0, 64);
        this._sent = false;
        this._pending = { kind: 'join' };
    }

    render() {
        const link = absoluteUrl('#/join/' + this._gameId);
        return h('div', { class: 'screen' },
            h('div', { class: 'screen__body' },
                h('div', { class: 'card join-card' },
                    h('div', { class: 'card__body card__body--stack' },
                        h('div', { class: 'join-card__spinner-row' },
                            h('span', { class: 'queue-status__spinner', 'aria-hidden': 'true' }),
                            h('div', {},
                                h('div', { class: 'join-card__title' }, 'Joining game'),
                                h('div', { class: 'join-card__id mono' }, '#' + this._gameId),
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
                            'Share this link with a friend so a second player can join from anywhere.'),
                        h('div', { style: { display: 'flex', gap: '8px' } },
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
        this._ensureGuestHandle();
        this.sub(socket.on('game_joined',  raw => this._onJoined(raw)));
        this.sub(socket.on('game_start',   raw => this._onJoined(raw)));
        this.sub(socket.on('error',        raw => this._onError(raw)));
        this.sub(socket.onState(state => {
            if (state === 'connected') this._trySend();
            else if (state === 'connecting') this._say('Connecting to the server…');
            else this._say('Offline — reconnecting…');
        }));
        if (socket.isConnected()) this._trySend();
    }

    _trySend() {
        if (this._sent) return;
        this._sent = true;
        this._say('Asking to join…');
        this.ctx.socket.send(this.ctx.Outbound.joinGame(this._username(), this._gameId));
    }

    _onJoined(raw) {
        const norm = this.ctx.Inbound.normalize(raw);
        // A pending create_game elsewhere could echo game_start too; only act
        // if this message is about the game we asked for.
        if (norm.gameId && String(norm.gameId) !== this._gameId) return;

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
        this.ctx.toast.danger(norm.message || 'That game could not be joined.', { title: 'Join failed' });
        this._say(norm.message || 'This game is no longer available. Try creating a new one.');
    }

    _say(text) { if (this._hintEl) this._hintEl.textContent = text; }

    _username() { return (this.ctx.store.session.username || 'Player').slice(0, 24); }

    _ensureGuestHandle() {
        const name = this._username();
        if (name && name !== 'Player') return;
        const pick = GUEST_ADJECTIVES[Math.floor(Math.random() * GUEST_ADJECTIVES.length)];
        const num  = 1000 + Math.floor(Math.random() * 9000);
        this.ctx.store.setSession({ username: `${pick}${num}` });
    }

    async _copyLink(link) {
        const res = await copyText(link);
        if (res.ok) this.ctx.toast.success('Link copied.', { duration: 1800 });
        else this.ctx.toast.warning('Copy blocked by the browser — select and copy the field.', { duration: 3000 });
    }
}
