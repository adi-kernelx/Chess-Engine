import { createSealer } from './sealed.js';

const AUTH_TYPES = new Set(['login', 'register', 'google_auth']);
const loadProvider = async () => (await import('./pqc-provider.js')).pqcProvider;

/** One authentication exchange at a time. Never queue credentials/replay an
 * envelope across reconnects, and never downgrade after a sealing failure. */
export class AuthClient {
    constructor(socket, { policy = 'required', pinnedKeys = [],
        provider = loadProvider, timeoutMs = 15000 } = {}) {
        this.socket = socket;
        this.policy = policy;
        this.pinnedKeys = [...pinnedKeys];
        this.provider = provider;
        this.timeoutMs = timeoutMs;
        this.busy = false;
    }

    request(message) {
        if (!AUTH_TYPES.has(message?.type)) return Promise.resolve({ ok: false, code: 'internal' });
        if (this.busy) return Promise.resolve({ ok: false, code: 'auth_busy' });
        if (!this.socket.isConnected()) return Promise.resolve({ ok: false, code: 'unavailable' });
        const local = (() => {
            try {
                const url = new URL(this.socket.url);
                return /^(localhost|127\.0\.0\.1|\[::1\])$/.test(url.hostname);
            } catch { return false; }
        })();
        const unsealed = this.policy === 'local-unsealed' && local;
        if (!unsealed && (this.policy !== 'required' || !this.pinnedKeys.length)) {
            return Promise.resolve({ ok: false, code: 'seal_configuration' });
        }
        this.busy = true;
        return new Promise(resolve => {
            let settled = false;
            let phase = 'offer';
            const cleanup = [];
            const finish = result => {
                if (settled) return;
                settled = true;
                this.busy = false;
                for (const off of cleanup) off();
                resolve(result);
            };
            const send = frame => {
                if (settled) return;
                if (!this.socket.sendImmediate(frame)) finish({ ok: false, code: 'unavailable' });
            };
            cleanup.push(this.socket.on('auth_ok', data => {
                if (phase === 'sent') finish({ ok: true, data });
            }));
            cleanup.push(this.socket.on('auth_error', error =>
                finish({ ok: false, code: error.code || 'internal' })));
            cleanup.push(this.socket.onState(state => {
                if (state !== 'connected') finish({ ok: false, code: 'unavailable' });
            }));
            const timer = setTimeout(() => finish({ ok: false, code: 'unavailable' }), this.timeoutMs);
            cleanup.push(() => clearTimeout(timer));
            if (unsealed) {
                phase = 'sent';
                send(message);
                return;
            }
            cleanup.push(this.socket.on('seal_key', async offer => {
                if (settled || phase !== 'offer') return;
                phase = 'sealing'; // Ignore duplicate offers while crypto is running.
                try {
                    const pqc = await this.provider();
                    if (settled) return;
                    const { type, ...payload } = message;
                    const sealed = await createSealer(pqc, this.pinnedKeys).seal(offer, { ...payload, type });
                    if (settled) return;
                    phase = 'sent';
                    send({ type, sealed });
                } catch {
                    finish({ ok: false, code: 'seal_failed' });
                }
            }));
            send({ type: 'seal_request' });
        });
    }
}
