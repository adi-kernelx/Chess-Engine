-- Additive migration: no account, history or Supabase Auth rows are removed.
ALTER TABLE public.players ADD COLUMN IF NOT EXISTS email_verified BOOLEAN NOT NULL DEFAULT FALSE;
-- These addresses were accepted only after verified Google JWT validation.
UPDATE public.players SET email_verified=TRUE WHERE google_sub IS NOT NULL AND email IS NOT NULL;
-- Fail explicitly on pre-existing case-insensitive collisions; do not merge data.
CREATE UNIQUE INDEX idx_players_email_ci ON public.players(lower(email)) WHERE email IS NOT NULL;

CREATE TABLE public.email_challenges (
    token_hash TEXT PRIMARY KEY,
    purpose TEXT NOT NULL CHECK (purpose IN ('register','recovery_email','reset')),
    email TEXT NOT NULL,
    player_id BIGINT REFERENCES public.players(id) ON DELETE CASCADE,
    token_epoch INTEGER,
    username TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at TIMESTAMPTZ NOT NULL,
    CHECK ((purpose='register' AND player_id IS NULL AND username IS NOT NULL)
        OR (purpose='recovery_email' AND player_id IS NOT NULL AND token_epoch IS NOT NULL
            AND username IS NULL)
        OR (purpose='reset' AND ((player_id IS NOT NULL AND token_epoch IS NOT NULL)
            OR (player_id IS NULL AND token_epoch IS NULL))
            AND username IS NULL))
);
CREATE INDEX idx_email_challenges_player ON public.email_challenges(player_id);
CREATE INDEX idx_email_challenges_email_created ON public.email_challenges(email,created_at);
CREATE INDEX idx_email_challenges_expiry ON public.email_challenges(expires_at);
ALTER TABLE public.email_challenges ENABLE ROW LEVEL SECURITY;
REVOKE ALL ON TABLE public.email_challenges FROM PUBLIC;
DO $$ DECLARE browser_role text; BEGIN
    FOR browser_role IN SELECT rolname FROM pg_roles WHERE rolname IN ('anon','authenticated') LOOP
        EXECUTE format('REVOKE ALL ON TABLE public.email_challenges FROM %I', browser_role);
    END LOOP;
END $$;
