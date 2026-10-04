import assert from 'node:assert/strict';

// Import the real module under both host profiles; no browser or credentials.
for (const [hostname, origin, sealing] of [
    ['localhost', 'http://localhost:8000', 'local-unsealed'],
    ['chess.example.test', 'https://chess.example.test', 'required'],
]) {
    globalThis.location = { hostname, origin };
    const { CONFIG, googleAuthorizeUrl } = await import(
        `../frontend/js/config.js?google-config=${hostname}`);
    const url = new URL(googleAuthorizeUrl());
    assert.equal(url.origin, 'https://hpukcrhxpzyeidoxwqvi.supabase.co');
    assert.equal(url.pathname, '/auth/v1/authorize');
    assert.equal(url.searchParams.get('provider'), 'google');
    assert.equal(url.searchParams.get('redirect_to'), origin + '/');
    assert.equal(CONFIG.authSealing, sealing);
    if (sealing === 'required') {
        assert.ok(CONFIG.pinnedKeys.length > 0);
        for (const pin of CONFIG.pinnedKeys) {
            assert.match(pin, /^[A-Za-z0-9+/]{64}$/);
            assert.equal(Buffer.from(pin, 'base64').length, 48);
        }
    } else {
        assert.deepEqual(CONFIG.pinnedKeys, []);
    }
    const saved = CONFIG.supabaseUrl;
    CONFIG.supabaseUrl = '';
    assert.equal(googleAuthorizeUrl(), null); // Missing config stays disabled.
    CONFIG.supabaseUrl = saved;
}
delete globalThis.location;
console.log('PASS Google config: local/production redirects, disabled state, unchanged sealing');
