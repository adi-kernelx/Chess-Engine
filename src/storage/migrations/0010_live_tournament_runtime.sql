-- Phase 1: durable tournament lifecycle and scheduling.
-- Database::apply_migration owns the transaction; do not add BEGIN/COMMIT.

ALTER TABLE tournaments
    ADD COLUMN registration_deadline TIMESTAMPTZ,
    ADD COLUMN first_round_starts_at TIMESTAMPTZ,
    ADD COLUMN round_duration_seconds INTEGER NOT NULL DEFAULT 3600,
    ADD COLUMN registration_open BOOLEAN NOT NULL DEFAULT TRUE,
    ADD COLUMN registration_closed_at TIMESTAMPTZ;

UPDATE tournaments
SET registration_deadline = created_at + INTERVAL '1 day',
    first_round_starts_at = created_at + INTERVAL '1 day 5 minutes',
    registration_open = (status = 'registration'),
    registration_closed_at = CASE WHEN status <> 'registration' THEN started_at ELSE NULL END;

ALTER TABLE tournaments
    ALTER COLUMN registration_deadline SET NOT NULL,
    ALTER COLUMN first_round_starts_at SET NOT NULL,
    ADD CONSTRAINT tournaments_schedule_order_check
        CHECK (first_round_starts_at >= registration_deadline + INTERVAL '30 seconds'),
    ADD CONSTRAINT tournaments_round_duration_check
        CHECK (round_duration_seconds >= 60);

ALTER TABLE tournaments DROP CONSTRAINT tournaments_status_check;
ALTER TABLE tournaments ADD CONSTRAINT tournaments_status_check
    CHECK (status IN ('registration', 'scheduled', 'in_progress', 'completed'));

CREATE TABLE tournament_rounds (
    tournament_id BIGINT NOT NULL REFERENCES tournaments(id) ON DELETE CASCADE,
    round INTEGER NOT NULL CHECK (round > 0),
    earliest_start_at TIMESTAMPTZ NOT NULL,
    actual_start_at TIMESTAMPTZ,
    check_in_closes_at TIMESTAMPTZ NOT NULL,
    completed_at TIMESTAMPTZ,
    status TEXT NOT NULL DEFAULT 'scheduled'
        CHECK (status IN ('scheduled', 'live', 'completed')),
    PRIMARY KEY (tournament_id, round),
    CHECK (check_in_closes_at >= earliest_start_at)
);

INSERT INTO tournament_rounds
    (tournament_id, round, earliest_start_at, check_in_closes_at, status,
     actual_start_at, completed_at)
SELECT t.id, r,
       t.first_round_starts_at + (r - 1) * t.round_duration_seconds * INTERVAL '1 second',
       t.first_round_starts_at + r * t.round_duration_seconds * INTERVAL '1 second',
       CASE WHEN r < t.current_round THEN 'completed'
            WHEN r = t.current_round AND t.status = 'in_progress' THEN 'live'
            ELSE 'scheduled' END,
       CASE WHEN r <= t.current_round AND t.status <> 'registration' THEN t.started_at END,
       CASE WHEN r < t.current_round OR t.status = 'completed' THEN t.completed_at END
FROM tournaments t CROSS JOIN LATERAL generate_series(1, t.rounds) AS r;

ALTER TABLE tournament_pairings
    ADD COLUMN result_source TEXT,
    ADD COLUMN result_recorded_at TIMESTAMPTZ;

UPDATE tournament_pairings
SET result_source = CASE WHEN result = 'bye' THEN 'bye'
                         WHEN result <> 'pending' THEN 'manual' END,
    result_recorded_at = CASE WHEN result <> 'pending' THEN created_at END;

ALTER TABLE tournament_pairings DROP CONSTRAINT tournament_pairings_result_check;
ALTER TABLE tournament_pairings ADD CONSTRAINT tournament_pairings_result_check
    CHECK (result IN ('pending', '1-0', '0-1', '1/2-1/2', 'bye', 'double_forfeit'));
ALTER TABLE tournament_pairings ADD CONSTRAINT tournament_pairings_result_source_check
    CHECK (result_source IS NULL OR result_source IN ('manual', 'game', 'forfeit', 'bye', 'override'));
ALTER TABLE tournament_pairings DROP CONSTRAINT tournament_pairings_check;
ALTER TABLE tournament_pairings ADD CONSTRAINT tournament_pairings_shape_check
    CHECK ((black_player_id IS NULL AND result = 'bye') OR
           (black_player_id IS NOT NULL AND result <> 'bye'));

CREATE UNIQUE INDEX uq_tournament_pairings_game_id
    ON tournament_pairings(game_id) WHERE game_id IS NOT NULL;

CREATE TABLE tournament_round_checkins (
    tournament_id BIGINT NOT NULL,
    round INTEGER NOT NULL,
    player_id BIGINT NOT NULL REFERENCES players(id),
    checked_in_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (tournament_id, round, player_id),
    FOREIGN KEY (tournament_id, round)
        REFERENCES tournament_rounds(tournament_id, round) ON DELETE CASCADE
);

CREATE TABLE tournament_result_overrides (
    id BIGSERIAL PRIMARY KEY,
    pairing_id BIGINT NOT NULL REFERENCES tournament_pairings(id) ON DELETE CASCADE,
    actor_player_id BIGINT NOT NULL REFERENCES players(id),
    old_result TEXT NOT NULL,
    new_result TEXT NOT NULL,
    reason TEXT NOT NULL CHECK (length(btrim(reason)) > 0),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_tournament_rounds_due
    ON tournament_rounds(status, earliest_start_at);
CREATE INDEX idx_tournament_checkins_round
    ON tournament_round_checkins(tournament_id, round);
CREATE INDEX idx_tournament_overrides_pairing
    ON tournament_result_overrides(pairing_id, created_at DESC);
