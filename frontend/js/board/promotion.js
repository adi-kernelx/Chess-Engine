/**
 * promotion.js — Promotion picker overlay.
 *
 * Positioned absolutely inside the board container (which must be
 * position:relative). Shows Q / R / B / N as a vertical strip
 * anchored to the destination square, one square wide.
 *
 * Backend accepts `promotion: 'q' | 'r' | 'b' | 'n'`; the raw char
 * is what we hand back to the caller.
 */

import { h } from '../core/dom.js';

const CHOICES = [
    { type: 'queen',  code: 'q' },
    { type: 'rook',   code: 'r' },
    { type: 'bishop', code: 'b' },
    { type: 'knight', code: 'n' },
];

export class PromotionPicker {

    /**
     * @param {HTMLElement} container  position:relative parent (board container).
     * @param {BoardRenderer} renderer  used to look up piece rasters and square origin.
     */
    constructor(container, renderer) {
        this.container = container;
        this.renderer = renderer;
        this.overlay = null;
    }

    /**
     * @param {string} sq          destination square, e.g. 'e8' or 'b1'
     * @param {'w'|'b'} color      promoting side (also determines strip direction)
     * @returns {Promise<string|null>}  resolves with 'q'|'r'|'b'|'n' or null on cancel
     */
    ask(sq, color) {
        this.close();
        return new Promise((resolve) => {
            const origin = this.renderer.squareOrigin(sq);
            const sz = this.renderer.squareSize;
            if (!origin) { resolve(null); return; }

            // Strip grows downward when the piece will sit at the top of the board,
            // and upward when it'll sit at the bottom. The visual row depends on
            // whether the board is flipped.
            const rank = parseInt(sq[1], 10);
            const isTop = (this.renderer.isWhiteOnBottom() ? rank === 8 : rank === 1);

            const optionEls = CHOICES.map(({ type, code }) => {
                const btn = h('button', {
                    class: 'promo-picker__option',
                    'aria-label': `Promote to ${type}`,
                    onclick: (e) => { e.stopPropagation(); done(code); },
                });
                const img = this.renderer.rasters[`${color}_${type}`];
                if (img) {
                    // Clone the image so we don't disturb the raster cache.
                    const clone = new Image();
                    clone.src = img.src;
                    clone.className = 'promo-picker__img';
                    btn.appendChild(clone);
                }
                return btn;
            });

            const stripHeight = sz * CHOICES.length;
            const top  = isTop ? origin.y : origin.y - (stripHeight - sz);
            const left = origin.x;

            const strip = h('div', {
                class: 'promo-picker',
                style: {
                    left:   left + 'px',
                    top:    top + 'px',
                    width:  sz + 'px',
                    height: stripHeight + 'px',
                },
            }, ...optionEls);

            const scrim = h('div', {
                class: 'promo-picker__scrim',
                onclick: () => done(null),
            }, strip);

            this.container.appendChild(scrim);
            this.overlay = scrim;

            let focusIdx = 0;
            const keyHandler = (e) => {
                if (e.key === 'Escape') { done(null); return; }
                if (e.key === 'ArrowDown' || e.key === 'ArrowRight') {
                    e.preventDefault();
                    focusIdx = (focusIdx + 1) % optionEls.length;
                    optionEls[focusIdx].focus();
                } else if (e.key === 'ArrowUp' || e.key === 'ArrowLeft') {
                    e.preventDefault();
                    focusIdx = (focusIdx - 1 + optionEls.length) % optionEls.length;
                    optionEls[focusIdx].focus();
                } else if (e.key === 'Enter' || e.key === ' ') {
                    e.preventDefault();
                    optionEls[focusIdx].click();
                }
            };
            window.addEventListener('keydown', keyHandler);

            const done = (code) => {
                window.removeEventListener('keydown', keyHandler);
                this.close();
                resolve(code);
            };

            requestAnimationFrame(() => {
                if (optionEls[0]) optionEls[0].focus();
            });
        });
    }

    close() {
        if (this.overlay) {
            this.overlay.remove();
            this.overlay = null;
        }
    }
}
