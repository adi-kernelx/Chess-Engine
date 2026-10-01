-- 0007_initial_rating_800.sql
--
-- Product policy: new accounts start at 800 rather than 1200.
-- The narrow backfill updates only accounts that still have the old default
-- and have never completed a rated game. Established ratings are untouched.
--
-- Deliberately no BEGIN/COMMIT: the migration runner owns the transaction.

ALTER TABLE players
    ALTER COLUMN elo_rating SET DEFAULT 800;

UPDATE players
SET elo_rating = 800
WHERE elo_rating = 1200
  AND games_played = 0;
