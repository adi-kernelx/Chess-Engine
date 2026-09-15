/**
 * screen.js — Base class for every screen.
 *
 * Contract:
 *   render()     — return a root DOM node.
 *   onMount()    — after DOM insertion; do subscriptions here via this.sub(off).
 *   onUnmount()  — before DOM removal; anything not registered via sub()
 *                  must be torn down here.
 *
 * Subscriptions and setInterval/animationFrame handles registered with
 * `this.sub(off)` and `this.raf(id)` are auto-cleaned on unmount.
 *
 * Screens set `this.preview = true` in their constructor to display the
 * "Preview — backend pending" badge in their header block automatically.
 */

import { h } from '../core/dom.js';

export class Screen {
    constructor(ctx) {
        this.ctx = ctx;
        this.root = null;
        this.preview = false;
        this._subs = [];
        this._rafs = new Set();
        this._intervals = new Set();
        this._timeouts = new Set();
    }

    /* ── Lifecycle called by Router ── */

    mount(container) {
        this.root = this.render();
        container.appendChild(this.root);
        this.onMount();
    }

    unmount() {
        this.onUnmount();
        for (const off of this._subs)     { try { off(); } catch (_) {} }
        for (const id of this._rafs)      cancelAnimationFrame(id);
        for (const id of this._intervals) clearInterval(id);
        for (const id of this._timeouts)  clearTimeout(id);
        this._subs = [];
        this._rafs.clear();
        this._intervals.clear();
        this._timeouts.clear();
        if (this.root && this.root.parentNode) this.root.parentNode.removeChild(this.root);
        this.root = null;
    }

    /* ── Auto-cleanup helpers ── */
    sub(offFn)   { if (offFn) this._subs.push(offFn); return offFn; }
    raf(fn)      { const id = requestAnimationFrame(fn); this._rafs.add(id); return id; }
    interval(fn, ms) { const id = setInterval(fn, ms); this._intervals.add(id); return id; }
    timeout(fn, ms)  { const id = setTimeout(fn, ms);  this._timeouts.add(id);  return id; }

    /* ── Overridable ── */
    render()   { return h('div'); }
    onMount()  { /* subclasses */ }
    onUnmount(){ /* subclasses */ }

    /* ── Header helper — every screen calls this to keep the layout consistent ── */
    header(title, subtitle, right) {
        const badges = [];
        if (this.preview) {
            badges.push(h('span', { class: 'badge badge--preview' }, 'Preview — backend pending'));
        }
        return h('div', { class: 'screen__header' },
            h('div', { class: 'screen__title-block' },
                h('div', {},
                    h('h1', { class: 'screen__title' }, title),
                    subtitle ? h('div', { class: 'screen__subtitle' }, subtitle) : null
                ),
                ...badges
            ),
            right || null
        );
    }
}
