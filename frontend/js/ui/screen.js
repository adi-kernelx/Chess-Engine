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
 * Production screens must render one of three truthful states: live data, an
 * empty state, or an explicit unavailable/error state. The old automatic
 * preview badge was removed after the backend contracts were implemented.
 */

import { h } from '../core/dom.js';

export class Screen {
    constructor(ctx) {
        this.ctx = ctx;
        this.root = null;
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
        // LLD-7: subscription-cleanup guarantee. `onUnmount()` runs
        // subclass-specific teardown that may throw (a mis-shaped
        // ResizeObserver, an already-detached listener). Even if it
        // does, every registered subscription / RAF / interval /
        // timeout below MUST still be released — otherwise a screen
        // that fails to unmount cleanly leaks handlers into the next
        // one, which caused Phase 9 "ghost move_made after
        // navigation" bugs in the pre-refactor code.
        try { this.onUnmount(); }
        catch (err) { console.error('[screen] onUnmount threw:', err); }
        for (const off of this._subs)     { try { off(); } catch (_) {} }
        for (const id of this._rafs)      { try { cancelAnimationFrame(id); } catch (_) {} }
        for (const id of this._intervals) { try { clearInterval(id); }       catch (_) {} }
        for (const id of this._timeouts)  { try { clearTimeout(id); }        catch (_) {} }
        this._subs = [];
        this._rafs.clear();
        this._intervals.clear();
        this._timeouts.clear();
        try {
            if (this.root && this.root.parentNode) this.root.parentNode.removeChild(this.root);
        } catch (err) {
            console.error('[screen] root removeChild threw:', err);
        }
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
        return h('div', { class: 'screen__header' },
            h('div', { class: 'screen__title-block' },
                h('div', {},
                    h('h1', { class: 'screen__title' }, title),
                    subtitle ? h('div', { class: 'screen__subtitle' }, subtitle) : null
                ),
            ),
            right || null
        );
    }
}
