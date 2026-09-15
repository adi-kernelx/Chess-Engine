/**
 * board-test.js — Temporary Part-3 verification screen.
 *
 * Not part of the final navigation. Mounted at #/board-test so we can
 * verify the board renders, flips, animates, resizes crisply, honours
 * theme swaps, honours piece-set swaps, and pops the promotion picker
 * — all without needing a real game server.
 *
 * Removed at end of Part 4 once the real game screen is in place.
 */

import { Screen } from '../ui/screen.js';
import { h, q } from '../core/dom.js';
import { BoardRenderer } from '../board/renderer.js';
import { BoardInteraction } from '../board/interaction.js';
import { BoardTheme, THEMES } from '../board/theme.js';
import { PromotionPicker } from '../board/promotion.js';
import { parseFen, findKingSquare, capturedPieces, materialBalance, isPromotion, START_FEN } from '../board/chess.js';

const PRESET_FENS = [
    { label: 'Start',           fen: START_FEN },
    { label: 'Italian',         fen: 'r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3' },
    { label: 'Kiwipete',        fen: 'r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1' },
    { label: 'Promotion soon',  fen: '4k3/4P3/8/8/8/8/8/4K3 w - - 0 1' },
    { label: 'Endgame + check', fen: '4k3/8/8/8/3Q4/8/8/4K3 b - - 0 1' },
];

export class BoardTestScreen extends Screen {
    constructor(ctx) {
        super(ctx);
        this._fen = START_FEN;
        this._themeKey = 'classic';
        this._pieceSet = 'classic';
        this._resizeObs = null;
    }

    render() {
        return h('div', { class: 'screen' },
            this.header('Board Test', 'Part-3 verification harness — renderer, interaction, animation, promotion.'),
            h('div', { class: 'screen__body board-test' },
                h('div', { class: 'board-test__grid' },
                    // ── Left column: the board ──
                    h('div', { class: 'board-test__stage' },
                        h('div', { class: 'board-test__strip', ref: (el) => this._capturedTop = el }),
                        h('div', { class: 'board-frame' },
                            h('div', { class: 'board-container', ref: (el) => this._container = el },
                                h('canvas', { class: 'board-canvas', ref: (el) => this._canvas = el })
                            )
                        ),
                        h('div', { class: 'board-test__strip', ref: (el) => this._capturedBot = el }),
                    ),
                    // ── Right column: controls ──
                    h('div', { class: 'board-test__controls' },
                        h('div', { class: 'card' },
                            h('div', { class: 'card__header' },
                                h('div', { class: 'card__title' }, 'Position')),
                            h('div', { class: 'card__body' },
                                h('div', { class: 'field' },
                                    h('div', { class: 'field__label' }, 'Preset'),
                                    h('div', { class: 'pill-group' },
                                        ...PRESET_FENS.map((p, idx) =>
                                            h('button', {
                                                class: 'pill-group__item' + (idx === 0 ? ' is-active' : ''),
                                                dataset: { presetIdx: String(idx) },
                                                onclick: (e) => this._applyPreset(idx, e.currentTarget),
                                            }, p.label)
                                        )
                                    )
                                ),
                                h('div', { class: 'field' },
                                    h('div', { class: 'field__label' }, 'FEN'),
                                    h('input', {
                                        class: 'input input--mono',
                                        ref: (el) => this._fenInput = el,
                                        value: this._fen,
                                        onchange: (e) => this._applyFen(e.target.value),
                                        onkeydown: (e) => { if (e.key === 'Enter') this._applyFen(e.target.value); },
                                    })
                                ),
                                h('div', { style: { display: 'flex', gap: '8px', flexWrap: 'wrap' } },
                                    h('button', { class: 'btn btn--sm', onclick: () => this.renderer.flip() }, 'Flip'),
                                    h('button', { class: 'btn btn--sm', onclick: () => this._demoAnimate() }, 'Animate e2→e4'),
                                    h('button', { class: 'btn btn--sm', onclick: () => this._demoPromo() }, 'Promotion picker'),
                                    h('button', { class: 'btn btn--sm', onclick: () => this._demoCheck() }, 'Show check'),
                                )
                            )
                        ),
                        h('div', { class: 'card' },
                            h('div', { class: 'card__header' },
                                h('div', { class: 'card__title' }, 'Appearance')),
                            h('div', { class: 'card__body' },
                                h('div', { class: 'field' },
                                    h('div', { class: 'field__label' }, 'Theme'),
                                    h('div', { class: 'pill-group' },
                                        ...Object.entries(THEMES).map(([key, spec]) =>
                                            h('button', {
                                                class: 'pill-group__item' + (key === this._themeKey ? ' is-active' : ''),
                                                dataset: { theme: key },
                                                onclick: (e) => this._applyTheme(key, e.currentTarget),
                                            }, spec.label)
                                        )
                                    )
                                ),
                                h('div', { class: 'field' },
                                    h('div', { class: 'field__label' }, 'Pieces'),
                                    h('div', { class: 'pill-group' },
                                        h('button', {
                                            class: 'pill-group__item is-active',
                                            dataset: { pset: 'classic' },
                                            onclick: (e) => this._applyPieceSet('classic', e.currentTarget),
                                        }, 'Classic'),
                                        h('button', {
                                            class: 'pill-group__item',
                                            dataset: { pset: 'modern' },
                                            onclick: (e) => this._applyPieceSet('modern', e.currentTarget),
                                        }, 'Modern')
                                    )
                                ),
                            )
                        ),
                        h('div', { class: 'card' },
                            h('div', { class: 'card__header' },
                                h('div', { class: 'card__title' }, 'Diagnostics')),
                            h('div', { class: 'card__body' },
                                h('div', { class: 'field' },
                                    h('div', { class: 'field__label' }, 'Move log'),
                                    h('div', {
                                        ref: (el) => this._log = el,
                                        style: {
                                            fontFamily: 'var(--font-mono)', fontSize: 'var(--fs-xs)',
                                            color: 'var(--text-secondary)', maxHeight: '160px',
                                            overflowY: 'auto', background: 'var(--surface-1)',
                                            padding: 'var(--sp-2) var(--sp-3)', borderRadius: 'var(--r-2)',
                                            border: '1px solid var(--border-subtle)',
                                        },
                                    })
                                ),
                                h('div', { class: 'field__hint', ref: (el) => this._stats = el })
                            )
                        )
                    )
                )
            )
        );
    }

    onMount() {
        this.renderer = new BoardRenderer(this._canvas, BoardTheme.classic());
        this.interaction = new BoardInteraction(this._canvas, this.renderer);
        this.promotion = new PromotionPicker(this._container, this.renderer);

        this.interaction.onMoveIntent = async (from, to) => {
            const parsed = parseFen(this._fen);
            let promo = null;
            if (isPromotion(from, to, parsed)) {
                const p = parsed.squares[require_algToIdx(from)];
                promo = await this.promotion.ask(to, p.color);
                if (!promo) {
                    this._writeLog(`(cancelled ${from}→${to})`);
                    return;
                }
            }
            this._writeLog(`${from} → ${to}${promo ? '=' + promo.toUpperCase() : ''}`);
            this.renderer.setLastMove(from, to);
            // We don't have a real move engine on the client — just visualize.
            this.renderer.animateMove(from, to, () => { /* server would send new FEN */ });
        };

        // Observe the container so any layout change (nav, resize, screen swap)
        // keeps the board crisp. This replaces the old 0×0 hack.
        this._resizeObs = new ResizeObserver((entries) => {
            for (const e of entries) {
                const size = Math.min(e.contentRect.width, e.contentRect.height);
                if (size > 0) {
                    this.renderer.resize(size).then(() => {
                        this._recomputeCaptured();
                        this._recomputeStats();
                    });
                }
            }
        });
        this._resizeObs.observe(this._container);

        // Race sync + rAF + timeout so the board always sizes, even in
        // environments where rAF is throttled.
        let booted = false;
        const doIt = () => {
            if (booted) return;
            const rect = this._container.getBoundingClientRect();
            const size = Math.min(rect.width, rect.height);
            if (size <= 0) return;
            booted = true;
            this.renderer.resize(size).then(() => this._applyFen(this._fen, true));
        };
        doIt();
        if (!booted) requestAnimationFrame(doIt);
        this.timeout(doIt, 60);
        this.timeout(doIt, 250);
    }

    onUnmount() {
        if (this._resizeObs) { this._resizeObs.disconnect(); this._resizeObs = null; }
        if (this.interaction) this.interaction.destroy();
        if (this.promotion) this.promotion.close();
    }

    /* ── Actions ── */

    _applyPreset(idx, btn) {
        const p = PRESET_FENS[idx];
        if (!p) return;
        this._fenInput.value = p.fen;
        this._applyFen(p.fen);
        for (const el of btn.parentElement.children) el.classList.remove('is-active');
        btn.classList.add('is-active');
    }

    _applyFen(fen, silent = false) {
        this._fen = fen;
        this.renderer.setPosition(fen);
        this.renderer.clearLastMove();
        this.renderer.clearCheck();
        this._recomputeCaptured();
        this._recomputeStats();
        if (!silent) this._writeLog(`FEN: ${fen.slice(0, 40)}${fen.length > 40 ? '…' : ''}`);
    }

    async _applyTheme(key, btn) {
        this._themeKey = key;
        this.renderer.setTheme(THEMES[key].factory());
        for (const el of btn.parentElement.children) el.classList.remove('is-active');
        btn.classList.add('is-active');
    }

    async _applyPieceSet(name, btn) {
        this._pieceSet = name;
        await this.renderer.setPieceSet(`assets/pieces/${name}.svg`);
        for (const el of btn.parentElement.children) el.classList.remove('is-active');
        btn.classList.add('is-active');
    }

    _demoAnimate() {
        this._applyFen(START_FEN);
        this.renderer.setLastMove(null, null);
        // Update the underlying board so the animation has a piece to render.
        const after = 'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1';
        setTimeout(() => {
            this.renderer.setPosition(after);
            this.renderer.animateMove('e2', 'e4', () => {
                this.renderer.setLastMove('e2', 'e4');
                this._writeLog('animated e2→e4');
            });
        }, 100);
    }

    async _demoPromo() {
        this._applyFen('4k3/4P3/8/8/8/8/8/4K3 w - - 0 1');
        const code = await this.promotion.ask('e8', 'w');
        this._writeLog(code ? `promoted to ${code.toUpperCase()}` : '(promotion cancelled)');
    }

    _demoCheck() {
        this._applyFen('rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3');
        const kSq = findKingSquare(parseFen(this._fen), 'w');
        if (kSq) this.renderer.setCheck(kSq);
        this._writeLog(`check on white king (${kSq})`);
    }

    _recomputeCaptured() {
        const parsed = parseFen(this._fen);
        const cap = capturedPieces(parsed);
        const bal = materialBalance(parsed);
        // Top strip shows pieces captured from Black (i.e. taken BY White).
        // Bottom strip shows pieces captured from White.
        this._paintCapturedInto(this._capturedTop, cap.b, 'b', -bal);
        this._paintCapturedInto(this._capturedBot, cap.w, 'w',  bal);
    }

    _paintCapturedInto(root, list, capturedColor, advantage) {
        while (root.firstChild) root.removeChild(root.firstChild);
        const wrap = h('div', { class: 'captured' });
        const pieces = h('div', { class: 'captured__pieces' });
        for (const t of list) {
            const key = `${capturedColor}_${t}`;
            const img = this.renderer.rasters[key];
            if (!img) continue;
            const clone = new Image();
            clone.src = img.src;
            clone.className = 'captured__piece';
            pieces.appendChild(clone);
        }
        wrap.appendChild(pieces);
        if (advantage > 0) {
            wrap.appendChild(h('span', { class: 'captured__advantage' }, `+${advantage}`));
        }
        root.appendChild(wrap);
    }

    _recomputeStats() {
        if (!this._stats) return;
        this._stats.textContent =
            `canvas ${this.renderer.canvas.width}×${this.renderer.canvas.height} @${this.renderer.dpr}× ` +
            `· square ${this.renderer.squareSize.toFixed(1)}px`;
    }

    _writeLog(line) {
        if (!this._log) return;
        const stamp = new Date().toLocaleTimeString();
        this._log.appendChild(h('div', {}, `[${stamp}] ${line}`));
        this._log.scrollTop = this._log.scrollHeight;
    }
}

/** Small helper used only here. */
function require_algToIdx(sq) {
    return (parseInt(sq[1], 10) - 1) * 8 + (sq.charCodeAt(0) - 97);
}
