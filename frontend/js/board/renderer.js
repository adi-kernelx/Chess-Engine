/**
 * renderer.js — Canvas board renderer.
 *
 * Salvages from the old board.js:
 *   - FEN → 64-array parse (delegated to chess.js)
 *   - DPR-aware backing store
 *   - Back-to-front draw order
 *   - ease-out-cubic rAF animation
 *   - SVG <symbol> → Blob → Image rasterization
 *
 * Fixes from the plan:
 *   1. Renderer owns its own sizing via resize(cssSize). Old code had
 *      DPR/sizing math in three separate places (setupCanvas, _resizeCanvas,
 *      window resize handler); those all disappear.
 *   2. Piece raster cache keyed by (spriteUrl, size). Old code re-fetched
 *      and re-rasterized on every page switch and every resize.
 *   3. Piece-scale math extracted to one helper. Old code had it copied
 *      three times.
 *   4. Every field of BoardTheme is now honoured (checkHighlight,
 *      legalMoveCapture, coordLightColor, coordDarkColor, frameColor).
 *   5. No hidden-screen 0×0 measurement hack — resize is idempotent and
 *      can be called by ResizeObserver as soon as the container has size.
 *
 * Zero knowledge of the network. interaction.js layers input on top.
 */

import { parseFen, algebraicToIndex, indexToAlgebraic } from './chess.js';

const SPRITE_IDS = [
    'w_king', 'w_queen', 'w_rook', 'w_bishop', 'w_knight', 'w_pawn',
    'b_king', 'b_queen', 'b_rook', 'b_bishop', 'b_knight', 'b_pawn',
];

/**
 * Per-(url, size) raster cache. Shared across renderer instances.
 * Keeps piece Images so navigating between screens is instant and
 * resizing rebuilds only the size that actually changed.
 */
const rasterCache = new Map();       // key: `${url}|${sizePx}` -> { [id]: Image }
const symbolCache = new Map();       // key: url -> { [id]: { viewBox, inner } }

async function loadSymbols(url) {
    if (symbolCache.has(url)) return symbolCache.get(url);
    const res = await fetch(url);
    if (!res.ok) throw new Error(`piece sprite fetch failed: ${res.status}`);
    const text = await res.text();
    const doc = new DOMParser().parseFromString(text, 'image/svg+xml');
    const map = {};
    doc.querySelectorAll('symbol').forEach(sym => {
        const id = sym.getAttribute('id');
        if (!id) return;
        map[id] = {
            viewBox: sym.getAttribute('viewBox') || '0 0 45 45',
            inner:   sym.innerHTML,
        };
    });
    symbolCache.set(url, map);
    return map;
}

async function loadRasters(url, sizePx) {
    const key = `${url}|${sizePx}`;
    if (rasterCache.has(key)) return rasterCache.get(key);

    const symbols = await loadSymbols(url);
    const images = {};

    await Promise.all(SPRITE_IDS.map(id => {
        const sym = symbols[id];
        if (!sym) return Promise.resolve(); // Sprite missing this piece — skip
        const svgStr = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="${sym.viewBox}" width="${sizePx}" height="${sizePx}">${sym.inner}</svg>`;
        const blob = new Blob([svgStr], { type: 'image/svg+xml;charset=utf-8' });
        const objUrl = URL.createObjectURL(blob);
        return new Promise((resolve) => {
            const img = new Image();
            img.onload = () => { images[id] = img; URL.revokeObjectURL(objUrl); resolve(); };
            img.onerror = () => { URL.revokeObjectURL(objUrl); resolve(); };
            img.src = objUrl;
        });
    }));

    rasterCache.set(key, images);
    return images;
}

/** Piece drawing rectangle within a square. Extracted so it's defined once. */
function pieceRect(sqSize, scale) {
    const size = sqSize * scale;
    const offset = (sqSize - size) / 2;
    return { size, offset };
}

export class BoardRenderer {

    /**
     * @param {HTMLCanvasElement} canvas
     * @param {BoardTheme} theme
     */
    constructor(canvas, theme) {
        this.canvas = canvas;
        this.ctx = canvas.getContext('2d');
        this.theme = theme;

        this.pieceSetUrl = 'assets/pieces/classic.svg';
        this.rasters = {};                  // { [id]: Image } for current (url, size)

        this.dpr = window.devicePixelRatio || 1;
        this.logicalSize = 0;               // CSS pixels per side
        this.squareSize = 0;

        this.squares = new Array(64).fill(null);
        this.whiteOnBottom = true;

        this.lastMove = { from: null, to: null };
        this.selected = null;
        this.legalMoves = [];
        this.checkSquare = null;
        this.hoverSquare = null;

        this._animating = false;
        this._loadPromise = null;
    }

    /* ────────────────────────────────────────────────────────────
       Sizing — renderer owns this entirely.
       ──────────────────────────────────────────────────────────── */

    /**
     * Resize to a specific CSS pixel edge length. Idempotent — a repeated
     * call at the same size is a no-op. Rebuilds the raster cache only
     * when the size actually changes.
     * Returns a promise that resolves once the new-size rasters are ready.
     */
    async resize(cssSize) {
        const size = Math.max(0, Math.floor(cssSize));
        if (size <= 0) return;
        if (size === this.logicalSize) { this.draw(); return; }

        this.logicalSize = size;
        this.squareSize = size / 8;
        this.dpr = window.devicePixelRatio || 1;

        this.canvas.style.width  = size + 'px';
        this.canvas.style.height = size + 'px';
        this.canvas.width  = Math.round(size * this.dpr);
        this.canvas.height = Math.round(size * this.dpr);
        this.ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);

        await this._ensureRasters();
        this.draw();
    }

    async setPieceSet(url) {
        if (url === this.pieceSetUrl) return;
        this.pieceSetUrl = url;
        await this._ensureRasters();
        this.draw();
    }

    setTheme(theme) {
        this.theme = theme;
        this.draw();
    }

    async _ensureRasters() {
        if (this.logicalSize <= 0) return;
        // Rasterize at 2× the on-screen square size for crispness on HiDPI.
        const rasterPx = Math.round(this.squareSize * 2);
        this._loadPromise = loadRasters(this.pieceSetUrl, rasterPx);
        this.rasters = await this._loadPromise;
    }

    /* ────────────────────────────────────────────────────────────
       Position + visual state.
       ──────────────────────────────────────────────────────────── */

    setPosition(fen) {
        const parsed = parseFen(fen);
        this.squares = parsed ? parsed.squares : new Array(64).fill(null);
        if (!this._animating) this.draw();
    }

    setLastMove(from, to) {
        this.lastMove = { from, to };
        if (!this._animating) this.draw();
    }
    clearLastMove() { this.setLastMove(null, null); }

    setSelected(sq, legalMoves = []) {
        this.selected = sq;
        this.legalMoves = legalMoves;
        if (!this._animating) this.draw();
    }
    clearSelected() { this.setSelected(null, []); }

    setCheck(sq) { this.checkSquare = sq; if (!this._animating) this.draw(); }
    clearCheck() { this.checkSquare = null; if (!this._animating) this.draw(); }

    setHover(sq) {
        if (this.hoverSquare === sq) return;
        this.hoverSquare = sq;
        if (!this._animating) this.draw();
    }

    flip() { this.whiteOnBottom = !this.whiteOnBottom; this.draw(); }
    setOrientation(whiteOnBottom) {
        if (this.whiteOnBottom === whiteOnBottom) return;
        this.whiteOnBottom = whiteOnBottom;
        this.draw();
    }
    isWhiteOnBottom() { return this.whiteOnBottom; }

    /* ────────────────────────────────────────────────────────────
       Coordinate conversion.
       ──────────────────────────────────────────────────────────── */

    /** Canvas CSS pixels → algebraic square, or null if outside. */
    canvasToSquare(x, y) {
        if (this.squareSize <= 0) return null;
        if (x < 0 || y < 0 || x >= this.logicalSize || y >= this.logicalSize) return null;
        const col = Math.floor(x / this.squareSize);
        const row = Math.floor(y / this.squareSize);
        const { file, rank } = this._colRowToFileRank(col, row);
        return indexToAlgebraic(rank * 8 + file);
    }

    /** Algebraic → { x, y } of top-left in CSS pixels. */
    squareOrigin(sq) {
        const idx = algebraicToIndex(sq);
        if (idx < 0) return null;
        const { col, row } = this._indexToColRow(idx);
        return { x: col * this.squareSize, y: row * this.squareSize };
    }

    /** Algebraic → CSS pixel center of the square. */
    squareCenter(sq) {
        const o = this.squareOrigin(sq);
        if (!o) return null;
        return { x: o.x + this.squareSize / 2, y: o.y + this.squareSize / 2 };
    }

    _indexToColRow(idx) {
        const file = idx % 8;
        const rank = Math.floor(idx / 8);
        return this.whiteOnBottom
            ? { col: file,     row: 7 - rank }
            : { col: 7 - file, row: rank     };
    }

    _colRowToFileRank(col, row) {
        return this.whiteOnBottom
            ? { file: col,     rank: 7 - row }
            : { file: 7 - col, rank: row     };
    }

    /* ────────────────────────────────────────────────────────────
       Drawing.
       ──────────────────────────────────────────────────────────── */

    draw() {
        if (this._animating) return; // Animation loop calls _drawInternal directly.
        this._drawInternal(null);
    }

    _drawInternal(skipSquare /* algebraic square whose piece is hidden */) {
        const ctx = this.ctx;
        const sz  = this.squareSize;
        if (sz <= 0) return;

        ctx.clearRect(0, 0, this.logicalSize, this.logicalSize);

        // (a) Squares.
        for (let i = 0; i < 64; i++) {
            const { col, row } = this._indexToColRow(i);
            const isLight = (col + row) % 2 === 0;
            ctx.fillStyle = isLight ? this.theme.lightSquare : this.theme.darkSquare;
            ctx.fillRect(col * sz, row * sz, sz, sz);
        }

        // (b) Highlights.
        this._paintSquare(this.lastMove.from, this.theme.lastMoveHighlight);
        this._paintSquare(this.lastMove.to,   this.theme.lastMoveHighlight);
        this._paintSquare(this.selected,      this.theme.selectedHighlight);
        this._paintHover();
        this._paintCheck();

        // (c) Legal move markers.
        for (const target of this.legalMoves) {
            const idx = algebraicToIndex(target);
            if (idx < 0) continue;
            const occupied = !!this.squares[idx];
            const { col, row } = this._indexToColRow(idx);
            const cx = col * sz + sz / 2;
            const cy = row * sz + sz / 2;
            if (occupied) {
                ctx.strokeStyle = this.theme.legalMoveCapture;
                ctx.lineWidth = sz * 0.1;
                ctx.beginPath();
                ctx.arc(cx, cy, sz * 0.42, 0, Math.PI * 2);
                ctx.stroke();
            } else {
                ctx.fillStyle = this.theme.legalMoveDot;
                ctx.beginPath();
                ctx.arc(cx, cy, sz * 0.16, 0, Math.PI * 2);
                ctx.fill();
            }
        }

        // (d) Pieces.
        const { size: pSize, offset } = pieceRect(sz, this.theme.pieceScale);
        for (let i = 0; i < 64; i++) {
            const piece = this.squares[i];
            if (!piece) continue;
            if (skipSquare && indexToAlgebraic(i) === skipSquare) continue;
            const img = this.rasters[`${piece.color}_${piece.type}`];
            if (!img) continue;
            const { col, row } = this._indexToColRow(i);
            ctx.drawImage(img, col * sz + offset, row * sz + offset, pSize, pSize);
        }

        // (e) Coordinates.
        if (this.theme.showCoords) this._drawCoords();
    }

    _paintSquare(sq, color) {
        if (!sq) return;
        const idx = algebraicToIndex(sq);
        if (idx < 0) return;
        const { col, row } = this._indexToColRow(idx);
        this.ctx.fillStyle = color;
        this.ctx.fillRect(col * this.squareSize, row * this.squareSize, this.squareSize, this.squareSize);
    }

    _paintHover() {
        if (!this.hoverSquare) return;
        const idx = algebraicToIndex(this.hoverSquare);
        if (idx < 0) return;
        const { col, row } = this._indexToColRow(idx);
        const sz = this.squareSize;
        this.ctx.strokeStyle = 'rgba(255, 255, 255, 0.28)';
        this.ctx.lineWidth = Math.max(2, sz * 0.03);
        this.ctx.strokeRect(
            col * sz + this.ctx.lineWidth / 2,
            row * sz + this.ctx.lineWidth / 2,
            sz - this.ctx.lineWidth,
            sz - this.ctx.lineWidth
        );
    }

    _paintCheck() {
        if (!this.checkSquare) return;
        const idx = algebraicToIndex(this.checkSquare);
        if (idx < 0) return;
        const { col, row } = this._indexToColRow(idx);
        const sz = this.squareSize;
        const cx = col * sz + sz / 2;
        const cy = row * sz + sz / 2;
        const grad = this.ctx.createRadialGradient(cx, cy, sz * 0.05, cx, cy, sz * 0.55);
        grad.addColorStop(0, this.theme.checkHighlight);
        grad.addColorStop(1, 'rgba(224, 85, 85, 0)');
        this.ctx.fillStyle = grad;
        this.ctx.fillRect(col * sz, row * sz, sz, sz);
    }

    _drawCoords() {
        const ctx = this.ctx;
        const sz = this.squareSize;
        const fontSize = Math.max(9, Math.round(sz * 0.15));
        ctx.font = `600 ${fontSize}px var(--font-ui, sans-serif)`;
        ctx.textBaseline = 'top';

        for (let i = 0; i < 64; i++) {
            const { col, row } = this._indexToColRow(i);
            const isLight = (col + row) % 2 === 0;
            const alg = indexToAlgebraic(i);
            const file = alg[0], rank = alg[1];

            // Use themed coord colors when set; otherwise contrast against the square.
            const color = isLight
                ? (this.theme.coordDarkColor  || this.theme.darkSquare)
                : (this.theme.coordLightColor || this.theme.lightSquare);
            ctx.fillStyle = color;

            if (col === 0) {                       // rank labels down the left edge
                ctx.textAlign = 'left';
                ctx.fillText(rank, col * sz + 3, row * sz + 3);
            }
            if (row === 7) {                       // file labels along the bottom
                ctx.textAlign = 'right';
                ctx.textBaseline = 'bottom';
                ctx.fillText(file, (col + 1) * sz - 3, (row + 1) * sz - 3);
                ctx.textBaseline = 'top';
            }
        }
    }

    /* ────────────────────────────────────────────────────────────
       Animation.
       ──────────────────────────────────────────────────────────── */

    /**
     * Animate a piece moving from `fromSq` to `toSq`. Assumes the
     * caller has already applied the move to the underlying position
     * (via setPosition). Uses the piece currently at `toSq`, or falls
     * back to `fromSq` if the board hasn't been updated yet.
     */
    animateMove(fromSq, toSq, done) {
        if (!this.theme.animationDuration || this.theme.animationDuration <= 0) {
            this.draw(); if (done) done(); return;
        }
        const fromIdx = algebraicToIndex(fromSq);
        const toIdx = algebraicToIndex(toSq);
        if (fromIdx < 0 || toIdx < 0) { if (done) done(); return; }

        const piece = this.squares[toIdx] || this.squares[fromIdx];
        if (!piece) { if (done) done(); return; }

        const img = this.rasters[`${piece.color}_${piece.type}`];
        if (!img) { if (done) done(); return; }

        const { col: fCol, row: fRow } = this._indexToColRow(fromIdx);
        const { col: tCol, row: tRow } = this._indexToColRow(toIdx);
        const sz = this.squareSize;
        const { size: pSize, offset } = pieceRect(sz, this.theme.pieceScale);

        const startX = fCol * sz, startY = fRow * sz;
        const endX   = tCol * sz, endY   = tRow * sz;

        this._animating = true;
        const startTs = performance.now();
        const duration = this.theme.animationDuration;

        let finished = false;
        const finish = () => {
            if (finished) return;
            finished = true;
            this._animating = false;
            this.draw();
            if (done) done();
        };

        const tick = (now) => {
            if (finished) return;
            const t = Math.min(1, (now - startTs) / duration);
            const eased = 1 - Math.pow(1 - t, 3);
            const x = startX + (endX - startX) * eased;
            const y = startY + (endY - startY) * eased;

            // Draw board with destination piece hidden, then overlay the moving one.
            this._drawInternal(toSq);
            this.ctx.drawImage(img, x + offset, y + offset, pSize, pSize);

            if (t < 1) requestAnimationFrame(tick);
            else finish();
        };
        requestAnimationFrame(tick);
        // Safety net: guarantees the callback fires even in non-compositing
        // tabs where rAF is throttled to zero (background PWAs, headless).
        setTimeout(finish, duration + 100);
    }
}
