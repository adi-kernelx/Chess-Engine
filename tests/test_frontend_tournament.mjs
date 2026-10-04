import assert from 'node:assert/strict';

globalThis.location = {
    origin: 'http://localhost:8000',
    pathname: '/',
};

const { Inbound, Outbound } = await import('../frontend/js/net/protocol.js');
const {
    tournamentErrorMessage,
    tournamentPath,
    validateTournamentSchedule,
    TournamentScreen,
    canJoinTournamentRound,
    tournamentRoundStatus,
} = await import('../frontend/js/screens/tournament.js');

const state = Inbound.normalize({
    type: 'tournament_state',
    tournament: {
        id: 42, name: 'Local Swiss', format: 'swiss', rounds: 2,
        current_round: 1, time_base: 180, time_inc: 2,
        status: 'in_progress', created_by: 6, created_by_username: 'Alice',
    },
    standings: [{
        player_id: 6, username: 'Alice', elo: 827, score: 1,
        buchholz: 0, whites_played: 1, withdrawn: true,
    }],
    pairings: [{
        id: 9, round: 1,
        white_player_id: 6, white_username: 'Alice',
        black_player_id: 3, black_username: 'Bob',
        game_id: 77, replay_game_id: 501, result: 'pending',
    }],
    check_ins: [{ round: 1, player_id: 6, username: 'Alice' }],
});

assert.equal(state.standings[0].username, 'Alice');
assert.equal(state.tournament.createdByUsername, 'Alice');
assert.equal(state.pairings[0].whiteUsername, 'Alice');
assert.equal(state.pairings[0].blackUsername, 'Bob');
assert.equal(state.pairings[0].gameId, 77);
assert.equal(state.pairings[0].replayGameId, 501);
assert.equal(state.standings[0].withdrawn, true);
assert.deepEqual(state.checkIns[0], { round: 1, playerId: 6, username: 'Alice' });
assert.deepEqual(Outbound.leaveTournament('token', 42), {
    type: 'leave_tournament', access_token: 'token', tournament_id: 42,
});

const now = Date.parse('2026-10-01T10:00:00Z');
const valid = validateTournamentSchedule({
    name: 'Scheduled Swiss', rounds: 3, base: 180, inc: 2, roundMinutes: 15,
    registrationDeadline: '2026-10-01T10:05:00Z',
    firstRoundStartsAt: '2026-10-01T10:06:00Z',
}, now);
assert.equal(valid.valid, true);
assert.equal(valid.value.roundDurationSeconds, 900);
const invalid = validateTournamentSchedule({
    name: '', rounds: 0, base: 0, inc: -1, roundMinutes: 0,
    registrationDeadline: '2026-10-01T09:59:00Z',
    firstRoundStartsAt: '2026-10-01T10:00:00Z',
}, now);
assert.equal(invalid.valid, false);
assert.ok(invalid.errors.name);
assert.ok(invalid.errors.registrationDeadline);

const active = Inbound.normalize({ type: 'active_game', game: {
    game_id: 77, color: 'black', opponent: 'Alice', white_time: 180000,
    black_time: 180000, time_base: 180, time_inc: 2,
    tournament_id: 42, pairing_id: 9,
} });
assert.equal(active.game.tournamentId, 42);
assert.equal(active.game.pairingId, 9);
assert.equal(tournamentPath(42), '/tournaments/42');
assert.equal(tournamentPath('bad'), '/tournaments');
assert.equal(
    tournamentErrorMessage('not_creator', 'Could not start.'),
    'Only the tournament creator can change tournament controls.',
);
assert.equal(
    tournamentErrorMessage('not_enough_players', 'Could not start.'),
    'At least two players must register before registration can close.',
);
assert.equal(
    tournamentErrorMessage('', 'Tournament action failed.'),
    'Tournament action failed.',
);

console.log('frontend tournament normalization and copy tests passed');

const automatic = validateTournamentSchedule({
    name: 'Advance', format: 'winners_advance', rounds: '', base: 60, inc: 0, roundMinutes: 1,
    registrationDeadline: '2026-10-01T10:05:00Z', firstRoundStartsAt: '2026-10-01T10:06:00Z',
}, now);
assert.equal(automatic.valid, true);
assert.equal(automatic.value.rounds, 1);
assert.equal(Outbound.createTournament('token', 'Advance', 1, 60, 0, automatic.value).format, 'winners_advance');
assert.equal(Outbound.createTournament('token', 'Swiss', 3, 60, 0).format, 'swiss');

const elimination = { tournament: { format: 'winners_advance', status: 'in_progress', currentRound: 2 },
    pairings: [{ round: 1, whitePlayerId: 1, blackPlayerId: 2, result: '1-0' }], checkIns: [] };
assert.equal(tournamentRoundStatus(elimination, { playerId: 2 }, { round: 2 }), 'Eliminated');
assert.equal(canJoinTournamentRound(elimination, { playerId: 2 }, { round: 2 }), false);
elimination.tournament.status = 'completed';
assert.equal(tournamentRoundStatus(elimination, { playerId: 1, rank: 1 }, null), 'Champion');
elimination.pairings[0].result = '1/2-1/2';
assert.equal(tournamentRoundStatus(elimination, { playerId: 2, rank: 1 }, null), 'Champion');
assert.equal(tournamentRoundStatus(elimination, { playerId: 2, rank: 2 }, null), 'Finished');

// No browser interaction: exercise asynchronous write outcomes and duplicate
// submission guards using the real screen method with rendering stubbed.
for (const outcome of [{ live: false }, { live: false, error: { message: 'registration_closed' } }, { live: true }]) {
    let sends = 0, successes = 0, warnings = 0, errors = 0;
    let resolveRequest;
    const screen = new TournamentScreen({
        session: { accessTokenForRequest: async () => 'token' }, Outbound,
        capability: { request: () => { sends++; return new Promise(resolve => resolveRequest = resolve); } },
        toast: { success: () => successes++, warning: () => warnings++ },
    });
    screen.root = {};
    screen._renderDetail = () => {};
    screen._refreshDetail = async () => {};
    screen._showError = () => errors++;
    const request = screen._join({ id: 42, name: 'Test' });
    await Promise.resolve();
    await Promise.resolve();
    await screen._join({ id: 42, name: 'Test' });
    assert.equal(sends, 1);
    resolveRequest(outcome);
    await request;
    assert.equal(errors, outcome.error ? 1 : 0);
    assert.equal(warnings, !outcome.live && !outcome.error ? 1 : 0);
    assert.equal(successes, outcome.live ? 1 : 0);
    assert.equal(screen._joiningTournamentId, !outcome.live && !outcome.error ? 42 : null);
}
console.log('frontend format choice, elimination outcomes and pending registration tests passed');

{
    let confirmed = 0;
    const screen = new TournamentScreen({
        session: { username: 'Alice', isAuthenticated: true },
        Outbound, toast: { success: () => confirmed++ },
    });
    screen.root = {};
    screen._detailTournamentId = 42;
    screen._joiningTournamentId = 42;
    screen._renderDetail = () => {};
    screen._readRequest = async () => ({ live: true, data: {
        ...state, standings: state.standings.map(row => ({ ...row, withdrawn: false })),
    } });
    await screen._refreshDetail(true);
    assert.equal(screen._joiningTournamentId, null);
    assert.equal(confirmed, 1);
}
const ranked = Inbound.normalize({ type: 'tournament_state', tournament: { format: 'winners_advance' },
    standings: [{ player_id: 1, username: 'A', round_wins: 3, round_draws: 2, rank: 1 }] });
assert.equal(ranked.standings[0].roundWins, 3);
assert.equal(ranked.standings[0].roundDraws, 2);
assert.equal(ranked.standings[0].rank, 1);
console.log('frontend authoritative registration confirmation and ranking contract tests passed');

const { tournamentRoundHasStarted, tournamentRegistrationCutoff } = await import('../frontend/js/screens/tournament.js');
assert.equal(tournamentRegistrationCutoff({ registrationDeadline: 1180, firstRoundStartsAt: 1200 }), 1110);
assert.equal(tournamentRoundHasStarted({ status: 'scheduled', earliestStartAt: 1200 }, 1201), false);
assert.equal(tournamentRoundHasStarted({ status: 'live', earliestStartAt: 1200 }, 1199), false);
assert.equal(tournamentRoundHasStarted({ status: 'live', earliestStartAt: 1200 }, 1200), true);

for (const action of ['registration', 'check-in']) {
    for (const outcome of [{ live: false }, { live: false, error: { message: 'pairings_locked' } },
        { live: true, data: { roomReady: true, gameId: 999 } }]) {
        let sends = 0, errors = 0, warnings = 0, entries = 0;
        let resolveRequest;
        const screen = new TournamentScreen({ Outbound,
            session: { accessTokenForRequest: async () => 'token' },
            capability: { request: (_message, options) => {
                assert.equal(options.correlated, true);
                sends++; return new Promise(resolve => resolveRequest = resolve);
            } }, toast: { success() {}, warning: () => warnings++ },
        });
        screen.root = {};
        screen._renderDetail = () => {};
        screen._refreshDetail = async () => {};
        screen._load = async () => {};
        screen._showError = () => errors++;
        screen._enterGame = () => entries++;
        const run = () => action === 'registration' ? screen._setRegistration({ id: 42 }, false)
            : screen._checkIn({ tournament: { id: 42 } }, { round: 1, status: 'scheduled', earliestStartAt: Date.now() / 1000 + 90 }, {});
        const pending = run();
        await Promise.resolve(); await Promise.resolve();
        await run();
        assert.equal(sends, 1);
        resolveRequest(outcome); await pending;
        assert.equal(errors, outcome.error ? 1 : 0);
        assert.equal(warnings, !outcome.live && !outcome.error ? 1 : 0);
        assert.equal(entries, 0, 'an early room-ready response must not enter the board');
        assert.equal(!!(action === 'registration' ? screen._registrationPending : screen._checkInPending), !outcome.live && !outcome.error);
    }
}
console.log('PASS: registration/check-in pending feedback, duplicate guards and early-entry rejection');
