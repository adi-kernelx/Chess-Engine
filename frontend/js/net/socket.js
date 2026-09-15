/**
 * socket.js — Reconnecting WebSocket client.
 *
 * Salvaged from the old websocket.js: exponential backoff and offline queue.
 * Rewritten with fixes:
 *   - Handlers are a Set per type, so multiple screens can observe the same
 *     message without silently clobbering each other (a real bug before).
 *   - Emits its own state events ('open', 'close', 'reconnecting', 'error')
 *     so the connection indicator and store update reactively.
 *   - Every send goes out as JSON via protocol.js; nothing else should call
 *     `raw send`. The `sendRaw` method exists only for capability.js.
 *   - Intentional disconnects (`close()`) do NOT trigger reconnect.
 *
 * IMPORTANT: All outbound objects MUST have `type` as their first key —
 * the server's router does a string.find on "\"type\"" rather than a JSON
 * parse (src/net/websocket.cpp:329-365). Factory objects in protocol.js
 * are constructed to satisfy this.
 */

import { EventBus } from '../core/events.js';

const DEFAULT_URL = 'ws://localhost:9000';
const BASE_DELAY  = 1000;
const MAX_DELAY   = 30_000;
const MAX_ATTEMPTS = Infinity; // Keep trying — the user can be away

export class ChessSocket {
    constructor(url = DEFAULT_URL) {
        this.url = url;
        this.ws = null;
        this.bus = new EventBus();       // 'message:<type>', 'state', 'error'
        this.state = 'offline';          // 'offline' | 'connecting' | 'connected'
        this._queue = [];
        this._handlers = new Map();      // type -> Set<fn>
        this._closedByUser = false;
        this._reconnectAttempts = 0;
        this._reconnectTimer = null;
    }

    /* ── Public API ── */

    connect() {
        if (this.state === 'connected' || this.state === 'connecting') return;
        this._closedByUser = false;
        this._open();
    }

    close() {
        this._closedByUser = true;
        this._clearReconnect();
        if (this.ws) {
            try { this.ws.close(1000, 'client-close'); } catch (_) {}
        }
    }

    /** Send an object as a JSON text frame. Queued if not currently open. */
    send(obj) {
        if (obj == null || typeof obj !== 'object') {
            console.warn('[socket] send() ignored non-object', obj);
            return;
        }
        const text = JSON.stringify(obj);
        if (this.state === 'connected' && this.ws && this.ws.readyState === WebSocket.OPEN) {
            try { this.ws.send(text); }
            catch (err) { console.error('[socket] send failed:', err); this._queue.push(text); }
        } else {
            this._queue.push(text);
        }
    }

    /** Escape hatch — used by capability.js which needs the exact serialized form. */
    sendRaw(text) {
        if (this.state === 'connected' && this.ws && this.ws.readyState === WebSocket.OPEN) {
            try { this.ws.send(text); return true; }
            catch (err) { console.error('[socket] sendRaw failed:', err); this._queue.push(text); return false; }
        }
        this._queue.push(text);
        return false;
    }

    /**
     * Subscribe to a message type. Returns an unsubscribe function.
     * Multiple subscribers to the same type all receive the message.
     */
    on(type, fn) {
        let set = this._handlers.get(type);
        if (!set) { set = new Set(); this._handlers.set(type, set); }
        set.add(fn);
        return () => {
            set.delete(fn);
            if (set.size === 0) this._handlers.delete(type);
        };
    }

    /** One-shot listener. */
    once(type, fn) {
        const off = this.on(type, (msg) => { off(); fn(msg); });
        return off;
    }

    /** Subscribe to socket lifecycle: 'state' (with new state), 'error'. */
    onState(fn) { return this.bus.on('state', fn); }
    onError(fn) { return this.bus.on('error', fn); }

    isConnected() { return this.state === 'connected'; }

    /* ── Internals ── */

    _open() {
        this._setState('connecting');
        let ws;
        try {
            ws = new WebSocket(this.url);
        } catch (err) {
            console.error('[socket] constructor threw:', err);
            this._setState('offline');
            this._scheduleReconnect();
            return;
        }
        this.ws = ws;

        ws.onopen = () => {
            this._reconnectAttempts = 0;
            this._setState('connected');
            // Flush queued messages in order.
            while (this._queue.length > 0) {
                const text = this._queue.shift();
                try { ws.send(text); }
                catch (err) {
                    console.error('[socket] flush failed, requeue:', err);
                    this._queue.unshift(text);
                    break;
                }
            }
        };

        ws.onmessage = (ev) => this._handleMessage(ev.data);

        ws.onerror = (ev) => {
            // Note: the browser deliberately hides error details for security.
            this.bus.emit('error', ev);
        };

        ws.onclose = () => {
            this.ws = null;
            this._setState('offline');
            if (!this._closedByUser) this._scheduleReconnect();
        };
    }

    _handleMessage(raw) {
        let msg;
        try { msg = JSON.parse(raw); }
        catch (err) {
            console.error('[socket] non-JSON frame:', raw);
            return;
        }
        if (!msg || typeof msg !== 'object' || !msg.type) {
            console.warn('[socket] malformed frame:', msg);
            return;
        }

        const set = this._handlers.get(msg.type);
        if (set) {
            for (const fn of Array.from(set)) {
                try { fn(msg); }
                catch (err) { console.error(`[socket] "${msg.type}" handler:`, err); }
            }
        }
        // Fan out on the bus too — capability.js and diagnostics can listen here.
        this.bus.emit('message:' + msg.type, msg);
        this.bus.emit('message', msg);
    }

    _setState(next) {
        if (this.state === next) return;
        this.state = next;
        this.bus.emit('state', next);
    }

    _scheduleReconnect() {
        this._clearReconnect();
        if (this._reconnectAttempts >= MAX_ATTEMPTS) return;
        const attempt = this._reconnectAttempts++;
        const delay = Math.min(BASE_DELAY * Math.pow(2, attempt), MAX_DELAY);
        this._reconnectTimer = setTimeout(() => this._open(), delay);
    }

    _clearReconnect() {
        if (this._reconnectTimer) {
            clearTimeout(this._reconnectTimer);
            this._reconnectTimer = null;
        }
    }
}
