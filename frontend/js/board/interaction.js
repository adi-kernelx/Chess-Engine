/**
 * interaction.js — Input handling on top of a BoardRenderer.
 *
 * Renamed from the old (misnamed) pieces.js. Fires a single event:
 *   onMoveIntent(from, to)
 * The game screen decides what to do next — including surfacing a
 * promotion picker when needed.
 *
 * Design:
 *   - Zero knowledge of legality. Every drag from an own piece to a
 *     non-own square fires. The server is authoritative.
 *   - Optional `legalTargetsProvider(fromSq)` — if supplied, the
 *     screen can pre-highlight legal moves; otherwise no dots show.
 *   - Click-to-select AND drag-and-drop, both driven by the same
 *     underlying state machine. Touch is handled uniformly.
 */

import { parseFen, algebraicToIndex, indexToAlgebraic } from './chess.js';

export class BoardInteraction {

    constructor(canvas, renderer) {
        this.canvas = canvas;
        this.renderer = renderer;

        this.onMoveIntent = null;          // (from, to) => void
        this.legalTargetsProvider = null;  // (from) => string[]
        this.playerColor = null;           // 'w' | 'b' | null (null = both, for analysis)
        this.enabled = true;

        this._selected = null;
        this._dragging = null;             // { from, x, y, piece }

        // Bind once so remove works.
        this._onDown = this._onDown.bind(this);
        this._onMove = this._onMove.bind(this);
        this._onUp = this._onUp.bind(this);
        this._onLeave = this._onLeave.bind(this);
        this._onTouchStart = this._onTouchStart.bind(this);
        this._onTouchMove = this._onTouchMove.bind(this);
        this._onTouchEnd = this._onTouchEnd.bind(this);

        canvas.addEventListener('mousedown',  this._onDown);
        window.addEventListener('mousemove',  this._onMove);
        window.addEventListener('mouseup',    this._onUp);
        canvas.addEventListener('mouseleave', this._onLeave);
        canvas.addEventListener('touchstart', this._onTouchStart, { passive: false });
        window.addEventListener('touchmove',  this._onTouchMove,  { passive: false });
        window.addEventListener('touchend',   this._onTouchEnd);
    }

    destroy() {
        this.canvas.removeEventListener('mousedown',  this._onDown);
        window.removeEventListener('mousemove',  this._onMove);
        window.removeEventListener('mouseup',    this._onUp);
        this.canvas.removeEventListener('mouseleave', this._onLeave);
        this.canvas.removeEventListener('touchstart', this._onTouchStart);
        window.removeEventListener('touchmove',  this._onTouchMove);
        window.removeEventListener('touchend',   this._onTouchEnd);
    }

    /** External state setters (game.js wires these on turn change). */
    setPlayerColor(color) { this.playerColor = color; }
    setEnabled(enabled) {
        this.enabled = enabled;
        if (!enabled) this._deselect();
    }

    /* ── Coordinate helpers ── */
    _localPoint(clientX, clientY) {
        const rect = this.canvas.getBoundingClientRect();
        return { x: clientX - rect.left, y: clientY - rect.top };
    }
    _sqAt(clientX, clientY) {
        const p = this._localPoint(clientX, clientY);
        return this.renderer.canvasToSquare(p.x, p.y);
    }
    _pieceAt(sq) {
        if (!sq) return null;
        const idx = algebraicToIndex(sq);
        return idx >= 0 ? this.renderer.squares[idx] : null;
    }
    _isOwn(piece) {
        if (!piece) return false;
        if (!this.playerColor) return true;
        return piece.color === this.playerColor;
    }

    /* ── Mouse ── */
    _onDown(e) {
        if (!this.enabled || e.button !== 0) return;
        this._pointerDown(e.clientX, e.clientY);
        // Don't preventDefault — we want focus/blur to work normally.
    }

    _onMove(e) {
        if (!this.enabled) return;
        this._pointerMove(e.clientX, e.clientY);
    }

    _onUp(e) {
        if (!this.enabled) return;
        this._pointerUp(e.clientX, e.clientY);
    }

    _onLeave() {
        this.renderer.setHover(null);
    }

    /* ── Touch ── */
    _onTouchStart(e) {
        if (!this.enabled) return;
        if (e.touches.length !== 1) return;
        e.preventDefault();
        const t = e.touches[0];
        this._pointerDown(t.clientX, t.clientY);
    }
    _onTouchMove(e) {
        if (!this._dragging && !this._selected) return;
        if (e.touches.length !== 1) return;
        e.preventDefault();
        const t = e.touches[0];
        this._pointerMove(t.clientX, t.clientY);
    }
    _onTouchEnd(e) {
        const t = e.changedTouches[0];
        this._pointerUp(t.clientX, t.clientY);
    }

    /* ── Pointer state machine ── */
    _pointerDown(clientX, clientY) {
        const sq = this._sqAt(clientX, clientY);
        if (!sq) return;

        const piece = this._pieceAt(sq);

        // If a piece is already selected and this square is a legal target
        // (or empty non-own square), interpret as a move intent.
        if (this._selected && sq !== this._selected) {
            const own = piece && this._isOwn(piece);
            if (!own) {
                this._issueIntent(this._selected, sq);
                return;
            }
            // Own piece: swap selection.
        }

        // Otherwise, start a fresh selection on an own piece.
        if (piece && this._isOwn(piece)) {
            this._select(sq);
            this._dragging = { from: sq, x: clientX, y: clientY, piece };
        } else {
            this._deselect();
        }
    }

    _pointerMove(clientX, clientY) {
        if (this._dragging) {
            this._dragging.x = clientX;
            this._dragging.y = clientY;
            const sq = this._sqAt(clientX, clientY);
            this.renderer.setHover(sq);
        } else {
            const sq = this._sqAt(clientX, clientY);
            // Only show hover cursor over own pieces or selected-target squares.
            this._updateCursor(sq);
        }
    }

    _pointerUp(clientX, clientY) {
        this.renderer.setHover(null);
        if (!this._dragging) return;

        const from = this._dragging.from;
        const sq = this._sqAt(clientX, clientY);
        this._dragging = null;

        if (!sq || sq === from) {
            // Click without drag → keep selection so click-to-move works.
            return;
        }

        const piece = this._pieceAt(sq);
        const own = piece && this._isOwn(piece);
        if (own) {
            // Dropped on another own piece → treat as re-selection.
            this._select(sq);
            return;
        }
        this._issueIntent(from, sq);
    }

    _updateCursor(sq) {
        const p = this._pieceAt(sq);
        if (this._selected) {
            this.canvas.style.cursor = 'pointer';
        } else if (p && this._isOwn(p)) {
            this.canvas.style.cursor = 'grab';
        } else {
            this.canvas.style.cursor = 'default';
        }
    }

    _select(sq) {
        this._selected = sq;
        const targets = this.legalTargetsProvider ? this.legalTargetsProvider(sq) : [];
        this.renderer.setSelected(sq, targets);
        this.canvas.style.cursor = 'grabbing';
    }

    _deselect() {
        this._selected = null;
        this.renderer.clearSelected();
        this.canvas.style.cursor = 'default';
    }

    _issueIntent(from, to) {
        const wasSelected = this._selected;
        this._deselect();
        if (this.onMoveIntent) {
            try { this.onMoveIntent(from, to); }
            catch (err) { console.error('[interaction] onMoveIntent threw:', err); }
        }
        // Nothing to do on failure — server rejects with move_rejected,
        // the game screen will show a toast and re-sync.
        void wasSelected;
    }
}
