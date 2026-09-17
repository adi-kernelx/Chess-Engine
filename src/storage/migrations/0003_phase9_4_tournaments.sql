-- Migration 0003 — Phase 9.4, Swiss tournament system.
--
-- Three tables:
--   * tournaments        — one row per tournament, holds lifecycle state
--   * tournament_players — registration + running standings per player
--   * tournament_pairings — one row per (round × pairing) with a result slot
--
-- The `tournaments.status` column is the state machine:
--     registration → in_progress → completed
-- Registration accepts joins; in_progress runs rounds; completed is terminal.
-- A tournament in registration has current_round = 0; the first round starts
-- at current_round = 1 the moment status flips to in_progress.
--
-- Cascade policy mirrors phase 9.3's cheat_reports discipline:
--   * ON DELETE CASCADE on tournament_id everywhere — deleting a tournament
--     wipes its participants and pairings; no orphans.
--   * NO cascade on player_id — deleting a player must not silently erase
--     their tournament history; the row survives with a dangling reference
--     the moderation layer can spot.
--
-- The pairings table stores game_id NULL until the actual GameRoom is
-- created for that pairing (deferred wiring — see log narrative). A
-- bye is represented by `black_player_id IS NULL` and `result = 'bye'`.

CREATE TABLE tournaments (
    id                        BIGSERIAL   PRIMARY KEY,
    name                      TEXT        NOT NULL,
    format                    TEXT        NOT NULL CHECK (format IN ('swiss')),
    rounds                    INTEGER     NOT NULL CHECK (rounds > 0),

    -- 0 while in registration; 1..rounds once in_progress.
    current_round             INTEGER     NOT NULL DEFAULT 0
                                          CHECK (current_round >= 0),

    -- Time control applied to every game in the tournament.
    time_control_initial_ms   INTEGER     NOT NULL CHECK (time_control_initial_ms > 0),
    time_control_increment_ms INTEGER     NOT NULL CHECK (time_control_increment_ms >= 0),

    status                    TEXT        NOT NULL
                                          CHECK (status IN ('registration', 'in_progress', 'completed')),

    -- The user who created the tournament. Preserved on user deletion
    -- (audit) — no ON DELETE CASCADE.
    created_by                BIGINT      NOT NULL REFERENCES players(id),

    created_at                TIMESTAMPTZ NOT NULL DEFAULT now(),
    started_at                TIMESTAMPTZ,
    completed_at              TIMESTAMPTZ,

    -- current_round cannot exceed rounds.
    CHECK (current_round <= rounds)
);

CREATE INDEX idx_tournaments_status ON tournaments(status, created_at DESC);


CREATE TABLE tournament_players (
    tournament_id             BIGINT      NOT NULL REFERENCES tournaments(id) ON DELETE CASCADE,
    player_id                 BIGINT      NOT NULL REFERENCES players(id),

    -- Snapshot of the player's ELO at registration time. Used as the
    -- pairing tiebreak and standings tiebreak. Snapshotted (not JOINed
    -- live) so a rating change mid-tournament does not scramble
    -- pairings.
    initial_elo               INTEGER     NOT NULL,

    -- Running Swiss score: +1 win, +0.5 draw, +0 loss, +1 for a bye.
    score                     DOUBLE PRECISION NOT NULL DEFAULT 0.0
                                          CHECK (score >= 0.0),

    -- Colour history — how many times this player has held White so far.
    -- Used by the pairing algorithm to balance colours across rounds.
    whites_played             INTEGER     NOT NULL DEFAULT 0
                                          CHECK (whites_played >= 0),

    -- True once this player has already had a bye. Second-bye is legal
    -- (see swiss.cpp) but strongly disfavoured.
    received_bye              BOOLEAN     NOT NULL DEFAULT FALSE,

    withdrawn                 BOOLEAN     NOT NULL DEFAULT FALSE,

    registered_at             TIMESTAMPTZ NOT NULL DEFAULT now(),

    PRIMARY KEY (tournament_id, player_id)
);

CREATE INDEX idx_tournament_players_player
    ON tournament_players(player_id, registered_at DESC);


CREATE TABLE tournament_pairings (
    id                        BIGSERIAL   PRIMARY KEY,
    tournament_id             BIGINT      NOT NULL REFERENCES tournaments(id) ON DELETE CASCADE,

    round                     INTEGER     NOT NULL CHECK (round > 0),

    white_player_id           BIGINT      NOT NULL REFERENCES players(id),

    -- NULL iff this is a bye pairing.
    black_player_id           BIGINT      REFERENCES players(id),

    -- The GameRoom this pairing spawns, once created. NULL while pending.
    -- Not FK'd back to games(id) yet — the game_id we store is the runtime
    -- GameId assigned by RoomManager, and its persistence into games.id
    -- happens only on completion. Keeping this as a plain BIGINT avoids
    -- an insertion-order coupling with the games table. See log narrative.
    game_id                   BIGINT,

    -- Result slot. Values match the games.result convention plus:
    --   * 'pending' — pairing exists, game not yet played
    --   * 'bye'     — automatic full-point bye
    result                    TEXT        NOT NULL DEFAULT 'pending'
                                          CHECK (result IN ('pending', '1-0', '0-1', '1/2-1/2', 'bye')),

    created_at                TIMESTAMPTZ NOT NULL DEFAULT now(),

    -- A player appears at most once per round, on either side.
    UNIQUE (tournament_id, round, white_player_id),
    UNIQUE (tournament_id, round, black_player_id),

    -- A bye pairing has null black and result='bye'; a non-bye must have
    -- black not-null and result in {pending, 1-0, 0-1, 1/2-1/2}. These
    -- two conditions collapse to a single CHECK:
    CHECK ((black_player_id IS NULL AND result = 'bye') OR
           (black_player_id IS NOT NULL AND result <> 'bye'))
);

CREATE INDEX idx_tournament_pairings_round
    ON tournament_pairings(tournament_id, round);

CREATE INDEX idx_tournament_pairings_pending
    ON tournament_pairings(tournament_id) WHERE result = 'pending';
