-- 0012_tournament_replay_identity.sql
--
-- A live tournament pairing has two different game identities:
--   * game_id is the process-local/reserved room identity used while playing;
--   * replay_game_id is the durable games.id assigned after persistence.
-- Keeping them separate prevents a completed pairing from opening an unrelated
-- replay whose database id happens to equal the runtime room id.

ALTER TABLE tournament_pairings
    ADD COLUMN IF NOT EXISTS replay_game_id BIGINT REFERENCES games(id) ON DELETE SET NULL;

CREATE UNIQUE INDEX IF NOT EXISTS uq_tournament_pairings_replay_game_id
    ON tournament_pairings(replay_game_id) WHERE replay_game_id IS NOT NULL;
