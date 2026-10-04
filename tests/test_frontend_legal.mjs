import assert from 'node:assert/strict';
import { readFile, access } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const root = fileURLToPath(new URL('../frontend/', import.meta.url));
const read = name => readFile(path.join(root, name), 'utf8');
for (const name of ['index.html', 'privacy.html', 'terms.html']) {
    const html = await read(name);
    assert.match(html, /<html lang="en">/);
    assert.match(html, /name="viewport"/);
    assert.match(html, /<footer\b/);
    assert.match(html, /href="privacy\.html"/);
    assert.match(html, /href="terms\.html"/);
    assert.match(html, /href="css\/legal\.css"/);
    assert.match(html, /mailto:aditya\.gupta\.cloud\.1@gmail\.com/);
    assert.doesNotMatch(html, /aditya308989@gmail\.com|Chess Platform/);
    assert.match(html, /<title>[^<]*Multiplayer Chess<\/title>/);
    assert.equal((html.match(/<main\b/g) || []).length, 1);
    for (const match of html.matchAll(/(?:href|src)="([^"]+)"/g)) {
        const target = match[1];
        if (/^(https?:|mailto:|#)/.test(target)) continue;
        await access(path.resolve(root, target.split(/[?#]/)[0]));
    }
    if (name !== 'index.html') {
        assert.doesNotMatch(html, /<script\b|\son\w+=/i);
        assert.equal((html.match(/<h1\b/g) || []).length, 1);
        assert.match(html, /id="content"/);
        assert.match(html, /href="#content"/);
        assert.doesNotMatch(html, /fonts\.googleapis\.com/);
    }
}
const privacy = await read('privacy.html');
assert.match(privacy, /Password registration does not collect an email address/);
assert.doesNotMatch(privacy, /email address where supplied through account creation/);
for (const disclosure of ['Supabase', 'Vercel', 'Google Cloud Run', 'openid',
    'refresh token', 'move timings', 'deletion', 'anti-cheat']) {
    assert.ok(privacy.includes(disclosure), `Missing disclosure: ${disclosure}`);
}
const css = await read('css/legal.css');
assert.match(css, /overflow-wrap: anywhere/);
assert.match(css, /:focus-visible/);
assert.match(css, /flex-wrap: wrap/);
assert.match(css, /@media/);
// The existing production headers must cover standalone HTML too.
const config = JSON.parse(await read('vercel.json'));
assert.ok(config.headers.some(rule => rule.source === '/(.*)' &&
    rule.headers.some(header => header.key === 'Content-Security-Policy')));
await assert.rejects(access(path.join(root, 'nonexistent-legal-page.html')), { code: 'ENOENT' });
// Import the production router and exercise the title after real route mounting.
const { Router } = await import('../frontend/js/ui/router.js');
await import('../frontend/js/core/share.js');
globalThis.window = { addEventListener() {} };
globalThis.document = { title: '' };
const router = new Router({ firstChild: null }, {});
for (const navKey of ['play', 'tournaments', 'profile']) {
    router._mount({ meta: { navKey }, factory: () => ({ mount() {}, unmount() {} }) }, {}, '/' + navKey);
    assert.equal(document.title, `${navKey[0].toUpperCase() + navKey.slice(1)} — Multiplayer Chess`);
}
delete globalThis.window;
delete globalThis.document;
console.log('PASS public legal pages: links/assets, signed-out static content, disclosures, responsive/focus rules and CSP coverage');
