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

import { INITIAL_RATING } from '../core/rating.js';

/* ────────────────────────────────────────────────────────────
   Outbound message factories.
   time_base / time_inc are SECONDS on the wire (backend expectation).
   ──────────────────────────────────────────────────────────── */

export const Outbound = {

    // ── Live: Phases 1–6 ────────────────────────────

    // The four game-starting commands (create_game / join_game / quick_play /
    // play_ai) require an access_token as of the Phase 9 pre-work auth
    // migration. The server reads the caller's username + ELO from the
    // authenticated players row — the client no longer sends them.
    createGame(accessToken, timeBaseSec, timeIncSec) {
        return {
            type: 'create_game',
            access_token: accessToken,
            time_base: timeBaseSec,
            time_inc: timeIncSec,
        };
    },

    joinGame(accessToken, gameId) {
        return { type: 'join_game', access_token: accessToken, game_id: Number(gameId) };
    },

    makeMove(from, to, promotion = null) {
        return promotion
            ? { type: 'make_move', from, to, promotion }
            : { type: 'make_move', from, to };
    },

    resign() { return { type: 'resign' }; },

    offerDraw(accessToken) {
        return { type: 'offer_draw', access_token: accessToken };
    },
    drawResponse(accessToken, accept) {
        return { type: 'draw_response', access_token: accessToken, accept: !!accept };
    },
    offerRematch(accessToken, gameId) {
        return { type: 'offer_rematch', access_token: accessToken, game_id: gameId };
    },
    rematchResponse(accessToken, gameId, accept) {
        return {
            type: 'rematch_response', access_token: accessToken,
            game_id: gameId, accept: !!accept,
        };
    },
    pendingRematch(accessToken) {
        return { type: 'get_pending_rematch', access_token: accessToken };
    },

    quickPlay(accessToken, timeBaseSec, timeIncSec) {
        return {
            type: 'quick_play',
            access_token: accessToken,
            time_base: timeBaseSec,
            time_inc: timeIncSec,
        };
    },

    cancelQueue() { return { type: 'cancel_queue' }; },

    listGames()   { return { type: 'list_games' }; },

    gameState(accessToken) {
        return { type: 'game_state', access_token: accessToken };
    },
    activeGame(accessToken) {
        return { type: 'get_active_game', access_token: accessToken };
    },

    playAI(accessToken, difficulty, timeBaseSec, timeIncSec) {
        return {
            type: 'play_ai',
            access_token: accessToken,
            difficulty,                // 'easy' | 'medium' | 'hard' | 'max'
            time_base: timeBaseSec,
            time_inc:  timeIncSec,
        };
    },

    // ── Identity and query routes (live backend) ──────────────

    login(username, password)    { return { type: 'login',    username, password }; },
    register(username, password, email) { return { type: 'register', username, password, email }; },
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
    leaderboard(limit = 100)     { return { type: 'get_leaderboard', limit }; },
    // Phase 9.1 — spectator mode. Live going through the real server.
    // list_live_games needs no auth (public directory of ongoing games).
    // spectate requires access_token, matching the game-starting contract.
    // stop_spectating is a "stop sending" signal, no auth needed.
    listLiveGames()              { return { type: 'list_live_games' }; },
    spectate(accessToken, gameId) {
        return { type: 'spectate', access_token: accessToken, game_id: gameId };
    },
    stopSpectating(gameId)       { return { type: 'stop_spectating', game_id: gameId }; },
    getGame(gameId)              { return { type: 'get_game', game_id: gameId }; },
    analyzePosition(fen, depth = 12) { return { type: 'analyze_position', fen, depth }; },
    // Phase 9.3 — statistical anti-cheat analysis of a completed game.
    // Returns a `cheat_report` with a per-side verdict; unauthenticated,
    // synchronous, and safe to call on any persisted game_id.
    analyzeGame(gameId)              { return { type: 'analyze_game', game_id: gameId }; },

    // ── Phase 9.4 — Swiss tournaments ──────────────────
    // Four commands are creator/participant-authenticated (create/join/start
    // and the dev-facing report_result); tournament_state and list are
    // unauthenticated reads. The `time_base`/`time_inc` on create match the
    // create_game contract (seconds).
    createTournament(accessToken, name, rounds, timeBaseSec, timeIncSec, schedule = null) {
        const message = {
            type: 'create_tournament',
            access_token: accessToken,
            name,
            rounds,
            format: schedule?.format || 'swiss',
            time_base: timeBaseSec,
            time_inc:  timeIncSec,
        };
        if (schedule) {
            message.registration_deadline = schedule.registrationDeadline;
            message.first_round_starts_at = schedule.firstRoundStartsAt;
            message.round_duration_seconds = schedule.roundDurationSeconds;
        }
        return message;
    },
    joinTournament(accessToken, tournamentId) {
        return { type: 'join_tournament', access_token: accessToken, tournament_id: tournamentId };
    },
    leaveTournament(accessToken, tournamentId) {
        return { type: 'leave_tournament', access_token: accessToken, tournament_id: tournamentId };
    },
    startTournament(accessToken, tournamentId) {
        return { type: 'start_tournament', access_token: accessToken, tournament_id: tournamentId };
    },
    setTournamentRegistration(accessToken, tournamentId, open) {
        return { type: 'set_tournament_registration', access_token: accessToken,
            tournament_id: tournamentId, open };
    },
    checkInTournamentRound(accessToken, tournamentId, round) {
        return { type: 'check_in_tournament_round', access_token: accessToken,
            tournament_id: tournamentId, round };
    },
    tournamentState(tournamentId) {
        return { type: 'tournament_state', tournament_id: tournamentId };
    },
    listTournaments(status = null, limit = 25) {
        const msg = { type: 'list_tournaments', limit };
        if (status) msg.status = status;
        return msg;
    },
    reportTournamentResult(accessToken, pairingId, result, reason = 'Creator correction via tournament console') {
        return {
            type: 'report_tournament_result',
            access_token: accessToken,
            pairing_id: pairingId,
            result,     // '1-0' | '0-1' | '1/2-1/2' | 'bye'
            reason,
        };
    },
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

/** move_made has no "who moved" field — leave that to callers with turn parity.
 *  Phase 9.1: `fen` is now included on the wire so spectators (and seat players)
 *  can update the board without a follow-up game_state round-trip. */
const normMoveMade = (raw) => ({
    type: 'move_made',
    from: raw.from,
    to:   raw.to,
    san:  raw.san,
    promotion: raw.promotion || null,
    whiteMs: Number(raw.white_time),
    blackMs: Number(raw.black_time),
    fen:  raw.fen || null,
    legalMoves: Array.isArray(raw.legal_moves)
        ? raw.legal_moves.filter(m => typeof m === 'string')
        : [],
});

const normGameState = (raw) => ({
    type: 'game_state',
    gameId:  raw.game_id,
    whiteUsername: String(raw.white_username || ''),
    blackUsername: String(raw.black_username || ''),
    fen:     raw.fen,
    state:   raw.state,               // 'waiting' | 'in_progress' | 'finished'
    whiteMs: Number(raw.white_time),
    blackMs: Number(raw.black_time),
    moves:   (raw.moves || []).map(m => ({ san: m.san, thinkMs: Number(m.think_ms) })),
    legalMoves: Array.isArray(raw.legal_moves)
        ? raw.legal_moves.filter(m => typeof m === 'string')
        : [],
    result:  raw.result || null,
    reason:  raw.reason || null,
    drawOfferFrom: raw.draw_offer_from === 'white' ? 'w'
        : raw.draw_offer_from === 'black' ? 'b' : null,
});

const normActiveGame = (raw) => {
    if (!raw.game) return { type: 'active_game', game: null };
    const g = raw.game;
    return {
        type: 'active_game',
        game: {
            gameId:      Number(g.game_id),
            color:       g.color === 'black' ? 'b' : 'w',
            opponent:    g.opponent || 'Opponent',
            whiteMs:     Number(g.white_time),
            blackMs:     Number(g.black_time),
            timeBaseSec: Number(g.time_base),
            timeIncSec:  Number(g.time_inc),
            isAI:        !!g.ai_game,
            state:       g.state === 'in_progress' ? 'in_progress' : 'waiting',
            tournamentId: Number(g.tournament_id || 0) || null,
            pairingId:    Number(g.pairing_id || 0) || null,
        },
    };
};

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

const normRematchStarted = (raw) => ({
    type:        'rematch_started',
    gameId:      raw.game_id,
    color:       raw.color,
    opponent:    raw.opponent || 'Opponent',
    whiteMs:     Number(raw.white_time),
    blackMs:     Number(raw.black_time),
    timeBaseSec: Number(raw.time_base),
    timeIncSec:  Number(raw.time_inc),
    isAI:        !!raw.ai_game,
});

const normPendingRematch = (raw) => ({
    type: 'pending_rematch',
    offer: raw.offer ? {
        gameId: Number(raw.offer.game_id),
        from: String(raw.offer.from || 'Opponent'),
        role: raw.offer.role === 'sender' ? 'sender' : 'recipient',
        expiresInMs: Number(raw.offer.expires_in_ms || 45_000),
    } : null,
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

/* Phase 8 — profile --------------------------------------------- */

/** Profile rows arrive in the storage/API snake_case contract. Keep this
 *  conversion at the protocol boundary so the screen only handles one shape. */
const normProfile = (raw) => ({
    type:          'profile',
    username:      String(raw.username || ''),
    elo:           Number(raw.elo ?? INITIAL_RATING),
    gamesPlayed:   Number(raw.games_played || 0),
    wins:          Number(raw.wins || 0),
    losses:        Number(raw.losses || 0),
    draws:         Number(raw.draws || 0),
    ratingHistory: Array.isArray(raw.rating_history)
        ? raw.rating_history.map(p => ({ ts: p.ts, rating: Number(p.rating || 0) }))
        : [],
    recentGames: Array.isArray(raw.recent_games) ? raw.recent_games.map(g => ({
        gameId:      g.game_id,
        opponent:    String(g.opponent || 'Player'),
        opponentElo: Number(g.opponent_elo || 0),
        result:      g.result,
        color:       g.color,
        termination: g.termination || '',
        playedAt:    g.started_at,
        moves:       Number(g.move_count || 0),
        timeControl: String(g.time_control || ''),
        rated:       g.rated !== false,
    })) : [],
});

const normLeaderboard = (raw) => ({
    type: 'leaderboard',
    players: Array.isArray(raw.players) ? raw.players.map(p => ({
        rank:        Number(p.rank || 0),
        username:    String(p.username || ''),
        elo:         Number(p.elo ?? INITIAL_RATING),
        wins:        Number(p.wins || 0),
        losses:      Number(p.losses || 0),
        draws:       Number(p.draws || 0),
        gamesPlayed: Number(p.games_played || 0),
    })) : [],
});

/* Phase 9.1 — spectator mode ------------------------------------- */

const normLiveGameList = (raw) => ({
    type: 'live_game_list',
    games: (raw.games || []).map(g => ({
        gameId:         g.game_id,
        white:          g.white || 'Player',
        black:          g.black || 'Player',
        timeControl:    g.time_control || '',
        spectatorCount: Number(g.spectator_count || 0),
        moveCount:      Number(g.move_count || 0),
    })),
});

const normSpectateStart = (raw) => ({
    type: 'spectate_start',
    gameId:         raw.game_id,
    white:          raw.white || 'Player',
    black:          raw.black || 'Player',
    fen:            raw.fen,
    whiteMs:        Number(raw.white_time),
    blackMs:        Number(raw.black_time),
    timeControl:    raw.time_control || '',
    spectatorCount: Number(raw.spectator_count || 0),
    moves:          (raw.moves || []).map(m => ({
        san: m.san, thinkMs: Number(m.think_ms),
    })),
});

const normSpectateEnd = (raw) => ({ type: 'spectate_end', gameId: raw.game_id });

/* Phase 9.2 — replay + analysis ---------------------------------- */

/** history rows come in snake_case; camelCase them so the list screen
 *  reads {gameId, opponent, opponentElo, myColor, result, reason,
 *         playedAt, moveCount, timeControl}. */
const normHistory = (raw) => ({
    type: 'history',
    username: raw.username || '',
    games: (raw.games || []).map(g => ({
        gameId:       g.game_id,
        opponent:     g.opponent || 'Player',
        opponentElo:  Number(g.opponent_elo || 0),
        myColor:      g.my_color,           // "w" or "b"
        result:       g.result,             // "w" | "l" | "d"
        reason:       g.reason,
        playedAt:     g.played_at,
        moveCount:    Number(g.move_count || 0),
        timeControl:  g.time_control || '',
        rated:        g.rated !== false,
    })),
});

/** The `game` payload is bulky; pass it through mostly untouched (the
 *  replay detail screen already reads snake_case fields directly for
 *  metadata) but standardise the positions array so the caller can rely
 *  on `positions[i].fen/san/from/to/thinkMs/evalCp`. */
const normGame = (raw) => ({
    type: 'game',
    gameId:      raw.game_id,
    white:       raw.white,
    black:       raw.black,
    // Keep snake_case aliases too — the existing screen reads white_elo etc.
    white_elo:   raw.white_elo,
    black_elo:   raw.black_elo,
    result:      raw.result,
    reason:      raw.reason,
    time_control: raw.time_control,
    started_at:  raw.started_at,
    ended_at:    raw.ended_at,
    move_count:  raw.move_count,
    rated:       raw.rated !== false,
    positions: (raw.positions || []).map(p => ({
        ply:     Number(p.ply || 0),
        fen:     p.fen,
        san:     p.san || '',
        from:    p.from,
        to:      p.to,
        thinkMs: Number(p.think_ms || 0),
        eval_cp: p.eval_cp == null ? null : Number(p.eval_cp),
    })),
});

const normAnalysis = (raw) => ({
    type: 'analysis',
    fen:      raw.fen,
    eval_cp:  Number(raw.eval_cp || 0),
    bestMove: raw.best_move || '',
    depth:    Number(raw.depth || 0),
    nodes:    Number(raw.nodes || 0),
    terminal: !!raw.terminal,
});

/* Phase 9.3 — anti-cheat --------------------------------------- */

// A per-side verdict block. Numeric signal fields may arrive as JSON
// null when the analyzer could not compute them (all-terminal plies,
// zero-variance series). null propagates through — do NOT coerce to 0.
const normSideReport = (r) => ({
    pliesAnalyzed:      Number(r.plies_analyzed || 0),
    pliesMatchedEngine: Number(r.plies_matched_engine || 0),
    engineAgreementPct: r.engine_agreement_pct == null
                          ? null : Number(r.engine_agreement_pct),
    timeCv:             r.time_cv == null ? null : Number(r.time_cv),
    complexityCorr:     r.complexity_corr == null
                          ? null : Number(r.complexity_corr),
    flagged:            !!r.flagged,
    reasons:            Array.isArray(r.reasons) ? r.reasons : [],
});

const normCheatReport = (raw) => ({
    type:   'cheat_report',
    gameId: raw.game_id,
    white:  normSideReport(raw.white || {}),
    black:  normSideReport(raw.black || {}),
});

// ── Phase 9.4 — tournaments ────────────────────────────
// Each helper turns the server's snake_case shape into camelCase and
// coerces the primitive types the UI expects. Nullable server fields
// (black_player_id, game_id, started_at/completed_at) are preserved as
// null rather than coerced to 0 or '' — a bye ROW has a real null there
// and the UI must be able to tell.

const normTournamentSummary = (t) => ({
    id:            t.id,
    name:          String(t.name || ''),
    format:        String(t.format || ''),
    rounds:        Number(t.rounds || 0),
    currentRound:  Number(t.current_round || 0),
    timeBase:      Number(t.time_base || 0),
    timeInc:       Number(t.time_inc  || 0),
    status:        String(t.status || ''),
    registrationDeadline: Number(t.registration_deadline || 0),
    firstRoundStartsAt: Number(t.first_round_starts_at || 0),
    roundDurationSeconds: Number(t.round_duration_seconds || 0),
    registrationOpen: !!t.registration_open,
    createdBy:     t.created_by,
    createdByUsername: String(t.created_by_username || ''),
    createdAt:     String(t.created_at   || ''),
    startedAt:     String(t.started_at   || ''),
    completedAt:   String(t.completed_at || ''),
});

const normStanding = (s) => ({
    playerId:     s.player_id,
    username:     String(s.username || 'Unknown player'),
    elo:          Number(s.elo || 0),
    score:        Number(s.score || 0),
    roundWins:    Number(s.round_wins || 0),
    roundDraws:   Number(s.round_draws || 0),
    rank:         Number(s.rank || 0),
    buchholz:     Number(s.buchholz || 0),
    withdrawn:    !!s.withdrawn,
    whitesPlayed: Number(s.whites_played || 0),
    receivedBye:  !!s.received_bye,
});

const normPairing = (p) => ({
    id:             p.id,
    round:          Number(p.round || 0),
    whitePlayerId:  p.white_player_id,
    whiteUsername:  String(p.white_username || 'Unknown player'),
    blackPlayerId:  p.black_player_id == null ? null : p.black_player_id,
    blackUsername:  p.black_username == null ? null : String(p.black_username),
    gameId:         p.game_id         == null ? null : p.game_id,
    replayGameId:   p.replay_game_id  == null ? null : p.replay_game_id,
    result:         String(p.result || 'pending'),
    resultSource:   String(p.result_source || ''),
});

const normTournamentRound = (r) => ({
    round: Number(r.round || 0),
    status: String(r.status || ''),
    earliestStartAt: Number(r.earliest_start_at || 0),
    actualStartAt: Number(r.actual_start_at || 0),
    checkInClosesAt: Number(r.check_in_closes_at || 0),
    completedAt: Number(r.completed_at || 0),
});

const normTournamentCreated = (raw) => ({
    type: 'tournament_created', tournamentId: raw.tournament_id,
});
const normTournamentJoined = (raw) => ({
    type: 'tournament_joined',  tournamentId: raw.tournament_id,
});
const normTournamentStarted = (raw) => ({
    type: 'tournament_started', tournamentId: raw.tournament_id, round: Number(raw.round || 1),
});
const normTournamentResultRecorded = (raw) => ({
    type: 'tournament_result_recorded',
    pairingId: raw.pairing_id, result: String(raw.result || ''),
});
const normTournamentState = (raw) => ({
    type:       'tournament_state',
    tournament: normTournamentSummary(raw.tournament || {}),
    standings:  Array.isArray(raw.standings) ? raw.standings.map(normStanding) : [],
    pairings:   Array.isArray(raw.pairings)  ? raw.pairings.map(normPairing)   : [],
    rounds:     Array.isArray(raw.rounds) ? raw.rounds.map(normTournamentRound) : [],
    checkIns:   Array.isArray(raw.check_ins) ? raw.check_ins.map(c => ({
        round: Number(c.round || 0),
        playerId: Number(c.player_id || 0),
        username: String(c.username || 'Unknown player'),
    })) : [],
});
const normTournamentList = (raw) => ({
    type:        'tournament_list',
    tournaments: Array.isArray(raw.tournaments) ? raw.tournaments.map(normTournamentSummary) : [],
});

const normalizers = {
    error:            normError,
    move_rejected:    normMoveRejected,
    game_list:        normGameList,
    move_made:        normMoveMade,
    game_state:       normGameState,
    active_game:      normActiveGame,
    game_over:        normGameOver,
    game_created:     normGameCreated,
    game_joined:      normGameJoined,
    game_start:       normGameStart,
    rematch_started:  normRematchStarted,
    pending_rematch:  normPendingRematch,
    match_found:      normMatchFound,
    queued:           normQueued,
    queue_cancelled:  normQueueCancelled,
    profile:          normProfile,
    leaderboard:      normLeaderboard,
    live_game_list:   normLiveGameList,
    spectate_start:   normSpectateStart,
    spectate_end:     normSpectateEnd,
    history:          normHistory,
    game:             normGame,
    analysis:         normAnalysis,
    cheat_report:     normCheatReport,
    tournament_created:          normTournamentCreated,
    tournament_joined:           normTournamentJoined,
    tournament_left: (raw) => ({
        type: raw.type, tournamentId: Number(raw.tournament_id),
    }),
    tournament_started:          normTournamentStarted,
    tournament_state:            normTournamentState,
    tournament_list:             normTournamentList,
    tournament_result_recorded:  normTournamentResultRecorded,
    tournament_registration_updated: (raw) => ({
        type: raw.type, tournamentId: raw.tournament_id, open: !!raw.open,
    }),
    tournament_round_checked_in: (raw) => ({
        type: raw.type, tournamentId: raw.tournament_id, round: Number(raw.round || 0),
        roomReady: !!raw.room_ready, gameStarted: !!raw.game_started,
        gameId: raw.game_id == null ? null : raw.game_id,
        color: raw.color == null ? null : String(raw.color),
    }),
    tournament_game_ready: (raw) => ({
        type: raw.type, tournamentId: raw.tournament_id, pairingId: raw.pairing_id,
        gameId: raw.game_id, color: String(raw.color || ''),
        state: String(raw.state || 'waiting'),
    }),
};
