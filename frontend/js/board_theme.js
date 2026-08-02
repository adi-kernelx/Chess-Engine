/**
 * BoardTheme Configuration System
 * 
 * V2 EXTENSIBILITY PLAN:
 * This class is designed as the single source of truth for all board rendering visuals.
 * In v2, the application will introduce user-selectable themes (e.g., 'wood', 'marble', 
 * 'glass') and various piece sets (e.g., 'cburnett', 'merida', 'alpha').
 * 
 * By encapsulating all color, highlight, and asset configuration here, we can simply 
 * swap out the BoardTheme instance on the board renderer to completely reskin the game 
 * without changing the underlying rendering logic. Future enhancements may include 
 * asset preloading, image based board textures, and more robust piece set definitions.
 */

/**
 * BoardTheme — Encapsulates all visual configuration for a chessboard.
 * 
 * In v2, users will be able to choose from multiple themes (wood, marble,
 * tournament green, etc.) and multiple piece sets. This class is designed
 * to be the single point of configuration for all rendering decisions.
 */
class BoardTheme {
    /**
     * Create a new BoardTheme.
     * @param {Object} options - Configuration options to override the defaults.
     */
    constructor(options = {}) {
        /** 
         * Color of the light squares. 
         * @type {string} 
         */
        this.lightSquare = options.lightSquare || '#f0d9b5';
        
        /** 
         * Color of the dark squares. 
         * @type {string} 
         */
        this.darkSquare  = options.darkSquare  || '#b58863';
        
        /** 
         * Highlight color for the origin and destination squares of the last move (RGBA). 
         * @type {string} 
         */
        this.lastMoveHighlight  = options.lastMoveHighlight  || 'rgba(255, 255, 50, 0.4)';
        
        /** 
         * Highlight color for the currently selected square (RGBA). 
         * @type {string} 
         */
        this.selectedHighlight  = options.selectedHighlight  || 'rgba(20, 85, 30, 0.5)';
        
        /** 
         * Color of the dot indicating a legal non-capture move (RGBA). 
         * @type {string} 
         */
        this.legalMoveDot       = options.legalMoveDot       || 'rgba(0, 0, 0, 0.25)';
        
        /** 
         * Color of the indicator (often a ring) for a legal capture move (RGBA). 
         * @type {string} 
         */
        this.legalMoveCapture   = options.legalMoveCapture   || 'rgba(0, 0, 0, 0.25)';
        
        /** 
         * Highlight color for the king's square when in check (RGBA). 
         * @type {string} 
         */
        this.checkHighlight     = options.checkHighlight     || 'rgba(255, 0, 0, 0.6)';
        
        /** 
         * Highlight color for a pre-move (future v2 feature) (RGBA). 
         * @type {string} 
         */
        this.preMoveDot         = options.preMoveDot         || 'rgba(0, 100, 200, 0.4)';
        
        /** 
         * Color of the coordinate labels (files a-h, ranks 1-8). 
         * @type {string} 
         */
        this.coordColor     = options.coordColor     || '#888888';
        
        /** 
         * Font specification for the coordinate labels. 
         * @type {string} 
         */
        this.coordFont      = options.coordFont      || 'bold 12px "Inter", sans-serif';
        
        /** 
         * Whether to render coordinate labels. 
         * @type {boolean} 
         */
        this.showCoords     = options.showCoords !== undefined ? options.showCoords : true;
        
        /** 
         * The piece set identifier to use (for v2 extensibility: 'cburnett', 'merida', etc.). 
         * @type {string} 
         */
        this.pieceSet       = options.pieceSet       || 'default';
        
        /** 
         * The scale of the pieces relative to the square size (e.g., 0.85 means 85%). 
         * @type {number} 
         */
        this.pieceScale     = options.pieceScale     || 0.85;
        
        /** 
         * The color of the board border. 
         * @type {string} 
         */
        this.borderColor    = options.borderColor    || '#2a2a3e';
        
        /** 
         * The width of the board border in pixels. 
         * @type {number} 
         */
        this.borderWidth    = options.borderWidth    || 2;
        
        /** 
         * The duration for piece animations in milliseconds. 
         * @type {number} 
         */
        this.animationDuration = options.animationDuration || 150;
    }
    
    /**
     * Factory method for the default classic theme.
     * @returns {BoardTheme} A new BoardTheme instance with classic styling.
     */
    static classic() { 
        return new BoardTheme(); 
    }
    
    /**
     * Factory method for a dark-styled theme.
     * @returns {BoardTheme} A new BoardTheme instance with dark styling.
     */
    static dark() {
        return new BoardTheme({
            lightSquare: '#4a4a5e',
            darkSquare: '#2d2d3f',
            coordColor: '#7a7a8e',
            borderColor: '#1a1a2e'
        });
    }
    
    /**
     * Factory method for a tournament-style green and white theme.
     * @returns {BoardTheme} A new BoardTheme instance with tournament styling.
     */
    static tournament() {
        return new BoardTheme({
            lightSquare: '#eeeed2',
            darkSquare: '#769656',
        });
    }
}

// Export the class to the global window object
window.BoardTheme = BoardTheme;
