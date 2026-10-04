import assert from 'node:assert/strict';

class MemoryStorage {
    constructor() { this.values = new Map(); }
    get length() { return this.values.size; }
    getItem(key) { return this.values.has(key) ? this.values.get(key) : null; }
    setItem(key, value) { this.values.set(key, String(value)); }
    removeItem(key) { this.values.delete(key); }
    key(index) { return [...this.values.keys()][index] ?? null; }
    clear() { this.values.clear(); }
}

class FakeSocket {
    constructor() {
        this.stateListeners = new Set();
        this.messageListeners = new Map();
        this.sent = [];
        this.connected = true;
    }

    onState(fn) {
        this.stateListeners.add(fn);
        return () => this.stateListeners.delete(fn);
    }

    on(type, fn) {
        if (!this.messageListeners.has(type)) this.messageListeners.set(type, new Set());
        this.messageListeners.get(type).add(fn);
        return () => this.messageListeners.get(type)?.delete(fn);
    }

    send(message) { this.sent.push(message); }
    isConnected() { return this.connected; }

    emitState(state) {
        this.connected = state === 'connected';
        for (const fn of [...this.stateListeners]) fn(state);
    }

    emit(type, message = {}) {
        for (const fn of [...(this.messageListeners.get(type) || [])]) fn(message);
    }
}

globalThis.localStorage = new MemoryStorage();
Object.defineProperty(globalThis, 'navigator', { configurable: true, value: {
    locks: {
        requests: 0,
        async request(_name, _options, fn) {
            this.requests += 1;
            return fn();
        },
    },
} });
const { Session } = await import('../frontend/js/net/session.js');

function persistSession(refreshToken = 'refresh-1') {
    localStorage.setItem('chess:refreshToken', JSON.stringify(refreshToken));
    localStorage.setItem('chess:sessionIdentity', JSON.stringify({ username: 'adi', elo: 800 }));
}

// A browser with no persisted session is ready immediately.
{
    localStorage.clear();
    const session = new Session(new FakeSocket());
    await session.whenReady();
    assert.equal(session.isRestoring, false);
    assert.equal(session.isAuthenticated, false);
}

// A persisted session waits for the server, shares clustered reconnect refreshes,
// and becomes authenticated only after auth_ok arrives.
{
    localStorage.clear();
    persistSession();
    const socket = new FakeSocket();
    const session = new Session(socket);
    assert.equal(session.isRestoring, true);

    socket.emitState('connected');
    socket.emitState('connected');
    assert.equal(socket.sent.length, 1);
    assert.deepEqual(socket.sent[0], { type: 'refresh', refresh_token: 'refresh-1' });

    socket.emit('auth_ok', {
        access_token: 'access-1', refresh_token: 'refresh-2',
        username: 'adi', elo: 800,
    });
    await session.whenReady();
    assert.equal(session.isRestoring, false);
    assert.equal(session.isAuthenticated, true);
    assert.equal(session.refreshToken, 'refresh-2');
}

// A rejected persisted session settles the startup gate and clears stale data.
{
    localStorage.clear();
    persistSession('revoked');
    const socket = new FakeSocket();
    const session = new Session(socket);
    socket.emitState('connected');
    socket.emit('auth_error', { code: 'invalid_refresh' });
    await session.whenReady();
    assert.equal(session.isRestoring, false);
    assert.equal(session.isAuthenticated, false);
    assert.equal(session.hasRefreshToken, false);
    assert.equal(localStorage.getItem('chess:refreshToken'), null);
}

// An unrelated auth failure can arrive while route requests overlap session
// restoration. It must not consume the refresh request or clear local state.
{
    localStorage.clear();
    persistSession('still-valid');
    const socket = new FakeSocket();
    const session = new Session(socket);
    socket.emitState('connected');
    socket.emit('auth_error', { code: 'unauthorized' });
    assert.equal(session.hasRefreshToken, true);
    assert.equal(session.isRestoring, true);

    socket.emit('auth_ok', {
        access_token: 'access-2', refresh_token: 'refresh-3',
        username: 'adi', elo: 800,
    });
    await session.whenReady();
    assert.equal(session.isAuthenticated, true);
    assert.equal(session.refreshToken, 'refresh-3');
}

// If another tab rotates the token after this tab sent its request, a delayed
// invalid_refresh for the predecessor must retry the newer shared token rather
// than signing the account out.
{
    localStorage.clear();
    persistSession('predecessor');
    const socket = new FakeSocket();
    const session = new Session(socket);
    socket.emitState('connected');
    assert.equal(socket.sent.at(-1).refresh_token, 'predecessor');

    localStorage.setItem('chess:refreshToken', JSON.stringify('successor'));
    socket.emit('auth_error', { code: 'invalid_refresh' });
    await Promise.resolve();
    assert.equal(socket.sent.at(-1).refresh_token, 'successor');
    assert.equal(session.hasRefreshToken, true);

    socket.emit('auth_ok', {
        access_token: 'access-new', refresh_token: 'successor-2',
        username: 'adi', elo: 800,
    });
    await session.whenReady();
    assert.equal(session.isAuthenticated, true);
    assert.equal(session.refreshToken, 'successor-2');
}

// An authenticated action can actively finish restoration when the first
// startup attempt has not produced an access token yet.
{
    localStorage.clear();
    persistSession('action-refresh');
    const socket = new FakeSocket();
    const session = new Session(socket);
    const tokenPromise = session.accessTokenForRequest();
    await Promise.resolve();
    assert.equal(socket.sent.at(-1).refresh_token, 'action-refresh');
    socket.emit('auth_ok', {
        access_token: 'action-access', refresh_token: 'action-successor',
        username: 'adi', elo: 800, access_expires_in: 900,
    });
    assert.equal(await tokenPromise, 'action-access');
    assert.equal(session.isAuthenticated, true);
    await session.logout();
}

assert.ok(navigator.locks.requests >= 2,
          'session restoration should use the cross-tab refresh lock');

console.log('frontend session restoration tests passed');

// A slow refresh on a live socket must keep its reply listener instead of
// rotating the same token again. The late successor must not be discarded.
{
    const savedSetTimeout = globalThis.setTimeout;
    const savedClearTimeout = globalThis.clearTimeout;
    const timers = new Map();
    let timerId = 0;
    globalThis.setTimeout = (fn, delay) => { timers.set(++timerId, { fn, delay }); return timerId; };
    globalThis.clearTimeout = id => timers.delete(id);
    try {
        localStorage.clear(); persistSession('slow-refresh');
        const socket = new FakeSocket();
        const session = new Session(socket);
        let ready = false;
        session.whenReady().then(() => { ready = true; });
        socket.emitState('connected');
        const timeout = [...timers.values()].find(t => t.delay === 4000);
        assert.ok(timeout); timeout.fn();
        for (let i = 0; i < 10; ++i) await Promise.resolve();
        assert.equal(ready, false);
        assert.equal(session.isRestoring, true);
        assert.equal(session.hasRefreshToken, true);
        const retry = [...timers.values()].find(t => t.delay === 1500);
        assert.equal(retry, undefined, 'do not resend a refresh with an unknown committed outcome');
        for (let i = 0; i < 10; ++i) await Promise.resolve();
        assert.equal(socket.sent.length, 1);
        socket.emit('auth_ok', { access_token: 'restored', refresh_token: 'next', username: 'adi', elo: 800 });
        await session.whenReady();
        assert.equal(session.isAuthenticated, true);
        assert.equal(session.isRestoring, false);
    } finally {
        globalThis.setTimeout = savedSetTimeout;
        globalThis.clearTimeout = savedClearTimeout;
    }
}

// Navigating during Connecting/restoration must not bypass main's startup gate.
{
    const handlers = new Map();
    globalThis.window = { addEventListener: (name, fn) => handlers.set(name, fn) };
    globalThis.location = { hash: '#/tournaments/42' };
    globalThis.document = { title: '' };
    const { Router } = await import('../frontend/js/ui/router.js');
    const router = new Router({ firstChild: null }, {});
    let mounts = 0;
    router.add('/spectate/:id', () => ({ mount() { mounts++; }, unmount() {} }));
    location.hash = '#/spectate/77';
    handlers.get('hashchange')();
    assert.equal(mounts, 0);
    router.start();
    assert.equal(mounts, 1);
    assert.equal(router.current.path, '/spectate/77');
}
console.log('frontend delayed-restoration and early-navigation tests passed');

// Exercise the 12-minute background refresh, not just hard-reload restoration.
// Artificial timers make this deterministic without a browser or real sleep.
{
    const originalSetTimeout = globalThis.setTimeout;
    const originalClearTimeout = globalThis.clearTimeout;
    const timers = new Map();
    let timerId = 0;
    globalThis.setTimeout = (fn, delay) => { timers.set(++timerId, { fn, delay }); return timerId; };
    globalThis.clearTimeout = id => timers.delete(id);
    const drain = async () => { for (let i = 0; i < 12; ++i) await Promise.resolve(); };
    const fire = delay => {
        const entry = [...timers].find(([, timer]) => Math.abs(timer.delay - delay) < 100);
        assert.ok(entry, `expected ${delay}ms timer`);
        timers.delete(entry[0]); entry[1].fn();
    };
    try {
        localStorage.clear();
        const socket = new FakeSocket();
        const session = new Session(socket);
        session.adopt({ access_token: 'initial-access', refresh_token: 'initial-refresh',
            username: 'adi', elo: 800, access_expires_in: 900 });
        let expirations = 0;
        session.on('expired', () => expirations++);
        fire(720000); await drain();
        assert.equal(socket.sent.length, 1);
        fire(4000); await drain();
        assert.equal(socket.sent.length, 1);
        assert.equal([...timers.values()].some(t => t.delay === 1500), false);
        assert.equal(session.isAuthenticated, true);
        socket.emit('auth_ok', { access_token: 'late-access', refresh_token: 'late-successor',
            username: 'adi', elo: 800, access_expires_in: 900 });
        await drain();
        assert.equal(session.refreshToken, 'late-successor');
        assert.equal(expirations, 0);

        // The next scheduled refresh uses the delivered successor, not the
        // predecessor or a token superseded by a duplicate rotation.
        fire(720000); await drain();
        assert.equal(socket.sent.at(-1).refresh_token, 'late-successor');
        socket.emit('auth_error', { code: 'internal' }); await drain();
        assert.equal(session.isAuthenticated, true);
        assert.equal(session.refreshToken, 'late-successor');
        fire(1500); await drain();
        assert.equal(socket.sent.at(-1).refresh_token, 'late-successor');
        socket.emit('auth_ok', { access_token: 'recovered-access', refresh_token: 'recovered-successor',
            username: 'adi', elo: 800, access_expires_in: 900 });
        await drain();
        assert.equal(session.refreshToken, 'recovered-successor');

        // Disconnect still releases the waiter and permits transport recovery.
        fire(720000); await drain();
        socket.emitState('offline'); await drain();
        assert.equal(session.hasRefreshToken, true);
        const sentBeforeReconnect = socket.sent.length;
        fire(1500); await drain();
        assert.equal(socket.sent.length, sentBeforeReconnect, 'offline retry must not queue a rotating token');
        socket.emitState('connected');
        fire(1500); await drain();
        socket.emit('auth_ok', { access_token: 'reconnected', refresh_token: 'reconnected-successor',
            username: 'adi', elo: 800, access_expires_in: 900 });
        await drain();
        assert.equal(session.refreshToken, 'reconnected-successor');

        // A pending reply must not bring the user back after explicit logout.
        fire(720000); await drain();
        await session.logout(); await drain();
        socket.emit('auth_ok', { access_token: 'obsolete', refresh_token: 'obsolete', username: 'adi' });
        await drain();
        assert.equal(session.isAuthenticated, false);
        assert.equal(session.hasRefreshToken, false);
        assert.equal(localStorage.getItem('chess:refreshToken'), null);
        assert.equal(expirations, 0);
    } finally {
        globalThis.setTimeout = originalSetTimeout;
        globalThis.clearTimeout = originalClearTimeout;
    }
}
console.log('frontend automatic-refresh delayed-reply/lifecycle tests passed');
