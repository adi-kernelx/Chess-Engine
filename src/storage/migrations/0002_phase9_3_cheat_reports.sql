-- Migration 0002 — Phase 9.3, statistical anti-cheat reports.
--
-- One row per (game_id, player_id) pair. Analysis is per-side per-game so
-- a report on White does not imply anything about Black. `flagged` is the
-- boolean decision; `reasons` carries the ordered list of flag names that
-- fired so a reviewer can see which signals were tripped.
--
-- Not deleted on player removal (opposite convention to move_times) —
-- the audit trail must survive an account being deleted, otherwise
-- deleting one's account becomes a way to erase evidence.
--
-- Deleted on game removal — reports for a game that no longer exists
-- are noise; no reviewer can meaningfully re-check a claim against a
-- game they cannot see.

CREATE TABLE cheat_reports (
    id                   BIGSERIAL PRIMARY KEY,
    game_id              BIGINT       NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    player_id            BIGINT       NOT NULL REFERENCES players(id),
    side                 CHAR(1)      NOT NULL CHECK (side IN ('w', 'b')),

    plies_analyzed       INTEGER      NOT NULL CHECK (plies_analyzed >= 0),
    plies_matched_engine INTEGER      NOT NULL CHECK (plies_matched_engine >= 0),

    -- 0.0 - 100.0. NULL when the analyzer could not compute (e.g. every
    -- ply was terminal — degenerate) — the analyzer emits NaN and the
    -- repository stores NULL for it.
    engine_agreement_pct DOUBLE PRECISION,
    time_cv              DOUBLE PRECISION,
    complexity_corr      DOUBLE PRECISION,

    flagged              BOOLEAN      NOT NULL DEFAULT FALSE,

    -- Newline-separated flag names ('engine_agreement_high',
    -- 'time_cv_low', 'complexity_correlation_low'). Empty string when
    -- flagged = FALSE.
    reasons              TEXT         NOT NULL DEFAULT '',

    created_at           TIMESTAMPTZ  NOT NULL DEFAULT now(),

    UNIQUE (game_id, player_id)
);

CREATE INDEX idx_cheat_reports_player  ON cheat_reports(player_id, created_at DESC);
CREATE INDEX idx_cheat_reports_flagged ON cheat_reports(flagged, created_at DESC)
    WHERE flagged = TRUE;
