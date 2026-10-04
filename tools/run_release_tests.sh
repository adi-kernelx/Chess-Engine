#!/usr/bin/env bash
# Never source .env: every DB-backed binary targets only the disposable local DB.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
export DATABASE_URL='postgresql://localhost/chess_test?host=/var/run/postgresql&user=adi'
export PGOPTIONS='-c client_min_messages=warning'
export PGCONNECT_TIMEOUT=5
mkdir -p build_release/verification
failed=0
count=0
for binary in build_release/test_*; do
    [[ -f "$binary" && -x "$binary" ]] || continue
    name=${binary##*/}
    count=$((count + 1))
    if timeout 180 "$binary" >"build_release/verification/$name.log" 2>&1; then
        printf 'PASS %s\n' "$name"
    else
        printf 'FAIL %s (see build_release/verification/%s.log)\n' "$name" "$name"
        failed=$((failed + 1))
    fi
done
for script in tests/test_frontend_*.mjs tests/seal_vectors.mjs; do
    name=${script##*/}
    count=$((count + 1))
    if timeout 60 node "$script" >"build_release/verification/$name.log" 2>&1; then
        printf 'PASS %s\n' "$name"
    else
        printf 'FAIL %s (see build_release/verification/%s.log)\n' "$name" "$name"
        failed=$((failed + 1))
    fi
done
printf 'TOTAL %s suites, %s failures\n' "$count" "$failed"
[[ "$failed" == 0 ]]
