/**
 * tournament.js — PREVIEW screen (Phase 9).
 *
 * Expected backend contract:
 *   → { type: 'list_tournaments' }
 *   ← { type: 'tournaments', tournaments: [{
 *         tournament_id, name, format, rounds, current_round, status,
 *         participants, time_control, starts_at
 *     }] }
 *
 *   → { type: 'create_tournament', name, format, rounds, time_base, time_inc }
 *   ← { type: 'tournament_created', tournament_id }
 *
 *   → { type: 'join_tournament', tournament_id }
 *   ← { type: 'joined_tournament', tournament_id }
 *
 *   → { type: 'tournament_standings', tournament_id }
 *   ← { type: 'standings', tournament_id, rounds_completed,
 *         standings: [{
 *             rank, username, score, wins, losses, draws, buchholz, sonneborn
 *         }],
 *         pairings: [{ round, white, black, result }]
 *     }
 */

import { Screen } from '../ui/screen.js';
import { h, clear, icon } from '../core/dom.js';
import { initials, parseAndFormatTimeControl } from '../core/format.js';

export class TournamentScreen extends Screen {
    constructor(ctx) {
        super(ctx);
        this.preview = true;
    }

    render() {
        return h('div', { class: 'screen' },
            this.header('Tournaments', 'Swiss tournaments, standings, and pairings.',
                h('button', {
                    class: 'btn btn--primary',
                    onclick: () => this._openCreate(),
                }, 'Create tournament')
            ),
            h('div', { class: 'screen__body' },
                h('div', { class: 'card' },
                    h('div', { class: 'card__header' },
                        icon('trophy', 'icon--sm'),
                        h('div', { class: 'card__title' }, 'Active tournaments'),
                    ),
                    h('div', { style: { padding: 0 }, ref: el => this._body = el },
                        h('div', { class: 'empty' }, 'Loading…')
                    )
                )
            )
        );
    }

    async onMount() {
        const { data, live } = await this.ctx.capability.request(
            this.ctx.Outbound.listTournaments(),
            { expect: 'tournaments', timeout: 1500, demo: () => this._demoList() },
        );
        void live;
        this._tournaments = data.tournaments || [];
        this._renderList();
    }

    _renderList() {
        clear(this._body);
        if (!this._tournaments || this._tournaments.length === 0) {
            this._body.appendChild(h('div', { class: 'empty' }, 'No tournaments right now.'));
            return;
        }
        this._body.appendChild(
            h('div', { class: 'table-wrap' },
                h('table', { class: 'table' },
                    h('thead', {}, h('tr', {},
                        h('th', {}, 'Name'),
                        h('th', {}, 'Format'),
                        h('th', {}, 'Round'),
                        h('th', {}, 'Players'),
                        h('th', {}, 'Time'),
                        h('th', {}, 'Status'),
                        h('th', { style: { textAlign: 'right' } }, ''),
                    )),
                    h('tbody', {},
                        ...this._tournaments.map(t => h('tr', {},
                            h('td', {},
                                h('div', {}, t.name),
                                h('div', { style: { fontSize: 'var(--fs-2xs)', color: 'var(--text-muted)' } },
                                    t.rounds + ' rounds'),
                            ),
                            h('td', {}, h('span', { class: 'badge' }, (t.format || 'swiss').toUpperCase())),
                            h('td', { class: 'mono' }, `${t.current_round || 0}/${t.rounds}`),
                            h('td', { class: 'mono' }, String(t.participants || 0)),
                            h('td', { class: 'mono' }, parseAndFormatTimeControl(t.time_control)),
                            h('td', {}, this._statusBadge(t.status)),
                            h('td', { style: { textAlign: 'right', display: 'flex', gap: '6px', justifyContent: 'flex-end' } },
                                h('button', {
                                    class: 'btn btn--sm',
                                    onclick: () => this._openStandings(t),
                                }, 'Standings'),
                                t.status === 'registration'
                                    ? h('button', { class: 'btn btn--sm btn--primary', onclick: () => this._join(t) }, 'Join')
                                    : null,
                            )
                        ))
                    )
                )
            )
        );
    }

    _statusBadge(status) {
        const map = {
            registration: { cls: 'badge badge--info',    label: 'Open' },
            in_progress:  { cls: 'badge badge--success', label: 'Live' },
            completed:    { cls: 'badge',                label: 'Done' },
        };
        const s = map[status] || map.registration;
        return h('span', { class: s.cls }, s.label);
    }

    _openCreate() {
        let name = 'Weekly Blitz Arena';
        let rounds = 5;
        let base = 300, inc = 3;
        const body = h('div', { class: 'field-stack' },
            h('div', { class: 'field' },
                h('label', { class: 'field__label' }, 'Name'),
                h('input', { class: 'input', value: name, oninput: (e) => name = e.target.value }),
            ),
            h('div', { class: 'field' },
                h('label', { class: 'field__label' }, 'Rounds'),
                h('input', { class: 'input', type: 'number', min: 3, max: 15, value: String(rounds), oninput: (e) => rounds = parseInt(e.target.value, 10) || 5 }),
            ),
            h('div', { class: 'field' },
                h('label', { class: 'field__label' }, 'Time control (base + inc seconds)'),
                h('div', { style: { display: 'flex', gap: '8px' } },
                    h('input', { class: 'input mono', type: 'number', min: 30, value: String(base), oninput: (e) => base = parseInt(e.target.value, 10) || 300 }),
                    h('input', { class: 'input mono', type: 'number', min: 0,  value: String(inc),  oninput: (e) => inc  = parseInt(e.target.value, 10) || 0 }),
                ),
            ),
            h('div', { class: 'card-hint' }, 'Preview — the tournament is added to your local list.'),
        );
        const footer = h('div', { style: { display: 'flex', gap: '8px' } },
            h('button', { class: 'btn btn--ghost', onclick: () => dlg.close() }, 'Cancel'),
            h('button', {
                class: 'btn btn--primary',
                onclick: async () => {
                    dlg.close();
                    const msg = this.ctx.Outbound.listTournaments();  // placeholder; real create below
                    const create = { type: 'create_tournament', name, format: 'swiss', rounds, time_base: base, time_inc: inc };
                    // Try to send if live-supported; otherwise just add locally
                    const { live } = await this.ctx.capability.request(create, {
                        expect: 'tournament_created', timeout: 1200, demo: () => ({}),
                    });
                    this._tournaments.unshift({
                        tournament_id: 'demo-' + Date.now(),
                        name, format: 'swiss', rounds, current_round: 0,
                        status: 'registration',
                        participants: 1,
                        time_control: `${base}+${inc}`,
                        starts_at: Date.now() + 3600_000,
                    });
                    this._renderList();
                    this.ctx.toast.success(live ? 'Tournament created.' : 'Tournament added (preview).', { duration: 2400 });
                    void msg;
                },
            }, 'Create'),
        );
        const dlg = this.ctx.modal.open({
            title: 'Create tournament', body, footer, dismissible: true,
        });
    }

    async _join(t) {
        const { live } = await this.ctx.capability.request(
            this.ctx.Outbound.joinTournament(t.tournament_id),
            { expect: 'joined_tournament', timeout: 1200, demo: () => ({}) },
        );
        t.participants = (t.participants || 0) + 1;
        this._renderList();
        this.ctx.toast.success(live ? `Joined "${t.name}"` : `Joined "${t.name}" (preview)`, { duration: 2200 });
    }

    async _openStandings(t) {
        const { data, live } = await this.ctx.capability.request(
            this.ctx.Outbound.tournamentStandings(t.tournament_id),
            { expect: 'standings', timeout: 1200, demo: () => this._demoStandings(t) },
        );
        void live;
        let inner, tabStandings, tabPairings;
        const body = h('div', {},
            h('div', { class: 'tabs' },
                h('button', { class: 'tabs__tab is-active', ref: el => (tabStandings = el), onclick: () => showStandings() }, 'Standings'),
                h('button', { class: 'tabs__tab',           ref: el => (tabPairings  = el), onclick: () => showPairings() },  'Pairings'),
            ),
            h('div', { ref: el => (inner = el) }),
        );
        const showStandings = () => {
            tabStandings.classList.add('is-active'); tabPairings.classList.remove('is-active');
            clear(inner);
            inner.appendChild(this._standingsTable(data.standings || []));
        };
        const showPairings = () => {
            tabPairings.classList.add('is-active'); tabStandings.classList.remove('is-active');
            clear(inner);
            inner.appendChild(this._pairingsTable(data.pairings || []));
        };
        // Open modal — passing a body element that will populate `inner` right after.
        const dlg = this.ctx.modal.open({
            title: t.name,
            subtitle: `${t.format || 'Swiss'} · ${t.rounds} rounds · round ${t.current_round || 0} / ${t.rounds}`,
            body,
            footer: h('button', { class: 'btn', onclick: () => dlg.close() }, 'Close'),
            dismissible: true,
        });
        showStandings();
    }

    _standingsTable(standings) {
        if (!standings.length) return h('div', { class: 'empty' }, 'No standings yet.');
        const myName = this.ctx.store.session.username;
        return h('table', { class: 'table' },
            h('thead', {}, h('tr', {},
                h('th', { style: { width: '48px' } }, '#'),
                h('th', {}, 'Player'),
                h('th', {}, 'Score'),
                h('th', {}, 'W'),
                h('th', {}, 'L'),
                h('th', {}, 'D'),
                h('th', {}, 'Buch.'),
            )),
            h('tbody', {},
                ...standings.map(s => h('tr', {
                    class: s.username === myName ? 'is-you' : null,
                },
                    h('td', {}, this._rank(s.rank)),
                    h('td', {},
                        h('div', { style: { display: 'flex', alignItems: 'center', gap: '10px' } },
                            h('div', { class: 'avatar avatar--sm' }, initials(s.username)),
                            h('span', {}, s.username),
                        )
                    ),
                    h('td', { class: 'mono' },
                        h('span', { class: 'badge badge--accent' }, String(s.score.toFixed ? s.score.toFixed(1) : s.score))
                    ),
                    h('td', { class: 'mono', style: { color: 'var(--success)' } }, String(s.wins)),
                    h('td', { class: 'mono', style: { color: 'var(--danger)' } },  String(s.losses)),
                    h('td', { class: 'mono', style: { color: 'var(--info)' } },    String(s.draws)),
                    h('td', { class: 'mono', style: { color: 'var(--text-muted)' } }, (s.buchholz || 0).toFixed ? s.buchholz.toFixed(1) : s.buchholz),
                ))
            )
        );
    }
    _rank(r) {
        if (r === 1) return h('span', { class: 'rank-medal rank-medal--gold' },   '1');
        if (r === 2) return h('span', { class: 'rank-medal rank-medal--silver' }, '2');
        if (r === 3) return h('span', { class: 'rank-medal rank-medal--bronze' }, '3');
        return h('span', { class: 'rank-num' }, String(r));
    }

    _pairingsTable(pairings) {
        if (!pairings.length) return h('div', { class: 'empty' }, 'No pairings yet.');
        return h('table', { class: 'table' },
            h('thead', {}, h('tr', {},
                h('th', { style: { width: '48px' } }, 'R'),
                h('th', {}, 'White'),
                h('th', {}, 'Black'),
                h('th', {}, 'Result'),
            )),
            h('tbody', {},
                ...pairings.map(p => h('tr', {},
                    h('td', { class: 'mono' }, String(p.round)),
                    h('td', {}, p.white),
                    h('td', {}, p.black),
                    h('td', { class: 'mono' }, p.result || '*'),
                ))
            )
        );
    }

    /* ── Demo data ─────────────────────────────────────── */

    _demoList() {
        return {
            type: 'tournaments',
            tournaments: [
                { tournament_id: 't-1', name: 'Sunday Blitz Arena',   format: 'swiss', rounds: 7, current_round: 3, status: 'in_progress', participants: 18, time_control: '180+2', starts_at: Date.now() - 3600_000 },
                { tournament_id: 't-2', name: 'IIT Patna Rapid Cup',  format: 'swiss', rounds: 5, current_round: 0, status: 'registration', participants: 6,  time_control: '600+5', starts_at: Date.now() + 7200_000 },
                { tournament_id: 't-3', name: 'Late Night Bullet',    format: 'swiss', rounds: 9, current_round: 9, status: 'completed',    participants: 24, time_control: '60+0',  starts_at: Date.now() - 3 * 86400_000 },
            ],
        };
    }

    _demoStandings(t) {
        const NAMES = ['Marta', 'Kai', 'Fen', 'Aria', 'Nikko', 'Jae', 'Vex', 'Sable', 'Corvin', 'Wren', 'Yuki', 'Poe'];
        const rounds = t.current_round || 3;
        const standings = NAMES.slice(0, 8).map((n, i) => {
            const wins = Math.max(0, rounds - i);
            const losses = Math.max(0, Math.min(rounds, i - 1));
            const draws = Math.max(0, rounds - wins - losses);
            return {
                rank: i + 1, username: n,
                score: wins + draws * 0.5,
                wins, losses, draws,
                buchholz: (rounds * 3.5) - i * 0.4,
                sonneborn: 0,
            };
        });
        // Splice user in mid-pack
        const me = this.ctx.store.session.username || 'Player';
        standings.splice(4, 0, {
            rank: 5, username: me, score: Math.floor(rounds * 0.6) + 0.5, wins: 1, losses: 1, draws: 1, buchholz: 8.5, sonneborn: 0,
        });
        standings.forEach((s, i) => s.rank = i + 1);

        const pairings = [];
        for (let r = 1; r <= rounds; r++) {
            for (let i = 0; i < 4; i++) {
                const w = standings[(i * 2)     % standings.length].username;
                const b = standings[(i * 2 + 1) % standings.length].username;
                const rs = ['1-0', '0-1', '1/2-1/2'][r % 3];
                pairings.push({ round: r, white: w, black: b, result: rs });
            }
        }
        return { type: 'standings', tournament_id: t.tournament_id, rounds_completed: rounds, standings, pairings };
    }
}
