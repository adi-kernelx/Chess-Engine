-- 0001_phase8_game_persistence.sql
--
-- Phase 8.1 extends the Phase-7 identity schema. This file deliberately does
-- not contain BEGIN/COMMIT: Database::apply_migration() owns the transaction
-- that couples this DDL to its schema_migrations record.

-- Rating aggregates belong to the player row because the leaderboard and
-- profile paths read them far more often than games are written. The CHECKs
-- make a broken repository update fail at the storage boundary.
ALTER TABLE players
    ADD COLUMN games_played INTEGER NOT NULL DEFAULT 0,
    ADD COLUMN wins        INTEGER NOT NULL DEFAULT 0,
    ADD COLUMN losses      INTEGER NOT NULL DEFAULT 0,
    ADD COLUMN draws       INTEGER NOT NULL DEFAULT 0,
    ADD CONSTRAINT players_games_played_nonnegative CHECK (games_played >= 0),
    ADD CONSTRAINT players_wins_nonnegative        CHECK (wins >= 0),
    ADD CONSTRAINT players_losses_nonnegative      CHECK (losses >= 0),
    ADD CONSTRAINT players_draws_nonnegative       CHECK (draws >= 0),
    ADD CONSTRAINT players_stats_add_up CHECK (games_played = wins + losses + draws);

-- DESC matches leaderboard order; id is a deterministic tiebreaker for equal
-- ratings and lets Postgres satisfy ORDER BY elo_rating DESC, id ASC directly.
CREATE INDEX idx_players_elo ON players(elo_rating DESC, id ASC);

CREATE TABLE games (
    id           BIGSERIAL PRIMARY KEY,
    white_id     BIGINT NOT NULL REFERENCES players(id),
    black_id     BIGINT NOT NULL REFERENCES players(id),
    moves        TEXT NOT NULL,
    result       TEXT NOT NULL
                 CHECK (result IN ('1-0', '0-1', '1/2-1/2', '*')),
    termination  TEXT NOT NULL
                 CHECK (termination IN (
                     'checkmate', 'resignation', 'timeout', 'stalemate',
                     'draw_agreement', 'insufficient_material',
                     'threefold_repetition', 'fifty_move_rule'
                 )),
    opening_eco  TEXT,
    white_elo    INTEGER NOT NULL CHECK (white_elo >= 0),
    black_elo    INTEGER NOT NULL CHECK (black_elo >= 0),
    time_control TEXT NOT NULL,
    started_at   TIMESTAMPTZ NOT NULL,
    ended_at     TIMESTAMPTZ NOT NULL,
    move_count   INTEGER NOT NULL CHECK (move_count >= 0),

    CONSTRAINT games_distinct_players CHECK (white_id <> black_id),
    CONSTRAINT games_time_order CHECK (ended_at >= started_at)
);

CREATE INDEX idx_games_white
    ON games(white_id, started_at DESC);
CREATE INDEX idx_games_black
    ON games(black_id, started_at DESC);
CREATE INDEX idx_games_opening
    ON games(opening_eco);

-- `ply_number` is intentionally not called move_number: one row represents
-- one player's turn, so 1.e4 and 1...e5 are plies 1 and 2, not two rows with
-- an ambiguous shared move number.
CREATE TABLE move_times (
    game_id      BIGINT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    ply_number   INTEGER NOT NULL CHECK (ply_number > 0),
    player_id    BIGINT NOT NULL REFERENCES players(id),
    think_time_ms INTEGER NOT NULL CHECK (think_time_ms >= 0),

    PRIMARY KEY (game_id, ply_number)
);
