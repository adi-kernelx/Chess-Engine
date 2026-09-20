-- 0004_lld4_completion_uuid.sql
--
-- LLD-4.2 idempotency key. Every completed game now carries a stable
-- 128-bit UUID stamped by `GameRoom::finish_game` at the moment of the
-- terminal transition. `GameCompletionService` uses that key to make
-- retries of `save_completed_game` a no-op instead of inserting a
-- duplicate row and double-updating ELO/stats.
--
-- Column is NULLABLE so historical rows (pre-migration) stay valid;
-- the partial unique index enforces uniqueness only for rows that
-- carry the key. That means production writes going forward must
-- always populate it — the completion service enforces this at the
-- application layer (empty uuid → bypass the SELECT-first check and
-- fall through to the plain INSERT path, preserving pre-LLD-4.2
-- behaviour for any caller that hasn't been migrated).
--
-- Deliberately no BEGIN/COMMIT here — Database::apply_migration owns
-- the transaction that couples this DDL to its schema_migrations row.

ALTER TABLE games
    ADD COLUMN completion_uuid TEXT;

CREATE UNIQUE INDEX idx_games_completion_uuid
    ON games(completion_uuid)
    WHERE completion_uuid IS NOT NULL;
