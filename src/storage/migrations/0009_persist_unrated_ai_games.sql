-- 0009_persist_unrated_ai_games.sql
--
-- Keep completed AI games in durable replay history without inventing a
-- players row for the computer and without changing human ratings/statistics.

ALTER TABLE games
    ALTER COLUMN black_id DROP NOT NULL,
    ADD COLUMN rated BOOLEAN NOT NULL DEFAULT TRUE,
    ADD COLUMN black_display_name TEXT;

ALTER TABLE move_times
    ALTER COLUMN player_id DROP NOT NULL;

ALTER TABLE games
    ADD CONSTRAINT games_participant_shape CHECK (
        (rated AND black_id IS NOT NULL AND black_display_name IS NULL)
        OR
        (NOT rated AND black_id IS NULL
         AND NULLIF(BTRIM(black_display_name), '') IS NOT NULL)
    );
