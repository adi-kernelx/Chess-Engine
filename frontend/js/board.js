/**
 * Main Chess Board Renderer
 * 
 * This file contains the BoardRenderer class responsible for rendering a chess board
 * onto an HTML5 Canvas. It handles piece rendering, highlights, animations, and coordinate
 * transformations. It does not handle user input directly.
 * 
 * Architecture:
 * - The board state is stored as a 64-element array mapping to a1=0 ... h8=63.
 * - Drawing is done back-to-front (squares, highlights, dots, pieces, coordinates).
 * - Pieces are loaded from an SVG sprite sheet, rasterized dynamically into Images for performance.
 * - Device pixel ratio is handled for crisp high-DPI rendering.
 */

class BoardRenderer {
    /**
     * @param {HTMLCanvasElement} canvas - The canvas element to draw on
     * @param {Object} theme - Visual configuration (BoardTheme instance)
     */
    constructor(canvas, theme = window.BoardTheme ? window.BoardTheme.classic() : null) {
        this.canvas = canvas;
        this.ctx = canvas.getContext('2d');
        this.theme = theme;
        
        // Read dimensions from the canvas (DPI scaling is handled externally by init code)
        this.dpr = window.devicePixelRatio || 1;
        this.logicalWidth = canvas.width / this.dpr;
        this.logicalHeight = canvas.height / this.dpr;
        
        this.squareSize = this.logicalWidth / 8;
        
        // Internal state
        this.board = new Array(64).fill(null);
        this.whiteOnBottom = true;
        this.pieceImages = {};
        
        // Visual state
        this.lastMove = { from: null, to: null };
        this.selectedSquare = null;
        this.legalMoves = [];
        this.checkSquare = null;
        
        // Animation state
        this.animating = false;
        this.animPiece = null;
        this.animFrom = null;
        this.animTo = null;
        this.animProgress = 0; // 0 to 1
    }

    // ── Helper Methods ──

    _algebraicToIndex(sq) {
        if (!sq || sq.length !== 2) return null;
        const file = sq.charCodeAt(0) - 'a'.charCodeAt(0);
        const rank = parseInt(sq[1]) - 1;
        if (file < 0 || file > 7 || rank < 0 || rank > 7) return null;
        return rank * 8 + file;
    }

    _indexToAlgebraic(index) {
        if (index < 0 || index > 63) return null;
        const file = index % 8;
        const rank = Math.floor(index / 8);
        return String.fromCharCode('a'.charCodeAt(0) + file) + (rank + 1);
    }

    _getColRow(index) {
        const file = index % 8;
        const rank = Math.floor(index / 8);
        
        let col, row;
        if (this.whiteOnBottom) {
            col = file;
            row = 7 - rank;
        } else {
            col = 7 - file;
            row = rank;
        }
        return { col, row };
    }

    _getXYForColRow(col, row) {
        return {
            x: col * this.squareSize,
            y: row * this.squareSize
        };
    }

    _getFenPieces(fen) {
        const placement = fen.split(' ')[0];
        const rows = placement.split('/');
        const tempBoard = new Array(64).fill(null);
        
        // FEN starts from rank 8 down to rank 1
        for (let r = 0; r < 8; r++) {
            const rankStr = rows[r];
            const rankIndex = 7 - r; // rank 8 is index 7
            let fileIndex = 0;
            
            for (let i = 0; i < rankStr.length; i++) {
                const char = rankStr[i];
                if (!isNaN(char)) {
                    fileIndex += parseInt(char);
                } else {
                    const color = (char === char.toUpperCase()) ? 'w' : 'b';
                    let type = '';
                    switch (char.toLowerCase()) {
                        case 'p': type = 'pawn'; break;
                        case 'n': type = 'knight'; break;
                        case 'b': type = 'bishop'; break;
                        case 'r': type = 'rook'; break;
                        case 'q': type = 'queen'; break;
                        case 'k': type = 'king'; break;
                    }
                    const idx = rankIndex * 8 + fileIndex;
                    tempBoard[idx] = { type, color };
                    fileIndex++;
                }
            }
        }
        return tempBoard;
    }

    // ── Core rendering ──

    /**
     * Load piece images from the SVG sprite sheet. Must be called before first draw.
     * @param {string} svgPath - Path to the SVG spritesheet
     * @returns {Promise<void>}
     */
    async loadPieces(svgPath = 'assets/pieces/pieces.svg') {
        try {
            const response = await fetch(svgPath);
            const svgText = await response.text();
            const parser = new DOMParser();
            const doc = parser.parseFromString(svgText, 'image/svg+xml');
            const symbols = doc.querySelectorAll('symbol');
            
            const promises = Array.from(symbols).map(symbol => {
                return new Promise((resolve) => {
                    const id = symbol.getAttribute('id');
                    const viewBox = symbol.getAttribute('viewBox') || '0 0 45 45';
                    const content = symbol.innerHTML;
                    
                    const svgString = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="${viewBox}" width="${this.squareSize}" height="${this.squareSize}">${content}</svg>`;
                    const blob = new Blob([svgString], { type: 'image/svg+xml;charset=utf-8' });
                    const url = URL.createObjectURL(blob);
                    
                    const img = new Image();
                    img.onload = () => {
                        this.pieceImages[id] = img;
                        URL.revokeObjectURL(url);
                        resolve();
                    };
                    img.src = url;
                });
            });
            
            await Promise.all(promises);
            this.draw();
        } catch (error) {
            console.error('Failed to load piece SVGs:', error);
        }
    }

    /**
     * Set the board position from a FEN string and redraw
     * @param {string} fen - FEN notation string
     */
    setPosition(fen) {
        this.board = this._getFenPieces(fen);
        if (!this.animating) {
            this.draw();
        }
    }

    /**
     * Full redraw of the board in its current state
     */
    draw() {
        if (this.animating) return; // Handled by animation loop
        this._drawInternal();
    }

    _drawInternal(animatingPieceSq = null) {
        this.ctx.clearRect(0, 0, this.logicalWidth, this.logicalHeight);

        // a. Draw squares
        for (let i = 0; i < 64; i++) {
            const { col, row } = this._getColRow(i);
            const isLight = (col + row) % 2 === 0;
            this.ctx.fillStyle = isLight ? this.theme.lightSquare : this.theme.darkSquare;
            this.ctx.fillRect(col * this.squareSize, row * this.squareSize, this.squareSize, this.squareSize);
        }

        // b. Draw highlights
        if (this.lastMove.from) {
            const fromIdx = this._algebraicToIndex(this.lastMove.from);
            if (fromIdx !== null) this._drawHighlight(fromIdx, this.theme.lastMoveHighlight);
        }
        if (this.lastMove.to) {
            const toIdx = this._algebraicToIndex(this.lastMove.to);
            if (toIdx !== null) this._drawHighlight(toIdx, this.theme.lastMoveHighlight);
        }
        if (this.selectedSquare) {
            const selIdx = this._algebraicToIndex(this.selectedSquare);
            if (selIdx !== null) this._drawHighlight(selIdx, this.theme.selectedHighlight);
        }
        if (this.checkSquare) {
            const checkIdx = this._algebraicToIndex(this.checkSquare);
            if (checkIdx !== null) {
                const { col, row } = this._getColRow(checkIdx);
                const x = col * this.squareSize;
                const y = row * this.squareSize;
                const cx = x + this.squareSize / 2;
                const cy = y + this.squareSize / 2;
                
                const gradient = this.ctx.createRadialGradient(cx, cy, 0, cx, cy, this.squareSize / 2);
                gradient.addColorStop(0, 'rgba(255, 0, 0, 0.8)');
                gradient.addColorStop(1, 'rgba(255, 0, 0, 0)');
                
                this.ctx.fillStyle = gradient;
                this.ctx.fillRect(x, y, this.squareSize, this.squareSize);
            }
        }

        // c. Draw legal move indicators
        for (const sq of this.legalMoves) {
            const idx = this._algebraicToIndex(sq);
            if (idx === null) continue;
            
            const { col, row } = this._getColRow(idx);
            const x = col * this.squareSize;
            const y = row * this.squareSize;
            const cx = x + this.squareSize / 2;
            const cy = y + this.squareSize / 2;
            
            const piece = this.board[idx];
            
            this.ctx.fillStyle = this.theme.legalMoveDot;
            this.ctx.strokeStyle = this.theme.legalMoveDot;
            this.ctx.beginPath();
            
            if (piece) {
                // Capture ring
                this.ctx.lineWidth = this.squareSize * 0.1;
                this.ctx.arc(cx, cy, this.squareSize * 0.4, 0, 2 * Math.PI);
                this.ctx.stroke();
            } else {
                // Empty square dot
                this.ctx.arc(cx, cy, this.squareSize * 0.15, 0, 2 * Math.PI);
                this.ctx.fill();
            }
        }

        // d. Draw pieces
        for (let i = 0; i < 64; i++) {
            if (animatingPieceSq && this._indexToAlgebraic(i) === animatingPieceSq) {
                continue; // Skip the piece being animated
            }
            
            const piece = this.board[i];
            if (!piece) continue;
            
            const { col, row } = this._getColRow(i);
            const x = col * this.squareSize;
            const y = row * this.squareSize;
            
            const key = `${piece.color}_${piece.type}`;
            const img = this.pieceImages[key];
            if (img) {
                const scale = (this.theme && this.theme.pieceScale) || 0.85;
                const pieceSize = this.squareSize * scale;
                const offset = (this.squareSize - pieceSize) / 2;
                this.ctx.drawImage(img, x + offset, y + offset, pieceSize, pieceSize);
            }
        }

        // e. Draw coordinates (respects theme.showCoords)
        if (!this.theme || !this.theme.showCoords) return;
        
        const coordFontSize = Math.max(10, this.squareSize * 0.17);
        this.ctx.font = this.theme.coordFont 
            ? this.theme.coordFont.replace(/\d+px/, coordFontSize + 'px')
            : `bold ${coordFontSize}px sans-serif`;
        this.ctx.textBaseline = 'top';

        for (let i = 0; i < 64; i++) {
            const { col, row } = this._getColRow(i);
            const isLight = (col + row) % 2 === 0;
            this.ctx.fillStyle = isLight ? this.theme.darkSquare : this.theme.lightSquare;
            
            const alg = this._indexToAlgebraic(i);
            const file = alg[0];
            const rank = alg[1];

            // Rank labels on the left-most visible file (col 0)
            if (col === 0) {
                this.ctx.textAlign = 'left';
                this.ctx.fillText(rank, 2, row * this.squareSize + 2);
            }
            
            // File labels on the bottom-most visible rank (row 7)
            if (row === 7) {
                this.ctx.textAlign = 'right';
                this.ctx.textBaseline = 'bottom';
                this.ctx.fillText(file, (col + 1) * this.squareSize - 2, (row + 1) * this.squareSize - 2);
                this.ctx.textBaseline = 'top'; // reset
            }
        }
    }

    _drawHighlight(index, color) {
        const { col, row } = this._getColRow(index);
        this.ctx.fillStyle = color;
        this.ctx.fillRect(col * this.squareSize, row * this.squareSize, this.squareSize, this.squareSize);
    }

    // ── Visual state ──

    /**
     * Highlight the last move (from and to squares)
     * @param {string} fromSq - Algebraic string like 'e2'
     * @param {string} toSq - Algebraic string like 'e4'
     */
    setLastMove(fromSq, toSq) {
        this.lastMove = { from: fromSq, to: toSq };
        if (!this.animating) this.draw();
    }

    /** Clear last move highlight */
    clearLastMove() {
        this.lastMove = { from: null, to: null };
        if (!this.animating) this.draw();
    }

    /**
     * Set which square is selected, show legal move dots
     * @param {string} sq - Algebraic string
     * @param {string[]} legalMoves - Array of algebraic strings
     */
    setSelectedSquare(sq, legalMoves = []) {
        this.selectedSquare = sq;
        this.legalMoves = legalMoves;
        if (!this.animating) this.draw();
    }

    /** Clear selection and legal move dots */
    clearSelection() {
        this.selectedSquare = null;
        this.legalMoves = [];
        if (!this.animating) this.draw();
    }

    /**
     * Highlight the king square in check
     * @param {string} kingSq - Algebraic string
     */
    setCheck(kingSq) {
        this.checkSquare = kingSq;
        if (!this.animating) this.draw();
    }

    /** Clear check highlight */
    clearCheck() {
        this.checkSquare = null;
        if (!this.animating) this.draw();
    }

    // ── Board orientation ──

    /** Flip the board (toggle between white/black perspective) */
    flip() {
        this.whiteOnBottom = !this.whiteOnBottom;
        if (!this.animating) this.draw();
    }

    /**
     * Set orientation explicitly
     * @param {boolean} whiteOnBottom - true = white on bottom, false = black on bottom
     */
    setOrientation(whiteOnBottom) {
        this.whiteOnBottom = whiteOnBottom;
        if (!this.animating) this.draw();
    }

    /** Get current orientation */
    isWhiteOnBottom() {
        return this.whiteOnBottom;
    }

    // ── Coordinate conversion ──

    /**
     * Convert canvas pixel coordinates to algebraic square string
     * @param {number} canvasX - X pixel coordinate
     * @param {number} canvasY - Y pixel coordinate
     * @returns {string|null} Algebraic square string, or null if outside
     */
    canvasToSquare(canvasX, canvasY) {
        // Adjust if css scale is different, but assuming logical size maps 1:1 to event offsets
        if (canvasX < 0 || canvasX >= this.logicalWidth || canvasY < 0 || canvasY >= this.logicalHeight) {
            return null;
        }
        
        const col = Math.floor(canvasX / this.squareSize);
        const row = Math.floor(canvasY / this.squareSize);
        
        let file, rank;
        if (this.whiteOnBottom) {
            file = col;
            rank = 7 - row;
        } else {
            file = 7 - col;
            rank = row;
        }
        
        const index = rank * 8 + file;
        return this._indexToAlgebraic(index);
    }

    /**
     * Convert algebraic square to canvas pixel coordinates (center of the square)
     * @param {string} algebraicSq - Algebraic string
     * @returns {Object|null} {x, y} coordinates of the square's center
     */
    squareToCanvas(algebraicSq) {
        const index = this._algebraicToIndex(algebraicSq);
        if (index === null) return null;
        
        const { col, row } = this._getColRow(index);
        return {
            x: (col + 0.5) * this.squareSize,
            y: (row + 0.5) * this.squareSize
        };
    }

    // ── Animation ──

    /**
     * Animate a piece moving from one square to another
     * @param {string} fromSq - Starting square
     * @param {string} toSq - Ending square
     * @param {Function} callback - Called when animation completes
     */
    animateMove(fromSq, toSq, callback) {
        const fromIdx = this._algebraicToIndex(fromSq);
        const toIdx = this._algebraicToIndex(toSq);
        
        if (fromIdx === null || toIdx === null) {
            if (callback) callback();
            return;
        }
        
        // Find the piece at the target square (since the board state might already be updated)
        // If not, use piece from source square.
        let piece = this.board[toIdx] || this.board[fromIdx];
        if (!piece) {
            if (callback) callback();
            return;
        }

        this.animating = true;
        this.animPiece = piece;
        this.animFrom = fromSq;
        this.animTo = toSq;
        this.animProgress = 0;
        
        const duration = (this.theme && this.theme.animationDuration) || 150;
        const startTime = performance.now();
        
        const { col: fCol, row: fRow } = this._getColRow(fromIdx);
        const { col: tCol, row: tRow } = this._getColRow(toIdx);
        
        const startX = fCol * this.squareSize;
        const startY = fRow * this.squareSize;
        const endX = tCol * this.squareSize;
        const endY = tRow * this.squareSize;

        const animate = (currentTime) => {
            const elapsed = currentTime - startTime;
            let progress = elapsed / duration;
            
            if (progress >= 1) {
                this.animating = false;
                this.draw();
                if (callback) callback();
                return;
            }
            
            // Ease-out cubic
            const easeProgress = 1 - Math.pow(1 - progress, 3);
            
            const currentX = startX + (endX - startX) * easeProgress;
            const currentY = startY + (endY - startY) * easeProgress;
            
            // Draw background and other pieces (skipping the piece at its new position)
            this._drawInternal(this.animTo);
            
            // Draw animating piece (with theme-aware scaling)
            const key = `${this.animPiece.color}_${this.animPiece.type}`;
            const img = this.pieceImages[key];
            if (img) {
                const scale = (this.theme && this.theme.pieceScale) || 0.85;
                const pieceSize = this.squareSize * scale;
                const offset = (this.squareSize - pieceSize) / 2;
                this.ctx.drawImage(img, currentX + offset, currentY + offset, pieceSize, pieceSize);
            }
            
            requestAnimationFrame(animate);
        };
        
        requestAnimationFrame(animate);
    }

    // ── Theme ──

    /**
     * Change the board theme and redraw
     * @param {Object} theme - BoardTheme instance
     */
    setTheme(theme) {
        this.theme = theme;
        if (!this.animating) this.draw();
    }

    /**
     * Get the current theme
     * @returns {Object} The current theme
     */
    getTheme() {
        return this.theme;
    }
}

window.BoardRenderer = BoardRenderer;
