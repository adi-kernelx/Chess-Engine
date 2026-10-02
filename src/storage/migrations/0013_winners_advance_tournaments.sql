-- Custom elimination: dynamic rounds; a second draw advances both players.
ALTER TABLE tournaments DROP CONSTRAINT IF EXISTS tournaments_format_check;
ALTER TABLE tournaments ADD CONSTRAINT tournaments_format_check
    CHECK (format IN ('swiss', 'winners_advance'));
