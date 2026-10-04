/**
 * config.js — Phase 7.10.
 *
 * The one file the operator edits before a production deploy. Everything
 * else in the frontend imports its values from here so a mis-pointed URL
 * or a stale key pin can be fixed in one place.
 *
 * Environment resolution:
 *   * `localhost` / `127.*` / `[::1]` hostnames → local dev backend on 9000.
 *   * Anything else                              → production backend below.
 *
 * The frontend is served static from Vercel; there is no build-time env
 * inlining, so "environment-aware" means "picks by hostname at load time."
 * That's enough for two environments; if we grow a staging tier, promote
 * this to a proper mapping.
 *
 * PINNED_KEYS is the ML-DSA-65 identity fingerprint(s) the sealed-envelope
 * client will accept from the server. Every entry is
 *   base64(SHA-384(public_key))
 * as printed by `tools/gen_server_identity`. Multiple entries are allowed so
 * a rotation is possible without a client-side flag day: publish new + old,
 * cut over servers, then remove the old entry on the next frontend deploy.
 * An empty array rejects every sealed offer, never trusts an arbitrary key.
 * Loopback uses the explicit unsealed development profile instead; production
 * requires a configured pin and fails closed when the list is empty.
 */

const IS_LOCAL = /^(localhost|127(\.\d+){3}|\[::1\])$/.test(location.hostname);

export const CONFIG = {
    // Production never falls back to clear authentication. Local loopback
    // preserves the existing explicitly unsealed development server profile.
    authSealing: IS_LOCAL ? 'local-unsealed' : 'required',
    // WebSocket URL for the chess backend.
    //
    // Local: the C++ server on 9000 over ws:// (no TLS locally — the browser
    // does not require it for loopback origins).
    //
    // Prod: replace <cloud-run-host> with the actual Cloud Run URL. The scheme
    // MUST be wss:// — a ws:// value here would be blocked by the CSP in
    // frontend/vercel.json AND rejected by browsers as mixed content.
    wsUrl: IS_LOCAL
        ? 'ws://localhost:9000'
        : 'wss://chess-server-591282043392.asia-northeast1.run.app',

    // Server ML-DSA-65 identity fingerprints. Compare bytewise to
    // base64(SHA-384(offer.identity_pk)) received in `seal_key`.
    // Production identity prepared locally; install its matching private key
    // through Secret Manager, never in the image or repository.
    pinnedKeys: IS_LOCAL ? [] : [
        '0Bh+A9rKUcKQCna6lkJT4kXHwRJ8pKNUH3RxuLk7hnUEZ91xn/jST6B3eWKv7JWN',
    ],

    // Supabase project used for Google Sign-In. The OAuth flow is a plain
    // window.location redirect — no @supabase/supabase-js needed, no
    // <script> tag, and therefore no CSP relaxation beyond the connect-src
    // entry in vercel.json.
    //
    // If supabaseUrl is empty, the Google option remains visible but explains
    // that it becomes active after cloud deployment.
    supabaseUrl:      'https://hpukcrhxpzyeidoxwqvi.supabase.co',
    supabaseAnonKey:  IS_LOCAL ? '' : '<supabase-anon-key>',

    // Where Supabase redirects the browser after Google consent. Must be
    // registered in the Supabase project's "Additional Redirect URLs".
    // Default: the current origin's root — the callback runs on every page
    // load and parses `location.hash` regardless of the entry route.
    supabaseRedirect: (typeof location !== 'undefined')
        ? location.origin + '/'
        : '',
};

/**
 * Build the Supabase OAuth authorize URL for Google.
 * Returns null if Supabase is not configured (Google Sign-In disabled).
 */
export function googleAuthorizeUrl() {
    if (!CONFIG.supabaseUrl) return null;
    const params = new URLSearchParams({
        provider:     'google',
        redirect_to:  CONFIG.supabaseRedirect,
    });
    return `${CONFIG.supabaseUrl}/auth/v1/authorize?${params.toString()}`;
}
