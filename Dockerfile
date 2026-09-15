# syntax=docker/dockerfile:1.6
#
# Dockerfile — Phase 7.10 deployment-readiness artifact.
#
# Two-stage build:
#   1. `builder` — apt-installs the toolchain, configures with cmake, runs make.
#   2. `runtime` — a slim image that carries only the compiled `chess_server`
#      binary and the runtime shared libraries it links against.
#
# Ubuntu 26.04 was picked so `libssl-dev` gets 3.5.5 — the first apt-shipped
# OpenSSL with ML-KEM (FIPS 203), ML-DSA (FIPS 204) and Argon2id in the
# default provider. Do NOT downgrade the base image; a build against
# OpenSSL 3.0.2 (Ubuntu 22.04) fails at link time on `EVP_KDF_fetch("ARGON2ID")`
# and at runtime on the ML-* algorithm lookups. The project migrated distros
# for exactly this reason (see docs/implementation_log.md, Sept 2026).
#
# `libpq-dev` gives us the Postgres client library. There is no server here —
# the deployed instance talks to Supabase over TLS.
#
# ── Cloud Run notes ────────────────────────────────────────────────────────
# * The container listens on the port passed via $PORT (`main.cpp::resolve_port`).
#   Cloud Run sets it to 8080 by default; local dev without $PORT falls back
#   to 9000.
# * `--set-secrets` mounts secrets as files:
#     JWT_SIGNING_KEY=projects/…/secrets/jwt-signing-key/versions/latest
#     SERVER_IDENTITY_KEY_PATH=/secrets/server_identity.key
#     …plus SUPABASE_JWT_SECRET, SUPABASE_ISSUER, SUPABASE_AUDIENCE.
# * DATABASE_URL is passed via `--set-secrets` too (postgres://... string).
# * See docs/SECURITY.md for the full env-var contract and rotation procedure.
#
# ── What this file DOES NOT do ─────────────────────────────────────────────
# It builds an image. It does not push, does not deploy, does not touch a
# Cloud Run project. Live deploy is deferred to after the whole project is
# complete (user constraint recorded during §7.10).

# ── Stage 1: build ─────────────────────────────────────────────────────────
FROM ubuntu:26.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        libssl-dev \
        libpq-dev \
        nlohmann-json3-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src/     src/
COPY tests/   tests/
COPY tools/   tools/

# -O3 + -DNDEBUG. This is Release; the CLAUDE.md note about the default no-O
# build applies to the local WSL dev loop only.
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target chess_server -j "$(nproc)"

# ── Stage 2: runtime ───────────────────────────────────────────────────────
FROM ubuntu:26.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 \
        libpq5 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --user-group --no-create-home chess

WORKDIR /app
COPY --from=builder /src/build/chess_server ./chess_server

USER chess

# Cloud Run overrides EXPOSE, but declaring it documents intent and helps
# `docker run` locally. The actual bind port comes from $PORT at runtime.
EXPOSE 8080

# Fail fast on any unhandled signal; Cloud Run treats a non-zero exit as
# "instance unhealthy" and starts a new one.
ENTRYPOINT ["/app/chess_server"]
