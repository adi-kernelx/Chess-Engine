/**
 * leaderboard.js — PREVIEW screen (Phase 8).
 *
 * Expected backend contract:
 *   → { type: 'leaderboard', limit }
 *   ← { type: 'leaderboard', players: [{
 *         rank, username, elo, wins, losses, draws, games_played
 *     }] }
 *
 * On live: renders straight from server data.
 * On preview: renders a deterministic demo roster.
 */

import { Screen } from '../ui/screen.js';
import { h, clear, icon } from '../core/dom.js';
import { initials, formatElo } from '../core/format.js';

const CATEGORIES = ['All', 'Bullet', 'Blitz', 'Rapid'];

export class LeaderboardScreen extends Screen {
    constructor(ctx) {
        super(ctx);
        this.preview = true;
        this._category = 'All';
    }

    render() {
        return h('div', { class: 'screen' },
            this.header('Leaderboard', 'Top players by rating.'),
            h('div', { class: 'screen__body' },
                h('div', { class: 'card' },
                    h('div', { class: 'card__header' },
                        icon('trophy', 'icon--sm'),
                        h('div', { class: 'card__title' }, 'Top players'),
                        h('div', {
                            style: { marginLeft: 'auto' },
                        }, this._categoryPicker())
                    ),
                    h('div', { style: { padding: 0 }, ref: el => this._tableWrap = el },
                        h('div', { class: 'empty' }, 'Loading rankings…')
                    ),
                )
            )
        );
    }

    _categoryPicker() {
        return h('div', { class: 'pill-group' },
            ...CATEGORIES.map(c =>
                h('button', {
                    class: 'pill-group__item' + (c === this._category ? ' is-active' : ''),
                    onclick: (e) => this._changeCategory(c, e.currentTarget),
                }, c)
            )
        );
    }

    _changeCategory(c, btn) {
        this._category = c;
        for (const el of btn.parentElement.children) el.classList.remove('is-active');
        btn.classList.add('is-active');
        // Re-render table with the (demo) reshuffled data for this category
        this._renderTable(this._filterByCategory(this._data));
    }

    async onMount() {
        const { data, live } = await this.ctx.capability.request(
            this.ctx.Outbound.leaderboard(100),
            {
                expect: 'leaderboard',
                timeout: 1500,
                demo: () => this._demo(),
            }
        );
        this._data = data;
        this._renderTable(this._filterByCategory(data));
    }

    _filterByCategory(data) {
        if (this._category === 'All') return data.players || [];
        // Preview: seed a small shuffle by category name.
        const players = (data.players || []).slice();
        const seed = this._category.charCodeAt(0);
        return players
            .map((p, i) => ({ p, k: (p.elo * 7 + i * 3 + seed) % 10000 }))
            .sort((a, b) => b.k - a.k)
            .map((x, i) => ({ ...x.p, rank: i + 1 }));
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

    _demo() {
        const NAMES = [
            'Marta', 'Kai', 'Fen', 'Aria', 'Nikko', 'Jae', 'Rue', 'Vex', 'Oli', 'Zed',
            'Marlow', 'Sable', 'Corvin', 'Tam', 'Rio', 'Ash', 'Ley', 'Bram', 'Nova', 'Odin',
            'Wren', 'Poe', 'Milo', 'Jinx', 'Rosa', 'Grim', 'Hex', 'Ivo', 'Sasha', 'Yuki',
        ];
        const myself = this.ctx.store.session.username || 'Player';
        const players = NAMES.map((n, i) => {
            const elo = 2400 - i * 35 - Math.floor(Math.random() * 20);
            const games = 100 + Math.floor(Math.random() * 500);
            const wins   = Math.floor(games * 0.55);
            const losses = Math.floor(games * 0.35);
            const draws  = games - wins - losses;
            return { rank: i + 1, username: n, elo, wins, losses, draws, gamesPlayed: games };
        });
        // Splice the current user into a plausible mid-pack rank so "You" chip shows.
        players.splice(12, 0, {
            rank: 13, username: myself, elo: 1200, wins: 3, losses: 5, draws: 1, gamesPlayed: 9,
        });
        players.forEach((p, i) => p.rank = i + 1);
        return { type: 'leaderboard', players };
    }
}
