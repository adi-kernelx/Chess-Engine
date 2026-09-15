/**
 * storage.js — Namespaced, JSON-safe localStorage wrapper.
 * Silently tolerates unavailable storage (private windows, quota exceeded).
 */

const NS = 'chess:';

function safe(fn, fallback) {
    try { return fn(); }
    catch (err) {
        console.warn('[storage]', err.message);
        return fallback;
    }
}

export const storage = {
    get(key, fallback = null) {
        return safe(() => {
            const raw = localStorage.getItem(NS + key);
            if (raw === null) return fallback;
            return JSON.parse(raw);
        }, fallback);
    },

    set(key, value) {
        safe(() => localStorage.setItem(NS + key, JSON.stringify(value)));
    },

    remove(key) {
        safe(() => localStorage.removeItem(NS + key));
    },

    /** Clear only keys in our namespace. */
    clear() {
        safe(() => {
            const toRemove = [];
            for (let i = 0; i < localStorage.length; i++) {
                const k = localStorage.key(i);
                if (k && k.startsWith(NS)) toRemove.push(k);
            }
            toRemove.forEach(k => localStorage.removeItem(k));
        });
    },
};
