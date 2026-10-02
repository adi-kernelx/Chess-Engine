/** Scheduled Swiss-tournament UI. Lifecycle decisions remain server-authoritative. */
import { Screen } from '../ui/screen.js';
import { h, clear, icon } from '../core/dom.js';
import { absoluteUrl, copyText } from '../core/share.js';
import { formatTimeControl } from '../core/format.js';

const TOURNAMENT_ERROR_COPY = Object.freeze({
    not_creator: 'Only the tournament creator can change tournament controls.',
    tournament_not_found: 'This tournament no longer exists.',
    tournament_not_in_registration: 'Registration for this tournament has closed.',
    registration_closed: 'Registration is closed.',
    registration_deadline_passed: 'The registration deadline has passed.',
    registration_state_locked: 'Registration can no longer be changed.',
    not_registered: 'You are not registered for this tournament.',
    player_eliminated: 'You have been eliminated. You can still spectate.',
    advancement_already_locked: 'That result cannot be changed after the next stage or final outcome is locked.',
    not_enough_players: 'At least two players must register before registration can close.',
    pairing_not_found: 'That tournament pairing no longer exists.',
    check_in_closed_or_not_registered: 'Round check-in is closed or you are not registered.',
    round_already_resolved_for_player: 'Your result for this round is already recorded. No check-in is needed.',
    override_reason_required: 'Explain why this result needs to be corrected.',
    result_already_recorded: 'This pairing already has a different result.',
});

export function tournamentErrorMessage(message, fallback) {
    return TOURNAMENT_ERROR_COPY[String(message || '')] || message || fallback;
}

export function tournamentPath(id) {
    const value = Number(id);
    return Number.isSafeInteger(value) && value > 0 ? `/tournaments/${value}` : '/tournaments';
}

/** Round outcome/readiness, rather than a stale check-in flag after play. */
export function tournamentRoundStatus(state, player, round, now = Date.now() / 1000) {
    if (player.withdrawn) return 'Withdrawn';
    if (state.tournament.format === 'winners_advance') {
        if (state.tournament.status === 'completed') return player.rank === 1 ? 'Champion' : 'Finished';
        const last = (state.pairings || []).filter(p => p.whitePlayerId === player.playerId || p.blackPlayerId === player.playerId)
            .sort((a, b) => b.round - a.round)[0];
        if (last && (last.result === 'double_forfeit' || last.result === '0-0'
            || (last.result === '1-0' && last.blackPlayerId === player.playerId)
            || (last.result === '0-1' && last.whitePlayerId === player.playerId))) return 'Eliminated';
    }
    const number = round ? round.round : state.tournament.currentRound;
    const pairing = (state.pairings || []).find(p => p.round === number
        && (p.whitePlayerId === player.playerId || p.blackPlayerId === player.playerId));
    if (pairing && pairing.result !== 'pending') {
        if (pairing.result === 'bye') return 'Bye';
        if (pairing.result === '1/2-1/2') return 'Draw';
        if (pairing.result === '0-0' || pairing.result === 'double_forfeit') return 'Double forfeit';
        const white = pairing.whitePlayerId === player.playerId;
        return (pairing.result === '1-0') === white ? 'Won' : 'Lost';
    }
    if (state.tournament.status === 'completed') return 'Finished';
    if (!round) return 'Waiting for round';
    if (round.status === 'scheduled') return `Round ${number} upcoming`;
    const checked = (state.checkIns || []).some(c => c.round === number && c.playerId === player.playerId);
    if (checked) return pairing && pairing.gameId ? 'Game assigned' : 'Checked in';
    return now > round.checkInClosesAt ? 'Missed check-in' : 'Check-in required';
}

export function toLocalDateTimeInput(date) {
    const d = new Date(date);
    if (!Number.isFinite(d.getTime())) return '';
    const local = new Date(d.getTime() - d.getTimezoneOffset() * 60_000);
    return local.toISOString().slice(0, 16);
}

export function validateTournamentSchedule(values, nowMs = Date.now()) {
    const errors = {};
    const name = String(values.name || '').trim();
    const format = values.format || 'swiss';
    const rounds = format === 'winners_advance' ? 1 : Number(values.rounds);
    const base = Number(values.base);
    const inc = Number(values.inc);
    const roundMinutes = Number(values.roundMinutes);
    const deadlineMs = new Date(values.registrationDeadline).getTime();
    const startMs = new Date(values.firstRoundStartsAt).getTime();
    if (!name) errors.name = 'Enter a tournament name.';
    else if (name.length > 128) errors.name = 'Use 128 characters or fewer.';
    if (!Number.isInteger(rounds) || rounds < 1 || rounds > 30) errors.rounds = 'Choose 1 to 30 rounds.';
    if (!['swiss', 'winners_advance'].includes(format)) errors.format = 'Choose a tournament format.';
    if (!Number.isInteger(base) || base < 1) errors.base = 'Base time must be at least 1 second.';
    if (!Number.isInteger(inc) || inc < 0) errors.inc = 'Increment cannot be negative.';
    if (!Number.isFinite(deadlineMs) || deadlineMs <= nowMs) errors.registrationDeadline = 'Choose a future registration deadline.';
    if (!Number.isFinite(startMs) || startMs < deadlineMs + 30_000) errors.firstRoundStartsAt = 'Start round one at least 30 seconds after registration closes.';
    if (!Number.isInteger(roundMinutes) || roundMinutes < 1) errors.roundMinutes = 'Round spacing must be at least 1 minute.';
    return {
        errors,
        valid: Object.keys(errors).length === 0,
        value: {
            name, format, rounds, base, inc,
            registrationDeadline: Math.floor(deadlineMs / 1000),
            firstRoundStartsAt: Math.floor(startMs / 1000),
            roundDurationSeconds: roundMinutes * 60,
        },
    };
}

export function canJoinTournamentRound(state, player, round) {
    if (!player || player.withdrawn || !round || state.tournament.status === 'completed') return false;
    if (tournamentRoundStatus(state, player, round) === 'Eliminated') return false;
    const pairing = (state.pairings || []).find(p => p.round === round.round
        && (p.whitePlayerId === player.playerId || p.blackPlayerId === player.playerId));
    return !pairing || pairing.result === 'pending';
}

export class TournamentScreen extends Screen {
    constructor(ctx, params = {}) {
        super(ctx);
        this._tournaments = [];
        this._detailState = null;
        this._detailTournamentId = null;
        this._detailLoading = false;
        this._detailTab = 'overview';
        this._detailDialog = null;
        const target = Number(params.tournamentId);
        this._targetTournamentId = Number.isSafeInteger(target) && target > 0 ? target : null;
    }

    render() {
        const signedIn = this.ctx.session && this.ctx.session.isAuthenticated;
        const action = signedIn
            ? h('button', { class: 'btn btn--primary btn--lg', onclick: () => this._openCreate() }, 'Create tournament')
            : h('a', { class: 'btn btn--primary', href: '#/login' }, 'Sign in to join');
        return h('div', { class: 'screen' },
            this.header('Tournaments', 'Scheduled events with live games and automatic results.', action),
            h('div', { class: 'screen__body' }, h('div', { class: 'card' },
                h('div', { class: 'card__header' }, icon('trophy', 'icon--sm'),
                    h('div', {}, h('div', { class: 'card__title' }, 'Tournament directory'),
                        h('div', { class: 'card__subtitle' }, 'Times are shown in your local timezone.')),
                    h('button', { class: 'btn btn--ghost btn--sm tournament-refresh', onclick: () => this._load() }, 'Refresh')),
                h('div', { class: 'tournament-list', ref: el => { this._body = el; } },
                    this._loading('Loading tournaments…')))));
    }

    async onMount() {
        this.sub(this.ctx.socket.on('tournament_game_ready', raw =>
            this._onTournamentReady(this.ctx.Inbound.normalize(raw))));
        this.sub(this.ctx.socket.onState(state => {
            if (state !== 'connected' || !this.root) return;
            void this._load();
            if (this._detailTournamentId) void this._refreshDetail(true);
        }));
        this.interval(() => {
            this._updateCountdown();
            if (this._detailTournamentId) void this._refreshDetail(true);
        }, 3000);
        await this._load();
        if (!this.root || !this._targetTournamentId) return;
        const summary = this._tournaments.find(t => Number(t.id) === this._targetTournamentId)
            || { id: this._targetTournamentId, name: `Tournament #${this._targetTournamentId}` };
        await this._openState(summary);
    }

    onUnmount() {
        if (this._detailDialog) this._detailDialog.close();
        this._detailDialog = null;
    }

    _loading(label) {
        return h('div', { class: 'empty', role: 'status', 'aria-busy': 'true' }, label);
    }

    async _load() {
        if (!this._body) return;
        clear(this._body); this._body.appendChild(this._loading('Loading tournaments…'));
        const { data, live, error } = await this._readRequest(
            this.ctx.Outbound.listTournaments(),
            { expect: 'tournament_list', timeout: 5000, demo: () => ({ tournaments: [] }), failOnError: true });
        if (!live) { this._renderUnavailable(error); return; }
        this._tournaments = data.tournaments || [];
        this._renderList();
    }

    _renderUnavailable(error) {
        clear(this._body);
        const message = error && error.message ? error.message : 'The tournament service did not respond.';
        this._body.appendChild(h('div', { class: 'empty' },
            h('div', { class: 'empty__title' }, 'Tournaments are unavailable'),
            h('div', { class: 'empty__sub' }, message),
            h('button', { class: 'btn', onclick: () => this._load() }, 'Try again')));
    }

    _renderList() {
        clear(this._body);
        if (!this._tournaments.length) {
            this._body.appendChild(h('div', { class: 'empty' },
                h('div', { class: 'empty__title' }, 'No tournaments yet'),
                h('div', { class: 'empty__sub' }, 'Create the first scheduled Swiss tournament.')));
            return;
        }
        const cards = this._tournaments.map(t => h('article', { class: 'tournament-card' },
            h('div', { class: 'tournament-card__main' },
                h('div', { class: 'tournament-card__eyebrow' }, this._statusBadge(t.status, t.registrationOpen)),
                h('h2', { class: 'tournament-card__title' }, t.name),
                h('div', { class: 'tournament-card__meta' },
                    h('span', {}, t.format === 'winners_advance' ? 'Winners advance · automatic rounds' : `${t.rounds} Swiss rounds`), h('span', {}, formatTimeControl(t.timeBase, t.timeInc)),
                    h('span', {}, t.currentRound ? `Round ${t.currentRound}` : 'Before round 1')),
                h('div', { class: 'tournament-card__time' }, this._listTimeText(t))),
            h('div', { class: 'tournament-card__actions' },
                h('button', { class: 'btn btn--primary', onclick: () => this.ctx.router.go(tournamentPath(t.id)) }, 'Open'),
                h('button', { class: 'btn btn--ghost', onclick: () => this._openShare(t) }, 'Share'))));
        this._body.appendChild(h('div', { class: 'tournament-grid' }, ...cards));
    }

    _listTimeText(t) {
        if (t.status === 'registration') return `Registration closes ${this._formatDate(t.registrationDeadline)}`;
        if (t.status === 'scheduled') return `Round one starts ${this._formatDate(t.firstRoundStartsAt)}`;
        if (t.status === 'in_progress') return 'Games and standings update automatically';
        return 'Tournament completed';
    }

    _statusBadge(status, registrationOpen = false) {
        const map = {
            registration: { cls: 'badge badge--info', label: registrationOpen ? 'Registration open' : 'Registration' },
            scheduled: { cls: 'badge badge--warning', label: 'Scheduled' },
            in_progress: { cls: 'badge badge--success', label: 'Live' },
            completed: { cls: 'badge', label: 'Completed' },
        };
        const value = map[status] || { cls: 'badge', label: status || 'Unknown' };
        return h('span', { class: value.cls }, value.label);
    }

    async _tokenOrLogin() {
        if (!this.ctx.session) {
            this.ctx.postAuthPath = this._detailTournamentId ? tournamentPath(this._detailTournamentId) : '/tournaments';
            this.ctx.router.go('/login'); return null;
        }
        const token = await this.ctx.session.accessTokenForRequest();
        if (!token) {
            if (this.ctx.session.hasRefreshToken) {
                this.ctx.toast.warning('Your session is temporarily reconnecting. Please try again.');
                return null;
            }
            this.ctx.postAuthPath = this._detailTournamentId ? tournamentPath(this._detailTournamentId) : '/tournaments';
            this.ctx.toast.warning('Session expired — please sign in again.');
            this.ctx.router.go('/login');
            return null;
        }
        return token;
    }

    async _readRequest(message, options) {
        let result = await this.ctx.capability.request(message, options);
        if (result.live || !this.root) return result;
        // The first attempt already waited for its full timeout. Yield once so
        // teardown can win, then retry without installing a timer that could
        // be cancelled while this promise is awaiting it.
        await Promise.resolve();
        if (!this.root) return result;
        result = await this.ctx.capability.request(message, options);
        return result;
    }

    _field(id, label, type, value, attrs = {}) {
        const error = h('div', { class: 'field__error', id: `${id}-error`, hidden: true });
        const input = h('input', { class: 'input', id, type, value, ...attrs, 'aria-describedby': `${id}-error` });
        return { input, error, node: h('div', { class: 'field' },
            h('label', { class: 'field__label', htmlFor: id }, label), input, error) };
    }

    _openCreate() {
        const now = Date.now();
        const formatInput = h('select', { class: 'input', id: 't-format' },
            h('option', { value: 'swiss' }, 'Swiss — fixed rounds'),
            h('option', { value: 'winners_advance' }, 'Winners advance — automatic rounds'));
        const formatField = h('div', { class: 'field' }, h('label', { class: 'field__label', htmlFor: 't-format' }, 'Tournament format'), formatInput,
            h('div', { class: 'field__help' }, 'Winners advance: replay the first draw; after two draws both advance. Final ranking uses game wins, excluding byes. Equal wins share a rank.'));
        const fields = {
            name: this._field('t-name', 'Name', 'text', '', { maxlength: 128, autocomplete: 'off' }),
            rounds: this._field('t-rounds', 'Swiss rounds', 'number', '5', { min: 1, max: 30, step: 1 }),
            base: this._field('t-base', 'Base time (seconds)', 'number', '300', { min: 1, step: 1 }),
            inc: this._field('t-inc', 'Increment (seconds)', 'number', '3', { min: 0, step: 1 }),
            registrationDeadline: this._field('t-deadline', 'Registration deadline', 'datetime-local', toLocalDateTimeInput(now + 60 * 60_000)),
            firstRoundStartsAt: this._field('t-start', 'First round starts', 'datetime-local', toLocalDateTimeInput(now + 65 * 60_000)),
            roundMinutes: this._field('t-window', 'Round spacing / check-in window (minutes)', 'number', '60', { min: 1, step: 1 }),
        };
        const summaryMessages = h('div');
        const summary = h('div', { class: 'form-error-summary', role: 'alert', tabindex: '-1', hidden: true },
            h('div', { class: 'form-error-summary__title' }, 'Check the highlighted settings.'), summaryMessages);
        const preview = h('div', { class: 'schedule-preview', role: 'status', 'aria-live': 'polite' });
        const values = () => ({
            name: fields.name.input.value, format: formatInput.value, rounds: fields.rounds.input.value,
            base: fields.base.input.value, inc: fields.inc.input.value,
            registrationDeadline: fields.registrationDeadline.input.value,
            firstRoundStartsAt: fields.firstRoundStartsAt.input.value,
            roundMinutes: fields.roundMinutes.input.value,
        });
        const validate = (focusSummary = false) => {
            const result = validateTournamentSchedule(values());
            for (const [key, field] of Object.entries(fields)) {
                const message = result.errors[key] || '';
                field.error.textContent = message; field.error.hidden = !message;
                field.input.setAttribute('aria-invalid', message ? 'true' : 'false');
            }
            summary.hidden = result.valid; clear(summaryMessages);
            if (!result.valid) {
                summaryMessages.appendChild(h('div', {}, Object.values(result.errors).join(' ')));
                if (focusSummary) summary.focus();
            }
            const deadline = new Date(fields.registrationDeadline.input.value);
            const start = new Date(fields.firstRoundStartsAt.input.value);
            preview.textContent = Number.isFinite(deadline.getTime()) && Number.isFinite(start.getTime())
                ? `Local preview: registration closes ${deadline.toLocaleString()}; round one starts ${start.toLocaleString()}. UTC is sent to the server.`
                : 'Choose both schedule times to see the local preview.';
            return result;
        };
        for (const field of Object.values(fields)) field.input.addEventListener('blur', () => validate(false));
        fields.registrationDeadline.input.addEventListener('input', () => validate(false));
        fields.firstRoundStartsAt.input.addEventListener('input', () => validate(false));
        const syncFormatFields = () => {
            fields.rounds.node.hidden = formatInput.value === 'winners_advance';
            fields.rounds.input.disabled = fields.rounds.node.hidden;
            validate(false);
        };
        formatInput.addEventListener('change', syncFormatFields);
        let dlg;
        const createButton = h('button', { class: 'btn btn--primary', onclick: async () => {
            const result = validate(true); if (!result.valid) return;
            createButton.disabled = true; createButton.textContent = 'Creating…';
            const tournamentId = await this._create(result.value);
            if (tournamentId) { dlg.close(); this.ctx.router.go(tournamentPath(tournamentId)); }
            else { createButton.disabled = false; createButton.textContent = 'Create tournament'; }
        } }, 'Create tournament');
        const body = h('div', { class: 'tournament-create' }, summary,
            h('div', { class: 'tournament-form-grid' },
                h('div', { class: 'tournament-form-grid__wide' }, fields.name.node),
                h('div', { class: 'tournament-form-grid__wide' }, formatField),
                fields.rounds.node, fields.base.node, fields.inc.node, fields.roundMinutes.node,
                fields.registrationDeadline.node, fields.firstRoundStartsAt.node), preview);
        dlg = this.ctx.modal.open({
            title: 'Create scheduled tournament', subtitle: 'All dates are entered in your local timezone.',
            className: 'modal--wide', body,
            footer: h('div', { class: 'button-row' },
                h('button', { class: 'btn btn--ghost', onclick: () => dlg.close() }, 'Cancel'), createButton),
            initialFocus: fields.name.input, dismissible: true,
        });
        syncFormatFields();
    }

    async _create(value) {
        const token = await this._tokenOrLogin(); if (!token) return false;
        const result = await this.ctx.capability.request(
            this.ctx.Outbound.createTournament(token, value.name, value.rounds, value.base, value.inc, value),
            { expect: 'tournament_created', timeout: 5000, demo: () => ({}), failOnError: true });
        if (!result.live) { this._showError(result.error, 'Tournament could not be created.'); return false; }
        this.ctx.toast.success('Tournament created. Registration is open.');
        await this._load();
        return Number(result.data.tournamentId);
    }

    async _openState(summary) {
        this._detailTournamentId = Number(summary.id);
        this._detailBody = h('div', {}, this._loading('Loading tournament…'));
        this._detailFooter = h('div', { class: 'button-row' });
        this._detailDialog = this.ctx.modal.open({
            title: summary.name || `Tournament #${summary.id}`,
            subtitle: 'Live schedule, check-in, games, and standings', className: 'modal--wide',
            body: this._detailBody, footer: this._detailFooter, dismissible: !this._targetTournamentId,
        });
        await this._refreshDetail(false);
    }

    async _refreshDetail(silent = false) {
        if (!this._detailTournamentId || this._detailLoading) return;
        this._detailLoading = true;
        const requestedId = Number(this._detailTournamentId);
        const result = await this._readRequest(this.ctx.Outbound.tournamentState(requestedId),
            { expect: 'tournament_state', timeout: 5000, demo: () => ({}), failOnError: true });
        this._detailLoading = false;
        if (!this.root || Number(this._detailTournamentId) !== requestedId) return;
        if (!result.live) { if (!silent) this._renderDetailError(result.error); return; }
        if (this._joiningTournamentId === requestedId && this._identity(result.data).joined) {
            this._joiningTournamentId = null;
            this.ctx.toast.success('Tournament registration confirmed.');
        }
        if (silent && this._detailState
            && JSON.stringify(this._detailState) === JSON.stringify(result.data)) {
            this._updateCountdown();
            return;
        }
        this._detailState = result.data; this._renderDetail();
    }

    _renderDetailError(error) {
        clear(this._detailBody);
        const message = tournamentErrorMessage(error && error.message, 'Tournament details could not be loaded.');
        this._detailBody.appendChild(h('div', { class: 'empty' },
            h('div', { class: 'empty__title' }, 'Details unavailable'), h('div', { class: 'empty__sub' }, message),
            h('button', { class: 'btn', onclick: () => this._refreshDetail(false) }, 'Try again')));
    }

    _identity(state) {
        const username = String(this.ctx.session && this.ctx.session.username || '').toLocaleLowerCase();
        const t = state.tournament;
        const standing = (state.standings || []).find(row => String(row.username || '').toLocaleLowerCase() === username);
        return { username, signedIn: !!(this.ctx.session && this.ctx.session.isAuthenticated),
            joined: !!standing && !standing.withdrawn, standing,
            isCreator: !!username && String(t.createdByUsername || '').toLocaleLowerCase() === username };
    }

    _activeRound(state) {
        return (state.rounds || []).find(r => r.status === 'live')
            || (state.rounds || []).find(r => r.status === 'scheduled') || null;
    }

    _renderDetail() {
        const state = this._detailState; if (!state) return;
        const t = state.tournament; const who = this._identity(state); const round = this._activeRound(state);
        clear(this._detailBody); clear(this._detailFooter);
        const tabs = ['overview', 'standings', 'pairings'].map(key => h('button', {
            class: 'tabs__tab' + (this._detailTab === key ? ' is-active' : ''),
            'aria-pressed': this._detailTab === key ? 'true' : 'false',
            onclick: () => { this._detailTab = key; this._renderDetail(); },
        }, key[0].toUpperCase() + key.slice(1)));
        this._detailBody.appendChild(h('div', { class: 'tournament-detail' },
            this._detailHero(state, who, round),
            h('div', { class: 'tabs tournament-tabs', role: 'tablist', 'aria-label': 'Tournament details' }, ...tabs),
            this._detailTab === 'overview' ? this._overview(state, who, round)
                : this._detailTab === 'standings' ? this._standingsTable(state, round)
                    : this._pairingsTable(state, who)));
        if (t.status === 'registration' && t.registrationOpen && who.joined) {
            this._detailFooter.appendChild(h('button', { class: 'btn btn--danger', onclick: () => this._leave(t) }, 'Unregister'));
        } else if (t.status === 'registration' && t.registrationOpen && who.signedIn && !who.joined) {
            const joining = this._joiningTournamentId === Number(t.id);
            this._detailFooter.appendChild(h('button', { class: 'btn btn--primary', disabled: joining,
                'aria-busy': joining ? 'true' : 'false', onclick: () => this._join(t) }, joining ? 'Confirming registration…' : 'Register'));
        } else if (!who.signedIn && t.status === 'registration' && t.registrationOpen) {
            this._detailFooter.appendChild(h('button', { class: 'btn btn--primary', onclick: () => {
                this.ctx.postAuthPath = tournamentPath(t.id); this._detailDialog.close(); this.ctx.router.go('/login');
            } }, 'Sign in to register'));
        }
        const deadlineHasNotPassed = Date.now() / 1000 < Number(t.registrationDeadline || 0);
        if (who.isCreator && (t.status === 'registration' || t.status === 'scheduled')
            && (t.registrationOpen || deadlineHasNotPassed)) {
            this._detailFooter.appendChild(h('button', { class: 'btn', onclick: () => this._setRegistration(t, !t.registrationOpen) },
                t.registrationOpen ? 'Close registration' : 'Reopen registration'));
        }
        if (who.joined && canJoinTournamentRound(state, who.standing, round)
            && Date.now() / 1000 <= round.checkInClosesAt) {
            const checked = this._isCheckedIn(state, round.round, who.username);
            this._detailFooter.appendChild(h('button', { class: 'btn btn--primary',
                onclick: event => this._checkIn(state, round, event.currentTarget) }, checked ? 'Rejoin round' : 'Join round'));
        }
        this._detailFooter.appendChild(h('button', { class: 'btn btn--ghost', onclick: () => this._openShare(t) }, 'Share'));
        this._detailFooter.appendChild(h('button', { class: 'btn', onclick: () => {
            this._detailDialog.close(); this._detailDialog = null; this._detailTournamentId = null;
            if (this._targetTournamentId) this.ctx.router.go('/tournaments');
        } }, 'Close'));
        this._updateCountdown();
    }

    _detailHero(state, who, round) {
        const t = state.tournament;
        const target = t.status === 'registration' ? t.registrationDeadline
            : round && round.status === 'scheduled' ? round.earliestStartAt
                : round && round.status === 'live' ? round.checkInClosesAt : 0;
        const label = t.status === 'registration' ? 'Registration closes'
            : round && round.status === 'scheduled' ? `Round ${round.round} starts`
                : round && round.status === 'live' ? `Round ${round.round} check-in closes` : 'Tournament complete';
        return h('section', { class: 'tournament-hero' },
            h('div', { class: 'tournament-hero__top' },
                h('div', {}, this._statusBadge(t.status, t.registrationOpen),
                    h('h2', { class: 'tournament-hero__title' }, t.name),
                    h('div', { class: 'tournament-hero__sub' },
                        `Created by ${t.createdByUsername || 'Unknown player'} · ${state.standings.length} registered`)),
                h('div', { class: 'tournament-hero__membership' }, who.joined ? 'Registered' : who.signedIn ? 'Not registered' : 'Signed out')),
            h('div', { class: 'tournament-countdown', role: 'timer', 'aria-live': 'off', 'aria-atomic': 'true',
                dataset: { deadline: target, label }, ref: el => { this._countdownEl = el; } }, label));
    }

    _overview(state, who, round) {
        const t = state.tournament;
        return h('div', { class: 'tournament-overview' },
            h('div', { class: 'tournament-summary-grid' },
                this._summary('Time control', formatTimeControl(t.timeBase, t.timeInc)),
                this._summary('Format', t.format === 'winners_advance' ? 'Winners advance' : 'Swiss'),
                this._summary('Rounds', t.format === 'winners_advance' ? `Stage ${t.currentRound || 1} · automatic` : `${t.currentRound}/${t.rounds}`),
                this._summary('Registration deadline', this._formatDate(t.registrationDeadline)),
                this._summary('First round', this._formatDate(t.firstRoundStartsAt)),
                this._summary('Round spacing', this._duration(t.roundDurationSeconds)),
                this._summary('Your round', !who.joined ? 'Not registered' : tournamentRoundStatus(state, who.standing, round))),
            this._sharePanel(t),
            h('div', { class: 'tournament-note' },
                (t.format === 'winners_advance'
                    ? 'Losers are eliminated; winners and bye recipients advance. A first draw replays the same pair with colors reversed; after two draws both advance and cannot meet again. When no legal game remains, play ends. Final ranking is by game wins only, excluding byes; equal wins share a rank. Other pairs minimize rating distance. '
                    : 'Pairings avoid rematches and minimize the total rating difference across the round. Scores break ties. ')
                + 'With an odd player count, the lowest-ranked player who has not had a bye sits out and receives one point. '
                + 'Games update standings automatically; the next round waits for all games to finish.'));
    }

    _summary(label, value) {
        return h('div', { class: 'tournament-summary' },
            h('div', { class: 'tournament-summary__label' }, label),
            h('div', { class: 'tournament-summary__value' }, value));
    }

    _standingsTable(state, round) {
        const rows = state.standings || [];
        const elimination = state.tournament.format === 'winners_advance';
        if (!rows.length) return h('div', { class: 'empty' }, 'No registered players yet.');
        const bodyRows = rows.map((row, index) => {
            const status = tournamentRoundStatus(state, row, round);
            return h('tr', {}, h('td', { class: 'mono' }, String(elimination ? row.rank : index + 1)), h('td', {}, row.username || 'Unknown player'),
                h('td', { class: 'mono' }, String(row.elo)), h('td', { class: 'mono' }, String(elimination ? row.roundWins : row.score)),
                h('td', { class: 'mono' }, String(elimination ? row.roundDraws : row.buchholz)),
                h('td', {}, this._smallStatus(status, ['Won', 'Checked in', 'Game assigned'].includes(status) ? 'success' : 'muted')));
        });
        return h('div', { class: 'table-wrap' }, h('table', { class: 'table' },
            h('thead', {}, h('tr', {}, h('th', {}, '#'), h('th', {}, 'Player'), h('th', {}, 'Rating'),
                h('th', {}, elimination ? 'Wins' : 'Score'), h('th', {}, elimination ? 'Draws' : 'Buchholz'), h('th', {}, 'Current round'))), h('tbody', {}, ...bodyRows)));
    }

    _smallStatus(label, kind) {
        return h('span', { class: kind === 'success' ? 'badge badge--success' : 'badge' }, label);
    }

    _pairingsTable(state, who) {
        const rows = state.pairings || [];
        if (!rows.length) return h('div', { class: 'empty' }, 'Pairings appear when the scheduled round opens.');
        const bodyRows = rows.map(row => {
            const isMine = who.username && [row.whiteUsername, row.blackUsername]
                .some(name => String(name || '').toLocaleLowerCase() === who.username);
            const action = row.result === 'pending' && row.gameId != null
                ? isMine ? h('button', { class: 'btn btn--sm btn--primary', onclick: event => {
                    const round = (state.rounds || []).find(r => r.round === row.round);
                    if (round) void this._checkIn(state, round, event.currentTarget);
                } }, 'Open game') : h('a', { class: 'btn btn--sm', href: `#/spectate/${row.gameId}` }, 'Spectate')
                : row.result !== 'pending' && row.replayGameId != null
                    ? h('a', { class: 'btn btn--sm', href: `#/replay/${row.replayGameId}` }, 'Replay')
                    : '—';
            const correctionLocked = state.tournament.format === 'winners_advance'
                && (state.tournament.status === 'completed' || row.round < state.tournament.currentRound);
            const correction = who.isCreator && row.blackPlayerId != null && !correctionLocked
                ? h('button', { class: 'btn btn--sm btn--ghost', onclick: () => this._openOverride(state.tournament, row) }, 'Correct result') : null;
            return h('tr', {}, h('td', { class: 'mono' }, String(row.round)),
                h('td', {}, row.whiteUsername || 'Unknown player'),
                h('td', {}, row.blackPlayerId == null ? 'Bye' : (row.blackUsername || 'Unknown player')),
                h('td', {}, h('div', { class: 'pairing-result' }, h('span', { class: 'mono' }, row.result || 'pending'),
                    row.resultSource ? h('span', { class: 'field__hint' }, row.resultSource) : null, correction)),
                h('td', {}, action));
        });
        return h('div', { class: 'table-wrap' }, h('table', { class: 'table' },
            h('thead', {}, h('tr', {}, h('th', {}, 'Round'), h('th', {}, 'White'), h('th', {}, 'Black'),
                h('th', {}, 'Result'), h('th', {}, 'Game'))), h('tbody', {}, ...bodyRows)));
    }

    _isCheckedIn(state, round, username) {
        const key = String(username || '').toLocaleLowerCase();
        return (state.checkIns || []).some(c => c.round === round && String(c.username || '').toLocaleLowerCase() === key);
    }

    async _join(tournament) {
        if (this._joiningTournamentId) return;
        const token = await this._tokenOrLogin(); if (!token) return;
        if (this._joiningTournamentId) return;
        this._joiningTournamentId = Number(tournament.id);
        this._renderDetail();
        let result;
        try {
            result = await this.ctx.capability.request(this.ctx.Outbound.joinTournament(token, tournament.id),
                { expect: 'tournament_joined', timeout: 15000, demo: () => ({}), failOnError: true });
        } catch (_) { result = { live: false }; }
        if (!this.root) return;
        if (!this._joiningTournamentId && this._detailState && this._identity(this._detailState).joined) return;
        if (result.live) {
            // A concurrent authoritative poll may already have confirmed it.
            if (this._joiningTournamentId) this.ctx.toast.success(`Registered for “${tournament.name}”.`);
            this._joiningTournamentId = null;
        } else if (result.error) {
            this._joiningTournamentId = null;
            this._showError(result.error, 'Could not register.');
        } else {
            // Timeout/disconnect is an unknown outcome, not a rejected write.
            this.ctx.toast.warning('Registration is still being confirmed. Please wait; do not submit again.');
        }
        this._renderDetail();
        await this._refreshDetail(true);
    }

    async _leave(tournament) {
        // ModalHost owns one dialog at a time, so opening confirmation closes
        // the detail dialog. Reopen the authoritative detail view afterwards.
        this._detailDialog = null;
        const confirmed = await this.ctx.modal.confirm({ title: 'Unregister from tournament?',
            message: 'You can register again only while registration remains open.', confirmLabel: 'Unregister', danger: true });
        if (!confirmed) { await this._openState(tournament); return; }
        const token = await this._tokenOrLogin(); if (!token) return;
        const result = await this.ctx.capability.request(this.ctx.Outbound.leaveTournament(token, tournament.id),
            { expect: 'tournament_left', timeout: 5000, demo: () => ({}), failOnError: true });
        if (!result.live) { this._showError(result.error, 'Could not unregister.'); await this._openState(tournament); return; }
        this.ctx.toast.success('You are no longer registered.'); await this._openState(tournament);
    }

    async _setRegistration(tournament, open) {
        const token = await this._tokenOrLogin(); if (!token) return;
        const result = await this.ctx.capability.request(this.ctx.Outbound.setTournamentRegistration(token, tournament.id, open),
            { expect: 'tournament_registration_updated', timeout: 5000, demo: () => ({}), failOnError: true });
        if (!result.live) { this._showError(result.error, 'Registration could not be changed.'); return; }
        this.ctx.toast.success(open ? 'Registration reopened.' : 'Registration closed; the event is scheduled.');
        await this._load(); await this._refreshDetail(false);
    }

    async _checkIn(state, round, button) {
        const token = await this._tokenOrLogin(); if (!token) return;
        if (button) { button.disabled = true; button.textContent = 'Joining…'; }
        const result = await this.ctx.capability.request(
            this.ctx.Outbound.checkInTournamentRound(token, state.tournament.id, round.round),
            { expect: 'tournament_round_checked_in', timeout: 5000, demo: () => ({}), failOnError: true });
        if (!result.live) {
            if (button) { button.disabled = false; button.textContent = 'Join round'; }
            this._showError(result.error, 'Could not join this round.'); return;
        }
        if (result.data.roomReady && result.data.gameId) { this._enterGame(result.data, state, round.round); return; }
        this.ctx.toast.success('Checked in. Your reserved game will open at the scheduled start.');
        await this._refreshDetail(false);
    }

    _onTournamentReady(message) {
        if (!message || !message.gameId || !this._detailState
            || Number(message.tournamentId) !== Number(this._detailState.tournament.id)) return;
        const pairing = (this._detailState.pairings || []).find(p => Number(p.id) === Number(message.pairingId));
        this._enterGame(message, this._detailState, pairing ? pairing.round : 0);
    }

    _enterGame(message, state, roundNumber) {
        const pairing = (state.pairings || []).find(p => Number(p.id) === Number(message.pairingId)
            || Number(p.gameId) === Number(message.gameId));
        const color = message.color === 'black' ? 'b' : 'w';
        const opponent = pairing ? (color === 'w' ? pairing.blackUsername : pairing.whiteUsername) : 'Opponent';
        const t = state.tournament;
        this.ctx.store.setGame({ gameId: Number(message.gameId), color, opponent: opponent || 'Opponent', isAI: false,
            whiteMs: t.timeBase * 1000, blackMs: t.timeBase * 1000, timeBaseSec: t.timeBase, timeIncSec: t.timeInc,
            tournamentId: Number(t.id), pairingId: pairing ? Number(pairing.id) : Number(message.pairingId || 0),
            tournamentName: t.name, tournamentRound: roundNumber || (pairing && pairing.round) || t.currentRound });
        if (this._detailDialog) this._detailDialog.close();
        this._detailDialog = null; this.ctx.router.go('/game/' + Number(message.gameId));
    }

    _openOverride(tournament, pairing) {
        this._detailDialog = null;
        let selected = pairing.result === 'pending' || pairing.result === 'double_forfeit' ? '1-0' : pairing.result;
        let reason = '';
        const reasonError = h('div', { class: 'field__error', hidden: true }, 'Enter a correction reason.');
        const reasonInput = h('textarea', { class: 'input tournament-reason', rows: 3, maxlength: 500,
            'aria-label': 'Correction reason', oninput: e => { reason = e.target.value; } });
        let dlg;
        const save = h('button', { class: 'btn btn--danger', onclick: async () => {
            if (!reason.trim()) { reasonError.hidden = false; reasonInput.setAttribute('aria-invalid', 'true'); reasonInput.focus(); return; }
            save.disabled = true; save.textContent = 'Saving correction…';
            const ok = await this._reportResult(tournament, pairing, selected, reason.trim());
            if (ok) { dlg.close(); await this._openState(tournament); }
            else { save.disabled = false; save.textContent = 'Save correction'; }
        } }, 'Save correction');
        dlg = this.ctx.modal.open({ title: 'Correct pairing result',
            subtitle: `${pairing.whiteUsername} vs ${pairing.blackUsername}`,
            body: h('div', { class: 'field-stack' },
                h('div', { class: 'tournament-warning', role: 'note' },
                    'This changes tournament standings and Buchholz only. It does not change the saved replay, player ratings, or profile records.'),
                h('div', { class: 'field' }, h('label', { class: 'field__label', htmlFor: 'override-result' }, 'Correct result'),
                    h('select', { class: 'input', id: 'override-result', onchange: e => { selected = e.target.value; } },
                        h('option', { value: '1-0', selected: selected === '1-0' }, '1-0 · White wins'),
                        h('option', { value: '0-1', selected: selected === '0-1' }, '0-1 · Black wins'),
                        h('option', { value: '1/2-1/2', selected: selected === '1/2-1/2' }, '½-½ · Draw'),
                        h('option', { value: 'double_forfeit', selected: selected === 'double_forfeit' }, 'Double forfeit'))),
                h('div', { class: 'field' }, h('label', { class: 'field__label' }, 'Mandatory reason'), reasonInput, reasonError)),
            footer: h('div', { class: 'button-row' },
                h('button', { class: 'btn btn--ghost', onclick: () => { dlg.close(); void this._openState(tournament); } }, 'Cancel'), save),
            initialFocus: reasonInput, dismissible: false });
    }

    async _reportResult(tournament, pairing, result, reason) {
        const token = await this._tokenOrLogin(); if (!token) return false;
        const response = await this.ctx.capability.request(
            this.ctx.Outbound.reportTournamentResult(token, pairing.id, result, reason),
            { expect: 'tournament_result_recorded', timeout: 5000, demo: () => ({}), failOnError: true });
        if (!response.live) { this._showError(response.error, 'Result correction could not be saved.'); return false; }
        this.ctx.toast.success('Tournament standings corrected; replay and rating were not changed.');
        return true;
    }

    _openShare(tournament) {
        const returnToDetail = !!this._detailDialog;
        if (returnToDetail) this._detailDialog = null;
        let dlg;
        dlg = this.ctx.modal.open({ title: 'Share tournament', subtitle: tournament.name,
            body: this._sharePanel(tournament), footer: h('button', { class: 'btn', onclick: () => {
                dlg.close(); if (returnToDetail) void this._openState(tournament);
            } }, 'Close'),
            dismissible: !returnToDetail });
    }

    _sharePanel(tournament) {
        const link = absoluteUrl('#' + tournamentPath(tournament.id));
        let linkInput = null;
        return h('div', { class: 'invite-panel tournament-invite' }, h('div', { class: 'invite-panel__body' },
            h('div', {}, h('div', { class: 'invite-panel__title' }, 'Tournament invite link'),
                h('div', { class: 'invite-panel__sub' }, 'Anyone can open the schedule; signing in is required to register.')),
            h('div', { class: 'invite-row' },
                h('input', { class: 'input mono invite-row__input', readonly: true, value: link,
                    'aria-label': 'Tournament invite link', ref: el => { linkInput = el; }, onclick: e => e.target.select() }),
                h('button', { class: 'btn btn--sm btn--primary', onclick: async () => {
                    const copied = await copyText(link);
                    if (copied.ok) this.ctx.toast.success('Tournament link copied.', { duration: 1800 });
                    else { if (linkInput) { linkInput.focus(); linkInput.select(); }
                        this.ctx.toast.warning('Copy was blocked. The link is selected — press Ctrl+C.', { duration: 4200 }); }
                } }, 'Copy link'))));
    }

    _updateCountdown() {
        const el = this._countdownEl; if (!el || !el.dataset) return;
        const deadline = Number(el.dataset.deadline || 0) * 1000;
        const label = el.dataset.label || '';
        if (!deadline) { el.textContent = label; return; }
        const remaining = deadline - Date.now();
        el.textContent = remaining <= 0 ? `${label}: now` : `${label} in ${this._countdown(remaining)}`;
    }

    _countdown(ms) {
        const total = Math.max(0, Math.ceil(ms / 1000));
        const days = Math.floor(total / 86400); const hours = Math.floor((total % 86400) / 3600);
        const minutes = Math.floor((total % 3600) / 60); const seconds = total % 60;
        if (days) return `${days}d ${hours}h`;
        if (hours) return `${hours}h ${minutes}m`;
        return `${minutes}m ${String(seconds).padStart(2, '0')}s`;
    }

    _formatDate(unix) {
        const value = Number(unix || 0);
        return value ? new Date(value * 1000).toLocaleString() : 'Not scheduled';
    }

    _duration(seconds) {
        const value = Number(seconds || 0);
        if (value % 3600 === 0) return `${value / 3600} hour${value === 3600 ? '' : 's'}`;
        return `${Math.round(value / 60)} minutes`;
    }

    _showError(error, fallback) {
        const message = error && error.message ? error.message : '';
        this.ctx.toast.error(tournamentErrorMessage(message, fallback), { duration: 4200 });
    }
}
