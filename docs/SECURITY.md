# Security

Security model and operational controls for the chess platform. See [Architecture](ARCHITECTURE.md) for boundaries and [Protocol](PROTOCOL.md) for messages.

## 1. Threat model and limitations

The service treats incoming frames, user input and the public frontend as untrusted. It aims to resist unauthorized gameplay/account actions, credential enumeration, malformed input, envelope replay/downgrade and browser access to backend tables.

A compromised operator, host, frontend origin or trusted database role is outside the protection offered by transport sealing. Custom cryptography remains security-sensitive even with known-answer/differential tests; this is not a claim of independent security audit or formally proven constant-time behavior.

HTTPS/WSS is required in production. The sealed authentication layer complements TLS rather than replacing it.

## 2. Cryptographic responsibilities

| Purpose | Mechanism |
| --- | --- |
| Password storage | OpenSSL Argon2id |
| Application access tokens | HMAC-SHA-384 |
| Refresh tokens | CSPRNG-generated opaque tokens; SHA-384 hashes in storage |
| Google identity | ES256/P-256 signatures, public HTTPS JWKS |
| Sealed payload agreement | ML-KEM-768 + X25519 hybrid |
| Sealed payload encryption/integrity | HKDF, AES-256-CTR + HMAC-SHA-384 |
| Server offer identity | ML-DSA-65 and pinned public identity |
| Randomness | OpenSSL CSPRNG |

Hash/HMAC/HKDF/AES implementations have known-answer and differential tests. Lattice primitives, X25519, P-256 verification, randomness and password hashing use library implementations rather than educational replacement code.

## 3. Sessions and revocation

The backend issues its own application session after password or Google login. Access tokens are short-lived and held in browser memory; the refresh token is persisted for reload recovery.

Refresh rotation is transactional. Migration 0011 adds rotation timestamps for bounded recovery of rapid refresh races. Recovery is not permission to reuse an old token indefinitely.

Logout revokes the current refresh family. Logout-all changes the player's token epoch and revokes sessions. Fresh identity/epoch reads preserve revocation visibility; pooled-read saturation is retryable rather than an unauthorized verdict.

The persisted refresh token is a high-value browser secret. XSS can still read/use it. CSP and avoiding untrusted executable content reduce risk but do not turn localStorage into a secure vault.

Production must verify refresh recovery beyond the access-token lifetime. Deployment itself does not fix an unexplained logout.

## 4. Google identity verification

Supabase performs the OAuth redirect flow. The backend validates the returned identity and exchanges it for an application session.

ES256 mode verifies signature, issuer, audience, supported Google provider and identity/claim requirements. Stable provider identity maps repeat logins to the same account. Accounts are not silently linked solely because emails match.

Public keys are fetched only from the configured standard Supabase project endpoint using verified HTTPS. Custom auth domains and RS256 are not supported by the current verifier.

Fetch controls include a 1.5-second connection timeout, 3-second overall timeout, bounded response/key count, no redirects, a five-minute cache and 30-second refresh cooldown. Unknown key IDs trigger a bounded refresh; expired cached keys are not accepted during an outage.

Explicit ES256 rejects HS256 even if a legacy secret exists. Legacy HS256 is a separate compatibility mode, not automatic signature fallback. The application does not need Supabase's private signing key or legacy JWT shared secret in ES256 mode.

Production OAuth origins/redirect allowlists and browser CSP must match the actual deployed frontend and Supabase origin.

## 5. Sealed authentication

When sealing is configured, login, registration and google_auth require a sealed payload.

1. Client requests a fresh offer containing one-time public key material and a server identity signature.
2. Client verifies the pinned public identity and offer signature before transmitting credentials.
3. ML-KEM encapsulation and ephemeral X25519 agreement produce shared key material; HKDF derives encryption/authentication keys.
4. Client sends an encrypted/authenticated payload, including its action, against the single-use offer.
5. Server consumes the offer, verifies integrity/action and invokes the auth route.

Offer expiry and per-IP/global limits bound short-lived private material. Tampering, replay, missing/mismatched inner actions and plaintext downgrade attempts are rejected. No shared encryption secret is transmitted in an offer.

The frontend uses a locally vendored crypto bundle with pinned archive/dependency/bundle integrity metadata and licenses. Trust/provider/crypto failure never falls back to plaintext. Sensitive auth is sent immediately, not stored in the reconnect queue.

Temporary byte secrets are wiped best-effort; JavaScript strings and garbage collection do not provide secure-memory guarantees. Refresh remains TLS-protected rotating-token traffic; moves are not part of a persistent sealed channel.

## 6. Runtime configuration

| Setting | Handling |
| --- | --- |
| PORT | Runtime-injected listener port; default 9000 locally |
| DATABASE_URL | Secret: PostgreSQL URI containing credentials |
| JWT_SIGNING_KEY | Secret: base64 encoding of exactly 32 random bytes |
| SERVER_IDENTITY_KEY_PATH | Secret-file mount path for the private ML-DSA identity |
| AUTH_SEAL_REQUIRED | Set 1 in production; missing auth/identity fails startup |
| SUPABASE_JWT_ALGORITHM | ES256 for P-256 public-key verification |
| SUPABASE_ISSUER | Public project issuer ending in /auth/v1 |
| SUPABASE_AUDIENCE | authenticated |
| SUPABASE_PROVIDER | google by default |
| SUPABASE_JWT_SECRET | Secret, legacy HS256 only; unused in explicit ES256 |
| AUTH_READ_POOL_SIZE | 0–4, default 2 |
| SERVER_WORKER_THREADS | 1–8, default 4; intended one-vCPU starting point: 2 |

A configured malformed identity fails startup even in compatibility mode. Only an explicit local profile without required sealing/identity can run unsealed auth. Production frontend pin failure is fail-closed.

Pooling is narrow: default two identity-read sessions plus three dedicated write/tournament/persistence sessions. Leases preserve transaction ownership and rollback unfinished transactions. This does not parallelize all writes or guarantee capacity.

### Text-safe identity files

The loader accepts the original 4,032-byte raw private key or a strict text wrapper:

- Header: `CHESS-ML-DSA-65-PRIVATE-KEY-V1`.
- One standard-base64 payload line encoding the same private key.
- LF/CRLF and an optional single final newline.
- Strict marker/alphabet/padding/size validation; extra content rejected.

The converter [convert_server_identity.mjs](../tools/convert_server_identity.mjs) creates a new output exclusively, preserves the source and does not print private bytes. The public pin stays unchanged because the identity is unchanged.

**Base64 is not encryption.** Protect raw and text copies equally. Never paste private keys or database/JWT secrets into public documentation, frontend configuration or build inputs. Use the rebuilt loader/image when mounting text identity files.

## 7. Database isolation

[Migration 0014](../src/storage/migrations/0014_backend_table_security.sql) enables default-deny RLS and revokes browser-role/PUBLIC privileges on backend tables and relevant sequences. It pins the username-trigger search path and adds six foreign-key indexes.

Application access is through the backend's parameterized SQL. Custom application JWTs do not become Supabase table identities. RLS enabled with no browser policy is intentional: adding allow-all policies merely to silence an Advisor warning would remove the boundary.

The trusted backend role's owner/BYPASSRLS access must be verified. This migration does not impose FORCE RLS on that trusted path. New table migrations must include appropriate grants/RLS explicitly.

The hosted migration and post-application denial/backend checks are recorded as complete. A fresh release must still verify role configuration; never assume a provider warning means a permissive policy is required.

## 8. Browser, transport and container hardening

- Structural JSON decoding and bounded frame/write buffers.
- Authentication/seat checks before mutations; spectators remain read-only.
- Rate limits before expensive credential/offer work.
- Non-enumerating password errors and dummy-hash verification for missing accounts.
- Redacted sensitive and unknown request logs.
- CSP limited to required script/connect origins, framing restrictions and security headers.
- Static frontend contains public pins/configuration, never backend private keys.
- Multi-stage non-root image and a build context excluding credentials.
- Runtime secrets supplied through scoped environment/file mounts, not image layers.
- Signal handlers request shutdown; ordinary code performs cleanup.

Configured CSP/backend placeholders must be replaced before publishing. A local restricted Docker smoke has passed; cloud secret access and production behavior remain deployment gates.

## 9. Rotation and incident response

### Application signing key

Generate a new random key into a protected file, add a secret version and roll the backend to that version. Existing access signatures become invalid; intact refresh sessions can issue new access tokens. Clients may need explicit recovery if stale access is presented before refreshing.

Do not promise a seamless cutover without a staging rehearsal. Retire the old secret after confirming the rollout and its rollback implications. Never log generated key content.

### Server identity

Publish the new public pin alongside the old pin first, then roll the backend to the new private identity. Verify sealed auth, allow in-flight offers/cached frontends to settle, and remove the old pin in a controlled frontend rollout.

A compromised identity demands a tighter cutover; keeping its pin trusted keeps accepting the compromised signer. Disable retired secret versions after validating rollout and rollback decisions.

### Supabase keys and database password

Supabase signing-key rotation must allow its public-key propagation/cache window. Application sessions use the backend's own tokens, so a provider-key change is not itself application logout.

For a leaked database credential, rotate the role password, update the secret and restart/roll connections. For a refresh leak, revoke affected sessions; for a broader identity compromise, revoke epochs/families and review account activity.

## 10. Production acceptance gates

- Required sealing, correct private identity and matching public pins.
- Verified secret mounts and least-privilege runtime secret access.
- Real HTTPS/WSS origins, CSP and OAuth redirect configuration.
- Password and Google login, same-account repeat login and sign-out.
- Refresh/session soak beyond access-token expiry.
- Cold-start and reconnect checks on the actual hosted service.
- Role-denial checks, durable completion/persistence and a tested rollback path.

No security mechanism here provides distributed live-room recovery or guarantees a zero-cost deployment.
