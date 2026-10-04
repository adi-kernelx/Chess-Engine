/**
 * application/auth/identity_extractor.h — the "who is calling" step in one
 * place, shared across every handler family that needs authenticated
 * identity.
 *
 * BEFORE (Phase 9 → LLD-1)
 *   `GameHandler::extract_identity` did this inline: check DB/signer,
 *   pull the `access_token`, run `authorize_access_token`, look up the
 *   `players` row, hand back `{db_player_id, username, elo_rating}`.
 *   Every handler class that needed identity would have duplicated it.
 *
 * NOW (LLD-2)
 *   One `IdentityExtractor` owned by the composition root. Every family
 *   handler holds a reference and calls `extract()` before it delegates
 *   to its service. Auth failure is returned as a `Result` carrying the
 *   pre-composed `auth_required` error code + message; the handler is
 *   responsible for encoding the wire frame from that Result — no
 *   sockets or JSON leak into this class.
 *
 * WHY NOT A "PIPELINE STAGE" INSTEAD
 *   LLD-5 plans an ordered request pipeline where auth is one stage.
 *   Until then, a plain shared helper is the smallest change that
 *   removes duplication without pre-committing to the pipeline shape.
 *
 * WIRE PARITY
 *   Every error path preserves the exact `auth_required` message
 *   `GameHandler::extract_identity` produced. Tests that grep the
 *   frame for the human-readable message keep matching.
 */

#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "application/request_context.h"
#include "application/result.h"
#include "auth/token.h"
#include "storage/database.h"
#include "storage/database_pool.h"

namespace chess::application::auth {

/// The wire error code emitted for rejected credentials (not SQL failure). Kept as a
/// constant so both the extractor and any consumer that composes an
/// error frame agree on the string.
inline constexpr const char* kAuthRequiredCode = "auth_required";

class IdentityExtractor {
public:
    /// Both dependencies MUST outlive this extractor. `db` and `signer`
    /// may be null when the server was launched without auth — in that
    /// case every `extract()` call fails closed with the same
    /// `auth_required` shape a bad token would produce, so no route
    /// silently downgrades.
    IdentityExtractor(chess::storage::Database* db,
                      chess::auth::TokenSigner* signer,
                      /// Injected clock in unix seconds. Defaults to
                      /// `std::time(nullptr)`; the fake in tests pins it.
                      int64_t (*now_unix)() = nullptr,
                      chess::storage::DatabasePool* read_pool = nullptr);

    /// Extract the caller's identity from the message's `access_token`
    /// field. Success returns an `AuthenticatedIdentity`; failure
    /// returns `Unauthorized` with the same wire-visible message the
    /// old `extract_identity` sent.
    Result<AuthenticatedIdentity> extract(const nlohmann::json& msg) const;

private:
    chess::storage::Database* db_     = nullptr;
    chess::auth::TokenSigner* signer_ = nullptr;
    int64_t (*now_unix_)()            = nullptr;
    chess::storage::DatabasePool* read_pool_ = nullptr;
};

} // namespace chess::application::auth
