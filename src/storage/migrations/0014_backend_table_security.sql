-- Backend-only data. Application JWTs are not Supabase Auth RLS identities.
-- Enable default-deny RLS and remove browser-role table/sequence privileges.
-- Backend must remain table owner or BYPASSRLS; never FORCE RLS here.
-- No BEGIN/COMMIT: reviewed runner must couple DDL and ledger atomically.
-- Excluded migrations 0005/0006 are intentionally untouched.

DO $$
DECLARE
    table_name text;
    browser_role text;
    sequence_name text;
BEGIN
    FOREACH table_name IN ARRAY ARRAY[
        'players','sessions','games','move_times','cheat_reports',
        'tournaments','tournament_players','tournament_pairings',
        'tournament_rounds','tournament_round_checkins',
        'tournament_result_overrides','schema_migrations'
    ] LOOP
        EXECUTE format('ALTER TABLE public.%I ENABLE ROW LEVEL SECURITY', table_name);
        EXECUTE format('REVOKE ALL ON TABLE public.%I FROM PUBLIC', table_name);
        FOR browser_role IN SELECT rolname FROM pg_roles
            WHERE rolname IN ('anon','authenticated') LOOP
            EXECUTE format('REVOKE ALL ON TABLE public.%I FROM %I', table_name, browser_role);
        END LOOP;
        -- Only sequences owned by the exact application tables, not unrelated
        -- public-schema objects. Defaults use id on the five serial tables.
        IF table_name IN ('players','games','cheat_reports','tournaments','tournament_pairings',
                          'tournament_result_overrides') THEN
            sequence_name := pg_get_serial_sequence(format('public.%I',table_name), 'id');
            IF sequence_name IS NOT NULL THEN
                EXECUTE format('REVOKE ALL ON SEQUENCE %s FROM PUBLIC', sequence_name);
                FOR browser_role IN SELECT rolname FROM pg_roles
                    WHERE rolname IN ('anon','authenticated') LOOP
                    EXECUTE format('REVOKE ALL ON SEQUENCE %s FROM %I', sequence_name, browser_role);
                END LOOP;
            END IF;
        END IF;
    END LOOP;
END;
$$;

-- Function uses only built-in lower() and NEW; no application-schema lookup.
ALTER FUNCTION public.assert_username_ci_matches() SET search_path = pg_catalog;

CREATE INDEX IF NOT EXISTS idx_move_times_player ON public.move_times(player_id);
CREATE INDEX IF NOT EXISTS idx_tournament_pairings_white ON public.tournament_pairings(white_player_id);
CREATE INDEX IF NOT EXISTS idx_tournament_pairings_black ON public.tournament_pairings(black_player_id);
CREATE INDEX IF NOT EXISTS idx_tournament_overrides_actor ON public.tournament_result_overrides(actor_player_id);
CREATE INDEX IF NOT EXISTS idx_tournament_checkins_player ON public.tournament_round_checkins(player_id);
CREATE INDEX IF NOT EXISTS idx_tournaments_creator ON public.tournaments(created_by);
-- replay_game_id already has a partial unique index covering every non-NULL
-- reference. Do not add a duplicate or remove indexes based on tiny RC usage.
