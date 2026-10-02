import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

// Non-UI regression: mount the real create-form builder in a minimal DOM,
// then dispatch its actual change handler. Browser visual QA remains manual.
const elements = new Map();
class TestNode {
    constructor(tag = '', text = '') {
        this.tag = tag; this.text = text; this.children = []; this.handlers = new Map();
        this.attributes = {}; this.style = {}; this.dataset = {};
        this.value = ''; this.hidden = false; this.disabled = false;
    }
    setAttribute(key, value) { this.attributes[key] = value; if (key === 'id') elements.set(value, this); }
    addEventListener(name, fn) { this.handlers.set(name, fn); }
    appendChild(child) {
        child.parentNode = this; this.children.push(child);
        if (this.tag === 'select' && this.children.length === 1) this.value = child.value;
    }
    removeChild(child) { this.children.splice(this.children.indexOf(child), 1); child.parentNode = null; }
    get firstChild() { return this.children[0]; }
    get textContent() { return this.text + this.children.map(child => child.textContent).join(''); }
    set textContent(value) { this.text = value; this.children = []; }
    focus() {}
}
globalThis.Node = TestNode;
globalThis.document = {
    createElement: tag => new TestNode(tag),
    createTextNode: text => new TestNode('', text),
};
globalThis.location = { origin: 'http://localhost:8000', pathname: '/' };
const { TournamentScreen } = await import('../frontend/js/screens/tournament.js');
let dialog;
const screen = new TournamentScreen({ modal: { open: options => { dialog = options; return { close() {} }; } } });
screen._openCreate();
const format = elements.get('t-format');
const rounds = elements.get('t-rounds');
assert.ok(dialog.body);
assert.equal(format.value, 'swiss');
assert.equal(rounds.parentNode.hidden, false);
assert.equal(rounds.disabled, false);
format.value = 'winners_advance';
format.handlers.get('change')();
assert.equal(rounds.parentNode.hidden, true);
assert.equal(rounds.disabled, true);
format.value = 'swiss';
format.handlers.get('change')();
assert.equal(rounds.parentNode.hidden, false);
assert.equal(rounds.disabled, false);
assert.equal(rounds.value, '5');

// This source-level CSS check guards the actual cascade regression, which a
// fake DOM alone cannot detect. It is not a computed-style/browser assertion.
const css = readFileSync(new URL('../frontend/css/base.css', import.meta.url), 'utf8');
assert.match(css, /\[hidden\]\s*\{\s*display:\s*none\s*!important\s*;/);
console.log('PASS: create-form Swiss → winners-advance → Swiss visibility/disabled state and semantic hidden CSS contract');
