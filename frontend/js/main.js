/**
 * main.js — Entry point.
 *
 * Wires services (store, bus, toast, modal, router) into the shell,
 * registers all screen routes, and boots the router.
 *
 * Networking (net/socket, net/protocol, net/capability) lands in Part 2.
 * Real lobby/game screens land in Part 4. For now every route mounts
 * a PlaceholderScreen so the shell can be verified end-to-end.
 */

import { Store }    from './core/store.js';
import { EventBus } from './core/events.js';
import { q, h }     from './core/dom.js';
import { Router }   from './ui/router.js';
import { Toaster }  from './ui/toast.js';
import { ModalHost } from './ui/modal.js';
import { ChessSocket } from './net/socket.js';
import { Capability } from './net/capability.js';
import { Outbound, Inbound } from './net/protocol.js';
import { Session } from './net/session.js';
import { CONFIG }  from './config.js';
import { SoundEngine } from './ui/sound.js';
import { placeholder } from './screens/placeholder.js';
import { BoardTestScreen } from './screens/board-test.js';
import { LandingScreen } from './screens/landing.js';
import { LobbyScreen } from './screens/lobby.js';
import { JoinScreen }  from './screens/join.js';
import { PuzzleScreen } from './screens/puzzle.js';
import { GameScreen } from './screens/game.js';
import { SettingsScreen }   from './screens/settings.js';
import { AuthScreen }       from './screens/auth.js';
import { ProfileScreen }    from './screens/profile.js';
import { LeaderboardScreen } from './screens/leaderboard.js';
import { SpectateListScreen, SpectateWatchScreen } from './screens/spectate.js';
import { ReplayListScreen, ReplayDetailScreen }    from './screens/replay.js';
import { TournamentScreen }                        from './screens/tournament.js';

function boot() {
    const store = new Store();
    const bus   = new EventBus();
    const toast = new Toaster(q('#toast-region'));
    const modal = new ModalHost();

    // ── Connection indicator ──
    const connEl = q('#nav-conn');
    const connText = q('#nav-conn-text');
    const setConn = (state) => {
        store.setConn(state);
        connEl.classList.remove('is-connected', 'is-connecting', 'is-offline');
        if (state === 'connected')  { connEl.classList.add('is-connected');  connText.textContent = 'Connected'; }
        else if (state === 'connecting') { connEl.classList.add('is-connecting'); connText.textContent = 'Connecting…'; }
        else                        { connEl.classList.add('is-offline');    connText.textContent = 'Offline'; }
    };
    setConn('offline');

    // ── WebSocket + capability probe + session ──
    const socket     = new ChessSocket(CONFIG.wsUrl);
    const capability = new Capability(socket);
    const session    = new Session(socket);

    // Session hydration → keep the store's session segment in lock-step so
    // existing screens (settings, etc.) reflect who the user is without
    // knowing about `Session`. `Session` is the source of truth for tokens;
    // `store.session` is the source of truth for what UI to render.
    session.on('change', (snap) => {
        store.setSession({
            username: snap.username || 'Player',
            elo:      snap.elo      || 1200,
            // `token` is intentionally NOT set — tokens live in Session only.
            authenticated: snap.authenticated,
        });
    });
    session.on('expired', () => {
        toast.warning('Signed out — please sign in again.', { duration: 3200 });
        if (location.hash !== '#/login') location.hash = '#/login';
    });

    // Drive the nav indicator + toast one-shot on first (re)connect.
    let hasEverConnected = false;
    socket.onState((state) => {
        setConn(state);
        if (state === 'connected') {
            if (!hasEverConnected) {
                hasEverConnected = true;
                toast.success('Connected to chess server.', { duration: 2400 });
            } else {
                toast.info('Reconnected.', { duration: 2400 });
            }
        } else if (state === 'offline' && hasEverConnected) {
            toast.warning('Connection lost — reconnecting…', { duration: 3000 });
        }
    });

    // Try to connect right away. If the C++ server isn't running the socket
    // will retry with exponential backoff (1s → 2s → 4s → … 30s cap).
    socket.connect();

    // ── Sound (Web Audio, respects store.prefs.sound) ──
    const sound = new SoundEngine(store);

    // ── Ctx passed to every screen ──
    const ctx = { store, bus, toast, modal, router: null, setConn, socket, capability, session, Outbound, Inbound, sound };

    // Google-callback handler runs AFTER router is built (below); factored
    // out so the code that needs `router` is defined after it exists.
    let oauthCallback = null;
    (function prepareOAuthCallback() {
        try {
            const raw = (location.hash || '').replace(/^#\/?/, '');
            if (!raw.includes('access_token=') || !raw.includes('refresh_token=')) return;
            const params = new URLSearchParams(raw);
            const supabaseJwt = params.get('access_token');
            if (!supabaseJwt) return;

            // Scrub the token out of the URL bar immediately — bookmarks
            // and history entries must not carry a bearer token.
            history.replaceState(null, '', location.pathname + location.search + '#/login');

            oauthCallback = () => {
                const kick = () => {
                    socket.send(Outbound.googleAuth(supabaseJwt));
                    const off = socket.on('auth_ok', (msg) => {
                        off();
                        session.adopt(msg);
                        toast.success(`Signed in as ${msg.username}`, { duration: 2600 });
                        ctx.router.go('/');
                    });
                    const offErr = socket.on('auth_error', (msg) => {
                        offErr();
                        toast.error('Google sign-in failed (' + (msg.code || 'error') + ').',
                                    { duration: 3600 });
                    });
                };
                if (socket.isConnected()) kick();
                else socket.onState((s) => { if (s === 'connected') kick(); });
            };
        } catch (err) {
            console.error('[main] oauth callback prep:', err);
        }
    })();

    // ── Router ──
    const router = new Router(q('#outlet'), ctx);
    ctx.router = router;

    // G1: '/' is the landing screen; the old lobby is at '/play'. Returning
    // visitors skip the landing so regulars go straight to the play menu.
    router.add('/',              (ctx) => new LandingScreen(ctx), { navKey: 'landing' });
    router.add('/play',          (ctx) => new LobbyScreen(ctx),   { navKey: 'play' });
    // Both patterns mount GameScreen; the URL variant makes games
    // refreshable, back-buttonable, and shareable (Part G2).
    router.add('/game/:gameId',  (ctx) => new GameScreen(ctx),    { navKey: 'game', hideNav: true });
    router.add('/game',          (ctx) => new GameScreen(ctx),    { navKey: 'game', hideNav: true });
    router.add('/join/:gameId',  (ctx, params) => new JoinScreen(ctx, params), { hideNav: true });
    router.add('/puzzle',        (ctx) => new PuzzleScreen(ctx), { navKey: 'puzzle' });
    router.add('/spectate/:gameId', (ctx, params) => new SpectateWatchScreen(ctx, params), { navKey: 'spectate' });
    router.add('/spectate',         (ctx) => new SpectateListScreen(ctx),                  { navKey: 'spectate' });
    router.add('/tournaments',      (ctx) => new TournamentScreen(ctx),                    { navKey: 'tournaments' });
    router.add('/leaderboard',   (ctx) => new LeaderboardScreen(ctx),                                     { navKey: 'leaderboard' });
    router.add('/replay/:gameId', (ctx, params) => new ReplayDetailScreen(ctx, params));
    router.add('/replay',         (ctx) => new ReplayListScreen(ctx),                    { navKey: 'replay' });
    router.add('/profile',       (ctx) => new ProfileScreen(ctx),                                        { navKey: 'profile' });
    router.add('/login',         (ctx) => new AuthScreen(ctx));
    router.add('/settings',      (ctx) => new SettingsScreen(ctx),                                       { navKey: 'settings' });
    router.add('/board-test',    (ctx) => new BoardTestScreen(ctx));

    // ── Mobile nav toggle ──
    const navToggle = q('#nav-toggle');
    const navLinksEl = q('#nav-links');
    if (navToggle && navLinksEl) {
        navToggle.addEventListener('click', () => {
            const open = navLinksEl.classList.toggle('is-open');
            navToggle.setAttribute('aria-expanded', String(open));
            navToggle.setAttribute('aria-label', open ? 'Close menu' : 'Open menu');
            const useEl = navToggle.querySelector('use');
            if (useEl) useEl.setAttribute('href', open ? '#i-close' : '#i-menu');
        });
    }

    // ── Highlight active nav link + close mobile menu on navigate ──
    const navLinks = document.querySelectorAll('[data-nav-key]');
    router.onChange((path, meta) => {
        navLinks.forEach(link => {
            link.classList.toggle('is-active', meta && link.dataset.navKey === meta.navKey);
        });
        if (navLinksEl) navLinksEl.classList.remove('is-open');
        if (navToggle) {
            navToggle.setAttribute('aria-expanded', 'false');
            navToggle.setAttribute('aria-label', 'Open menu');
            const useEl = navToggle.querySelector('use');
            if (useEl) useEl.setAttribute('href', '#i-menu');
        }
        const outlet = q('#outlet');
        if (outlet) {
            outlet.setAttribute('tabindex', '-1');
            outlet.focus({ preventScroll: true });
        }
    });

    // Returning-visitor bypass: after the first successful game, the landing
    // screen sets storage['visited']. From then on, going to '#/' punts to
    // '#/play' — regulars never see the marketing shell twice.
    (function landingBypass() {
        try {
            const path = (location.hash || '#/').slice(1) || '/';
            const stored = localStorage.getItem('chess:visited');
            if (path === '/' && stored) {
                history.replaceState(null, '', '#/play');
            }
        } catch (_) { /* private mode etc — no bypass */ }
    })();

    // Fire the queued OAuth callback (if any) now that router is live.
    if (oauthCallback) oauthCallback();

    router.start('/');

    // Dev handles for quick console poking
    Object.assign(window, { chess: { store, bus, router, toast, modal, socket, capability, session, Outbound, Inbound, sound } });
}

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot, { once: true });
} else {
    boot();
}
