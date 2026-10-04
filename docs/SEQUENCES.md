# Request and Lifecycle Sequences

Execution order, ownership boundaries and commit points for the shipping architecture. See [Architecture](ARCHITECTURE.md) and the [UML overview](../UML/README.md).

## 1. Accepted move

1. TcpServer schedules the connection's input onto a worker.
2. RequestPipeline identifies/decodes the route and applies its authorization policy.
3. GameplayService resolves the caller's room/seat and submits the move.
4. Under the room mutex, GameRoom verifies turn/legality and updates position, history, clocks and revision.
5. The room records move/completion events and builds any final snapshot.
6. The room lock is released before listener callbacks deliver replies/broadcasts.
7. Players and spectators receive the authoritative state. The browser interpolates clocks for display only.

An illegal move does not update the board or produce opponent/spectator move broadcasts. A finished room cannot accept another move. Slow database work is not performed under the room/transport-wide lock.

## 2. Durable completion and retry

1. GameRoom transitions to finished under its mutex.
2. It constructs an immutable final snapshot with a completion identity.
3. GameCompletionService receives the event after the room unlocks.
4. The persistence adapter checks/inserts that completion in a transaction.
5. Game, timings and applicable player statistics/ratings commit atomically.
6. The durable game ID is returned and associated with the tournament pairing when applicable.
7. Repeated delivery recognizes the completion rather than creating a second game or rating change.

The database COMMIT is the durable boundary. A final snapshot held only in memory can still be lost on process failure before successful persistence. Retry is not a durable outbox.

Failure is reported and retained for retry; it is not represented as a completed durable save. Live room ID and replay game ID must never be substituted for one another.

## 3. Sealed login or Google exchange

1. Browser sends seal_request.
2. Auth handling rate-limits issuance and obtains fresh one-time ML-KEM/X25519 public material with expiry/key ID.
3. Server signs the offer with its private ML-DSA identity.
4. Browser checks the public identity pin and signature.
5. Browser encapsulates/agrees on hybrid shared material and derives payload keys with HKDF.
6. It encrypts/authenticates the payload, including the inner action.
7. SealedRegistry consumes the identified offer once, authenticates/decrypts and checks the action.
8. AuthHandler verifies password or the Google identity token.
9. A database transaction creates the application session; browser receives application access/refresh tokens.

No master secret is sent in the offer. Invalid identity, signature, provider, ciphertext, action or reused/expired offer fails without plaintext fallback. See [Security](SECURITY.md).

## 4. Refresh and route restoration

1. Reload creates a new socket and begins session restoration from the saved refresh token.
2. Refresh is correlated with its pending operation; route changes do not imply authentication failure.
3. The server validates/rotates the token transactionally.
4. Bounded rotation recovery handles eligible rapid refresh races.
5. Client replaces the refresh token and restores access identity.
6. Current screen requests its game, tournament or saved replay state.

A timeout, unavailable identity read or pending connection is distinct from a definitively revoked/invalid session. Session restoration cannot revive a live room lost to a process restart.

## 5. Scheduled tournament and check-in

1. Creator submits format, time control and schedule; server validates and persists it.
2. Players register while allowed.
3. Registration closes at the deadline or first-round pairing lock, whichever occurs first.
4. At the first-round 90-second lock, pairings are prepared and reopening is denied.
5. Authenticated participants check in independently; early check-in reserves their seats.
6. At scheduled start, eligible occupied rooms become playable. Missing-seat rooms wait without running chess clocks.
7. Check-in expiry resolves remaining no-shows as single/double forfeits.
8. Finished played games persist and attach their own replay IDs.
9. Once every current pairing resolves, the manager schedules the next stage or completes the tournament.

Unregistered/eliminated users may spectate a live game but cannot take a reserved seat. Bye/unplayed pairings do not create unrelated replay links.

## 6. Winners Advance progression

1. Read all outcomes of the resolved stage.
2. Advance decisive winners and bye recipients; record eliminated players' stage.
3. A first same-pair draw creates a reversed-color replay.
4. After two draws that pair cannot play again; both remain eligible.
5. Build legal subsequent pairings or complete when no legal pairing remains.
6. Rank by advancement/elimination stage; apply wins only to ties among final survivors.

No Swiss round-count limit and no ordinary Rematch action drive this progression. Creator corrections cannot invalidate a stage that already advanced.

## 7. Engine snapshot and stale-result guard

Search receives an isolated position/revision snapshot rather than mutable room state. Before applying a result, gameplay checks that the room/revision remains compatible. A resignation, completion or newer position invalidates an obsolete result.

This boundary supports safe independent search state. It is not evidence of an unbounded asynchronous engine service or distributed job queue.

## 8. Pooled identity read

1. Identity extraction validates the application token.
2. It acquires an exclusive identity-read lease with bounded wait.
3. The fresh profile/epoch query verifies identity and revocation state.
4. The lease is returned; unfinished transactions are rolled back before reuse.
5. Query/checkout failure returns unavailable; an actual epoch mismatch returns unauthorized.

Write transactions retain their existing dedicated guarded sessions. Pooling does not permit two transactions to interleave on one connection.
