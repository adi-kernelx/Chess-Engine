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

const REFRESH_KEY   = 'refreshToken';
const IDENTITY_KEY  = 'sessionIdentity'; // { username, elo }
const REQUEST_TIMEOUT_MS = 4000;

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

        // If we already have a refresh token from a previous session, try to
        // upgrade it into a fresh access token on the next connect.
        socket.onState((state) => {
            if (state === 'connected' && this._refresh && !this._access) {
                this._refreshNow().catch(() => {});
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

    on(event, fn) { return this.bus.on(event, fn); }

    /**
     * Accept a fresh auth_ok payload from register / login / refresh /
     * google_auth. Persists refresh + identity, schedules the next refresh.
     */
    adopt(authOk) {
        this._access = authOk.access_token || null;
        this._refresh = authOk.refresh_token || null;
        this._accessExpiresAt = authOk.access_expires_in
            ? Date.now() + Number(authOk.access_expires_in) * 1000
            : 0;
        this._identity = {
            username: authOk.username,
            elo:      Number(authOk.elo || 1200),
        };
        storage.set(REFRESH_KEY,  this._refresh);
        storage.set(IDENTITY_KEY, this._identity);
        this._scheduleRefresh();
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
        if (!this._access) return null;
        if (Date.now() >= this._accessExpiresAt - 5_000) {
            try { await this._refreshNow(); } catch (_) { return null; }
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
        this._access = null;
        this._refresh = null;
        this._identity = null;
        this._accessExpiresAt = 0;
        if (this._refreshTimer) { clearTimeout(this._refreshTimer); this._refreshTimer = null; }
        storage.remove(REFRESH_KEY);
        storage.remove(IDENTITY_KEY);
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

    async _refreshNow() {
        if (!this._refresh) throw new Error('no refresh token');
        const data = await this._requestOnce(
            { type: 'refresh', refresh_token: this._refresh },
            'auth_ok'
        );
        if (!data) {
            // Refresh failed — the family may be revoked, the token expired,
            // or the server unreachable. Wipe and let the UI redirect.
            this._clear();
            this.bus.emit('expired');
            throw new Error('refresh failed');
        }
        this.adopt(data);
    }

    /**
     * Send one message, race for `expect` or `auth_error`, or time out.
     * Returns the normalized reply on success, `null` on any failure. The
     * exact failure mode is intentionally opaque to callers.
     */
    _requestOnce(message, expect) {
        return new Promise((resolve) => {
            let settled = false;
            const cleanup = [];
            const finish = (v) => {
                if (settled) return;
                settled = true;
                for (const off of cleanup) { try { off(); } catch (_) {} }
                resolve(v);
            };
            cleanup.push(this.socket.on(expect,       (m) => finish(m)));
            cleanup.push(this.socket.on('auth_error', ()  => finish(null)));
            const t = setTimeout(() => finish(null), REQUEST_TIMEOUT_MS);
            cleanup.push(() => clearTimeout(t));
            this.socket.send(message);
        });
    }
}
