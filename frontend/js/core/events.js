/**
 * events.js — Tiny pub/sub bus.
 * Decouples services (socket, store) from screens.
 * Handlers are stored as arrays so multiple subscribers coexist.
 */

export class EventBus {
    constructor() {
        this._handlers = new Map(); // type -> Set<fn>
    }

    /** Subscribe. Returns an unsubscribe function. */
    on(type, fn) {
        let set = this._handlers.get(type);
        if (!set) {
            set = new Set();
            this._handlers.set(type, set);
        }
        set.add(fn);
        return () => this.off(type, fn);
    }

    /** Subscribe once. */
    once(type, fn) {
        const off = this.on(type, (...args) => {
            off();
            fn(...args);
        });
        return off;
    }

    off(type, fn) {
        const set = this._handlers.get(type);
        if (!set) return;
        set.delete(fn);
        if (set.size === 0) this._handlers.delete(type);
    }

    emit(type, payload) {
        const set = this._handlers.get(type);
        if (!set) return;
        // Snapshot so handlers that unsubscribe mid-emit don't skip peers.
        for (const fn of Array.from(set)) {
            try { fn(payload); }
            catch (err) { console.error(`[EventBus] handler for "${type}" threw:`, err); }
        }
    }

    clear() { this._handlers.clear(); }
}
