/**
 * session.js — Phase 7.10 token lifecycle.
 *
 * The two-token model:
 *   * access_token  — a short-lived JWT (15 min). Lives IN MEMORY only.
 *                     An XSS on the page can read `localStorage`, so the
 *                     short-window secret does not go there.
 *   * refresh_token — an opaque 32-byte value (30 days). Lives in
 *                     `localStorage` so a page reload doesn't sign the user
 *                     out. The trade-off is deliberate: an XSS payload
 *                     that reads localStorage can hijack the account. That
 *                     is why the strict CSP in `frontend/vercel.json`
 *                     matters — the whole login story leans on it.
 *
 * Refresh strategy: schedule a background refresh at 80% of the access
 * token's TTL. If the refresh fails (revoked, family destroyed) the
 * session is cleared and the app is nudged back to /login.
 *
 * All wire calls go through the ChessSocket passed at construction. This
 * module is deliberately UI-agnostic — it exposes events (`change`,
 * `expired`) and mutation methods (`adopt`, `logout`, `logoutAll`) and
 * lets screens react.
 */

import { EventBus } from '../core/events.js';
import { storage }  from '../core/storage.js';
import { Inbound }  from './protocol.js';
import { INITIAL_RATING } from '../core/rating.js';

const REFRESH_KEY   = 'refreshToken';
const IDENTITY_KEY  = 'sessionIdentity'; // { username, elo }
const REQUEST_TIMEOUT_MS = 4000;
const REFRESH_LOCK_NAME = 'chess-session-refresh';
const REFRESH_RETRY_MS = 1500;

// Refresh at this fraction of the access-token TTL. 0.8 gives us a full
// 20% of the window to retry on transient failures before the token dies.
const REFRESH_AT = 0.80;

export class Session {
    /**
     * @param {ChessSocket} socket
     */
    constructor(socket) {
        this.socket = socket;
        this.bus    = new EventBus();

        // Access token is memory-only, ever. Never persisted.
        this._access = null;
        this._accessExpiresAt = 0;   // ms epoch

        // Identity is persisted so the UI can render "Welcome, alice" without
        // waiting for a refresh round-trip.
        this._identity = storage.get(IDENTITY_KEY) || null;   // { username, elo }
        this._refresh  = storage.get(REFRESH_KEY)  || null;   // opaque string

        this._refreshTimer = null;
        this._refreshRetryTimer = null;
        this._refreshInFlight = null;
        this._sessionEpoch = 0;
        this._cancelRefreshRequest = null;

        // A persisted refresh token means authentication is not yet known on
        // a hard reload. The router waits on this one-shot promise before it
        // mounts any screen, so no route can mistake "not restored yet" for
        // "signed out". Fresh/signed-out browsers are ready immediately.
        this._restoring = !!this._refresh;
        this._resolveReady = null;
        this._readyPromise = this._restoring
            ? new Promise(resolve => { this._resolveReady = resolve; })
            : Promise.resolve();

        // If we already have a refresh token from a previous session, try to
        // upgrade it into a fresh access token on the next connect.
        socket.onState((state) => {
            if (state === 'connected' && this._refresh && !this._access) {
                // A timeout is not a signed-out decision. Keep the startup
                // gate closed until refresh succeeds or is definitively revoked.
                this._refreshNow().catch(() => {});
            } else if (state === 'connected') {
                this._markReady();
            }
        });
    }

    /* ── Public ── */

    get isAuthenticated()   { return !!this._access; }
    get hasRefreshToken()   { return !!this._refresh; }
    get username()          { return this._identity ? this._identity.username : null; }
    get elo()               { return this._identity ? this._identity.elo : null; }
    get accessToken()       { return this._access; }
    get refreshToken()      { return this._refresh; }
    get isRestoring()       { return this._restoring; }

    on(event, fn) { return this.bus.on(event, fn); }

    /** Resolves once initial persisted-session restoration succeeds or fails. */
    whenReady() { return this._readyPromise; }

    /**
     * Accept a fresh auth_ok payload from register / login / refresh /
     * google_auth. Persists refresh + identity, schedules the next refresh.
     */
    adopt(authOk) {
        this._sessionEpoch++;
        if (this._cancelRefreshRequest) this._cancelRefreshRequest();
        if (this._refreshRetryTimer) {
            clearTimeout(this._refreshRetryTimer);
            this._refreshRetryTimer = null;
        }
        this._access = authOk.access_token || null;
        this._refresh = authOk.refresh_token || null;
        this._accessExpiresAt = authOk.access_expires_in
            ? Date.now() + Number(authOk.access_expires_in) * 1000
            : 0;
        this._identity = {
            username: authOk.username,
            elo:      Number(authOk.elo ?? INITIAL_RATING),
        };
        storage.set(REFRESH_KEY,  this._refresh);
        storage.set(IDENTITY_KEY, this._identity);
        this._scheduleRefresh();
        this._markReady();
        this.bus.emit('change', this._snapshot());
    }

    /** Client-side clear + best-effort logout to the server. */
    async logout() {
        const rt = this._refresh;
        this._clear();
        if (rt && this.socket.isConnected()) {
            try { this.socket.send({ type: 'logout', refresh_token: rt }); }
            catch (_) { /* fire-and-forget */ }
        }
    }

    /** Global sign-out (all devices). Needs a live access token. */
    async logoutAll() {
        if (!this._access) { this._clear(); return true; }
        const ok = await this._requestOnce(
            { type: 'logout_all', access_token: this._access },
            'logout_ok'
        );
        this._clear();
        return !!ok;
    }

    /**
     * Convenience: return the current access token, refreshing on the spot
     * if it's about to expire. Used by handlers that need a token RIGHT
     * NOW (link_google, logout_all).
     */
    async accessTokenForRequest() {
        // A transient startup refresh may have released the router so the UI
        // stays usable while a retry is scheduled. An authenticated action
        // should make one immediate recovery attempt instead of treating that
        // temporary no-access-token state as a sign-out.
        if (!this._access && this._refresh) {
            try { await this._refreshNow(); } catch (_) { return null; }
        }
        if (!this._access) return null;
        if (Date.now() >= this._accessExpiresAt - 5_000) {
            try { await this._refreshNow(); }
            catch (_) {
                // A transport hiccup is not session expiry. The current access
                // token remains usable until its actual expiry.
                if (!this._access || Date.now() >= this._accessExpiresAt) return null;
            }
        }
        return this._access;
    }

    /* ── Internals ── */

    _snapshot() {
        return {
            authenticated: this.isAuthenticated,
            username:      this.username,
            elo:           this.elo,
        };
    }

    _clear() {
        this._sessionEpoch++;
        if (this._cancelRefreshRequest) this._cancelRefreshRequest();
        this._access = null;
        this._refresh = null;
        this._identity = null;
        this._accessExpiresAt = 0;
        if (this._refreshTimer) { clearTimeout(this._refreshTimer); this._refreshTimer = null; }
        if (this._refreshRetryTimer) {
            clearTimeout(this._refreshRetryTimer);
            this._refreshRetryTimer = null;
        }
        storage.remove(REFRESH_KEY);
        storage.remove(IDENTITY_KEY);
        this._markReady();
        this.bus.emit('change', this._snapshot());
    }

    _scheduleRefresh() {
        if (this._refreshTimer) clearTimeout(this._refreshTimer);
        if (!this._accessExpiresAt) return;
        const untilExp = this._accessExpiresAt - Date.now();
        const delay = Math.max(5_000, untilExp * REFRESH_AT);
        this._refreshTimer = setTimeout(
            () => this._refreshNow().catch(() => {}),
            delay
        );
    }

    _markReady() {
        if (!this._restoring) return;
        this._restoring = false;
        if (this._resolveReady) {
            this._resolveReady();
            this._resolveReady = null;
        }
    }

    async _refreshNow() {
        // Socket reconnect events can cluster. Refresh tokens rotate, so two
        // concurrent refreshes with the same token could look like replay and
        // revoke the whole family. Share the one in-flight operation.
        if (this._refreshInFlight) return this._refreshInFlight;
        // localStorage is shared by browser tabs, but the in-memory de-dupe
        // above is not. Web Locks prevents two tabs from rotating the same
        // one-use token concurrently. Once the lock is acquired, reload the
        // token because the preceding tab may just have persisted a successor.
        const task = this._withCrossTabRefreshLock(async () => {
            const latest = storage.get(REFRESH_KEY);
            if (latest) this._refresh = latest;
            return this._performRefreshNow();
        });
        this._refreshInFlight = task;
        try { return await task; }
        finally {
            if (this._refreshInFlight === task) this._refreshInFlight = null;
        }
    }

    async _performRefreshNow() {
        if (!this._refresh) throw new Error('no refresh token');
        const epoch = this._sessionEpoch;
        const attemptedRefresh = this._refresh;
        const result = await this._requestOnceDetailed(
            { type: 'refresh', refresh_token: attemptedRefresh },
            'auth_ok',
            new Set(['invalid_refresh', 'internal']),
            true
        );
        // Logout or a fresh login while this request was pending wins over
        // its eventual response. Never resurrect or overwrite that session.
        if (epoch !== this._sessionEpoch) return;
        if (result.status === 'error' && result.code === 'invalid_refresh') {
            // Another tab may have rotated the shared token after this request
            // was sent (including browsers without Web Locks). If storage now
            // contains its successor, retry that credential before concluding
            // that the login family is invalid.
            const latest = storage.get(REFRESH_KEY);
            if (latest && latest !== attemptedRefresh) {
                this._refresh = latest;
                return this._performRefreshNow();
            }
            // Only the refresh handler's definitive invalid_refresh response
            // proves this credential is unusable. An unrelated auth_error,
            // database hiccup, disconnect, or timeout must not erase a valid
            // persisted login.
            this._clear();
            this.bus.emit('expired');
            throw new Error('refresh invalid');
        }
        if (result.status !== 'ok') {
            this._scheduleRefreshRetry();
            throw new Error('refresh temporarily unavailable');
        }
        this.adopt(result.data);
    }

    _withCrossTabRefreshLock(fn) {
        const locks = typeof navigator !== 'undefined' && navigator.locks;
        if (!locks || typeof locks.request !== 'function') return Promise.resolve().then(fn);
        return locks.request(REFRESH_LOCK_NAME, { mode: 'exclusive' }, fn);
    }

    _scheduleRefreshRetry() {
        if (this._refreshRetryTimer || !this._refresh) return;
        this._refreshRetryTimer = setTimeout(() => {
            this._refreshRetryTimer = null;
            if (this._refresh && this.socket.isConnected()) {
                this._refreshNow().catch(() => {});
            } else if (this._refresh) {
                this._scheduleRefreshRetry();
            }
        }, REFRESH_RETRY_MS);
    }

    /**
     * Send one message, race for `expect` or `auth_error`, or time out.
     * Returns the normalized reply on success, `null` on any failure. The
     * exact failure mode is intentionally opaque to callers.
     */
    _requestOnce(message, expect) {
        return this._requestOnceDetailed(message, expect).then(result =>
            result.status === 'ok' ? result.data : null);
    }

    /**
     * Detailed request outcome used by refresh restoration. When
     * `definitiveErrorCodes` is supplied, unrelated auth_error frames are
     * ignored instead of being allowed to sign the user out.
     */
    _requestOnceDetailed(message, expect, definitiveErrorCodes = null, waitForLateReply = false) {
        return new Promise((resolve) => {
            let settled = false;
            const cleanup = [];
            const finish = (v) => {
                if (settled) return;
                settled = true;
                for (const off of cleanup) { try { off(); } catch (_) {} }
                resolve(v);
            };
            if (waitForLateReply) {
                const cancel = () => finish({ status: 'cancelled' });
                this._cancelRefreshRequest = cancel;
                cleanup.push(() => {
                    if (this._cancelRefreshRequest === cancel) this._cancelRefreshRequest = null;
                });
            }
            cleanup.push(this.socket.on(expect, (m) =>
                finish({ status: 'ok', data: m })));
            cleanup.push(this.socket.on('auth_error', (m) => {
                const code = m && m.code;
                if (definitiveErrorCodes && !definitiveErrorCodes.has(code)) return;
                finish({ status: 'error', code: code || 'unknown' });
            }));
            cleanup.push(this.socket.onState(state => {
                if (state !== 'connected') finish({ status: 'disconnected' });
            }));
            const t = setTimeout(() => {
                if (!waitForLateReply) { finish({ status: 'timeout' }); return; }
                // Rotation may already have committed. Retrying on the SAME
                // live stream supersedes its successor and misattributes late
                // auth_ok replies. Keep the listener/in-flight lock until the
                // reply or a disconnect establishes the next transport state.
                console.warn('[session] refresh response delayed; waiting for the existing request');
            }, REQUEST_TIMEOUT_MS);
            cleanup.push(() => clearTimeout(t));
            this.socket.send(message);
        });
    }
}
