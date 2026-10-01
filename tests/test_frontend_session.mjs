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
    isConnected() { return true; }

    emitState(state) {
        for (const fn of [...this.stateListeners]) fn(state);
    }

    emit(type, message = {}) {
        for (const fn of [...(this.messageListeners.get(type) || [])]) fn(message);
    }
}

globalThis.localStorage = new MemoryStorage();
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
    socket.emit('auth_error', { message: 'expired' });
    await session.whenReady();
    assert.equal(session.isRestoring, false);
    assert.equal(session.isAuthenticated, false);
    assert.equal(session.hasRefreshToken, false);
    assert.equal(localStorage.getItem('chess:refreshToken'), null);
}

console.log('frontend session restoration tests passed');
