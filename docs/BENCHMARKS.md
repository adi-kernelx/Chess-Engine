# Benchmarks

Measured results, workload definitions and reproduction boundaries for the current release candidate. These are not a promised production player limit.

## 1. Measurement environment

Development measurements use an AMD Ryzen 5 5600H laptop under WSL2 Ubuntu 26.04, Release builds and local PostgreSQL 18 unless explicitly labeled hosted. Host activity and CPU affinity affect timings.

**CPU affinity is not a one-vCPU quota.** The mixed-load results below restrict eligible CPUs; the Docker smoke uses a CPU quota. Neither is a deployed Cloud Run capacity test.

The benchmark tools are [load_test.py](../tools/load_test.py), [bench_engine.cpp](../tools/bench_engine.cpp), [bench_identity_reads.cpp](../tools/bench_identity_reads.cpp) and [test_release_capacity.py](../tests/test_release_capacity.py).

## 2. Bounded pooling under database latency

Four concurrent fresh profile/epoch SELECTs were compared with one versus two leased connections. Tests exclude cold connection establishment and do not modify identities.

| Workload | One connection | Two connections | Meaning |
| --- | ---: | ---: | --- |
| Local PostgreSQL with 145 ms simulated wait, one eligible CPU | ~585 ms | ~294 ms | Independent waits overlap |
| Hosted Supabase, one eligible CPU, median of three warm batches | 632 ms | 308 ms | Similar benefit on the actual hosted read path |

Hosted warm batch samples:

| Run | One connection | Two connections |
| --- | ---: | ---: |
| 1 | 632.138 ms | 304.789 ms |
| 2 | 650.811 ms | 307.721 ms |
| 3 | 604.715 ms | 316.814 ms |

This demonstrates read-wait overlap, not doubled player capacity. The pool is used for identity reads only. Tournament/auth-write/persistence transactions still use their dedicated guarded sessions.

Default pool size is two plus three dedicated sessions: five connections per backend process. Connection budgeting must include revision overlap and provider limits.

## 3. Mixed local move and tournament-read workload

Accounts are pre-provisioned in a disposable local database. Each active player has one ordinary-game socket and a second socket polling a tournament containing 60 participants every 500 ms. Each pair plays a scripted 20-ply sequence. Login hashing, AI search and cold connection setup are excluded.

### Two eligible CPUs

| Active players | WebSocket clients | Move RTT p95 | Tournament-read RTT p95 |
| --- | ---: | ---: | ---: |
| 10 | 20 | 0.75 ms | 8.11 ms |
| 20 | 40 | 0.74 ms | 7.36 ms |
| 40 | 80 | 1.35 ms | 10.51 ms |
| 60 | 120 | 1.15 ms | 11.89 ms |

At 60 players: move p99 **1.61 ms**, read p99 **15.14 ms**. Peak resident memory was **29,232 KiB**. Short polling windows and move bursts produced zero client errors and 65 saved games across the tested batches.

### One eligible CPU, two workers, two identity-read connections

The final local pooled-server run also passed the 10/20/40/60-active-account batches with zero client errors and 65 saved games. At 60:

| Metric | p95 | p99 |
| --- | ---: | ---: |
| Move RTT | 46.14 ms | 58.50 ms |
| Tournament-read RTT | 53.13 ms | 58.87 ms |

Peak resident memory was **25,972 KiB**. A full Release compilation was running concurrently; this sample is contention-sensitive and must not be treated as a controlled speed comparison with the two-CPU run.

### What these tests do not establish

- Sustained capacity of a 60-player scheduled tournament.
- Production network/TLS/cold-start latency.
- Login/Google authentication storms, many AI games or reconnect/session soaks.
- Memory peaks under concurrent Argon2id and search work.
- Large-field pairing cost or reliable operation across multiple instances.

There is no measured production maximum player count yet.

## 4. Container and shutdown evidence

The rebuilt Linux Release image passed a local restricted smoke with one CPU, 512 MiB memory, non-root execution, read-only filesystem, dropped capabilities and no-new-privileges.

Checks covered startup, required-sealing failure, valid identity loading, WebSocket operation and graceful termination. The latest text-identity image stopped cleanly in approximately **0.71 s**. A separate database-blocked request drained during shutdown in approximately **1.925 s**.

These are operational checks, not sustained throughput or memory sizing results.

## 5. Historical engine/load measurements

Earlier engine warm runs measured roughly **316–390 K nodes/s**, and an earlier 12-worker loopback burst reached 500 clients. Those runs predate current search, worker limits and release hardening.

The current engine includes quiescence search. Historical NPS/depth numbers and older virtual-memory observations cannot be used to describe today's engine or to choose a cloud memory tier. A fresh current-engine run is required before publishing updated strength/performance headlines.

A prior knight/king attack-table refactor measured only noise-sized gains; it was retained for deduplication/readability, not advertised as a speedup.

## 6. Reproduction

Build inside Linux/WSL from the project root:

```bash
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release
cmake --build build_release --parallel 2 --target chess_server bench_engine bench_provision bench_identity_reads
./build_release/bench_engine
python3 tools/load_test.py --help
```

For `bench_identity_reads`, supply `DATABASE_URL` securely through the environment, then run:

```bash
taskset -c <eligible-cpu> ./build_release/bench_identity_reads
```

It makes read-only queries with a statement timeout and prints only timings. Use a warm process/connection comparison and repeat runs rather than reporting a single cold result.

Load tools can provision accounts and persist games. Run them against a deliberately selected disposable local database, never by accidentally sourcing production credentials. Read each tool's arguments/setup before running. The capacity script is a self-contained operational benchmark, not an argparse CLI: inspect its source and local database prerequisites before executing it.

## 7. Production benchmark gate

After deployment, measure one warm backend instance with the real hosted database:

1. Fix CPU quota, memory, worker count and pool size.
2. Separate baseline network RTT, database wait and application time.
3. Exercise human moves, tournament reads and a realistic polling cadence together.
4. Add bounded login, AI and reconnect workloads separately.
5. Report p50/p95/p99, failures, CPU, resident memory and database connections over a sustained interval.
6. Stop below the first unacceptable tail-latency/error/memory threshold and retain a safety margin.

Min-zero/max-one is an instance policy, not a capacity result or billing guarantee. Database RTT and serialized transaction work can become limiting well before socket count.
