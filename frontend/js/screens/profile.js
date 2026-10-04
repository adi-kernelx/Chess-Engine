/**
 * profile.js — Profile screen (Phase 8).
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
 * If the backend cannot answer, renders an explicitly marked empty profile;
 * it never invents games or rating history for a real username.
 */

import { Screen } from '../ui/screen.js';
import { h, clear, icon } from '../core/dom.js';
import { initials, relativeTime, parseAndFormatTimeControl, formatElo } from '../core/format.js';
import { INITIAL_RATING } from '../core/rating.js';

export class ProfileScreen extends Screen {
    constructor(ctx) {
        super(ctx);
    }

    render() {
        const signedIn = this.ctx.session && this.ctx.session.isAuthenticated;
        return h('div', { class: 'screen' },
            this.header('Profile', 'Rating, record, and recent games.',
                signedIn
                    ? h('button', { class: 'btn btn--ghost', onclick: () => this._signOut() }, 'Sign out')
                    : h('div', { style: { display: 'flex', gap: '8px' } },
                        h('a', { class: 'btn btn--ghost', href: '#/login' }, 'Sign in'),
                        h('a', { class: 'btn btn--primary', href: '#/register' }, 'Create account'))),
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
        if (!this.ctx.session || !this.ctx.session.isAuthenticated) {
            this._renderSignedOut();
            return;
        }

        const username = this.ctx.session.username;
        const { data, live, error } = await this.ctx.capability.request(
            this.ctx.Outbound.getProfile(username),
            {
                expect: 'profile',
                timeout: 5000,
                demo: () => this._emptyProfile(username),
                failOnError: true,
            }
        );
        if (!live) {
            this._renderUnavailable(error);
            return;
        }
        this._render(data);
    }

    _renderSignedOut() {
        clear(this._body);
        this._body.appendChild(h('div', { class: 'card' },
            h('div', { class: 'empty' },
                h('div', {}, 'Sign in to view your rating, record, and game history.'),
                h('div', { style: { display: 'flex', gap: '8px', justifyContent: 'center', marginTop: '16px' } },
                    h('a', { class: 'btn btn--primary', href: '#/login' }, 'Sign in'),
                    h('a', { class: 'btn btn--ghost', href: '#/register' }, 'Create account'))
            )));
    }

    _renderUnavailable(error) {
        clear(this._body);
        const detail = error && error.message ? ` (${error.message})` : '';
        this._body.appendChild(h('div', { class: 'card' },
            h('div', { class: 'empty' }, `Profile could not be loaded${detail}.`)));
    }

    async _signOut() {
        await this.ctx.session.logout();
        this.ctx.toast.success('Signed out.', { duration: 1800 });
        this.ctx.router.go('/login');
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
                        this._stat('Rated games', String(p.gamesPlayed)),
                        this._stat('Wins',    String(p.wins),   'success'),
                        this._stat('Losses',  String(p.losses), 'danger'),
                        this._stat('Draws',   String(p.draws),  'info'),
                        this._stat('Win rate', winRate + '%'),
                        this._stat('Rating',  formatElo(p.elo)),
                    ),
                    h('div',{class:'card'},h('div',{class:'card__body'},
                        h('div',{class:'card__title'},'Account recovery'),
                        h('p',{class:'auth-note'},'Older password accounts can add a verified recovery email. Google accounts already have a verified address.'),
                        h('a',{class:'btn btn--ghost',href:'#/recovery-email'},'Add recovery email'))),

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
                                    g.rated ? formatElo(g.opponentElo) : 'Unrated',
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

    _emptyProfile(username) {
        return {
            type: 'profile', username, elo: INITIAL_RATING,
            gamesPlayed: 0, wins: 0, losses: 0, draws: 0,
            ratingHistory: [], recentGames: [],
        };
    }
}
