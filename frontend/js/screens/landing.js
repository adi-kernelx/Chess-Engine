/**
 * landing.js — The front door (Part G1).
 *
 * Goal: a stranger goes from cold URL to moving a piece in under 10s and one click.
 *
 * Structure:
 *   - Hero:   one line + Play now + Tier-2 CSS-3D piece + presence
 *   - Modes:  three secondary options (quick match, play a bot, create game)
 *              rendered as Tier-1 tilt cards on desktop, flat on touch
 *   - Recruit: "How it works" strip — three steps, no jargon
 *
 * Play now:
 *   1. Ensure a guest handle exists (Knight4821-style)
 *   2. Send play_ai (medium, 5+3) immediately
 *   3. Navigate to /game as soon as game_start arrives
 *
 * Presence line stays hidden until backend item B1 lands
 * (design decision: no fake social proof — see growth plan §7).
 *
 * Returning-visitor bypass: on first successful Play, we mark
 * localStorage['chess.visited'] = '1'. main.js redirects '#/' to
 * '#/play' when the flag is set, so regulars never see this screen twice.
 */

import { Screen } from '../ui/screen.js';
import { h, clear, icon, q } from '../core/dom.js';
import { storage } from '../core/storage.js';
import { reconcileStreak } from '../core/streak.js';

const GUEST_ADJECTIVES = ['Knight', 'Bishop', 'Rook', 'Pawn', 'Castle', 'Gambit', 'Zugzwang', 'Fianchetto'];

/** Small joined SVG-planes king used in the hero. Kept as a template. */
const HERO_PIECE_LAYERS = [
    { z:  0, opacity: 0.35 },
    { z: 12, opacity: 0.55 },
    { z: 24, opacity: 0.80 },
    { z: 36, opacity: 1.00 },
];

const DEFAULT_PRESET = { label: '5+3', base: 300, inc: 3 };

export class LandingScreen extends Screen {
    constructor(ctx) {
        super(ctx);
        this._starting = false;
    }

    render() {
        return h('div', { class: 'landing' },
            this._renderHero(),
            this._renderModes(),
            this._renderPuzzleStrip(),
            this._renderHow()
        );
    }

    /* ── Puzzle + streak strip (G3) ────────────────────────── */

    _renderPuzzleStrip() {
        const s = reconcileStreak();
        const streakLine = s.current > 0
            ? `You're on a ${s.current}-day streak.`
            : 'Solve today to start a streak.';
        return h('section', { class: 'landing__puzzle' },
            h('div', { class: 'landing__puzzle-body' },
                h('div', { class: 'landing__puzzle-icon' }, icon('trophy', 'icon--lg')),
                h('div', {},
                    h('div', { class: 'landing__puzzle-title' }, 'Daily puzzle'),
                    h('div', { class: 'landing__puzzle-sub' },
                        streakLine + ' One tactic a day — takes a minute.'),
                ),
            ),
            h('a', { class: 'btn btn--primary', href: '#/puzzle' }, 'Solve today'),
        );
    }

    /* ── Hero ───────────────────────────────────────────────── */

    _renderHero() {
        return h('section', { class: 'landing__hero' },
            h('div', { class: 'landing__hero-copy' },
                h('h1', { class: 'landing__title' },
                    'Play chess. ',
                    h('span', { class: 'landing__title-accent' }, 'Right now.')
                ),
                h('p', { class: 'landing__lede' },
                    'One click to a game. No signup, no forms. Bring a friend by pasting a link, ',
                    'or let us find you an opponent while you warm up on a bot.'
                ),
                h('div', { class: 'landing__cta-row' },
                    h('button', {
                        class: 'btn btn--primary btn--xl landing__play',
                        onclick: () => this._onPlayNow(),
                        ref: el => this._playBtn = el,
                    },
                        icon('play', 'icon--md'),
                        h('span', {}, 'Play now'),
                        h('span', { class: 'landing__play-sub' }, '5 + 3')
                    ),
                    h('a', {
                        class: 'btn btn--ghost btn--lg',
                        href: '#/play',
                    }, 'More options'),
                ),
                h('div', {
                    class: 'landing__presence',
                    ref: el => this._presenceEl = el,
                    'aria-live': 'polite',
                }),
            ),
            this._renderHeroPiece()
        );
    }

    _renderHeroPiece() {
        // Layered SVG planes on translateZ — a Tier-2 CSS-3D piece.
        // Zero canvas, zero images, zero library. Renders as a flat silhouette
        // when perspective/3d is disabled (prefers-reduced-motion or Effects: off).
        const layers = HERO_PIECE_LAYERS.map(({ z, opacity }) =>
            h('div', {
                class: 'landing__piece-layer',
                style: { transform: `translateZ(${z}px)`, opacity: String(opacity) },
            },
                // Full Staunton king silhouette from the sprite. Inherits
                // currentColor so palette changes flow through.
                h('svg', { class: 'landing__piece-svg', viewBox: '0 0 45 45', 'aria-hidden': 'true' },
                    h('use', { href: '#i-piece-king' })
                )
            )
        );
        return h('div', {
            class: 'landing__piece-stage',
            'aria-hidden': 'true',
            ref: el => this._pieceStage = el,
        },
            h('div', { class: 'landing__piece', ref: el => this._piece = el }, ...layers)
        );
    }

    /* ── Modes ──────────────────────────────────────────────── */

    _renderModes() {
        const cards = [
            {
                icon: 'play',    key: 'quick',
                title: 'Quick match',
                sub: 'Get paired by rating — usually fast.',
                cta: 'Find a human',
                onclick: () => this._goPlay('quick'),
            },
            {
                icon: 'spark',   key: 'ai',
                title: 'Play a bot',
                sub: 'Instant. Pick a strength from the ladder.',
                cta: 'Choose a bot',
                onclick: () => this._goPlay('ai'),
            },
            {
                icon: 'trophy',  key: 'create',
                title: 'Invite a friend',
                sub: 'Post a game and share the link.',
                cta: 'Create game',
                onclick: () => this._goPlay('create'),
            },
        ];
        return h('section', { class: 'landing__modes' },
            ...cards.map(c => this._modeCard(c))
        );
    }

    _modeCard({ icon: iname, title, sub, cta, onclick }) {
        // data-tilt marks this element for tilt.js; the handler is idempotent
        // and self-registers per screen mount. See _wireTilt().
        return h('button', {
            class: 'landing__mode',
            type: 'button',
            onclick,
            'data-tilt': '',
        },
            h('div', { class: 'landing__mode-inner' },
                h('div', { class: 'landing__mode-icon' }, icon(iname, 'icon--lg')),
                h('div', { class: 'landing__mode-body' },
                    h('div', { class: 'landing__mode-title' }, title),
                    h('div', { class: 'landing__mode-sub' }, sub),
                ),
                h('div', { class: 'landing__mode-cta' }, cta),
                h('div', { class: 'landing__mode-glow', 'aria-hidden': 'true' }),
            )
        );
    }

    /* ── How-it-works strip ─────────────────────────────────── */

    _renderHow() {
        const steps = [
            { n: 1, t: 'Click Play now',    s: 'A game starts in under a second.' },
            { n: 2, t: 'Move a piece',      s: 'Drag or tap. Legal moves only — we check them.' },
            { n: 3, t: 'Bring a friend',    s: 'Every game has a link. Paste, they join.' },
        ];
        return h('section', { class: 'landing__how' },
            h('h2', { class: 'landing__how-title' }, 'How it works'),
            h('div', { class: 'landing__how-grid' },
                ...steps.map(s =>
                    h('div', { class: 'landing__step' },
                        h('div', { class: 'landing__step-n' }, String(s.n)),
                        h('div', { class: 'landing__step-t' }, s.t),
                        h('div', { class: 'landing__step-s' }, s.s),
                    )
                )
            )
        );
    }

    /* ── Lifecycle ──────────────────────────────────────────── */

    onMount() {
        const { socket } = this.ctx;

        // Watch for the AI game start we requested — navigate the instant
        // the server confirms. Both events fire depending on server timing.
        this.sub(socket.on('game_start',   raw => this._onGameStart(raw)));
        this.sub(socket.on('game_joined',  raw => this._onGameStart(raw)));
        this.sub(socket.on('error',        raw => this._onError(raw)));

        // Enable Play now only when we have a live socket
        this.sub(socket.onState(state => {
            const disabled = state !== 'connected';
            if (this._playBtn) this._playBtn.disabled = disabled;
        }));
        if (this._playBtn) this._playBtn.disabled = !socket.isConnected();

        this._wireTilt();
        this._wireHeroPiece();
    }

    onUnmount() {
        if (this._tiltTeardown) this._tiltTeardown();
        if (this._pieceTeardown) this._pieceTeardown();
    }

    /* ── Play now ───────────────────────────────────────────── */

    _onPlayNow() {
        if (this._starting) return;
        const { socket, Outbound, store } = this.ctx;
        if (!socket.isConnected()) return;

        this._starting = true;
        this._playBtn.classList.add('is-loading');

        // Give guests a real handle — nothing about "Player" is memorable.
        this._ensureGuestHandle();

        // Fire the AI game. play_ai lands game_start almost immediately;
        // the router jumps to /game on receipt. Difficulty and time are
        // fixed here on purpose — this button removes decisions, it does
        // not offer them. Anyone who wants control clicks "More options".
        this._pending = { kind: 'ai', preset: DEFAULT_PRESET, difficulty: 'medium', fromLanding: true };
        socket.send(Outbound.playAI(this._username(), 'medium', DEFAULT_PRESET.base, DEFAULT_PRESET.inc));

        // Failsafe: if for any reason the server never replies, stop hanging
        // the button. 6s is generous; the AI game normally starts in <500ms.
        this._startFallback = this.timeout(() => {
            this._starting = false;
            if (this._playBtn) this._playBtn.classList.remove('is-loading');
            this.ctx.toast.warning('That took longer than expected — try again.', { duration: 3000 });
        }, 6000);
    }

    _onGameStart(raw) {
        if (!this._pending || !this._pending.fromLanding) return;
        if (this._startFallback) clearTimeout(this._startFallback);

        const norm = this.ctx.Inbound.normalize(raw);
        const preset = this._pending.preset;

        this.ctx.store.setGame({
            gameId:      norm.gameId,
            color:       norm.color === 'black' ? 'b' : 'w',
            opponent:    norm.opponent || 'AI',
            isAI:        !!norm.isAI || this._pending.kind === 'ai',
            whiteMs:     norm.whiteMs,
            blackMs:     norm.blackMs,
            timeBaseSec: preset.base,
            timeIncSec:  preset.inc,
            difficulty:  this._pending.difficulty || null,
        });

        // Mark this visitor as a regular — main.js will redirect '#/' to
        // '#/play' from now on so returning players skip the landing.
        storage.set('visited', true);

        this.ctx.router.go('/game');
    }

    _onError(raw) {
        if (!this._starting) return;
        this._starting = false;
        if (this._playBtn) this._playBtn.classList.remove('is-loading');
        if (this._startFallback) clearTimeout(this._startFallback);
        const norm = this.ctx.Inbound.normalize(raw);
        if (norm.isUnknownType) return;
        this.ctx.toast.danger(norm.message || 'Could not start the game.', { title: 'Server' });
    }

    _goPlay(hint) {
        // Set a hint the lobby can consume to focus the right card.
        try { sessionStorage.setItem('play_focus', hint); } catch (_) {}
        this.ctx.router.go('/play');
    }

    /* ── Guest handle ───────────────────────────────────────── */

    _username() {
        return (this.ctx.store.session.username || 'Player').slice(0, 24);
    }

    _ensureGuestHandle() {
        const name = this._username();
        if (name && name !== 'Player') return;
        const pick = GUEST_ADJECTIVES[Math.floor(Math.random() * GUEST_ADJECTIVES.length)];
        const num  = 1000 + Math.floor(Math.random() * 9000);
        this.ctx.store.setSession({ username: `${pick}${num}` });
    }

    /* ── Tilt effect (Tier 1 — hover only, desktop only) ────── */

    _wireTilt() {
        // No hover on touch — skip entirely.
        if (matchMedia('(hover: none)').matches) return;
        // Respect user prefs and the Effects setting (defaults to full on desktop).
        if (matchMedia('(prefers-reduced-motion: reduce)').matches) return;
        if (document.documentElement.dataset.effects === 'off') return;

        const targets = this.root.querySelectorAll('[data-tilt]');
        if (!targets.length) return;

        const handlers = [];
        for (const el of targets) {
            let rafId = 0;
            const onMove = (e) => {
                const rect = el.getBoundingClientRect();
                const nx = (e.clientX - rect.left) / rect.width - 0.5;   // -0.5 … 0.5
                const ny = (e.clientY - rect.top)  / rect.height - 0.5;
                if (rafId) return;
                rafId = requestAnimationFrame(() => {
                    rafId = 0;
                    // ≤ 8° per §3.1. transform + opacity only.
                    el.style.setProperty('--tilt-x', (-ny * 8).toFixed(2) + 'deg');
                    el.style.setProperty('--tilt-y', ( nx * 8).toFixed(2) + 'deg');
                    el.style.setProperty('--tilt-mx', (nx * 100 + 50).toFixed(1) + '%');
                    el.style.setProperty('--tilt-my', (ny * 100 + 50).toFixed(1) + '%');
                });
            };
            const onEnter = () => { el.style.willChange = 'transform'; };
            const onLeave = () => {
                if (rafId) { cancelAnimationFrame(rafId); rafId = 0; }
                el.style.willChange = '';           // don't park will-change
                el.style.setProperty('--tilt-x', '0deg');
                el.style.setProperty('--tilt-y', '0deg');
            };
            el.addEventListener('pointerenter', onEnter);
            el.addEventListener('pointermove',  onMove);
            el.addEventListener('pointerleave', onLeave);
            handlers.push({ el, onEnter, onMove, onLeave, rafGetter: () => rafId });
        }
        this._tiltTeardown = () => {
            for (const { el, onEnter, onMove, onLeave } of handlers) {
                el.removeEventListener('pointerenter', onEnter);
                el.removeEventListener('pointermove',  onMove);
                el.removeEventListener('pointerleave', onLeave);
            }
        };
    }

    /* ── Hero piece parallax (Tier 2 — desktop only) ───────── */

    _wireHeroPiece() {
        if (!this._piece || !this._pieceStage) return;
        if (matchMedia('(hover: none)').matches) return;
        if (matchMedia('(prefers-reduced-motion: reduce)').matches) return;
        if (document.documentElement.dataset.effects === 'off') return;

        const stage = this._pieceStage;
        let rafId = 0;
        const onMove = (e) => {
            const rect = stage.getBoundingClientRect();
            const nx = (e.clientX - rect.left) / rect.width - 0.5;
            const ny = (e.clientY - rect.top)  / rect.height - 0.5;
            if (rafId) return;
            rafId = requestAnimationFrame(() => {
                rafId = 0;
                this._piece.style.setProperty('--rx', (-ny * 14).toFixed(2) + 'deg');
                this._piece.style.setProperty('--ry', ( nx * 18).toFixed(2) + 'deg');
            });
        };
        const onLeave = () => {
            if (rafId) { cancelAnimationFrame(rafId); rafId = 0; }
            this._piece.style.setProperty('--rx', '0deg');
            this._piece.style.setProperty('--ry', '0deg');
        };
        stage.addEventListener('pointermove',  onMove);
        stage.addEventListener('pointerleave', onLeave);
        this._pieceTeardown = () => {
            stage.removeEventListener('pointermove',  onMove);
            stage.removeEventListener('pointerleave', onLeave);
        };
    }
}
