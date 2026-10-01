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
        game_id: null, result: 'pending',
    }],
    check_ins: [{ round: 1, player_id: 6, username: 'Alice' }],
});

assert.equal(state.standings[0].username, 'Alice');
assert.equal(state.tournament.createdByUsername, 'Alice');
assert.equal(state.pairings[0].whiteUsername, 'Alice');
assert.equal(state.pairings[0].blackUsername, 'Bob');
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
