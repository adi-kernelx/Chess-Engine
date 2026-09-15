-- schema_phase7.sql — the minimum tables Phase 7 needs.
--
-- Deliberately a slice, not a finished schema: Phase 8 extends this (game
-- history, ratings history, tournaments) rather than replacing it. Anything
-- here that Phase 8 will keep must be right the first time; anything Phase 8
-- adds should stay easy to add.
--
-- Runs on Postgres 14+ (Supabase's current baseline). No PL/pgSQL, no
-- extensions — everything is stock SQL so this file can also seed a local
-- Postgres for testing without any Supabase-specific setup.

CREATE TABLE IF NOT EXISTS players (
    id            BIGSERIAL PRIMARY KEY,

    -- Two username columns, one exact and one case-folded. The exact one is
    -- what we show and let the user pick; the case-folded one enforces "adi"
    -- and "Adi" being the same account, without giving up the original casing.
    username      TEXT UNIQUE NOT NULL,
    username_ci   TEXT UNIQUE NOT NULL,

    -- email is optional at the account level: a password-only user might not
    -- give one, and a Google-only user's email lives in google_sub. UNIQUE
    -- with NULL means as many NULLs as we like, so this does not conflict.
    email         TEXT UNIQUE,

    -- password_hash is NULL for Google-only accounts, google_sub is NULL for
    -- password-only accounts. The CHECK below prevents an account with
    -- neither — which would be unreachable and would confuse audit tooling.
    password_hash TEXT,
    google_sub    TEXT UNIQUE,

    elo_rating    INTEGER NOT NULL DEFAULT 1200,

    -- Bump this to kill every outstanding session for this user in one write
    -- (`logout_all`). Access tokens carry `epoch`; a mismatch is treated as
    -- revoked without a per-session round trip.
    token_epoch   INTEGER NOT NULL DEFAULT 0,

    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_login    TIMESTAMPTZ,

    CONSTRAINT has_a_login_method CHECK
        (password_hash IS NOT NULL OR google_sub IS NOT NULL)
);

-- The columns username and username_ci should agree on their case-folded form,
-- so a caller who forgets to lowercase one of them corrupts nothing.
CREATE OR REPLACE FUNCTION assert_username_ci_matches()
RETURNS TRIGGER AS $$
BEGIN
    IF NEW.username_ci <> lower(NEW.username) THEN
        RAISE EXCEPTION 'username_ci must equal lower(username)';
    END IF;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_username_ci ON players;
CREATE TRIGGER trg_username_ci BEFORE INSERT OR UPDATE ON players
    FOR EACH ROW EXECUTE FUNCTION assert_username_ci_matches();


CREATE TABLE IF NOT EXISTS sessions (
    -- Store the HASH of the refresh token, never the token itself. A database
    -- read compromise then does not hand the attacker every user's active
    -- session; only the client that holds the real token can be that user.
    token_hash    TEXT PRIMARY KEY,

    player_id     BIGINT NOT NULL REFERENCES players(id) ON DELETE CASCADE,

    -- A family_id ties every token that traces back to one login. When a
    -- rotated token is re-used (which only happens if it was stolen), the
    -- WHOLE family is revoked in one delete — not just the reused row. This
    -- is the "reuse detection" defence from OAuth 2.1 §4.14.2.
    family_id     UUID NOT NULL,

    -- Set to TRUE the moment a token is exchanged for a successor. Presenting
    -- a rotated token again is the signal that a copy exists in someone
    -- else's hands.
    rotated       BOOLEAN NOT NULL DEFAULT FALSE,

    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at    TIMESTAMPTZ NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_sessions_player ON sessions(player_id);
CREATE INDEX IF NOT EXISTS idx_sessions_family ON sessions(family_id);
