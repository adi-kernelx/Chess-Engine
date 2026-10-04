import assert from 'node:assert/strict';
import { AuthClient } from '../frontend/js/net/auth-client.js';
import { ChessSocket } from '../frontend/js/net/socket.js';
import { parseOffer, fromBase64, toBase64, sha384, offerSigningInput } from '../frontend/js/net/sealed.js';
import { pqcProvider } from '../frontend/js/net/pqc-provider.js';
import { ml_kem768, ml_dsa65 } from '../frontend/js/vendor/pqc.min.js';
import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';

const bundle = readFileSync(new URL('../frontend/js/vendor/pqc.min.js', import.meta.url));
const manifest = JSON.parse(readFileSync(new URL('../frontend/js/vendor/pqc-manifest.json', import.meta.url)));
assert.equal(createHash('sha256').update(bundle).digest('hex'), manifest.bundleSha256);
globalThis.location = { hostname: 'localhost', origin: 'http://localhost' };
globalThis.document = { readyState: 'loading', addEventListener() {} };
await import('../frontend/js/main.js'); // Parse/import all changed integration modules without UI work.
const { AuthScreen } = await import('../frontend/js/screens/auth.js');

class Socket {
    url = 'ws://127.0.0.1:9000';
    connected = true;
    handlers = new Map();
    states = new Set();
    sent = [];
    isConnected() { return this.connected; }
    on(type, fn) {
        if (!this.handlers.has(type)) this.handlers.set(type, new Set());
        this.handlers.get(type).add(fn);
        return () => this.handlers.get(type).delete(fn);
    }
    onState(fn) { this.states.add(fn); return () => this.states.delete(fn); }
    sendImmediate(msg) { if (!this.connected) return false; this.sent.push(msg); return true; }
    emit(type, msg) { for (const fn of [...this.handlers.get(type) || []]) fn(msg); }
    disconnect() { this.connected = false; for (const fn of [...this.states]) fn('offline'); }
    assertClean() {
        assert.equal(this.states.size, 0);
        assert.equal([...this.handlers.values()].reduce((sum, set) => sum + set.size, 0), 0);
    }
}
const identity = ml_dsa65.keygen();
const kem = ml_kem768.keygen();
const ecc = await pqcProvider.x25519Generate();
const offer = { identity_pk: toBase64(identity.publicKey), key_id: toBase64(crypto.getRandomValues(new Uint8Array(16))),
    kem_ek: toBase64(kem.publicKey), x25519_pk: toBase64(ecc.publicKey), expires_in: 120,
    signature: toBase64(new Uint8Array(3309)) };
offer.signature = toBase64(ml_dsa65.sign(offerSigningInput(parseOffer(offer)), identity.secretKey));
const pin = toBase64(await sha384(identity.publicKey));
const credentials = { type: 'login', username: 'sealed_test', password: 'test-password-only' };
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const until = async fn => { for (let i = 0; i < 100 && !fn(); i++) await wait(2); assert.ok(fn()); };
const make = options => {
    const socket = new Socket();
    return { socket, client: new AuthClient(socket, { pinnedKeys: [pin], timeoutMs: 500, ...options }) };
};
for (const type of ['login', 'register', 'google_auth']) {
    const { socket, client } = make();
    const result = client.request(type === 'google_auth' ? { type, supabase_jwt: 'test-jwt-only' } : { ...credentials, type });
    socket.emit('seal_key', offer);
    socket.emit('seal_key', offer); // A duplicate cannot produce a second credential frame.
    await until(() => socket.sent.length === 2);
    assert.deepEqual(Object.keys(socket.sent[1]), ['type', 'sealed']);
    assert.equal(JSON.stringify(socket.sent).includes(credentials.password), false);
    assert.equal(JSON.stringify(socket.sent).includes('test-jwt-only'), false);
    socket.emit('auth_ok', { username: 'sealed_test' });
    assert.equal((await result).ok, true);
    socket.assertClean();
}
for (const change of [value => ({ ...value, signature: toBase64(new Uint8Array(3309)) }),
    value => ({ ...value, expires_in: 121 }), value => ({ ...value, kem_ek: 'bad' })]) {
    const { socket, client } = make();
    const result = client.request(credentials);
    socket.emit('seal_key', change(offer));
    assert.equal((await result).code, 'seal_failed');
    assert.equal(socket.sent.length, 1);
    socket.assertClean();
}
for (const options of [{ pinnedKeys: ['wrong'] }, { provider: async () => { throw Error('missing'); } }]) {
    const { socket, client } = make(options);
    const result = client.request(credentials);
    socket.emit('seal_key', offer);
    assert.equal((await result).code, 'seal_failed');
    assert.equal(socket.sent.length, 1);
    socket.assertClean();
}
{
    const { socket, client } = make({ pinnedKeys: [] });
    assert.equal((await client.request(credentials)).code, 'seal_configuration');
    assert.equal(socket.sent.length, 0);
}
{
    const { socket, client } = make({ timeoutMs: 5 });
    const pending = client.request(credentials);
    assert.equal((await client.request(credentials)).code, 'auth_busy');
    assert.equal((await pending).code, 'unavailable');
    socket.emit('seal_key', offer);
    assert.equal(socket.sent.length, 1);
    socket.assertClean();
}
{
    const { socket, client } = make();
    socket.sendImmediate = () => false;
    assert.equal((await client.request(credentials)).code, 'unavailable');
    assert.equal(client.busy, false);
    socket.assertClean();
}
{
    let release;
    const { socket, client } = make({ provider: () => new Promise(resolve => { release = resolve; }) });
    const pending = client.request(credentials);
    socket.emit('seal_key', offer);
    socket.disconnect();
    assert.equal((await pending).code, 'unavailable');
    release(pqcProvider);
    await wait(5);
    assert.equal(socket.sent.length, 1);
    socket.assertClean();
}
{
    const { socket, client } = make({ policy: 'local-unsealed', pinnedKeys: [] });
    const pending = client.request(credentials);
    socket.emit('auth_error', { code: 'invalid_credentials' });
    assert.equal((await pending).code, 'invalid_credentials');
    assert.deepEqual(socket.sent, [credentials]);
    socket.assertClean();
    socket.url = 'wss://production.example';
    assert.equal((await client.request(credentials)).code, 'seal_configuration');
    assert.equal(socket.sent.length, 1);
}
{
    const { socket, client } = make();
    socket.connected = false;
    assert.equal((await client.request(credentials)).code, 'unavailable');
    socket.assertClean();
    const native = new ChessSocket();
    assert.equal(native.sendImmediate(credentials), false);
    assert.equal(native._queue.length, 0);
}
{
    const screen = Object.create(AuthScreen.prototype);
    screen.ctx = { authClient: { request: async () => ({ ok: true, data: { username: 'test' } }) },
        Inbound: { normalize: data => ({ ...data, normalized: true }) } };
    assert.equal((await screen._requestAuth(credentials)).data.normalized, true);
}
{
    const screen = Object.create(AuthScreen.prototype);
    let adopted = 0, navigated = 0, warnings = 0, errors = 0, release;
    screen._mode = 'login'; screen._busy = false;
    screen._userInput = { value: 'test_user' }; screen._passInput = { value: '' };
    screen._submitBtn = { disabled: false, textContent: 'Sign in', setAttribute() {}, removeAttribute() {} };
    screen._googleBtn = { disabled: false };
    screen.ctx = { Outbound: { login: (username, password) => ({ type: 'login', username, password }) },
        toast: { warning() { warnings++; }, error() { errors++; }, success() {} },
        session: { adopt() { adopted++; } }, router: { go() { navigated++; } } };
    screen._requestAuth = () => new Promise(resolve => { release = resolve; });
    await screen._submit();
    assert.equal(warnings, 1); // Empty credentials never leave the form.
    screen._passInput.value = 'test-password';
    const pending = screen._submit();
    assert.equal(screen._submitBtn.disabled, true);
    assert.equal(screen._submitBtn.textContent, 'Verifying…');
    release({ ok: false, code: 'seal_failed' });
    await pending;
    assert.equal(errors, 1); assert.equal(adopted, 0); assert.equal(navigated, 0);
    assert.equal(screen._submitBtn.disabled, false); assert.equal(screen._busy, false);
    const success = screen._submit();
    release({ ok: true, data: { username: 'test_user' } });
    await success;
    assert.equal(adopted, 1); assert.equal(navigated, 1);
    assert.equal(screen._submitBtn.textContent, 'Sign in');
}
assert.equal(fromBase64('AA\u013d='), null);
assert.equal(parseOffer({ ...offer, expires_in: 0x100000000 }), null);
{
    const { createSealer } = await import('../frontend/js/net/sealed.js');
    const secret = new Uint8Array(32).fill(7);
    const failingProvider = { ...pqcProvider,
        mlKemEncapsulate: () => ({ ct: new Uint8Array(1088), sharedSecret: secret }),
        x25519Derive: async () => { throw Error('derive failed'); } };
    await assert.rejects(createSealer(failingProvider, [pin]).seal(offer, credentials));
    assert.equal(secret.every(byte => byte === 0), true, 'wipe temporary secret on failure too');
    const ephemeral = await pqcProvider.x25519Generate();
    assert.equal(ephemeral.privateKey.extractable, false);
}
console.log('PASS sealed-auth provider, wire secrecy, trust/signature failures, timeout, disconnect, no downgrade/queue, imports and local compatibility');
