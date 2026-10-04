import assert from 'node:assert/strict';
import { Capability } from '../frontend/js/net/capability.js';

class Socket {
    listeners = new Map(); sent = [];
    isConnected() { return true; }
    on(type, fn) {
        if (!this.listeners.has(type)) this.listeners.set(type, new Set());
        this.listeners.get(type).add(fn);
        return () => this.listeners.get(type).delete(fn);
    }
    send(message) { this.sent.push(message); }
    emit(type, message) { for (const fn of [...(this.listeners.get(type) || [])]) fn(message); }
}
const socket = new Socket();
const capability = new Capability(socket);
const options = { expect: 'tournament_registration_updated', demo: () => ({}), correlated: true, failOnError: true };
let completed = 0;
const first = capability.request({ type: 'set_tournament_registration' }, options).then(r => { completed++; return r; });
const second = capability.request({ type: 'set_tournament_registration' }, options);
socket.emit('error', { message: 'unrelated tournament poll failed' });
socket.emit(options.expect, { request_id: socket.sent[1].request_id, open: false });
assert.equal((await second).live, true);
assert.equal(completed, 0);
socket.emit('error', { request_id: socket.sent[0].request_id, message: 'pairings_locked' });
assert.equal((await first).error.message, 'pairings_locked');
assert.notEqual(socket.sent[0].request_id, socket.sent[1].request_id);
assert.equal([...socket.listeners.values()].reduce((n, rows) => n + rows.size, 0), 0);
console.log('PASS: concurrent typed requests isolate replies, unrelated errors and rejections');
