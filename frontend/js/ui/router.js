/**
 * router.js — Minimal hash-based screen router.
 *
 * Design:
 *   - One long-lived <script> loads; nothing tears down the WebSocket on navigation.
 *   - Screens register a factory: (ctx, params) => Screen instance.
 *   - Route match is a simple pattern language: '/', '/game', '/replay/:id'.
 *   - The first matching pattern wins.
 *
 * Screen lifecycle is delegated to Screen.mount() / .unmount(), called here.
 */

export class Router {
    /**
     * @param {HTMLElement} outlet - Element to mount screens into.
     * @param {Object} ctx - Shared context passed to every screen factory
     *                       (services, store, bus, toast, modal, sound, etc.)
     */
    constructor(outlet, ctx) {
        this.outlet = outlet;
        this.ctx = ctx;
        this.routes = [];    // [{ pattern, keys, factory, meta }]
        this.current = null; // { screen, path, factory }
        this._listeners = new Set();

        window.addEventListener('hashchange', () => this._resolve());
    }

    /**
     * Register a route.
     * @param {string} pattern - '/', '/game', '/replay/:id'
     * @param {(ctx, params) => Screen} factory
     * @param {Object} [meta] - e.g. { title: 'Lobby', navKey: 'lobby' }
     */
    add(pattern, factory, meta = {}) {
        const keys = [];
        const rx = new RegExp('^' + pattern.replace(/\//g, '\\/').replace(/:([^/]+)/g, (_, k) => {
            keys.push(k);
            return '([^/]+)';
        }) + '$');
        this.routes.push({ pattern, rx, keys, factory, meta });
        return this;
    }

    start(defaultPath = '/') {
        if (!location.hash || location.hash === '#') {
            history.replaceState(null, '', '#' + defaultPath);
        }
        this._resolve();
    }

    /** Programmatic navigation. */
    go(path) {
        if (location.hash === '#' + path) return;
        location.hash = path;
    }

    /** Fires (path, meta) whenever the active screen changes. */
    onChange(fn) {
        this._listeners.add(fn);
        return () => this._listeners.delete(fn);
    }

    _resolve() {
        const path = (location.hash || '#/').slice(1) || '/';

        for (const route of this.routes) {
            const m = path.match(route.rx);
            if (!m) continue;

            const params = {};
            route.keys.forEach((k, i) => { params[k] = decodeURIComponent(m[i + 1]); });

            this._mount(route, params, path);
            return;
        }

        // No match — fall through to the first registered route.
        if (this.routes.length > 0) {
            this._mount(this.routes[0], {}, this.routes[0].pattern);
        }
    }

    _mount(route, params, path) {
        // Tear down previous screen.
        if (this.current) {
            try { this.current.screen.unmount(); }
            catch (err) { console.error('[router] unmount:', err); }
            this.current = null;
        }

        // Clear outlet.
        while (this.outlet.firstChild) this.outlet.removeChild(this.outlet.firstChild);

        // Instantiate + mount.
        let screen;
        try {
            screen = route.factory(this.ctx, params);
            screen.mount(this.outlet);
        } catch (err) {
            console.error('[router] mount failed for', path, err);
            return;
        }

        this.current = { screen, path, meta: route.meta };

        // Update the document title for better browser-tab identification
        const label = route.meta.navKey
            ? route.meta.navKey.charAt(0).toUpperCase() + route.meta.navKey.slice(1)
            : path.slice(1) || 'Lobby';
        document.title = `${label} — Chess Platform`;

        this._listeners.forEach(fn => fn(path, route.meta));
    }
}
