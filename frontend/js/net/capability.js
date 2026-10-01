/**
 * capability.js — Live-vs-preview probe with graceful demo fallback.
 *
 * Reusable primitive so live screens do not copy request/reply timeout logic.
 * Each caller receives `{ data, live }` and is responsible for rendering an
 * explicit unavailable state when `live === false`.
 *
 * Semantics:
 *   1. If the outbound type is already known-unsupported this session,
 *      resolve immediately with demo data. (No wasted round-trip.)
 *   2. Otherwise: send the message, race:
 *      - a reply of `expect` type  →  { data: normalized reply, live: true }
 *                                     (cache as supported)
 *      - a normalized `error` with `isUnknownType: true`
 *                                  →  { data: demo(), live: false }
 *                                     (cache as unsupported)
 *      - timeout                    →  { data: demo(), live: false }
 *                                     (do NOT cache — slow server may reply later)
 *
 * The backend has no request-id correlation, so if two probes race for
 * the same reply type they might mis-attribute. This is acceptable for
 * preview screens — nothing safety-critical rides on it.
 */

import { Inbound } from './protocol.js';

const DEFAULT_TIMEOUT = 1500;

export class Capability {
    /**
     * @param {ChessSocket} socket
     */
    constructor(socket) {
        this.socket = socket;
        this._cache = new Map(); // outboundType -> 'live' | 'preview'
    }

    /**
     * @param {Object} message  Outbound object (built via Outbound.*).
     * @param {Object} opts
     * @param {string} opts.expect  Reply message type to listen for on success.
     * @param {Function} opts.demo  () => demo data (used on preview/timeout).
     * @param {number} [opts.timeout=1500]
     * @param {boolean} [opts.failOnError=false] Resolve immediately when the
     * server returns a regular error. Use only for an isolated request whose
     * error cannot be confused with another concurrent operation.
     * @returns {Promise<{ data: any, live: boolean, error?: any }>}
     */
    request(message, { expect, demo, timeout = DEFAULT_TIMEOUT, failOnError = false }) {
        if (!message || !message.type) {
            return Promise.resolve({ data: demo(), live: false });
        }
        const outType = message.type;

        // Fast path: known unsupported.
        if (this._cache.get(outType) === 'preview') {
            return Promise.resolve({ data: demo(), live: false });
        }

        // If we're not connected, skip the round-trip entirely.
        if (!this.socket.isConnected()) {
            return Promise.resolve({ data: demo(), live: false });
        }

        return new Promise((resolve) => {
            let settled = false;
            const cleanup = [];
            const finish = (result, cacheAs) => {
                if (settled) return;
                settled = true;
                for (const off of cleanup) { try { off(); } catch (_) {} }
                if (cacheAs) this._cache.set(outType, cacheAs);
                resolve(result);
            };

            cleanup.push(this.socket.on(expect, (raw) => {
                finish({ data: Inbound.normalize(raw), live: true }, 'live');
            }));
            cleanup.push(this.socket.on('error', (raw) => {
                const norm = Inbound.normalize(raw);
                if (norm.isUnknownType) {
                    finish({ data: demo(), live: false }, 'preview');
                } else if (failOnError) {
                    finish({ data: demo(), live: false, error: norm }, null);
                }
                // Other errors: ignore — the request may still legitimately produce `expect`.
            }));

            const timer = setTimeout(() => {
                finish({ data: demo(), live: false }, null /* don't cache timeouts */);
            }, timeout);
            cleanup.push(() => clearTimeout(timer));

            this.socket.send(message);
        });
    }

    /** Preview-force override — useful during Phase 5/6 development to preview UIs
     *  without waiting for the timeout on a live server that doesn't yet handle them. */
    markPreview(outboundType) { this._cache.set(outboundType, 'preview'); }
    markLive(outboundType)    { this._cache.set(outboundType, 'live'); }
    forget(outboundType)      { this._cache.delete(outboundType); }
    isKnownLive(outboundType) { return this._cache.get(outboundType) === 'live'; }
    isKnownPreview(outboundType) { return this._cache.get(outboundType) === 'preview'; }
}
