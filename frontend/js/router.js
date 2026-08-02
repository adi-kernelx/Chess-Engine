/**
 * PageRouter — Minimal client-side page router.
 *
 * Manages visibility of page sections (lobby vs game view)
 * using CSS class toggling. No hash URLs or pushState — just
 * simple show/hide with fade transitions.
 *
 * Usage:
 *   const router = new PageRouter();
 *   router.showLobby();   // Show lobby, hide game
 *   router.showGame();    // Show game, hide lobby
 */
class PageRouter {
    constructor() {
        this.lobbyPage = document.getElementById('lobby-page');
        this.boardPage = document.getElementById('board-page');
        this._currentPage = 'lobby';
    }

    /**
     * Show the lobby page, hide the game page.
     */
    showLobby() {
        if (this._currentPage === 'lobby') return;
        this._currentPage = 'lobby';

        this.boardPage.classList.remove('page-active');
        this.boardPage.classList.add('page-hidden');

        // Small delay so the fade-out starts before fade-in
        requestAnimationFrame(() => {
            this.lobbyPage.classList.remove('page-hidden');
            this.lobbyPage.classList.add('page-active');
        });
    }

    /**
     * Show the game page, hide the lobby page.
     */
    showGame() {
        if (this._currentPage === 'game') return;
        this._currentPage = 'game';

        this.lobbyPage.classList.remove('page-active');
        this.lobbyPage.classList.add('page-hidden');

        requestAnimationFrame(() => {
            this.boardPage.classList.remove('page-hidden');
            this.boardPage.classList.add('page-active');
        });
    }

    /**
     * Get the current page name.
     * @returns {'lobby'|'game'}
     */
    get currentPage() {
        return this._currentPage;
    }
}

window.PageRouter = PageRouter;
