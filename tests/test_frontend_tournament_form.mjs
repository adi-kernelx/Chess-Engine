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
    removeAttribute(key) { delete this.attributes[key]; }
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

// Dispatch real correction handlers: an old mandatory-reason error must clear
// when valid input is supplied, before the async save finishes.
screen._openOverride({ id: 15 }, { id: 1, result: '1-0', whiteUsername: 'A', blackUsername: 'B' });
const flatten = node => [node, ...node.children.flatMap(flatten)];
const reasonNode = flatten(dialog.body).find(node => node.tag === 'textarea');
const reasonError = flatten(dialog.body).find(node => node.attributes.class === 'field__error');
const saveCorrection = flatten(dialog.footer).find(node => node.tag === 'button' && node.textContent === 'Save correction');
await saveCorrection.handlers.get('click')();
assert.equal(reasonError.hidden, false);
assert.equal(reasonNode.attributes['aria-invalid'], 'true');
reasonNode.handlers.get('input')({ target: { value: '   ' } });
assert.equal(reasonError.hidden, false);
reasonNode.handlers.get('input')({ target: { value: 'RC correction test' } });
assert.equal(reasonError.hidden, true);
assert.equal(reasonNode.attributes['aria-invalid'], undefined);
console.log('PASS: correction reason validation clears stale inline error and invalid state on valid input');

// Render the actual pairing table/footer with stale server state. A locally
// elapsed cutoff must hide registration, and a room id alone cannot offer
// early board entry. This is structural/non-visual verification only.
const savedNow = Date.now;
let nowSeconds = 1000;
Date.now = () => nowSeconds * 1000;
try {
    const detail = new TournamentScreen({ session: { username: 'Alice', isAuthenticated: true } });
    detail._detailBody = new TestNode('div'); detail._detailFooter = new TestNode('div');
    detail._detailTab = 'pairings';
    detail._detailState = {
        tournament: { id: 42, name: 'Start gate', format: 'swiss', status: 'registration', registrationOpen: true,
            createdByUsername: 'Alice', registrationDeadline: 1170, firstRoundStartsAt: 1200 },
        standings: [{ username: 'Alice', playerId: 1 }], checkIns: [],
        rounds: [{ round: 1, status: 'live', earliestStartAt: 1200, checkInClosesAt: 1260 }],
        pairings: [{ id: 9, round: 1, whiteUsername: 'Alice', blackUsername: 'Bob', whitePlayerId: 1,
            blackPlayerId: 2, result: 'pending', gameId: 99 }],
    };
    detail._renderDetail();
    assert.match(detail._detailFooter.textContent, /Unregister/);
    assert.match(detail._detailBody.textContent, /Waiting for scheduled start/);
    assert.doesNotMatch(detail._detailBody.textContent, /Open game/);
    nowSeconds = 1110;
    detail._updateCountdown();
    assert.doesNotMatch(detail._detailFooter.textContent, /Unregister|Close registration|Reopen registration/);
    nowSeconds = 1200;
    detail._renderDetail();
    assert.match(detail._detailBody.textContent, /Open game/);
} finally { Date.now = savedNow; }
console.log('PASS: real pairing-table start gate and cutoff-driven registration controls');
