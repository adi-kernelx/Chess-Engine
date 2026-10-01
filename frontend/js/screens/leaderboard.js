/**
 * leaderboard.js — Live leaderboard screen (Phase 8).
 *
 * Expected backend contract:
 *   → { type: 'get_leaderboard', limit }
 *   ← { type: 'leaderboard', players: [{
 *         rank, username, elo, wins, losses, draws, games_played
 *     }] }
 *
 * The backend currently has one global rating. Time-control categories are
 * omitted until the persistence model can answer them truthfully.
 */

import { Screen } from '../ui/screen.js';
import { h, clear, icon } from '../core/dom.js';
import { initials, formatElo } from '../core/format.js';

export class LeaderboardScreen extends Screen {
    constructor(ctx) {
        super(ctx);
    }

    render() {
        return h('div', { class: 'screen' },
            this.header('Leaderboard', 'Top players by rating.'),
            h('div', { class: 'screen__body' },
                h('div', { class: 'card' },
                    h('div', { class: 'card__header' },
                        icon('trophy', 'icon--sm'),
                        h('div', { class: 'card__title' }, 'Top players'),
                        h('button', {
                            class: 'btn btn--ghost btn--sm',
                            style: { marginLeft: 'auto' },
                            onclick: () => this._load(),
                        }, 'Refresh')
                    ),
                    h('div', { style: { padding: 0 }, ref: el => this._tableWrap = el },
                        h('div', { class: 'empty' }, 'Loading rankings…')
                    ),
                )
            )
        );
    }

    async onMount() { await this._load(); }

    async _load() {
        clear(this._tableWrap);
        this._tableWrap.appendChild(h('div', { class: 'empty' }, 'Loading rankings…'));
        const { data, live, error } = await this.ctx.capability.request(
            this.ctx.Outbound.leaderboard(100),
            {
                expect: 'leaderboard',
                timeout: 5000,
                demo: () => null,
                failOnError: true,
            }
        );
        if (!live || !data) {
            clear(this._tableWrap);
            const detail = error && error.message
                ? error.message
                : 'Check the server and database connection.';
            this._tableWrap.appendChild(h('div', { class: 'empty' },
                h('div', {}, 'Leaderboard unavailable.'),
                h('div', { class: 'field__hint', style: { marginTop: '8px' } }, detail)));
            return;
        }
        this._renderTable(data.players || []);
    }

    _renderTable(players) {
        clear(this._tableWrap);
        if (!players || players.length === 0) {
            this._tableWrap.appendChild(h('div', { class: 'empty' }, 'No players yet.'));
            return;
        }
        const myName = this.ctx.store.session.username;
        const table = h('table', { class: 'table leaderboard-table' },
            h('thead', {},
                h('tr', {},
                    h('th', { style: { width: '48px' } }, 'Rank'),
                    h('th', {}, 'Player'),
                    h('th', {}, 'Rating'),
                    h('th', {}, 'W'),
                    h('th', {}, 'L'),
                    h('th', {}, 'D'),
                    h('th', {}, 'Games'),
                )),
            h('tbody', {},
                ...players.map(p => h('tr', {
                    class: (p.username === myName) ? 'is-you' : null,
                },
                    h('td', { class: 'rank' }, this._rankCell(p.rank)),
                    h('td', {},
                        h('div', { style: { display: 'flex', alignItems: 'center', gap: '10px' } },
                            h('div', { class: 'avatar avatar--sm' }, initials(p.username)),
                            h('div', {},
                                h('div', {}, p.username),
                                p.username === myName ? h('div', { style: { fontSize: 'var(--fs-2xs)', color: 'var(--accent)' } }, 'You') : null
                            )
                        )
                    ),
                    h('td', { class: 'mono' },
                        h('span', { class: 'badge badge--accent' }, formatElo(p.elo))
                    ),
                    h('td', { class: 'mono', style: { color: 'var(--success)' } }, String(p.wins)),
                    h('td', { class: 'mono', style: { color: 'var(--danger)'  } }, String(p.losses)),
                    h('td', { class: 'mono', style: { color: 'var(--info)'    } }, String(p.draws)),
                    h('td', { class: 'mono', style: { color: 'var(--text-muted)' } }, String(p.gamesPlayed || (p.wins + p.losses + p.draws))),
                ))
            )
        );
        this._tableWrap.appendChild(h('div', { class: 'table-wrap' }, table));
    }

    _rankCell(rank) {
        if (rank === 1) return h('span', { class: 'rank-medal rank-medal--gold' },   '1');
        if (rank === 2) return h('span', { class: 'rank-medal rank-medal--silver' }, '2');
        if (rank === 3) return h('span', { class: 'rank-medal rank-medal--bronze' }, '3');
        return h('span', { class: 'rank-num' }, String(rank));
    }

}
