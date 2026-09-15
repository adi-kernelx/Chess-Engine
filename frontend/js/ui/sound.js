/**
 * sound.js — Web Audio synthesis for chess events.
 *
 * All sounds are generated procedurally — no binary assets to ship.
 * Respects store.prefs.sound (toggleable in Settings, persisted).
 * A single AudioContext is created lazily on the first user-gesture-safe
 * playback so autoplay policy doesn't block us.
 */

export class SoundEngine {

    constructor(store) {
        this.store = store;
        this.ctx = null;
        this.master = null;
        this._resumeArmed = false;
    }

    _ensureContext() {
        if (this.ctx) return true;
        const Ctor = window.AudioContext || window.webkitAudioContext;
        if (!Ctor) return false;
        this.ctx = new Ctor();
        this.master = this.ctx.createGain();
        this.master.gain.value = 0.4;
        this.master.connect(this.ctx.destination);
        // Chrome/Safari block the context until a user gesture.
        this._armResumeOnFirstGesture();
        return true;
    }

    _armResumeOnFirstGesture() {
        if (this._resumeArmed || !this.ctx) return;
        this._resumeArmed = true;
        const resume = () => {
            if (this.ctx && this.ctx.state === 'suspended') this.ctx.resume();
        };
        ['pointerdown', 'keydown'].forEach(ev => {
            window.addEventListener(ev, resume, { once: false, passive: true });
        });
    }

    _enabled() {
        return this.store && this.store.prefs && this.store.prefs.sound !== false;
    }

    /** Basic tone: oscillator with an exponential envelope. */
    _tone({ freq, type = 'sine', duration = 0.08, gain = 1, detune = 0, sweep = 0 }) {
        if (!this._ensureContext() || !this._enabled()) return;
        const t0 = this.ctx.currentTime;
        const osc = this.ctx.createOscillator();
        const env = this.ctx.createGain();
        osc.type = type;
        osc.frequency.value = freq;
        if (detune) osc.detune.value = detune;
        if (sweep) osc.frequency.exponentialRampToValueAtTime(Math.max(30, freq + sweep), t0 + duration);
        env.gain.setValueAtTime(0.0001, t0);
        env.gain.exponentialRampToValueAtTime(gain, t0 + 0.006);
        env.gain.exponentialRampToValueAtTime(0.0001, t0 + duration);
        osc.connect(env).connect(this.master);
        osc.start(t0);
        osc.stop(t0 + duration + 0.02);
    }

    /** Brief noise burst (for captures). */
    _noise({ duration = 0.12, gain = 0.6, filterFreq = 1200 }) {
        if (!this._ensureContext() || !this._enabled()) return;
        const t0 = this.ctx.currentTime;
        const len = Math.floor(this.ctx.sampleRate * duration);
        const buf = this.ctx.createBuffer(1, len, this.ctx.sampleRate);
        const data = buf.getChannelData(0);
        for (let i = 0; i < len; i++) data[i] = (Math.random() * 2 - 1);
        const src = this.ctx.createBufferSource();
        const filt = this.ctx.createBiquadFilter();
        const env = this.ctx.createGain();
        src.buffer = buf;
        filt.type = 'lowpass';
        filt.frequency.value = filterFreq;
        env.gain.setValueAtTime(0.0001, t0);
        env.gain.exponentialRampToValueAtTime(gain, t0 + 0.005);
        env.gain.exponentialRampToValueAtTime(0.0001, t0 + duration);
        src.connect(filt).connect(env).connect(this.master);
        src.start(t0);
        src.stop(t0 + duration + 0.02);
    }

    /* ── Public event sounds ── */

    move()    { this._tone({ freq: 320, type: 'triangle', duration: 0.06, gain: 0.7, sweep: -80 }); }
    capture() {
        this._tone({ freq: 180, type: 'square', duration: 0.09, gain: 0.5, sweep: -60 });
        this._noise({ duration: 0.11, gain: 0.28, filterFreq: 900 });
    }
    check()   {
        this._tone({ freq: 660, type: 'sine', duration: 0.11, gain: 0.5 });
        setTimeout(() => this._tone({ freq: 880, type: 'sine', duration: 0.14, gain: 0.5 }), 110);
    }
    castle()  {
        this._tone({ freq: 330, type: 'triangle', duration: 0.09, gain: 0.5 });
        setTimeout(() => this._tone({ freq: 440, type: 'triangle', duration: 0.11, gain: 0.5 }), 60);
    }
    promote() {
        this._tone({ freq: 520, type: 'sine', duration: 0.09, gain: 0.6, sweep: 180 });
    }
    illegal() { this._tone({ freq: 140, type: 'sawtooth', duration: 0.12, gain: 0.5 }); }
    gameStart() {
        this._tone({ freq: 440, type: 'triangle', duration: 0.09, gain: 0.6 });
        setTimeout(() => this._tone({ freq: 660, type: 'triangle', duration: 0.14, gain: 0.6 }), 100);
    }
    gameEnd()   {
        this._tone({ freq: 660, type: 'sine', duration: 0.14, gain: 0.6 });
        setTimeout(() => this._tone({ freq: 440, type: 'sine', duration: 0.24, gain: 0.6 }), 130);
    }
    lowTime()   { this._tone({ freq: 800, type: 'square', duration: 0.03, gain: 0.4 }); }

    /**
     * Pick the right event for a move given its SAN and whether it captured.
     * SAN convention: 'x' = capture, '+' = check, '#' = mate, 'O-O' / 'O-O-O' = castle,
     * '=' = promotion.
     */
    forMove(san) {
        if (!san) return this.move();
        if (san.startsWith('O-O')) return this.castle();
        if (san.includes('#'))     return this.gameEnd();
        if (san.includes('+'))     return this.check();
        if (san.includes('='))     return this.promote();
        if (san.includes('x'))     return this.capture();
        return this.move();
    }
}
