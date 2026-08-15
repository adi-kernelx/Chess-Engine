/**
 * GameController orchestrates the chess game, connecting WebSocket,
 * BoardRenderer, and BoardInteraction.
 */
class GameController {
    /**
     * @param {Object} config
     * @param {BoardRenderer} config.renderer
     * @param {BoardInteraction} config.interaction
     * @param {ChessWebSocket} config.ws
     */
    constructor(config) {
        this.renderer = config.renderer;
        this.interaction = config.interaction;
        this.ws = config.ws;
        this.router = null;  // Set externally after construction

        // Game State
        this.gameId = null;
        this.myColor = null;  // 'w' or 'b'
        this.isMyTurn = false;
        this.gameActive = false;
        this.moves = [];  // array of SAN strings
        this.whiteTimeMs = 0;
        this.blackTimeMs = 0;
        this.clockInterval = null;
        this.opponentName = 'Opponent';
    }

    /**
     * Start the controller — register all WebSocket handlers and set up interaction
     */
    init() {
        // Set up Interaction callback
        this.interaction.onMoveIntent = (from, to, promotion) => {
            this._onMoveIntent(from, to, promotion);
        };

        // Register WebSocket handlers
        this.ws.on('move_made', (data) => this._onMoveMade(data));
        this.ws.on('game_over', (data) => this._onGameOver(data));
        this.ws.on('game_created', (data) => this._onGameCreated(data));
        this.ws.on('game_joined', (data) => this._onGameStart(data));
        this.ws.on('game_start', (data) => this._onGameStart(data));
        this.ws.on('game_state', (data) => this._onGameState(data));
        this.ws.on('match_found', (data) => this._onMatchFound(data));
        this.ws.on('queued', (data) => this._onQueued(data));
        this.ws.on('queue_cancelled', (data) => this._onQueueCancelled(data));
        this.ws.on('move_rejected', (data) => this._onMoveRejected(data));
        this.ws.on('error', (data) => this._onError(data));
        
        // Cache UI element references
        this.moveListEl = document.getElementById('move-list');
        this.moveCountEl = document.getElementById('move-count');
        this.statusBarEl = document.getElementById('game-status');
        this.playerClockEl = document.getElementById('player-clock');
        this.opponentClockEl = document.getElementById('opponent-clock');
        this.playerNameEl = document.getElementById('player-name');
        this.opponentNameEl = document.getElementById('opponent-name');
        this.resignBtn = document.getElementById('btn-resign');
        
        if (this.playerNameEl) {
            this.playerNameEl.textContent = 'You';
        }
    }

    /**
     * Create a new game
     * @param {string} username 
     * @param {number} timeBase 
     * @param {number} timeInc 
     */
    createGame(username, timeBase, timeInc) {
        this.ws.send({ type: 'create_game', username: username, time_base: timeBase, time_inc: timeInc });
        this._updateStatusBar('Creating game...', 'waiting');
    }

    /**
     * Join an existing game
     * @param {string} username 
     * @param {string} gameId 
     */
    joinGame(username, gameId) {
        this.ws.send({ type: 'join_game', username: username, game_id: gameId });
        this._updateStatusBar('Joining game...', 'waiting');
    }

    /**
     * Quick play matchmaking
     * @param {string} username 
     * @param {number} elo 
     * @param {number} timeBase 
     * @param {number} timeInc 
     */
    quickPlay(username, elo, timeBase, timeInc) {
        this.ws.send({ type: 'quick_play', username: username, elo: elo, time_base: timeBase, time_inc: timeInc });
        this._updateStatusBar('Searching for match...', 'waiting');
    }

    /**
     * Cancel matchmaking
     */
    cancelQueue() {
        this.ws.send({ type: 'cancel_queue' });
        this._updateStatusBar('Matchmaking cancelled', 'info');
    }

    /**
     * Start a game against the AI
     * @param {string} username 
     * @param {string} difficulty  'easy', 'medium', 'hard', or 'max'
     * @param {number} timeBase 
     * @param {number} timeInc 
     */
    playAI(username, difficulty, timeBase, timeInc) {
        this.ws.send({ type: 'play_ai', username: username, difficulty: difficulty, time_base: timeBase, time_inc: timeInc });
        this._updateStatusBar('Starting AI game...', 'waiting');
    }

    /**
     * Resign current game
     */
    resign() {
        if (this.gameActive && this.gameId) {
            this.ws.send({ type: 'resign' });
        }
    }

    // ── Internal ──

    /**
     * Called by BoardInteraction when user intends a move
     * @param {string} from 
     * @param {string} to 
     * @param {string} promotion 
     */
    _onMoveIntent(from, to, promotion) {
        if (!this.gameActive || !this.isMyTurn) {
            this.renderer.draw();
            return;
        }
        
        // Send move to server — do NOT update board yet. Wait for server confirmation.
        const msg = { type: 'make_move', from: from, to: to };
        if (promotion) msg.promotion = promotion;
        this.ws.send(msg);
    }

    /**
     * Handle server's move_made message
     * @param {Object} data 
     */
    _onMoveMade(data) {
        this.whiteTimeMs = data.white_time;
        this.blackTimeMs = data.black_time;
        
        if (data.san) {
            this.moves.push(data.san);
        }
        
        // Animate piece movement, then sync full state from server
        this.renderer.animateMove(data.from, data.to, () => {
            // Request full game state from server to get authoritative FEN
            this.ws.send({ type: 'game_state' });
            
            // Toggle turn
            this.isMyTurn = !this.isMyTurn;
            
            this.renderer.setLastMove(data.from, data.to);
            
            // Check detection from SAN
            if (data.san && (data.san.includes('+') || data.san.includes('#'))) {
                // We'll set the check highlight when game_state arrives with the FEN
            } else {
                this.renderer.clearCheck();
            }
            
            this._updateMoveList();
            this._updateClockDisplay();
            
            if (this.isMyTurn) {
                this._updateStatusBar('Your turn', 'turn');
            } else {
                this._updateStatusBar('Opponent\'s turn', 'info');
            }
            
            this.interaction.enabled = this.isMyTurn;
            this._startClock();
        });
    }

    /**
     * Handle server's game_over message
     * @param {Object} data 
     */
    _onGameOver(data) {
        this.gameActive = false;
        this.isMyTurn = false;
        this.interaction.enabled = false;
        this._stopClock();
        
        if (this.resignBtn) this.resignBtn.disabled = true;
        
        let resultText = data.result === '1-0' ? 'White wins' : 
                         data.result === '0-1' ? 'Black wins' : 'Draw';
        
        this._updateStatusBar(`Game Over — ${resultText}`, 'gameover');
        this._showGameOverModal(resultText, data.reason);
    }

    _onGameCreated(data) {
        this.gameId = data.game_id;
        this.myColor = data.color;

        // Switch to game page so user sees the "waiting" state
        if (this.router) {
            this.router.showGame();
            this._resizeCanvas();
        }

        this._updateStatusBar(`Game #${this.gameId} created. Waiting for opponent...`, 'waiting');
    }

    /**
     * Handle server's game_start or game_joined message
     * @param {Object} data 
     */
    _onGameStart(data) {
        this.gameId = data.game_id;
        this.myColor = data.color === 'white' ? 'w' : (data.color === 'black' ? 'b' : data.color);
        this.opponentName = data.opponent || 'Opponent';
        this.gameActive = true;
        this.moves = [];
        this.whiteTimeMs = data.white_time;
        this.blackTimeMs = data.black_time;
        
        if (this.opponentNameEl) {
            this.opponentNameEl.textContent = this.opponentName;
        }

        // Switch to game page
        if (this.router) {
            this.router.showGame();
            this._resizeCanvas();
        }

        // Set board orientation: white at bottom if we're white
        this.renderer.setOrientation(this.myColor === 'w');
        
        // Set player color for interaction handler
        this.interaction.playerColor = this.myColor;
        
        // White moves first
        this.isMyTurn = (this.myColor === 'w');
        this.interaction.enabled = this.isMyTurn;
        
        if (this.resignBtn) this.resignBtn.disabled = false;
        
        // Fetch initial game state to get FEN
        this.ws.send({ type: 'game_state' });
        
        this._hideGameOverModal();
        this._updateMoveList();
        this._updateClockDisplay();
        this._updateStatusBar(this.isMyTurn ? 'Your turn — game on!' : 'Opponent\'s turn', this.isMyTurn ? 'turn' : 'info');
        this._startClock();
    }

    /**
     * Handle game_state message
     * @param {Object} data 
     */
    _onGameState(data) {
        if (data.moves) {
            this.moves = data.moves.map(m => m.san);
        }
        
        this.whiteTimeMs = data.white_time;
        this.blackTimeMs = data.black_time;
        
        // Update board position from authoritative FEN
        if (data.fen) {
            this.renderer.setPosition(data.fen);
        }
        
        // Determine whose turn from FEN
        const parts = (data.fen || '').split(' ');
        const sideToMove = parts.length > 1 ? parts[1] : 'w';
        this.isMyTurn = (sideToMove === this.myColor);
        
        this.interaction.enabled = this.isMyTurn;
        
        // Check detection from last SAN
        if (this.moves.length > 0) {
            const lastSan = this.moves[this.moves.length - 1];
            if (lastSan.includes('+') || lastSan.includes('#')) {
                const kingSq = this._findKing(data.fen, sideToMove);
                if (kingSq) {
                    this.renderer.setCheck(kingSq);
                    if (this.isMyTurn) {
                        this._updateStatusBar('Check! Your turn', 'check');
                    }
                }
            } else {
                this.renderer.clearCheck();
            }
        } else {
            this.renderer.clearCheck();
        }
        
        this._updateMoveList();
        this._updateClockDisplay();
        
        if (data.state === 'finished') {
            this._onGameOver({ result: data.result, reason: data.reason });
        }
    }

    /**
     * Handle server's match_found message
     * @param {Object} data 
     */
    _onMatchFound(data) {
        // match_found has the same payload structure as game_start
        this._onGameStart(data);
        this._updateStatusBar('Match found! Game started.', 'turn');
    }
    
    _onQueued(data) {
        this._updateStatusBar(`In queue... (${data.queue_size} players)`, 'info');
    }
    
    _onQueueCancelled() {
        this._updateStatusBar('Queue cancelled.', 'info');
    }
    
    _onMoveRejected(data) {
        this._updateStatusBar(`Move rejected: ${data.error}`, 'check');
        // Re-fetch game state to sync board
        this.ws.send({ type: 'game_state' });
    }
    
    _onError(data) {
        this._updateStatusBar(`Error: ${data.message}`, 'info');
    }

    // ── Canvas Resize (needed after page switch) ──

    /**
     * Re-measure and resize the canvas after the game page becomes visible.
     * The canvas has zero dimensions while the page is display:none.
     */
    _resizeCanvas() {
        // Use requestAnimationFrame to ensure the page is actually rendered
        requestAnimationFrame(() => {
            const container = document.getElementById('board-container');
            const canvas = this.renderer.canvas;
            if (!container || !canvas) return;

            const rect = container.getBoundingClientRect();
            const size = Math.floor(Math.min(rect.width, rect.height));
            if (size <= 0) return;

            const dpr = window.devicePixelRatio || 1;
            canvas.style.width = size + 'px';
            canvas.style.height = size + 'px';
            canvas.width = size * dpr;
            canvas.height = size * dpr;
            const ctx = canvas.getContext('2d');
            ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

            this.renderer.logicalWidth = size;
            this.renderer.logicalHeight = size;
            this.renderer.squareSize = size / 8;
            this.renderer.dpr = dpr;

            // Re-load pieces at the new size and redraw
            this.renderer.loadPieces('assets/pieces/pieces.svg').then(() => {
                this.renderer.draw();
            }).catch(() => {
                this.renderer.draw();
            });
        });
    }

    // ── Clock Management ──

    _startClock() {
        this._stopClock();
        this._tickClock();
        this.clockInterval = setInterval(() => this._tickClock(), 100);
    }

    _stopClock() {
        if (this.clockInterval) {
            clearInterval(this.clockInterval);
            this.clockInterval = null;
        }
    }

    /**
     * Called every 100ms, updates the active player's time display
     */
    _tickClock() {
        if (!this.gameActive) return;
        
        const isWhiteTurn = (this.moves.length % 2 === 0);
        
        if (isWhiteTurn) {
            this.whiteTimeMs = Math.max(0, this.whiteTimeMs - 100);
        } else {
            this.blackTimeMs = Math.max(0, this.blackTimeMs - 100);
        }
        
        this._updateClockDisplay();
        
        if (this.whiteTimeMs === 0 || this.blackTimeMs === 0) {
            this._stopClock();
        }
    }

    // ── UI Updates ──

    /**
     * Update the move list panel DOM
     */
    _updateMoveList() {
        if (!this.moveListEl) return;
        
        if (this.moves.length === 0) {
            this.moveListEl.innerHTML = '<div class="move-list-empty">No moves yet</div>';
            if (this.moveCountEl) this.moveCountEl.textContent = '0';
            return;
        }
        
        this.moveListEl.innerHTML = '';
        const totalMoves = this.moves.length;
        const lastIdx = totalMoves - 1;
        
        for (let i = 0; i < totalMoves; i += 2) {
            const moveNum = Math.floor(i / 2) + 1;
            
            const row = document.createElement('div');
            row.className = 'move-row';
            
            const numSpan = document.createElement('span');
            numSpan.className = 'move-number';
            numSpan.textContent = `${moveNum}.`;
            row.appendChild(numSpan);
            
            // White's move
            const wSpan = document.createElement('span');
            wSpan.className = 'move-san white-move' + (i === lastIdx ? ' last-move' : '');
            wSpan.textContent = this.moves[i];
            row.appendChild(wSpan);
            
            // Black's move (if exists)
            if (this.moves[i + 1]) {
                const bSpan = document.createElement('span');
                bSpan.className = 'move-san black-move' + (i + 1 === lastIdx ? ' last-move' : '');
                bSpan.textContent = this.moves[i + 1];
                row.appendChild(bSpan);
            }
            
            this.moveListEl.appendChild(row);
        }
        
        if (this.moveCountEl) this.moveCountEl.textContent = String(totalMoves);
        this.moveListEl.scrollTop = this.moveListEl.scrollHeight;
    }

    /**
     * Format milliseconds into MM:SS
     * @param {number} ms 
     * @returns {string}
     */
    _formatTime(ms) {
        const totalSeconds = Math.ceil(ms / 1000);
        const minutes = Math.floor(totalSeconds / 60);
        const seconds = totalSeconds % 60;
        return `${minutes}:${seconds.toString().padStart(2, '0')}`;
    }

    /**
     * Update clock DOM elements
     */
    _updateClockDisplay() {
        if (!this.playerClockEl || !this.opponentClockEl) return;
        
        const myTimeMs = this.myColor === 'w' ? this.whiteTimeMs : this.blackTimeMs;
        const oppTimeMs = this.myColor === 'w' ? this.blackTimeMs : this.whiteTimeMs;
        
        this.playerClockEl.textContent = this._formatTime(myTimeMs);
        this.opponentClockEl.textContent = this._formatTime(oppTimeMs);
        
        if (myTimeMs < 30000) {
            this.playerClockEl.classList.add('low-time');
            this.playerClockEl.style.color = '#ff4444'; // Red
        } else {
            this.playerClockEl.classList.remove('low-time');
            this.playerClockEl.style.color = '';
        }
        
        if (oppTimeMs < 30000) {
            this.opponentClockEl.classList.add('low-time');
            this.opponentClockEl.style.color = '#ff4444'; // Red
        } else {
            this.opponentClockEl.classList.remove('low-time');
            this.opponentClockEl.style.color = '';
        }
    }

    /**
     * Update status bar
     * @param {string} text 
     * @param {string} type - 'info', 'check', 'turn', 'gameover'
     */
    _updateStatusBar(text, type) {
        if (!this.statusBarEl) return;
        
        this.statusBarEl.textContent = text;
        // Remove all type classes, then add the correct one
        this.statusBarEl.className = 'game-status ' + (type || 'info');
    }

    /**
     * Show game over modal
     * @param {string} result 
     * @param {string} reason 
     */
    _showGameOverModal(result, reason) {
        // Use the HTML modal that's already in the DOM
        const modal = document.getElementById('game-over-modal');
        if (!modal) return;
        
        // Set icon based on result
        const iconEl = document.getElementById('modal-icon');
        const titleEl = document.getElementById('modal-title');
        const subtitleEl = document.getElementById('modal-subtitle');
        
        if (iconEl) {
            if (result.includes('Draw')) iconEl.textContent = '🤝';
            else if (result.includes('White')) iconEl.textContent = '♔';
            else iconEl.textContent = '♚';
        }
        if (titleEl) titleEl.textContent = result;
        if (subtitleEl) {
            // Pretty-format the reason
            const reasonMap = {
                'checkmate': 'by checkmate',
                'resignation': 'by resignation',
                'timeout': 'on time',
                'stalemate': 'by stalemate',
                'fifty_move_rule': 'by fifty-move rule',
                'insufficient_material': 'by insufficient material',
                'threefold_repetition': 'by threefold repetition',
                'draw_agreement': 'by agreement'
            };
            subtitleEl.textContent = reasonMap[reason] || reason || '';
        }
        
        modal.classList.add('visible');
    }

    /**
     * Hide game over modal
     */
    _hideGameOverModal() {
        const modal = document.getElementById('game-over-modal');
        if (modal) {
            modal.classList.remove('visible');
        }
    }

    /**
     * Find king of specific color from FEN
     * @param {string} fen 
     * @param {string} color 'w' or 'b'
     * @returns {string|null} algebraic square e.g. 'e1'
     */
    _findKing(fen, color) {
        if (!fen) return null;
        
        const boardPart = fen.split(' ')[0];
        const rows = boardPart.split('/');
        const kingChar = color === 'w' ? 'K' : 'k';
        
        for (let r = 0; r < 8; r++) {
            let col = 0;
            for (let c = 0; c < rows[r].length; c++) {
                const char = rows[r][c];
                if (!isNaN(char)) {
                    col += parseInt(char, 10);
                } else {
                    if (char === kingChar) {
                        const file = String.fromCharCode('a'.charCodeAt(0) + col);
                        const rank = 8 - r;
                        return `${file}${rank}`;
                    }
                    col++;
                }
            }
        }
        return null;
    }
}

window.GameController = GameController;
