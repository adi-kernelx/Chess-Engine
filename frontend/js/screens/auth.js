/**
 * auth.js — LIVE screen (Phase 7.10).
 *
 * Password auth flow:
 *   1. Sends `login` / `register` and waits for `auth_ok` vs `auth_error`.
 *      Authentication fails closed on disconnect or timeout; it never creates
 *      a local identity that could be mistaken for a real authenticated user.
 *   2. On auth_ok, Session.adopt() persists the refresh token and
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
 *   * If Supabase isn't configured yet, the button stays visible and explains
 *     that it becomes active after cloud deployment.
 */

import { Screen } from '../ui/screen.js';
import { h, clear } from '../core/dom.js';
import { googleAuthorizeUrl } from '../config.js';

// auth_error codes from the server, mapped to human-readable strings. Any
// code not listed here falls through to a generic message.
const ERROR_COPY = {
    invalid_username:        'Username must be 3–20 letters, digits, or underscores.',
    weak_password:           'Password must be at least 8 characters.',
    invalid_email:           'Enter a valid email address.',
    email_unavailable:       'Email delivery is not configured or is busy. Please try again later.',
    username_taken:          'That username is already in use.',
    invalid_credentials:     'Wrong username or password.',
    rate_limited:            'Too many attempts. Please wait a minute and try again.',
    invalid_google:          'Google sign-in failed — please try again.',
    email_collision:         'An account with this email already exists. Sign in the usual way, then link Google from Settings.',
    google_disabled:         'Google sign-in is not configured on this deployment.',
    unauthorized:            'Session expired — please sign in again.',
    internal:                'Something went wrong on our end. Please try again.',
    unavailable:             'The authentication server did not respond. Check the server configuration and try again.',
    seal_configuration:      'Secure sign-in is not configured. Please contact the site operator.',
    seal_failed:             'The secure sign-in handshake failed. No credentials were sent. Please try again.',
    auth_busy:               'Another sign-in request is in progress. Please wait.',
};

export class AuthScreen extends Screen {
    constructor(ctx, initialMode = 'login') {
        super(ctx);
        // Authentication is implemented by the backend. Connection state is
        // shown by the global connection indicator; preview fallback is
        // reported by a toast only when it is actually used.
        this._mode = initialMode === 'register' ? 'register' : 'login';
        this._busy = false;
    }

    render() {
        const googleUrl = googleAuthorizeUrl();
        return h('div', { class: 'screen' },
            this.header(this._mode === 'login' ? 'Sign in' : 'Create account', this._mode === 'login'
                ? 'Sign in to save your rating and game history.'
                : 'Create an account to save your games and track your progress.'),
            h('div', { class: 'screen__body' },
                h('div', { class: 'auth-wrap' },
                    h('div', { class: 'card auth-card' },
                        h('div', { class: 'tabs', role: 'tablist', 'aria-label': 'Authentication mode' },
                            h('button', {
                                type: 'button',
                                role: 'tab',
                                class: 'tabs__tab' + (this._mode === 'login' ? ' is-active' : ''),
                                'aria-selected': this._mode === 'login' ? 'true' : 'false',
                                ref: el => this._loginTab = el,
                                onclick: () => this._switch('login'),
                            }, 'Sign in'),
                            h('button', {
                                type: 'button',
                                role: 'tab',
                                class: 'tabs__tab' + (this._mode === 'register' ? ' is-active' : ''),
                                'aria-selected': this._mode === 'register' ? 'true' : 'false',
                                ref: el => this._registerTab = el,
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
        if (this._busy || (mode !== 'login' && mode !== 'register')) return;
        this._mode = mode;

        const registering = mode === 'register';
        this._loginTab.classList.toggle('is-active', !registering);
        this._registerTab.classList.toggle('is-active', registering);
        this._loginTab.setAttribute('aria-selected', String(!registering));
        this._registerTab.setAttribute('aria-selected', String(registering));

        clear(this._form);
        this._form.appendChild(this._formBody(googleAuthorizeUrl()));

        const title = this.root && this.root.querySelector('.screen__title');
        if (title) title.textContent = registering ? 'Create account' : 'Sign in';
        const sub = this.root && this.root.querySelector('.screen__subtitle');
        if (sub) sub.textContent = registering
            ? 'Create an account to save your games and track your progress.'
            : 'Sign in to save your rating and game history.';
        if (this._userInput) this._userInput.focus({ preventScroll: true });
    }

    _formBody(googleUrl) {
        return h('div', { class: 'field-stack auth-form' },
            h('div', { class: 'auth-intro' },
                h('div', { class: 'auth-intro__logo', 'aria-hidden': 'true' }),
                h('h2', { class: 'auth-intro__title' }, this._mode === 'login' ? 'Welcome back' : 'Make your first move'),
                h('p', { class: 'auth-intro__copy' }, this._mode === 'login'
                    ? 'Your games, rating, and next opponent await.'
                    : 'Join the board. Build your rating. Keep every game.'),
            ),
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
                    maxlength: 256,
                    autocomplete: this._mode === 'login' ? 'current-password' : 'new-password',
                    ref: el => this._passInput = el,
                    onkeydown: (e) => { if (e.key === 'Enter') this._submit(); },
                }),
                this._mode === 'register'
                    ? h('div', { class: 'field__hint' }, '8 characters or more.')
                    : null
            ),
            this._mode === 'register' ? h('div', { class: 'field' },
                h('label', { class: 'field__label', for: 'auth-email' }, 'Email'),
                h('input', { id: 'auth-email', class: 'input', type: 'email', autocomplete: 'email', maxlength: 254, 'aria-describedby':'auth-status',
                    ref: el => this._emailInput = el, onkeydown: e => { if(e.key==='Enter') this._submit(); } }),
                h('div', { class: 'field__hint' }, 'Open the email link and confirm your password there to activate your account. This address also supports password recovery.')) : null,
            h('button', {
                type: 'button',
                class: 'btn btn--primary btn--block',
                onclick: () => this._submit(),
                ref: el => this._submitBtn = el,
            }, this._mode === 'login' ? 'Sign in' : 'Create account'),
            this._mode === 'login' ? h('a', { class: 'btn btn--ghost', href: '#/forgot-password' }, 'Forgot password?') : null,
            h('div', { id:'auth-status', role: 'status', 'aria-live': 'polite', class: 'auth-note', ref: el => this._status = el }),

            h('div', { class: 'auth-divider' }, h('span', {}, 'or continue with')),
            h('button', {
                type: 'button',
                class: 'btn btn--block auth-google',
                onclick: () => this._continueWithGoogle(googleUrl),
                ref: el => this._googleBtn = el,
            }, h('img', { class: 'auth-google__icon', src: 'assets/google-signin-icon.svg',
                width: 40, height: 40, alt: '', 'aria-hidden': 'true' }),
                h('span', { ref: el => this._googleLabel = el }, 'Continue with Google')),
            !googleUrl
                ? h('div', { class: 'field__hint auth-note' }, 'Google sign-in is not configured yet.')
                : null,
            h('p', { class: 'auth-note' }, 'Your rating and game history stay with your account.'),
        );
    }

    onMount() {
        if (this._userInput) this._userInput.focus({ preventScroll: true });
    }

    _continueWithGoogle(url) {
        if (this._busy) return;
        if (!url) {
            this.ctx.toast.info('Google sign-in is not configured yet.', { duration: 2800 });
            return;
        }
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
        const email=this._mode==='register'?(this._emailInput.value||'').trim():'';
        if(this._mode==='register' && (!email || !this._emailInput.checkValidity())) {
            this._status.textContent='Enter a valid email address.';this._status.setAttribute('role','alert');
            this.ctx.toast.warning('Enter a valid email address.'); this._emailInput.focus(); return;
        }
        this._busy = true;
        this._status.textContent='';this._status.setAttribute('role','status');
        this._submitBtn.disabled = true;
        this._googleBtn.disabled = true;
        this._submitBtn.setAttribute('aria-busy', 'true');
        this._submitBtn.textContent = this._mode === 'login' ? 'Verifying…' : 'Creating…';

        const mode = this._mode;
        const msg = mode === 'login'
            ? this.ctx.Outbound.login(username, password)
            : this.ctx.Outbound.register(username, password, email);

        try {
            const result = await this._requestAuth(msg);
            if (!result.ok) {
                const copy = ERROR_COPY[result.code] || 'Sign in failed.';
                this._status.textContent=copy;this._status.setAttribute('role','alert');
                this.ctx.toast.error(copy, { duration: 3600 });
                return;
            }

            if(mode==='register') {
                this._passInput.value='';
                this._status.textContent='Check your inbox. If this address can be used, you will receive a verification or set-password link. Your account is not signed in yet.';
                this._status.setAttribute('tabindex','-1'); this._status.focus();
                return;
            }
            this.ctx.session.adopt(result.data);
            this.ctx.toast.success(
                mode === 'login'
                    ? `Welcome back, ${result.data.username}!`
                    : 'Account created. Welcome!',
                { duration: 2600 }
            );
            const nextPath = this.ctx.postAuthPath || '/';
            this.ctx.postAuthPath = null;
            this.ctx.router.go(nextPath);
        } catch (err) {
            console.error('[auth] request failed:', err);
            this.ctx.toast.error(ERROR_COPY.unavailable, { duration: 3600 });
        } finally {
            this._resetSubmitButton();
        }
    }

    /** Authentication is security-critical and must never degrade into a
     *  convincing local demo session. Wait for a definitive live reply or
     *  fail visibly, leaving Session untouched. */
    _requestAuth(message) {
        return this.ctx.authClient.request(message).then(result => {
            if (result.ok && ['login','google_auth'].includes(message.type)) result.data = this.ctx.Inbound.normalize(result.data);
            return result;
        });
    }

    _resetSubmitButton() {
        this._busy = false;
        this._submitBtn.disabled = false;
        this._googleBtn.disabled = false;
        this._submitBtn.removeAttribute('aria-busy');
        this._submitBtn.textContent = this._mode === 'login' ? 'Sign in' : 'Create account';
    }
}
