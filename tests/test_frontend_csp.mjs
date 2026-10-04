import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const read = name => readFile(new URL('../frontend/' + name, import.meta.url), 'utf8');
const config = JSON.parse(await read('vercel.json'));
const globalHeaders = config.headers.find(rule => rule.source === '/(.*)').headers;
const csp = globalHeaders.find(header => header.key === 'Content-Security-Policy').value;

function validate(policy) {
    const directives = new Map();
    for (const part of policy.split(';').map(value => value.trim()).filter(Boolean)) {
        const [name, ...sources] = part.split(/\s+/);
        assert.ok(!directives.has(name), `Duplicate directive: ${name}`);
        directives.set(name, sources);
    }
    // Rasterized SVG images need blob URLs, but scripts/connect sources must
    // not inherit that exception or permit arbitrary external destinations.
    assert.deepEqual(directives.get('img-src'), ["'self'", 'blob:']);
    assert.deepEqual(directives.get('default-src'), ["'self'"]);
    assert.deepEqual(directives.get('script-src'), ["'self'"]);
    assert.deepEqual(directives.get('connect-src'), ["'self'",
        'wss://chess-server-591282043392.asia-northeast1.run.app',
        'https://hpukcrhxpzyeidoxwqvi.supabase.co']);
    for (const name of ['object-src', 'frame-ancestors', 'base-uri']) {
        assert.deepEqual(directives.get(name), ["'none'"]);
    }
    assert.deepEqual(directives.get('form-action'), ["'self'"]);
}
validate(csp);
assert.throws(() => validate(csp.replace("img-src 'self' blob:;", '')));
assert.throws(() => validate(csp.replace("img-src 'self' blob:", "img-src 'self'")));
assert.throws(() => validate(csp.replace("img-src 'self' blob:", 'img-src * blob:')));
assert.throws(() => validate(csp.replace("script-src 'self'", "script-src 'self' blob:")));
assert.throws(() => validate(csp + "; img-src *"));

assert.equal(globalHeaders.find(header => header.key === 'X-Content-Type-Options').value, 'nosniff');
assert.equal(globalHeaders.find(header => header.key === 'Referrer-Policy').value, 'no-referrer');
assert.ok(globalHeaders.find(header => header.key === 'Strict-Transport-Security'));
const { BoardRenderer } = await import('../frontend/js/board/renderer.js');
assert.equal(typeof BoardRenderer, 'function');
const renderer = await read('js/board/renderer.js');
assert.match(renderer, /URL\.createObjectURL\(blob\)/);
assert.match(renderer, /img\.src = objUrl/);
for (const set of ['classic', 'modern']) {
    const sprite = await read(`assets/pieces/${set}.svg`);
    for (const side of ['w', 'b']) {
        for (const piece of ['king', 'queen', 'rook', 'bishop', 'knight', 'pawn']) {
            assert.ok(sprite.includes(`id="${side}_${piece}"`), `${set}: missing ${side}_${piece}`);
        }
    }
}
console.log('PASS CSP: piece Blob images permitted, scripts/connect remain restricted, negative-policy cases and both complete sprites');
