-- 0011_refresh_rotation_retry_grace.sql
--
-- Record when a refresh token was rotated.  The authentication service uses
-- this timestamp for a bounded retry grace period when a browser hard-refresh
-- loses the rotation response.  Reuse after that grace period retains the
-- existing whole-family revocation behaviour.

ALTER TABLE sessions
    ADD COLUMN IF NOT EXISTS rotated_at TIMESTAMPTZ;
