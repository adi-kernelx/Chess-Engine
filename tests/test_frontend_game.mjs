import assert from 'node:assert/strict';

class TestNode {
    constructor(text = '') { this.text = text; this.children = []; this.style = {}; this.dataset = {}; }
    get textContent() { return this.text + this.children.map(c => c.textContent).join(''); }
    set textContent(value) { this.text = value; this.children = []; }
    get firstChild() { return this.children[0]; }
    appendChild(node) { this.children.push(node); }
    removeChild(node) { this.children.splice(this.children.indexOf(node), 1); }
    setAttribute() {}
    addEventListener() {}
}
globalThis.Node = TestNode;
globalThis.document = { createElement: () => new TestNode(), createTextNode: text => new TestNode(text) };
globalThis.location = { origin: 'http://localhost:8000', pathname: '/' };
const { GameScreen } = await import('../frontend/js/screens/game.js');
const { Inbound } = await import('../frontend/js/net/protocol.js');
const { formatTimeControl } = await import('../frontend/js/core/format.js');
const { tournamentRoundStatus, canJoinTournamentRound } = await import('../frontend/js/screens/tournament.js');

const store = { game: { gameId: 77 }, setGame(value) { this.game = value; } };
const screen = new GameScreen({ store });
screen._gameId = 77;
screen._myColor = 'w';
screen._tournamentId = 1;
screen._state = 'waiting';
screen._actionsEl = new TestNode();
screen._oppNameEl = new TestNode();
screen._oppAvatarEl = new TestNode();
screen._renderActions();
assert.match(screen._actionsEl.textContent, /Waiting for both players/);
assert.doesNotMatch(screen._actionsEl.textContent, /Open replay/);
let requests = 0;
screen._requestState = () => requests++;
screen._onTournamentReady({ gameId: 99, state: 'in_progress' });
assert.equal(requests, 0);
screen._onTournamentReady({ gameId: 77, state: 'in_progress' });
assert.equal(requests, 1);
for (const name of ['_syncDrawOffer', '_reanchorClock', '_maybeHighlightCheck', '_renderCaptured', '_renderMoveList', '_updateStatus']) screen[name] = () => {};
screen.renderer = { setPosition() {} };
screen.interaction = { setEnabled(value) { this.enabled = value; } };
const state = Inbound.normalize({ type: 'game_state', game_id: 77, state: 'in_progress',
    fen: 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
    white_username: 'Alice', black_username: 'Bob', moves: [], white_time: 60000, black_time: 60000 });
screen._onGameState(state);
assert.equal(screen.interaction.enabled, true);
assert.equal(screen._oppNameEl.textContent, 'Bob');
assert.equal(store.game.opponent, 'Bob');
assert.match(screen._actionsEl.textContent, /Resign/);
assert.match(screen._actionsEl.textContent, /Offer draw/);
screen._onGameState({ ...state, gameId: 999, state: 'waiting' });
assert.equal(screen._state, 'in_progress');
assert.equal(formatTimeControl(200, 3), '3m 20s+3');
assert.equal(formatTimeControl(180, 0), '3+0');
assert.equal(formatTimeControl(30, 0), '30s+0');
const tournament = { tournament: { currentRound: 1, status: 'completed' },
    pairings: [{ round: 1, whitePlayerId: 1, blackPlayerId: 2, result: '1-0' }], checkIns: [] };
assert.equal(tournamentRoundStatus(tournament, { playerId: 1 }, null), 'Won');
assert.equal(tournamentRoundStatus(tournament, { playerId: 2 }, null), 'Lost');
tournament.tournament.status = 'in_progress';
tournament.pairings[0].result = 'pending';
assert.equal(tournamentRoundStatus(tournament, { playerId: 1 }, { round: 1, status: 'scheduled' }), 'Round 1 upcoming');
assert.equal(tournamentRoundStatus(tournament, { playerId: 1 }, { round: 1, status: 'live', checkInClosesAt: 100 }, 101), 'Missed check-in');
tournament.pairings[0].result = 'bye';
tournament.pairings[0].blackPlayerId = null;
assert.equal(canJoinTournamentRound(tournament, { playerId: 1 }, { round: 1 }), false);
assert.equal(tournamentRoundStatus(tournament, { playerId: 1 }, { round: 1 }), 'Bye');
tournament.pairings[0].result = 'double_forfeit';
assert.equal(tournamentRoundStatus(tournament, { playerId: 1 }, { round: 1 }), 'Double forfeit');
console.log('PASS: shared tournament game transition, names, controls, snapshot isolation, round status and time labels');

const { SpectateWatchScreen } = await import('../frontend/js/screens/spectate.js');
const replies = [
    { live: false, error: { message: 'Game is not live' } },
    { live: true, data: { gameId: 77 } },
];
let tokens = 0, snapshots = 0, failures = [];
const watch = new SpectateWatchScreen({
    session: { accessTokenForRequest: async () => { tokens++; return 'fresh-token'; } },
    socket: { isConnected: () => true, on: () => () => {} },
    capability: { request: async request => {
        assert.equal(request.access_token, 'fresh-token'); return replies.shift();
    } },
    Outbound: { spectate: (token, id) => ({ access_token: token, game_id: id }) },
    toast: { error: message => failures.push(message) },
}, { gameId: '77' });
watch.root = {};
watch._status = new TestNode();
watch._applySnapshot = () => snapshots++;
watch.interval = () => {};
await watch._beginSpectating();
assert.match(watch._status.textContent, /Waiting for both players/);
assert.equal(watch._liveJoined, false);
assert.equal(watch._spectateTerminalError, undefined);
await watch._beginSpectating();
assert.equal(watch._liveJoined, true);
assert.equal(snapshots, 1);
assert.equal(tokens, 2);
assert.equal(failures.length, 0);
watch._liveJoined = false;
replies.push({ live: false, error: { message: 'You are already in a game (ID: 55)' } });
await watch._beginSpectating();
assert.equal(watch._spectateTerminalError, true);
assert.match(watch._status.textContent, /already in a game/);
assert.equal(failures.length, 1);
watch._spectateTerminalError = false;
watch.ctx.capability.request = async () => { throw new Error('connection interrupted'); };
await watch._beginSpectating();
assert.equal(watch._spectateStarting, false);
assert.match(watch._status.textContent, /Reconnecting/);
watch.ctx.session = { hasRefreshToken: true, accessTokenForRequest: async () => null };
await watch._beginSpectating();
assert.match(watch._status.textContent, /Restoring/);
let route;
watch.ctx.session = { hasRefreshToken: false, accessTokenForRequest: async () => null };
watch.ctx.router = { go: path => route = path };
watch.ctx.toast.warning = () => {};
await watch._beginSpectating();
assert.equal(route, '/login');
assert.equal(watch.ctx.postAuthPath, '/spectate/77');
console.log('PASS: spectator token recovery, waiting-room retry, successful join and truthful rejection');

// Import the composition module without booting UI. This checks all its ES
// module dependencies while preserving the user's manual UI-test workflow.
document.readyState = 'loading';
document.addEventListener = () => {};
await import('../frontend/js/main.js');
console.log('PASS: frontend composition module imports');
