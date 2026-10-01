-- Disconnect-expired games are completed with termination='abandonment'.
-- Existing Phase-8 schemas predate reconnect recovery and reject that value.
--
-- Deliberately no BEGIN/COMMIT: the migration runner owns the transaction.

ALTER TABLE games
    DROP CONSTRAINT IF EXISTS games_termination_check;

ALTER TABLE games
    ADD CONSTRAINT games_termination_check
    CHECK (termination IN (
        'checkmate', 'resignation', 'timeout', 'stalemate',
        'draw_agreement', 'insufficient_material',
        'threefold_repetition', 'fifty_move_rule', 'abandonment'
    ));
