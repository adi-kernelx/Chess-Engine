import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

class TestNode {
    constructor(tag = '', text = '') {
        this.tag = tag; this.text = text; this.children = []; this.attributes = {};
        this.handlers = {}; this.style = {}; this.dataset = {}; this.value = ''; this.disabled = false;
        this.classList = { toggle: (name, enabled) => {
            const names = new Set((this.attributes.class || '').split(' '));
            if (enabled) names.add(name); else names.delete(name);
            this.attributes.class = [...names].join(' ');
        } };
    }
    setAttribute(name, value) { this.attributes[name] = String(value); }
    removeAttribute(name) { delete this.attributes[name]; }
    addEventListener(name, fn) { this.handlers[name] = fn; }
    appendChild(node) { this.children.push(node); node.parentNode = this; }
    removeChild(node) { this.children.splice(this.children.indexOf(node), 1); }
    get firstChild() { return this.children[0]; }
    get textContent() { return this.text + this.children.map(node => node.textContent).join(''); }
    set textContent(value) { this.text = value; this.children = []; }
    querySelector(selector) {
        return flatten(this).find(node => selector.startsWith('.') &&
            (node.attributes.class || '').split(' ').includes(selector.slice(1))) || null;
    }
    focus() { this.focused = true; }
}
function flatten(node) { return [node, ...node.children.flatMap(flatten)]; }
globalThis.Node = TestNode;
globalThis.document = {
    createElement: tag => new TestNode(tag), createElementNS: (_, tag) => new TestNode(tag),
    createTextNode: text => new TestNode('', text),
};
let redirected = null;
globalThis.location = { hostname: 'localhost', origin: 'http://localhost:8000', assign: url => redirected = url };
const { AuthScreen } = await import('../frontend/js/screens/auth.js');
const { CONFIG } = await import('../frontend/js/config.js');
const notices = [];
let resolveRequest;
const screen = new AuthScreen({ store: { session: { username: '' } },
    toast: Object.fromEntries(['info', 'warning', 'error', 'success'].map(name => [name, message => notices.push(message)])),
    Outbound: { login: () => ({ type: 'login' }) },
    authClient: { request: () => new Promise(resolve => resolveRequest = resolve) },
});
screen.root = screen.render();
assert.match(screen.root.textContent, /Welcome back/);
assert.doesNotMatch(screen.root.textContent, /constant-time|after cloud deployment/);
const image = flatten(screen.root).find(node => node.tag === 'img');
assert.equal(image.attributes.src, 'assets/google-signin-icon.svg');
assert.equal(image.attributes.alt, '');
assert.equal(image.attributes['aria-hidden'], 'true');
assert.match(screen._googleBtn.textContent, /Continue with Google/);
assert.equal(screen._passInput.attributes.autocomplete, 'current-password');
screen._continueWithGoogle(null);
assert.equal(redirected, null);
assert.match(notices.at(-1), /not configured/);
screen._userInput.value = 'fixture'; screen._passInput.value = 'fixture-password';
const pending = screen._submit();
assert.equal(screen._googleBtn.disabled, true);
assert.equal(screen._submitBtn.attributes['aria-busy'], 'true');
screen._continueWithGoogle('https://should-not-navigate.invalid');
assert.equal(redirected, null);
resolveRequest({ ok: false, code: 'invalid_credentials' });
await pending;
assert.equal(screen._googleBtn.disabled, false);
assert.equal(screen._submitBtn.disabled, false);
assert.equal(screen._submitBtn.attributes['aria-busy'], undefined);
assert.match(notices.at(-1), /Wrong username or password/);
screen._continueWithGoogle('https://example.invalid/authorize');
assert.equal(redirected, 'https://example.invalid/authorize');
screen._switch('register');
assert.match(screen._form.textContent, /Make your first move/);
assert.equal(screen._passInput, null);
assert.equal(flatten(screen._form).some(node => node.attributes.type === 'password'), false);
assert.equal(screen._userInput.focused, true);
const configured = CONFIG.supabaseUrl; CONFIG.supabaseUrl = '';
screen._switch('login');
assert.match(screen._form.textContent, /not configured yet/);
CONFIG.supabaseUrl = configured;
const css = readFileSync(new URL('../frontend/css/screens.css', import.meta.url), 'utf8');
assert.match(css, /\.auth-form \.input, \.auth-form \.btn \{ min-height: 44px;/);
assert.match(css, /\.auth-divider::before, \.auth-divider::after/);
assert.match(css, /\.auth-form \.auth-google:hover:not\(:disabled\)/);
const asset = readFileSync(new URL('../frontend/assets/google-signin-icon.svg', import.meta.url), 'utf8');
assert.match(asset, /data:image\/png;base64,/);
assert.doesNotMatch(asset, /<script|<foreignObject/);
console.log('PASS auth screen: Google logo, mode switching, accessible inputs, disabled configuration, busy/error recovery, redirect');
