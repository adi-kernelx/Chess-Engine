/**
 * profile.js — PREVIEW screen (Phase 8).
 *
 * Expected backend contract:
 *   → { type: 'get_profile', username }
 *   ← { type: 'profile', username, elo, wins, losses, draws, games_played,
 *       rating_history: [{ ts: <ms>, rating: <int> }],
 *       recent_games:  [{ game_id, opponent, opponent_elo, result:'w'|'l'|'d',
 *                          color:'w'|'b', played_at:<ms>, moves:<int>,
 *                          time_control:'600+5' }] }
 *
 * On live: renders straight from server data.
 * On preview: fills with plausible demo data seeded from the username so
 * every load looks the same for a given user.
 */

import { Screen } from '../ui/screen.js';
import { h, clear, icon } from '../core/dom.js';
import { initials, relativeTime, parseAndFormatTimeControl, formatElo } from '../core/format.js';

export class ProfileScreen extends Screen {
    constructor(ctx) {
        super(ctx);
        this.preview = true;
    }

    render() {
        return h('div', { class: 'screen' },
            this.header('Profile', 'Rating, record, and recent games.'),
            h('div', { class: 'screen__body' },
                h('div', { class: 'profile-loading', ref: el => this._body = el },
                    h('div', { class: 'card' },
                        h('div', { class: 'empty' }, 'Loading profile…')
                    )
                )
            )
        );
    }

    async onMount() {
        const username = this.ctx.store.session.username || 'Player';
        const { data, live } = await this.ctx.capability.request(
            this.ctx.Outbound.getProfile(username),
            {
                expect: 'profile',
                timeout: 1500,
                demo: () => this._demo(username),
            }
        );
        if (!live) {
            this.preview = true;
            const h1 = this.root && this.root.querySelector('.screen__title-block');
            if (h1 && !h1.querySelector('.badge--preview')) {
                h1.appendChild(h('span', { class: 'badge badge--preview' }, 'Preview — backend pending'));
            }
        }
        this._render(data);
    }

    _render(p) {
        clear(this._body);
        const wl = (p.wins + p.losses) || 0;
        const winRate = wl > 0 ? Math.round((p.wins / wl) * 100) : 0;
        const netRating = p.ratingHistory && p.ratingHistory.length > 1
            ? p.ratingHistory[p.ratingHistory.length - 1].rating - p.ratingHistory[0].rating
            : 0;

        this._body.appendChild(
            h('div', { class: 'profile-grid' },

                // Left column: identity + stats + rating chart
                h('div', { class: 'profile-col' },

                    h('div', { class: 'card' },
                        h('div', { class: 'card__body' },
                            h('div', { class: 'profile-identity' },
                                h('div', { class: 'avatar avatar--lg' }, initials(p.username)),
                                h('div', {},
                                    h('div', { class: 'profile-name' }, p.username),
                                    h('div', { class: 'profile-elo' },
                                        h('span', { class: 'badge badge--accent' }, `${formatElo(p.elo)} rating`),
                                        h('span', {
                                            class: 'badge ' + (netRating >= 0 ? 'badge--success' : 'badge--danger'),
                                        }, (netRating >= 0 ? '+' : '') + netRating + ' recent')
                                    )
                                )
                            )
                        )
                    ),

                    h('div', { class: 'stats-grid' },
                        this._stat('Games',   String(p.gamesPlayed)),
                        this._stat('Wins',    String(p.wins),   'success'),
                        this._stat('Losses',  String(p.losses), 'danger'),
                        this._stat('Draws',   String(p.draws),  'info'),
                        this._stat('Win rate', winRate + '%'),
                        this._stat('Rating',  formatElo(p.elo)),
                    ),

                    h('div', { class: 'card' },
                        h('div', { class: 'card__header' },
                            icon('chart', 'icon--sm'),
                            h('div', { class: 'card__title' }, 'Rating history')),
                        h('div', { class: 'card__body' },
                            this._ratingChart(p.ratingHistory || []),
                        )
                    ),
                ),

                // Right column: recent games
                h('div', { class: 'profile-col' },
                    h('div', { class: 'card' },
                        h('div', { class: 'card__header' },
                            icon('replay', 'icon--sm'),
                            h('div', { class: 'card__title' }, 'Recent games'),
                            h('span', { style: { marginLeft: 'auto', fontSize: 'var(--fs-xs)', color: 'var(--text-muted)' } },
                                `${(p.recentGames || []).length} games`)
                        ),
                        h('div', { style: { padding: 0 } },
                            this._recentTable(p.recentGames || [])
                        )
                    )
                )
            )
        );
    }

    _stat(label, value, tone = null) {
        return h('div', { class: 'lobby-stat' },
            h('div', { class: 'lobby-stat__label' }, label),
            h('div', {
                class: 'lobby-stat__value',
                style: tone ? { color: `var(--${tone})` } : null,
            }, value)
        );
    }

    _recentTable(games) {
        if (games.length === 0) return h('div', { class: 'empty' }, 'No games yet.');
        return h('table', { class: 'table' },
            h('thead', {},
                h('tr', {},
                    h('th', {}, 'Result'),
                    h('th', {}, 'Opponent'),
                    h('th', {}, 'Time'),
                    h('th', { style: { textAlign: 'right' } }, 'Played'),
                )),
            h('tbody', {},
                ...games.map(g => h('tr', {},
                    h('td', {},
                        h('span', {
                            class: 'result-dot result-dot--' + g.result,
                            title: { w: 'Win', l: 'Loss', d: 'Draw' }[g.result] || 'Ongoing',
                        }, { w: 'W', l: 'L', d: 'D' }[g.result] || '?')
                    ),
                    h('td', {},
                        h('div', { style: { display: 'flex', alignItems: 'center', gap: '8px' } },
                            h('div', { class: 'avatar avatar--sm' }, initials(g.opponent)),
                            h('div', {},
                                h('div', {}, g.opponent),
                                h('div', { style: { fontSize: 'var(--fs-2xs)', color: 'var(--text-muted)' } },
                                    formatElo(g.opponentElo),
                                )
                            )
                        )
                    ),
                    h('td', { class: 'mono' }, parseAndFormatTimeControl(g.timeControl)),
                    h('td', { style: { textAlign: 'right', color: 'var(--text-muted)' } }, relativeTime(g.playedAt)),
                ))
            )
        );
    }

    _ratingChart(points) {
        if (points.length < 2) return h('div', { class: 'field__hint' }, 'More games needed for a chart.');
        const W = 560, H = 160, PAD = 16;
        const xs = points.map((_, i) => PAD + (W - 2 * PAD) * (i / (points.length - 1)));
        const ratings = points.map(p => p.rating);
        const minR = Math.min(...ratings) - 20;
        const maxR = Math.max(...ratings) + 20;
        const range = maxR - minR;
        const ys = ratings.map(r => PAD + (H - 2 * PAD) * (1 - (r - minR) / range));
        const path = xs.map((x, i) => `${i ? 'L' : 'M'} ${x.toFixed(1)} ${ys[i].toFixed(1)}`).join(' ');
        const areaPath = path + ` L ${xs[xs.length - 1].toFixed(1)} ${H - PAD} L ${xs[0].toFixed(1)} ${H - PAD} Z`;

        return h('svg', {
            viewBox: `0 0 ${W} ${H}`,
            width: '100%',
            style: { display: 'block', height: 'auto', maxHeight: '200px' },
            role: 'img',
            'aria-label': 'Rating history over time',
        },
            h('rect', { x: 0, y: 0, width: W, height: H, fill: 'transparent' }),
            h('path', { d: areaPath, fill: 'var(--accent-soft)' }),
            h('path', { d: path, fill: 'none', stroke: 'var(--accent)', 'stroke-width': '2', 'stroke-linejoin': 'round' }),
            // Dots at each data point
            ...xs.map((x, i) => h('circle', {
                cx: x.toFixed(1), cy: ys[i].toFixed(1), r: '3',
                fill: 'var(--accent)', stroke: 'var(--surface-1)', 'stroke-width': '1.5',
            })),
            // Y-axis text labels (min and max only)
            h('text', { x: 4, y: PAD, fill: 'var(--text-muted)', 'font-size': '10', 'font-family': 'var(--font-mono)' }, formatElo(maxR)),
            h('text', { x: 4, y: H - 4, fill: 'var(--text-muted)', 'font-size': '10', 'font-family': 'var(--font-mono)' }, formatElo(minR)),
        );
    }

    /** Deterministic demo profile seeded from the username. */
    _demo(username) {
        const rng = mulberry32(hashStr(username));
        const games = 20 + Math.floor(rng() * 100);
        const wins   = Math.floor(games * (0.35 + rng() * 0.3));
        const losses = Math.floor(games * (0.3  + rng() * 0.25));
        const draws  = Math.max(0, games - wins - losses);
        const elo = 1000 + Math.floor(rng() * 800);
        // Rating history — 20 points around the current
        const hist = [];
        let cur = elo - Math.floor(rng() * 300);
        for (let i = 0; i < 20; i++) {
            cur += Math.round((rng() - 0.5) * 40);
            hist.push({ ts: Date.now() - (20 - i) * 86_400_000 * 2, rating: Math.max(200, cur) });
        }
        hist[hist.length - 1].rating = elo;
        // Recent games
        const RESULTS = ['w', 'l', 'd', 'w', 'w', 'l'];
        const NAMES = ['Marta', 'Kai', 'Fen', 'Aria', 'Nikko', 'Jae', 'Rue', 'Vex', 'Oli', 'Zed'];
        const TCS = ['180+2', '300+3', '600+5', '900+10', '60+0'];
        const recent = [];
        for (let i = 0; i < 10; i++) {
            const opp = NAMES[Math.floor(rng() * NAMES.length)];
            recent.push({
                gameId: 100 + i,
                opponent: opp,
                opponentElo: elo + Math.round((rng() - 0.5) * 300),
                result: RESULTS[Math.floor(rng() * RESULTS.length)],
                color: rng() < 0.5 ? 'w' : 'b',
                playedAt: Date.now() - i * 3_600_000 - Math.floor(rng() * 3_600_000),
                moves: 20 + Math.floor(rng() * 60),
                timeControl: TCS[Math.floor(rng() * TCS.length)],
            });
        }
        return {
            type: 'profile',
            username, elo,
            gamesPlayed: games, wins, losses, draws,
            ratingHistory: hist,
            recentGames: recent,
        };
    }
}

function hashStr(s) {
    let h = 2166136261;
    for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 16777619); }
    return h >>> 0;
}
function mulberry32(seed) {
    return function () {
        let t = seed += 0x6D2B79F5;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}
