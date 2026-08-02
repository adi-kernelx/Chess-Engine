/**
 * BoardInteraction — Handles user input (click and drag) on the chess board canvas.
 *
 * This class does NOT know about the server or WebSocket. It emits move intents
 * to the game controller via a callback. The game controller decides whether
 * the move is valid and sends it to the server.
 */
class BoardInteraction {
    /**
     * @param {HTMLCanvasElement} canvas
     * @param {Object} renderer BoardRenderer instance
     */
    constructor(canvas, renderer) {
        this.canvas = canvas;
        this.renderer = renderer;
        
        this._onMoveIntent = null;
        this._playerColor = null;
        this._legalMovesMap = {};
        this._enabled = false;
        
        this.selectedSquare = null;
        this.isDragging = false;
        this.draggedSquare = null;
        this.dragPosition = { x: 0, y: 0 };
        
        this._bindEvents();
    }

    /** Set the callback that fires when user intends to make a move */
    set onMoveIntent(callback) {
        this._onMoveIntent = callback;
    }

    /** Set which color this player controls ('w' or 'b') */
    set playerColor(color) {
        this._playerColor = color;
    }

    /** Set the list of legal moves for the current position */
    set legalMovesMap(map) {
        this._legalMovesMap = map || {};
        if (this.selectedSquare && !this._legalMovesMap[this.selectedSquare]) {
            this._clearSelection();
        } else if (this.selectedSquare) {
            this.renderer.setSelectedSquare(this.selectedSquare, this._legalMovesMap[this.selectedSquare] || []);
            this._draw();
        }
    }

    /** Enable or disable interaction */
    set enabled(val) {
        this._enabled = val;
        if (!val) {
            this._clearSelection();
            if (this.isDragging) {
                this.isDragging = false;
                this.draggedSquare = null;
            }
            this.canvas.style.cursor = 'default';
            this._draw();
        } else {
            this._updateCursor();
        }
    }

    /** Remove all event listeners */
    destroy() {
        this._unbindEvents();
    }

    /** Convert algebraic square (e.g. 'e2') to board array index (0-63) */
    _sqToIndex(sq) {
        if (!sq) return -1;
        const f = sq.charCodeAt(0) - 97; // 'a' = 97
        const r = sq.charCodeAt(1) - 49; // '1' = 49
        return r * 8 + f;
    }

    /** Get the piece object at a given algebraic square */
    _getPieceAt(sq) {
        if (!sq) return null;
        const index = this._sqToIndex(sq);
        return this.renderer.board[index];
    }

    /** Check if a square has a piece belonging to the current player */
    _isOwnPiece(sq) {
        const piece = this._getPieceAt(sq);
        return piece && piece.color === this._playerColor;
    }

    /** Get legal target squares for a given square */
    _getLegalTargets(sq) {
        return this._legalMovesMap[sq] || [];
    }

    _bindEvents() {
        this._onMouseDown = this._handleMouseDown.bind(this);
        this._onMouseMove = this._handleMouseMove.bind(this);
        this._onMouseUp = this._handleMouseUp.bind(this);
        this._onContextMenu = (e) => e.preventDefault();

        this._onTouchStart = this._handleTouchStart.bind(this);
        this._onTouchMove = this._handleTouchMove.bind(this);
        this._onTouchEnd = this._handleTouchEnd.bind(this);

        this.canvas.addEventListener('mousedown', this._onMouseDown);
        window.addEventListener('mousemove', this._onMouseMove);
        window.addEventListener('mouseup', this._onMouseUp);
        this.canvas.addEventListener('contextmenu', this._onContextMenu);

        this.canvas.addEventListener('touchstart', this._onTouchStart, { passive: false });
        window.addEventListener('touchmove', this._onTouchMove, { passive: false });
        window.addEventListener('touchend', this._onTouchEnd);
    }

    _unbindEvents() {
        this.canvas.removeEventListener('mousedown', this._onMouseDown);
        window.removeEventListener('mousemove', this._onMouseMove);
        window.removeEventListener('mouseup', this._onMouseUp);
        this.canvas.removeEventListener('contextmenu', this._onContextMenu);

        this.canvas.removeEventListener('touchstart', this._onTouchStart);
        window.removeEventListener('touchmove', this._onTouchMove);
        window.removeEventListener('touchend', this._onTouchEnd);
    }

    _getEventPos(e) {
        const rect = this.canvas.getBoundingClientRect();
        let clientX, clientY;

        if (e.changedTouches && e.changedTouches.length > 0) {
            clientX = e.changedTouches[0].clientX;
            clientY = e.changedTouches[0].clientY;
        } else {
            clientX = e.clientX;
            clientY = e.clientY;
        }

        return {
            x: clientX - rect.left,
            y: clientY - rect.top
        };
    }

    _handleMouseDown(e) {
        if (!this._enabled) return;
        if (e.button !== 0 && e.type.startsWith('mouse')) return; // Only left click

        const pos = this._getEventPos(e);
        const sq = this.renderer.canvasToSquare(pos.x, pos.y);
        
        if (!sq) return;

        // If clicking on a target while something is selected
        if (this.selectedSquare && !this._isOwnPiece(sq)) {
            this._initiateMove(this.selectedSquare, sq);
            return;
        }

        // If clicking on own piece
        if (this._isOwnPiece(sq)) {
            this._selectSquare(sq);
            this.isDragging = true;
            this.draggedSquare = sq;
            this.dragPosition = pos;
            this._updateCursor();
            this._draw();
        } else {
            // Clicked on empty or enemy piece that is not a legal move target
            this._clearSelection();
            this._draw();
        }
    }

    _handleMouseMove(e) {
        if (!this._enabled) return;
        
        const pos = this._getEventPos(e);
        const hoverSq = this.renderer.canvasToSquare(pos.x, pos.y);

        if (this.isDragging) {
            this.dragPosition = pos;
            this._updateCursor();
            this._draw();
        } else {
            // Just hovering
            this._updateCursor(hoverSq);
        }
    }

    _handleMouseUp(e) {
        if (!this._enabled || !this.isDragging) return;
        
        const pos = this._getEventPos(e);
        const dropSq = this.renderer.canvasToSquare(pos.x, pos.y);

        this.isDragging = false;
        const startSq = this.draggedSquare;
        this.draggedSquare = null;

        if (dropSq && dropSq !== startSq && !this._isOwnPiece(dropSq)) {
            // Dropped on a valid target
            this._initiateMove(startSq, dropSq);
        } else if (dropSq === startSq) {
            // Dropped on the same square (click to select behavior)
            // Leave it selected
            this._updateCursor(dropSq);
            this._draw();
        } else {
            // Dropped invalidly, snap back
            // Selection remains
            this._updateCursor(dropSq);
            this._draw();
        }
    }

    _handleTouchStart(e) {
        if (e.cancelable) e.preventDefault();
        this._handleMouseDown(e);
    }

    _handleTouchMove(e) {
        if (this.isDragging && e.cancelable) e.preventDefault();
        this._handleMouseMove(e);
    }

    _handleTouchEnd(e) {
        if (e.cancelable) e.preventDefault();
        this._handleMouseUp(e);
    }

    _selectSquare(sq) {
        if (this.selectedSquare === sq) {
            // Deselect if clicking already selected
            this._clearSelection();
            return;
        }
        this.selectedSquare = sq;
        this.renderer.setSelectedSquare(sq, this._getLegalTargets(sq));
    }

    _clearSelection() {
        this.selectedSquare = null;
        this.renderer.clearSelection();
    }

    _initiateMove(fromSq, toSq) {
        this._clearSelection();
        this.isDragging = false;
        this.draggedSquare = null;
        this._draw();

        if (this._onMoveIntent) {
            let promotion;
            const piece = this._getPieceAt(fromSq);
            
            // Check for pawn promotion
            if (piece && piece.type === 'pawn') {
                const targetRank = toSq.charAt(1);
                if ((piece.color === 'w' && targetRank === '8') || 
                    (piece.color === 'b' && targetRank === '1')) {
                    promotion = 'q'; // Default to Queen for now
                }
            }

            this._onMoveIntent(fromSq, toSq, promotion);
        }
    }

    _updateCursor(hoverSq = null) {
        if (!this._enabled) {
            this.canvas.style.cursor = 'default';
            return;
        }

        if (this.isDragging) {
            this.canvas.style.cursor = 'grabbing';
        } else if (hoverSq && this._isOwnPiece(hoverSq)) {
            this.canvas.style.cursor = 'pointer'; // or 'grab' based on preference
        } else {
            this.canvas.style.cursor = 'default';
        }
    }

    _draw() {
        if (this.isDragging && this.draggedSquare) {
            if (typeof this.renderer._drawInternal === 'function') {
                this.renderer._drawInternal(this.draggedSquare);
                this._drawDraggedPiece();
            } else {
                // Fallback if _drawInternal is not implemented as requested
                this.renderer.draw();
            }
        } else {
            this.renderer.draw();
        }
    }

    _drawDraggedPiece() {
        const piece = this._getPieceAt(this.draggedSquare);
        if (!piece || !this.renderer.ctx) return;
        
        // Use the renderer's piece image key convention: "w_king", "b_pawn", etc.
        const pieceKey = `${piece.color}_${piece.type}`;
        const img = this.renderer.pieceImages && this.renderer.pieceImages[pieceKey];
        
        if (img) {
            const sqSize = this.renderer.squareSize;
            const scale = (this.renderer.theme && this.renderer.theme.pieceScale) || 0.85;
            const pieceSize = sqSize * scale;
            const ctx = this.renderer.ctx;
            ctx.save();
            ctx.shadowColor = 'rgba(0, 0, 0, 0.5)';
            ctx.shadowBlur = 10;
            ctx.shadowOffsetX = 5;
            ctx.shadowOffsetY = 5;
            ctx.drawImage(
                img,
                this.dragPosition.x - pieceSize / 2,
                this.dragPosition.y - pieceSize / 2,
                pieceSize,
                pieceSize
            );
            ctx.restore();
        }
    }
}

window.BoardInteraction = BoardInteraction;
