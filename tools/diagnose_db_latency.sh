#!/usr/bin/env bash
# Read-only probe. Never print the connection string or its credentials.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
set -a
source .env
set +a
: "${DATABASE_URL:?DATABASE_URL is required}"
export PGOPTIONS='-c default_transaction_read_only=on -c statement_timeout=5000'
export PGCONNECT_TIMEOUT=5
status=0
output=$(psql "$DATABASE_URL" -X -q -c '\timing on' -c 'SELECT 1' -c 'SELECT 1' -c 'SELECT 1' \
    -c 'SELECT status,count(*) FROM tournaments GROUP BY status ORDER BY status' \
    -c "SELECT count(*) AS live_rounds FROM tournament_rounds WHERE status='live'" \
    -c "SELECT count(*) AS pending_games FROM tournament_pairings WHERE result='pending'" 2>&1) || status=$?
printf '%s\n' "$output" | sed -E 's#postgres(ql)?://[^[:space:]"[:cntrl:]]+#[redacted connection]#g'
exit "$status"
