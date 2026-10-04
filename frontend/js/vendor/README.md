# Pinned browser cryptography

`pqc.min.js` is a locally served ESM bundle, not a CDN dependency. It exports
only ML-KEM-768 and ML-DSA-65 from noble/post-quantum 0.7.1. Native WebCrypto
implements X25519 and the existing symmetric envelope construction.

Upstream: https://github.com/paulmillr/noble-post-quantum

`pqc-manifest.json` records exact dependency/build-tool versions, npm archive
SHA-512 integrities and the bundle SHA-256. MIT licences are included. Rebuild
with `node tools/vendor_pqc.mjs` on x64 Windows/Linux; this fetches pinned
archives, checks integrity, and bundles without npm installation or lifecycle
scripts. An existing lock rejects changed package integrity or bundle output.
Dependency upgrades require explicit review and a deliberate lock update.

Tests: `node tests/test_frontend_sealed_auth.mjs` verifies the bundle digest,
provider and auth failure paths. In WSL, `python3 tests/test_sealed_auth_network.py`
uses an isolated real C++ server, local disposable postgres_test database,
ephemeral keys and synthetic Google JWTs. It never sources `.env` or connects
to the hosted application database. Production key/pin configuration is separate.
