// Called only by the isolated local Python harness; real browser-compatible
// crypto modules, real WebSocket client and C++ sealed-required auth routes.
import assert from 'node:assert/strict';
import { AuthClient } from '../frontend/js/net/auth-client.js';
import { ChessSocket } from '../frontend/js/net/socket.js';
import { createSealer, fromBase64, toBase64 } from '../frontend/js/net/sealed.js';
import { pqcProvider } from '../frontend/js/net/pqc-provider.js';
const [url, pin] = process.argv.slice(2);
assert.match(url, /^ws:\/\/127\.0\.0\.1:\d+$/);
assert.ok(pin);
const socket = new ChessSocket(url);
const frames = [];
const original = socket.sendImmediate.bind(socket);
socket.sendImmediate = frame => { frames.push(frame); return original(frame); };
const client = new AuthClient(socket, { pinnedKeys: [pin], timeoutMs: 3000 });
const once = (type, send) => new Promise((resolve, reject) => {
    const off = socket.on(type, value => { clearTimeout(timer); off(); resolve(value); });
    const timer = setTimeout(() => { off(); reject(Error('response timeout')); }, 3000);
    if (send) original(send);
});
const silent = async frame => {
    let replied = false;
    const offOk = socket.on('auth_ok', () => { replied = true; });
    const offError = socket.on('auth_error', () => { replied = true; });
    original(frame);
    await new Promise(resolve => setTimeout(resolve, 150));
    offOk(); offError();
    assert.equal(replied, false, 'malformed/replayed/clear required auth must be silently rejected');
};
try {
    await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(Error('connect timeout')), 5000);
        const off = socket.onState(state => { if (state === 'connected') { clearTimeout(timer); off(); resolve(); } });
        socket.connect();
    });
    const username = process.env.SEALED_TEST_USERNAME;
    assert.match(username, /^seal_[0-9a-f]{12}$/);
    const password = 'Disposable-Seal-Test-42!';
    const registered = await client.request({ type: 'register', username, password, email: 'synthetic@example.invalid' });
    assert.equal(registered.code, 'email_unavailable'); // No real SMTP credentials in this fixture.
    const registeredFrame = frames.at(-1);
    await silent(registeredFrame); // One-time-key replay denial.
    await silent({ type: 'login', username, password }); // No cleartext fallback on server.
    const login = await client.request({ type: 'login', username, password });
    assert.equal(login.ok, true, login.code);
    assert.equal(login.data.username, username);
    const bad = await client.request({ type: 'login', username, password: 'wrong-password' });
    assert.equal(bad.code, 'invalid_credentials');
    const refresh = await once('auth_ok', { type: 'refresh', refresh_token: login.data.refresh_token });
    assert.ok(refresh.access_token && refresh.refresh_token);
    assert.notEqual(refresh.refresh_token, login.data.refresh_token);
    const google = await client.request({ type: 'google_auth', supabase_jwt: 'invalid-test-jwt' });
    assert.equal(google.code, 'invalid_google'); // Reaches Google route only after valid opening.
    const googleLogin = await client.request({ type: 'google_auth', supabase_jwt: process.env.SEALED_TEST_GOOGLE_JWT });
    assert.equal(googleLogin.ok, true, googleLogin.code);
    for(const type of ['request_password_reset','reset_password','verify_email','set_recovery_email']) {
        const result=await client.request({type,email:'synthetic@example.invalid',email_token:'a'.repeat(43),password,
            access_token:login.data.access_token});
        assert.equal(result.code,'email_unavailable'); // Only after correct seal opening.
        await silent({type});
    }
    const offer = await once('seal_key', { type: 'seal_request' });
    const sealed = await createSealer(pqcProvider, [pin]).seal(offer, { type: 'login', username, password });
    const tag = fromBase64(sealed.tag); tag[0] ^= 1;
    await silent({ type: 'login', sealed: { ...sealed, tag: toBase64(tag) } });
    await silent({ type: 'login', sealed }); // A failed forgery burns the one-time key too.
    const relabelOffer = await once('seal_key', { type: 'seal_request' });
    const relabel = await createSealer(pqcProvider, [pin]).seal(relabelOffer, { type: 'register', username, password });
    await silent({ type: 'login', sealed: relabel });
    const legacyOffer = await once('seal_key', { type: 'seal_request' });
    const legacy = await createSealer(pqcProvider, [pin]).seal(legacyOffer, { username, password });
    await silent({ type: 'login', sealed: legacy });
    assert.equal(JSON.stringify(frames).includes(password), false);
    assert.equal(JSON.stringify(frames).includes('invalid-test-jwt'), false);
    assert.equal(JSON.stringify(frames).includes(process.env.SEALED_TEST_GOOGLE_JWT), false);
    console.log('PASS C++/JS ML-DSA + ML-KEM interoperability; sealed email/login/Google routing, missing SMTP denial, wrong-password handling, refresh, tamper, replay and downgrade denial');
} finally {
    socket.close();
}
// Native Node WebSocket can retain its transport close timers; this isolated
// probe has completed all assertions and closes only its own process/socket.
process.exit(0);
