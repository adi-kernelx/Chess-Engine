/**
 * Manages the WebSocket connection to the chess server with automatic reconnection.
 */
class ChessWebSocket {
    /**
     * @param {string} url - The WebSocket server URL
     */
    constructor(url = 'ws://localhost:9000') {
        this.url = url;
        this.ws = null;
        this.connected = false;
        this.reconnectAttempts = 0;
        this.maxReconnectAttempts = 10;
        this.baseReconnectDelay = 1000;
        this.maxReconnectDelay = 30000;
        this.handlers = {};
        this.onConnectionChange = null;
        this._isIntentionalDisconnect = false;
        this._messageQueue = [];
    }

    /**
     * Register a handler for a specific message type.
     * @param {string} type - The message type
     * @param {Function} callback - The callback function
     */
    on(type, callback) {
        this.handlers[type] = callback;
    }

    /**
     * Remove a handler for a specific message type.
     * @param {string} type - The message type
     */
    off(type) {
        delete this.handlers[type];
    }

    /**
     * Connect to the WebSocket server.
     */
    connect() {
        this._isIntentionalDisconnect = false;
        if (this.ws && (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING)) {
            return;
        }

        console.log(`Connecting to ${this.url}...`);
        this.ws = new WebSocket(this.url);
        this._setupListeners();
    }

    /**
     * Disconnect from the WebSocket server without auto-reconnecting.
     */
    disconnect() {
        this._isIntentionalDisconnect = true;
        if (this.ws) {
            this.ws.close();
            this.ws = null;
        }
    }

    /**
     * Send a JSON message to the server. Queues the message if not connected.
     * @param {Object} data - The data to send
     */
    send(data) {
        if (this.connected && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify(data));
        } else {
            console.warn('WebSocket not connected. Queuing message:', data);
            this._messageQueue.push(data);
        }
    }

    // ── Convenience methods ──

    /**
     * Create a new game.
     * @param {string} username - Player's username
     * @param {number} timeBase - Base time in seconds
     * @param {number} timeInc - Increment time in seconds
     */
    createGame(username, timeBase = 600, timeInc = 5) {
        this.send({ type: 'create_game', username, time_base: timeBase, time_inc: timeInc });
    }

    /**
     * Join an existing game.
     * @param {string} username - Player's username
     * @param {number|string} gameId - Game ID to join
     */
    joinGame(username, gameId) {
        this.send({ type: 'join_game', username, game_id: gameId });
    }

    /**
     * Make a move.
     * @param {string} from - Source square (e.g., 'e2')
     * @param {string} to - Destination square (e.g., 'e4')
     * @param {string|null} promotion - Promotion piece (e.g., 'q'), null if not applicable
     */
    makeMove(from, to, promotion = null) {
        const payload = { type: 'make_move', from, to };
        if (promotion) {
            payload.promotion = promotion;
        }
        this.send(payload);
    }

    /**
     * Resign from the current game.
     */
    resign() {
        this.send({ type: 'resign' });
    }

    /**
     * Join the quick play matchmaking queue.
     * @param {string} username - Player's username
     * @param {number} elo - Player's ELO rating
     * @param {number} timeBase - Preferred base time in seconds
     * @param {number} timeInc - Preferred increment time in seconds
     */
    quickPlay(username, elo = 1200, timeBase = 600, timeInc = 5) {
        this.send({ type: 'quick_play', username, elo, time_base: timeBase, time_inc: timeInc });
    }

    /**
     * Cancel the quick play matchmaking queue.
     */
    cancelQueue() {
        this.send({ type: 'cancel_queue' });
    }

    /**
     * Request a list of active games.
     */
    listGames() {
        this.send({ type: 'list_games' });
    }

    /**
     * Request the current game state.
     */
    requestGameState() {
        this.send({ type: 'game_state' });
    }

    // ── Internal ──

    /**
     * @private
     */
    _setupListeners() {
        this.ws.onopen = () => {
            console.log('WebSocket connected');
            this.connected = true;
            this.reconnectAttempts = 0;
            if (this.onConnectionChange) {
                this.onConnectionChange(true);
            }
            // Send any queued messages
            while (this._messageQueue.length > 0) {
                const data = this._messageQueue.shift();
                this.send(data);
            }
        };

        this.ws.onmessage = (event) => {
            this._handleMessage(event);
        };

        this.ws.onclose = () => {
            const wasConnected = this.connected;
            this.connected = false;
            if (wasConnected) {
                console.log('WebSocket disconnected');
            }
            if (this.onConnectionChange) {
                this.onConnectionChange(false);
            }
            
            if (!this._isIntentionalDisconnect) {
                this._scheduleReconnect();
            }
        };

        this.ws.onerror = (error) => {
            console.error('WebSocket error:', error);
        };
    }

    /**
     * @private
     * @param {MessageEvent} event
     */
    _handleMessage(event) {
        try {
            const data = JSON.parse(event.data);
            if (data && data.type) {
                const handler = this.handlers[data.type];
                if (handler) {
                    handler(data);
                } else {
                    console.log(`Unhandled message type: ${data.type}`, data);
                }
            }
        } catch (e) {
            console.error('Error parsing WebSocket message:', e);
        }
    }

    /**
     * @private
     */
    _scheduleReconnect() {
        if (this.reconnectAttempts >= this.maxReconnectAttempts) {
            console.error('Max reconnect attempts reached. Giving up.');
            return;
        }

        const delay = Math.min(this.baseReconnectDelay * Math.pow(2, this.reconnectAttempts), this.maxReconnectDelay);
        this.reconnectAttempts++;
        console.log(`Reconnecting in ${delay}ms (attempt ${this.reconnectAttempts})...`);
        setTimeout(() => this.connect(), delay);
    }
}

window.ChessWebSocket = ChessWebSocket;
