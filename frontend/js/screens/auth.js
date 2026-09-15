/**
 * auth.js — LIVE screen (Phase 7.10).
 *
 * Password auth flow:
 *   1. capability.request sends `login` / `register` and races for `auth_ok`
 *      vs `auth_error`. On timeout / unknown-type, falls back to a local
 *      demo session so the frontend is still usable while the backend
 *      isn't configured (matches the pattern the other preview screens use).
 *   2. On live auth_ok, Session.adopt() persists the refresh token and
 *      schedules automatic refresh; the store's session segment is updated
 *      via the Session → store bridge in main.js.
 *
 * Google Sign-In:
 *   * The "Continue with Google" button is a plain window.location redirect
 *     to `${SUPABASE_URL}/auth/v1/authorize?provider=google&redirect_to=…`.
 *     No <script> tag, no SDK — the whole flow is a URL and a browser
 *     hash-fragment read on the return trip.
 *   * The callback handler lives in main.js so it runs on any entry route,
 *     not just /login.
 *   * If Supabase isn't configured in `config.js`, the button is hidden.
 */

import { Screen } from '../ui/screen.js';
import { h, clear } from '../core/dom.js';
import { CONFIG, googleAuthorizeUrl } from '../config.js';

// auth_error codes from the server, mapped to human-readable strings. Any
// code not listed here falls through to a generic message.
const ERROR_COPY = {
    invalid_username:        'Username must be 3–20 letters, digits, or underscores.',
    weak_password:           'Password must be at least 8 characters.',
    username_taken:          'That username is already in use.',
    invalid_credentials:     'Wrong username or password.',
    rate_limited:            'Too many attempts. Please wait a minute and try again.',
    invalid_google:          'Google sign-in failed — please try again.',
    email_collision:         'An account with this email already exists. Sign in the usual way, then link Google from Settings.',
    google_disabled:         'Google sign-in is not configured on this deployment.',
    unauthorized:            'Session expired — please sign in again.',
    internal:                'Something went wrong on our end. Please try again.',
};

export class AuthScreen extends Screen {
    constructor(ctx) {
        super(ctx);
        // Marked preview so the badge shows until the FIRST live auth_ok
        // arrives; capability.request flips this internally on a live reply.
        this.preview = true;
        this._mode = 'login';
        this._busy = false;
    }

    render() {
        const googleUrl = googleAuthorizeUrl();
        return h('div', { class: 'screen' },
            this.header('Sign in', this._mode === 'login'
                ? 'Sign in to save your rating and game history.'
                : 'Create an account — takes ten seconds.'),
            h('div', { class: 'screen__body' },
                h('div', { class: 'auth-wrap' },
                    h('div', { class: 'card' },
                        h('div', { class: 'tabs' },
                            h('button', {
                                class: 'tabs__tab' + (this._mode === 'login' ? ' is-active' : ''),
                                onclick: () => this._switch('login'),
                            }, 'Sign in'),
                            h('button', {
                                class: 'tabs__tab' + (this._mode === 'register' ? ' is-active' : ''),
                                onclick: () => this._switch('register'),
                            }, 'Register'),
                        ),
                        h('div', { class: 'card__body', ref: el => this._form = el },
                            this._formBody(googleUrl)
                        )
                    )
                )
            )
        );
    }

    _switch(mode) {
        this._mode = mode;
        clear(this._form);
        this._form.appendChild(this._formBody(googleAuthorizeUrl()));
        const sub = this.root && this.root.querySelector('.screen__subtitle');
        if (sub) sub.textContent = mode === 'login'
            ? 'Sign in to save your rating and game history.'
            : 'Create an account — takes ten seconds.';
    }

    _formBody(googleUrl) {
        return h('div', { class: 'field-stack' },
            h('div', { class: 'field' },
                h('label', { class: 'field__label', for: 'auth-username' }, 'Username'),
                h('input', {
                    id: 'auth-username',
                    class: 'input',
                    autocomplete: 'username',
                    maxlength: 20,
                    value: this.ctx.store.session.username || '',
                    ref: el => this._userInput = el,
                    onkeydown: (e) => { if (e.key === 'Enter') this._submit(); },
                })
            ),
            h('div', { class: 'field' },
                h('label', { class: 'field__label', for: 'auth-password' }, 'Password'),
                h('input', {
                    id: 'auth-password',
                    class: 'input',
                    type: 'password',
                    autocomplete: this._mode === 'login' ? 'current-password' : 'new-password',
                    ref: el => this._passInput = el,
                    onkeydown: (e) => { if (e.key === 'Enter') this._submit(); },
                }),
                this._mode === 'register'
                    ? h('div', { class: 'field__hint' }, '8 characters or more.')
                    : null
            ),
            h('button', {
                class: 'btn btn--primary btn--block',
                onclick: () => this._submit(),
                ref: el => this._submitBtn = el,
            }, this._mode === 'login' ? 'Sign in' : 'Create account'),

            // Google button — hidden if Supabase isn't configured.
            googleUrl ? h('div', { class: 'field-stack__divider' }, 'or') : null,
            googleUrl ? h('button', {
                class: 'btn btn--ghost btn--block',
                onclick: () => this._continueWithGoogle(googleUrl),
            }, 'Continue with Google') : null,
        );
    }

    onMount() {
        if (this._userInput) this._userInput.focus({ preventScroll: true });
    }

    _continueWithGoogle(url) {
        // Full-page redirect. Supabase performs the OAuth handshake and
        // redirects back to CONFIG.supabaseRedirect with the tokens in
        // `location.hash`. main.js picks it up on load.
        location.assign(url);
    }

    async _submit() {
        if (this._busy) return;
        const username = (this._userInput.value || '').trim();
        const password = this._passInput.value || '';
        if (!username || !password) {
            this.ctx.toast.warning('Enter a username and a password.', { duration: 2200 });
            return;
        }
        if (this._mode === 'register' && password.length < 8) {
            this.ctx.toast.warning('Password should be at least 8 characters.', { duration: 2400 });
            return;
        }
        this._busy = true;
        this._submitBtn.disabled = true;
        this._submitBtn.textContent = 'Working…';

        const msg = this._mode === 'login'
            ? this.ctx.Outbound.login(username, password)
            : this.ctx.Outbound.register(username, password);

        // Listen for auth_error alongside auth_ok — capability.js does not
        // treat auth_error as a "preview" signal (it's a live-server reply),
        // so we wire our own one-shot to surface it in the toast.
        const errorPromise = new Promise((resolve) => {
            const off = this.ctx.socket.once('auth_error', (m) => resolve(m));
            setTimeout(() => { off(); resolve(null); }, 1500);
        });

        const { data, live } = await this.ctx.capability.request(msg, {
            expect: 'auth_ok',
            timeout: 1500,
            demo: () => ({
                username,
                elo: this._mode === 'register' ? 1200 : (600 + (username.length * 47) % 1200),
                access_token: 'demo-' + Date.now(),
                refresh_token: 'demo-refresh-' + Date.now(),
                access_expires_in: 900,
            }),
        });

        // Race: if the server sent auth_error before we resolved, prefer
        // that over the demo fallback so the user sees the real reason.
        const err = await errorPromise;
        if (!live && err) {
            const copy = ERROR_COPY[err.code] || 'Sign in failed.';
            this.ctx.toast.error(copy, { duration: 3600 });
            this._resetSubmitButton();
            return;
        }

        if (live) {
            this.ctx.session.adopt(data);
            this.preview = false;   // flip the badge off on the LIVE path
        } else {
            // Preview fallback — keep the existing local-demo behaviour.
            this.ctx.store.setSession({
                username: data.username || username,
                elo:      data.elo || 1200,
            });
        }

        this.ctx.toast.success(
            live
                ? (this._mode === 'login' ? `Welcome back, ${data.username}!` : `Account created. Welcome!`)
                : `Signed in as ${username} (preview).`,
            { duration: 2600 }
        );
        this.ctx.router.go('/');
    }

    _resetSubmitButton() {
        this._busy = false;
        this._submitBtn.disabled = false;
        this._submitBtn.textContent = this._mode === 'login' ? 'Sign in' : 'Create account';
    }
}
