/**
 * protocol.js — THE single source of truth for wire messages.
 *
 * Every outbound message is built through an Outbound factory here.
 * Nothing else in the app constructs message objects. Every factory
 * places `type` as the first key (mandatory — see socket.js header).
 *
 * Every inbound message is normalized through an Inbound normalizer,
 * so screens see clean shapes and the backend's inconsistencies
 * (`move_rejected.error` vs `error.message`, sec vs ms, empty host)
 * are absorbed in exactly one place.
 *
 * Message catalogue (Phases 1–6, live today):
 *   → create_game, join_game, make_move, resign, quick_play,
 *     cancel_queue, list_games, game_state, play_ai
 *   ← game_created, game_joined, game_start, move_made, move_rejected,
 *     game_over, game_list, game_state, queued, queue_cancelled,
 *     match_found, error
 *
 * Preview message types (Phase 7+) are added as their screens land.
 */

/* ────────────────────────────────────────────────────────────
   Outbound message factories.
   time_base / time_inc are SECONDS on the wire (backend expectation).
   ──────────────────────────────────────────────────────────── */

export const Outbound = {

    // ── Live: Phases 1–6 ────────────────────────────

    createGame(username, timeBaseSec, timeIncSec) {
        return { type: 'create_game', username, time_base: timeBaseSec, time_inc: timeIncSec };
    },

    joinGame(username, gameId) {
        return { type: 'join_game', username, game_id: gameId };
    },

    makeMove(from, to, promotion = null) {
        return promotion
            ? { type: 'make_move', from, to, promotion }
            : { type: 'make_move', from, to };
    },

    resign() { return { type: 'resign' }; },

    quickPlay(username, elo, timeBaseSec, timeIncSec) {
        return { type: 'quick_play', username, elo, time_base: timeBaseSec, time_inc: timeIncSec };
    },

    cancelQueue() { return { type: 'cancel_queue' }; },

    listGames()   { return { type: 'list_games' }; },

    gameState()   { return { type: 'game_state' }; },

    playAI(username, difficulty, timeBaseSec, timeIncSec) {
        return {
            type: 'play_ai',
            username,
            difficulty,                // 'easy' | 'medium' | 'hard' | 'max'
            time_base: timeBaseSec,
            time_inc:  timeIncSec,
        };
    },

    // ── Preview: Phase 7+ (backend TBD; capability.js falls back to demo) ──

    login(username, password)    { return { type: 'login',    username, password }; },
    register(username, password) { return { type: 'register', username, password }; },
    refresh(refreshToken)        { return { type: 'refresh',  refresh_token: refreshToken }; },
    logout(refreshToken)         { return { type: 'logout',   refresh_token: refreshToken }; },
    logoutAll(accessToken)       { return { type: 'logout_all', access_token: accessToken }; },
    googleAuth(supabaseJwt)      { return { type: 'google_auth', supabase_jwt: supabaseJwt }; },
    linkGoogle(accessToken, supabaseJwt) {
        return { type: 'link_google', access_token: accessToken, supabase_jwt: supabaseJwt };
    },
    unlinkGoogle(accessToken)    { return { type: 'unlink_google', access_token: accessToken }; },
    sealRequest()                { return { type: 'seal_request' }; },
    getProfile(username)         { return { type: 'get_profile', username }; },
    getHistory(username, limit = 20) { return { type: 'get_history', username, limit }; },
    leaderboard(limit = 100)     { return { type: 'leaderboard', limit }; },
    listLiveGames()              { return { type: 'list_live_games' }; },
    spectate(gameId)             { return { type: 'spectate', game_id: gameId }; },
    stopSpectating(gameId)       { return { type: 'stop_spectating', game_id: gameId }; },
    getGame(gameId)              { return { type: 'get_game', game_id: gameId }; },
    analyzePosition(fen, depth = 12) { return { type: 'analyze_position', fen, depth }; },
    listTournaments()            { return { type: 'list_tournaments' }; },
    joinTournament(tournamentId) { return { type: 'join_tournament', tournament_id: tournamentId }; },
    tournamentStandings(id)      { return { type: 'tournament_standings', tournament_id: id }; },
};

/* ────────────────────────────────────────────────────────────
   Inbound normalizers.
   Screens should read normalized fields, not raw ones.
   For messages we don't need to touch (yet), pass-through is fine.
   ──────────────────────────────────────────────────────────── */

export const Inbound = {

    /** Uniform wrapper — accepts a raw frame, returns a normalized object.
     *  Falls back to the raw frame for anything not listed. */
    normalize(raw) {
        if (!raw || !raw.type) return raw;
        const fn = normalizers[raw.type];
        return fn ? fn(raw) : raw;
    },
};

/** A player error (unknown command, invalid input, etc). */
const normError = (raw) => ({
    type: 'error',
    message: String(raw.message || 'Unknown server error'),
    isUnknownType: raw.message === 'Unknown message type',
});

/** Move rejection uses the key `error` instead of `message`. Normalize. */
const normMoveRejected = (raw) => ({
    type: 'move_rejected',
    reason: String(raw.error || 'Move rejected'),
});

/** game_list rows currently arrive with an empty `host` (backend bug).
 *  Substitute a friendly placeholder so the table isn't blank. */
const normGameList = (raw) => ({
    type: 'game_list',
    games: (raw.games || []).map(g => ({
        gameId: g.game_id,
        host: g.host && g.host.length > 0 ? g.host : `Player #${g.game_id}`,
        timeControl: g.time_control || '',
    })),
});

/** move_made has no "who moved" field — leave that to callers with turn parity. */
const normMoveMade = (raw) => ({
    type: 'move_made',
    from: raw.from,
    to:   raw.to,
    san:  raw.san,
    promotion: raw.promotion || null,
    whiteMs: Number(raw.white_time),
    blackMs: Number(raw.black_time),
});

const normGameState = (raw) => ({
    type: 'game_state',
    gameId:  raw.game_id,
    fen:     raw.fen,
    state:   raw.state,               // 'waiting' | 'in_progress' | 'finished'
    whiteMs: Number(raw.white_time),
    blackMs: Number(raw.black_time),
    moves:   (raw.moves || []).map(m => ({ san: m.san, thinkMs: Number(m.think_ms) })),
    result:  raw.result || null,
    reason:  raw.reason || null,
});

const normGameOver = (raw) => ({
    type: 'game_over',
    result: raw.result,               // '1-0' | '0-1' | '1/2-1/2' | '*'
    reason: raw.reason,               // 'checkmate' | 'stalemate' | ...
});

const normGameCreated = (raw) => ({
    type: 'game_created',
    gameId: raw.game_id,
    color: raw.color,                 // always 'white'
});

const normGameJoined = (raw) => ({
    type: 'game_joined',
    gameId:  raw.game_id,
    color:   raw.color,               // always 'black'
    whiteMs: Number(raw.white_time),
    blackMs: Number(raw.black_time),
});

const normGameStart = (raw) => ({
    type: 'game_start',
    gameId:   raw.game_id,
    color:    raw.color,              // 'white' | 'black'
    opponent: raw.opponent || 'Opponent',
    whiteMs:  Number(raw.white_time),
    blackMs:  Number(raw.black_time),
    isAI:     !!raw.ai_game,
});

const normMatchFound = (raw) => ({
    type: 'match_found',
    gameId:   raw.game_id,
    color:    raw.color,
    opponent: raw.opponent || 'Opponent',
    whiteMs:  Number(raw.white_time),
    blackMs:  Number(raw.black_time),
    isAI:     false,
});

const normQueued          = (raw) => ({ type: 'queued', queueSize: Number(raw.queue_size || 0) });
const normQueueCancelled  = () => ({ type: 'queue_cancelled' });

const normalizers = {
    error:            normError,
    move_rejected:    normMoveRejected,
    game_list:        normGameList,
    move_made:        normMoveMade,
    game_state:       normGameState,
    game_over:        normGameOver,
    game_created:     normGameCreated,
    game_joined:      normGameJoined,
    game_start:       normGameStart,
    match_found:      normMatchFound,
    queued:           normQueued,
    queue_cancelled:  normQueueCancelled,
};
