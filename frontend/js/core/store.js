/**
 * store.js — App state container.
 * Segments:
 *   session — { username, elo, token? }
 *   prefs   — { boardTheme, pieceSet, showCoords, sound, animate }
 *   game    — active game snapshot (or null when in lobby)
 *   conn    — { state: 'offline'|'connecting'|'connected' }
 *
 * Changes emit '<segment>:change' on the bus, and a wildcard 'change'.
 */

import { EventBus } from './events.js';
import { storage } from './storage.js';

const PREFS_KEY = 'prefs';
const SESSION_KEY = 'session';

const DEFAULT_PREFS = {
    boardTheme: 'classic',
    pieceSet:   'classic',
    showCoords: true,
    sound:      true,
    animate:    true,
};

const DEFAULT_SESSION = {
    username: 'Player',
    elo:      1200,
    token:    null,
};

export class Store {
    constructor() {
        this.bus = new EventBus();

        this._state = {
            session: { ...DEFAULT_SESSION, ...(storage.get(SESSION_KEY) || {}) },
            prefs:   { ...DEFAULT_PREFS,   ...(storage.get(PREFS_KEY)   || {}) },
            game:    null,
            conn:    { state: 'offline' },
        };
    }

    get session() { return this._state.session; }
    get prefs()   { return this._state.prefs; }
    get game()    { return this._state.game; }
    get conn()    { return this._state.conn; }

    setSession(patch) {
        this._state.session = { ...this._state.session, ...patch };
        storage.set(SESSION_KEY, this._state.session);
        this._notify('session');
    }

    setPrefs(patch) {
        this._state.prefs = { ...this._state.prefs, ...patch };
        storage.set(PREFS_KEY, this._state.prefs);
        this._notify('prefs');
    }

    setGame(game) {
        this._state.game = game;
        this._notify('game');
    }

    setConn(state) {
        if (this._state.conn.state === state) return;
        this._state.conn = { state };
        this._notify('conn');
    }

    on(segment, fn) { return this.bus.on(segment + ':change', fn); }

    _notify(segment) {
        this.bus.emit(segment + ':change', this._state[segment]);
        this.bus.emit('change', { segment, value: this._state[segment] });
    }
}
