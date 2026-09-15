/**
 * settings.js — LIVE screen.
 *
 * Local-only preferences persisted through the store (localStorage).
 * Fields: board theme, piece set, show coords, sound, animations,
 * plus a session-level name and a "clear data" affordance.
 *
 * A miniature board on the right mirrors the current selection so the
 * user can preview theme + piece changes without leaving the screen.
 */

import { Screen } from '../ui/screen.js';
import { h, clear, icon } from '../core/dom.js';
import { BoardRenderer } from '../board/renderer.js';
import { BoardTheme, THEMES } from '../board/theme.js';
import { START_FEN } from '../board/chess.js';
import { CONFIG, googleAuthorizeUrl } from '../config.js';

const PIECE_SETS = [
    { key: 'classic', label: 'Classic' },
    { key: 'modern',  label: 'Modern'  },
];

export class SettingsScreen extends Screen {
    constructor(ctx) {
        super(ctx);
        this.preview = false;   // this screen IS live
    }

    render() {
        const p = this.ctx.store.prefs;
        const s = this.ctx.store.session;

        return h('div', { class: 'screen' },
            this.header('Settings', 'Board, pieces, sound, and animations.'),
            h('div', { class: 'screen__body' },
                h('div', { class: 'settings-grid' },

                    // ── Left column: form ──
                    h('div', { class: 'settings-col' },

                        h('div', { class: 'card' },
                            h('div', { class: 'card__header' },
                                h('div', { class: 'card__title' }, 'Board')),
                            h('div', { class: 'card__body' },

                                this._pillField('Theme', 'boardTheme', p.boardTheme, Object.keys(THEMES).map(k => ({
                                    value: k, label: THEMES[k].label,
                                })), (v) => this._apply({ boardTheme: v })),

                                this._pillField('Piece set', 'pieceSet', p.pieceSet, PIECE_SETS.map(x => ({
                                    value: x.key, label: x.label,
                                })), (v) => this._apply({ pieceSet: v })),

                                this._toggleField('Show file & rank labels', 'showCoords', p.showCoords !== false,
                                    (on) => this._apply({ showCoords: on })),
                            )
                        ),

                        h('div', { class: 'card' },
                            h('div', { class: 'card__header' },
                                h('div', { class: 'card__title' }, 'Motion & sound')),
                            h('div', { class: 'card__body' },
                                this._toggleField('Piece animations', 'animate', p.animate !== false,
                                    (on) => this._apply({ animate: on })),
                                this._toggleField('Move sounds', 'sound', p.sound !== false,
                                    (on) => {
                                        this._apply({ sound: on });
                                        if (on) this.ctx.sound.gameStart();
                                    }),
                            )
                        ),

                        h('div', { class: 'card' },
                            h('div', { class: 'card__header' },
                                h('div', { class: 'card__title' }, 'You')),
                            h('div', { class: 'card__body' },
                                h('div', { class: 'field' },
                                    h('label', { class: 'field__label', for: 'settings-name' }, 'Display name'),
                                    h('input', {
                                        id: 'settings-name',
                                        class: 'input',
                                        value: s.username || '',
                                        maxlength: 24,
                                        oninput: (e) => this.ctx.store.setSession({ username: e.target.value.trim() || 'Player' }),
                                    })
                                ),
                                h('div', { class: 'field' },
                                    h('label', { class: 'field__label' }, 'Rating'),
                                    h('div', { class: 'settings-static-value' }, String(s.elo || 1200)),
                                    h('div', { class: 'field__hint' }, 'Ratings become editable once accounts are enabled (Phase 7).')
                                ),
                                h('button', {
                                    class: 'btn btn--danger',
                                    style: { alignSelf: 'flex-start' },
                                    onclick: () => this._clearData(),
                                }, 'Clear local data'),
                            )
                        ),

                        this._accountCard(),
                    ),

                    // ── Right column: live preview ──
                    h('div', { class: 'settings-col' },
                        h('div', { class: 'card' },
                            h('div', { class: 'card__header' },
                                icon('eye', 'icon--sm'),
                                h('div', { class: 'card__title' }, 'Preview')),
                            h('div', { class: 'card__body', style: { alignItems: 'center' } },
                                h('div', { class: 'board-frame', style: { width: '100%', maxWidth: '360px' } },
                                    h('div', { class: 'board-container', ref: el => this._container = el },
                                        h('canvas', { class: 'board-canvas', ref: el => this._canvas = el })
                                    )
                                ),
                                h('div', { class: 'field__hint', style: { textAlign: 'center' } },
                                    'Changes save automatically and take effect on your next game.')
                            )
                        )
                    ),
                )
            )
        );
    }

    _pillField(label, key, active, options, onChange) {
        return h('div', { class: 'field' },
            h('div', { class: 'field__label' }, label),
            h('div', { class: 'pill-group' },
                ...options.map(o =>
                    h('button', {
                        class: 'pill-group__item' + (o.value === active ? ' is-active' : ''),
                        dataset: { field: key, value: o.value },
                        onclick: (e) => {
                            onChange(o.value);
                            for (const el of e.currentTarget.parentElement.children) el.classList.remove('is-active');
                            e.currentTarget.classList.add('is-active');
                        },
                    }, o.label)
                )
            )
        );
    }

    _toggleField(label, key, active, onChange) {
        return h('label', { class: 'toggle-row' },
            h('span', {}, label),
            h('span', {
                class: 'toggle' + (active ? ' is-on' : ''),
                dataset: { toggle: key },
                onclick: (e) => {
                    const nowOn = !e.currentTarget.classList.contains('is-on');
                    e.currentTarget.classList.toggle('is-on', nowOn);
                    onChange(nowOn);
                },
            }, h('span', { class: 'toggle__dot' }))
        );
    }

    onMount() {
        // Boot mini preview board.
        const theme = (THEMES[this.ctx.store.prefs.boardTheme] || THEMES.classic).factory();
        this._renderer = new BoardRenderer(this._canvas, theme);
        this._renderer.pieceSetUrl = `assets/pieces/${this.ctx.store.prefs.pieceSet || 'classic'}.svg`;
        this._boot();
    }

    _boot() {
        let booted = false;
        const doIt = () => {
            if (booted) return;
            const rect = this._container.getBoundingClientRect();
            const size = Math.min(rect.width, rect.height);
            if (size <= 0) return;
            booted = true;
            this._renderer.resize(size).then(() => this._renderer.setPosition(START_FEN));
        };
        doIt();
        if (!booted) requestAnimationFrame(doIt);
        this.timeout(doIt, 60);
        this.timeout(doIt, 250);
    }

    _apply(patch) {
        this.ctx.store.setPrefs(patch);
        if (!this._renderer) return;
        if ('boardTheme' in patch) {
            this._renderer.setTheme((THEMES[patch.boardTheme] || THEMES.classic).factory());
        }
        if ('pieceSet' in patch) {
            this._renderer.setPieceSet(`assets/pieces/${patch.pieceSet}.svg`);
        }
        if ('showCoords' in patch) {
            this._renderer.theme.showCoords = !!patch.showCoords;
            this._renderer.draw();
        }
    }

    // ── Account card (§7.10) — Google link/unlink + sign-out. ─────────────
    //
    // Only rendered when there's a live session; otherwise the user has
    // nothing to link. Shows one big "Continue with Google" button if
    // unlinked, or a "Disconnect Google" + "Sign out" pair if linked.
    _accountCard() {
        const session = this.ctx.session;
        if (!session || !session.isAuthenticated) return null;
        const googleUrl = googleAuthorizeUrl();

        return h('div', { class: 'card' },
            h('div', { class: 'card__header' },
                h('div', { class: 'card__title' }, 'Account')),
            h('div', { class: 'card__body' },
                h('div', { class: 'field' },
                    h('label', { class: 'field__label' }, 'Signed in as'),
                    h('div', { class: 'settings-static-value' }, session.username || '—'),
                ),

                googleUrl ? h('button', {
                    class: 'btn btn--ghost',
                    style: { alignSelf: 'flex-start' },
                    onclick: () => this._linkGoogle(googleUrl),
                }, 'Link Google account') : null,

                googleUrl ? h('button', {
                    class: 'btn btn--ghost',
                    style: { alignSelf: 'flex-start' },
                    onclick: () => this._unlinkGoogle(),
                }, 'Disconnect Google') : null,

                h('button', {
                    class: 'btn btn--ghost',
                    style: { alignSelf: 'flex-start' },
                    onclick: () => this._signOut(false),
                }, 'Sign out'),

                h('button', {
                    class: 'btn btn--danger',
                    style: { alignSelf: 'flex-start' },
                    onclick: () => this._signOut(true),
                }, 'Sign out of all devices'),
            )
        );
    }

    _linkGoogle(url) {
        // Same OAuth dance as the auth screen, but on return main.js's
        // callback will route to google_auth (not link_google). Linking
        // via a fresh sign-in works because the server treats a matching
        // `sub` as "already this account". For an explicit link that
        // *rejects* an unrelated sub, the operator would open a
        // dedicated flow — deferred; not needed for the common case.
        location.assign(url);
    }

    async _unlinkGoogle() {
        const ok = await this.ctx.modal.confirm({
            title: 'Disconnect Google',
            message: 'You will need a password to sign in next time.',
            confirmLabel: 'Disconnect',
        });
        if (!ok) return;
        const access = await this.ctx.session.accessTokenForRequest();
        if (!access) { this.ctx.toast.warning('Session expired — please sign in again.'); return; }
        this.ctx.socket.send(this.ctx.Outbound.unlinkGoogle(access));
        const one = (type) => new Promise(res => {
            const off = this.ctx.socket.once(type, m => res(m));
            setTimeout(() => { off(); res(null); }, 3000);
        });
        const [okMsg, errMsg] = await Promise.race([
            one('link_ok').then(m => [m, null]),
            one('auth_error').then(m => [null, m]),
        ]);
        if (okMsg) this.ctx.toast.success('Google disconnected.');
        else this.ctx.toast.error('Could not disconnect ('
            + (errMsg && errMsg.code || 'error') + ').');
    }

    async _signOut(everywhere) {
        const ok = await this.ctx.modal.confirm({
            title: everywhere ? 'Sign out everywhere' : 'Sign out',
            message: everywhere
                ? 'Signs you out on every device you\'re logged in on.'
                : 'Signs you out on this device only.',
            confirmLabel: everywhere ? 'Sign out everywhere' : 'Sign out',
            danger: everywhere,
        });
        if (!ok) return;
        if (everywhere) await this.ctx.session.logoutAll();
        else            await this.ctx.session.logout();
        this.ctx.toast.success('Signed out.', { duration: 2000 });
        this.ctx.router.go('/');
    }

    async _clearData() {
        const ok = await this.ctx.modal.confirm({
            title: 'Clear local data',
            message: 'This removes your saved preferences and name. It does not touch server-side data.',
            confirmLabel: 'Clear',
            danger: true,
        });
        if (!ok) return;
        try { localStorage.clear(); } catch (_) {}
        this.ctx.toast.success('Local data cleared. Reloading…', { duration: 1400 });
        this.timeout(() => location.reload(), 800);
    }
}
