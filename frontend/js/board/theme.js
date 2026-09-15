/**
 * theme.js — Board visual presets.
 *
 * Every field is HONOURED by renderer.js (the old BoardTheme had 7 of 16
 * fields defined but ignored). Colors, animation timing, and coord
 * behaviour all live here so a preset swap fully reskins the board.
 */

export class BoardTheme {
    constructor(o = {}) {
        // Squares
        this.lightSquare       = o.lightSquare       || '#eeead2';
        this.darkSquare        = o.darkSquare        || '#8b7355';

        // Highlights
        this.lastMoveHighlight = o.lastMoveHighlight || 'rgba(255, 218, 87, 0.35)';
        this.selectedHighlight = o.selectedHighlight || 'rgba(96, 165, 255, 0.45)';
        this.checkHighlight    = o.checkHighlight    || 'rgba(224, 85, 85, 0.85)';
        this.preMoveHighlight  = o.preMoveHighlight  || 'rgba(180, 96, 255, 0.35)';

        // Legal-move markers
        this.legalMoveDot      = o.legalMoveDot      || 'rgba(20, 20, 20, 0.28)';
        this.legalMoveCapture  = o.legalMoveCapture  || 'rgba(20, 20, 20, 0.28)';

        // Coordinates
        this.showCoords        = o.showCoords !== false;
        this.coordLightColor   = o.coordLightColor   || null;   // fall back to opposite square color
        this.coordDarkColor    = o.coordDarkColor    || null;

        // Frame
        this.frameColor        = o.frameColor        || '#1a1815';

        // Pieces
        this.pieceScale        = o.pieceScale        || 0.9;

        // Motion
        this.animationDuration = o.animationDuration ?? 160;   // ms
    }

    static classic() {
        return new BoardTheme();
    }

    static tournament() {
        return new BoardTheme({
            lightSquare: '#e8e6d3',
            darkSquare:  '#6a8a4a',
            lastMoveHighlight: 'rgba(255, 235, 90, 0.45)',
        });
    }

    static midnight() {
        return new BoardTheme({
            lightSquare: '#5d6673',
            darkSquare:  '#2f363f',
            lastMoveHighlight: 'rgba(232, 180, 85, 0.35)',
            selectedHighlight: 'rgba(232, 180, 85, 0.4)',
            legalMoveDot:     'rgba(232, 180, 85, 0.42)',
            legalMoveCapture: 'rgba(232, 180, 85, 0.42)',
            coordLightColor:  'rgba(255, 255, 255, 0.35)',
            coordDarkColor:   'rgba(255, 255, 255, 0.28)',
            frameColor: '#141618',
        });
    }

    static walnut() {
        return new BoardTheme({
            lightSquare: '#e3c98d',
            darkSquare:  '#6b4423',
            frameColor:  '#2a1a0f',
        });
    }
}

/** Registry so settings.js and lobby chip pickers can enumerate options. */
export const THEMES = {
    classic:    { label: 'Classic',    factory: BoardTheme.classic },
    tournament: { label: 'Tournament', factory: BoardTheme.tournament },
    walnut:     { label: 'Walnut',     factory: BoardTheme.walnut },
    midnight:   { label: 'Midnight',   factory: BoardTheme.midnight },
};
