/**
 * evp_raii.h — Ownership wrappers for the OpenSSL handles used in Phase 7.3.
 *
 * The project rule is zero raw new/delete, and that has to extend to C
 * libraries that hand back owning pointers. OpenSSL's EVP objects are
 * reference-counted heap allocations freed with a bespoke function, so the
 * natural C++ expression is a unique_ptr with a stateless custom deleter.
 *
 * This matters more than it looks. The PQC code below has half a dozen early
 * returns — a size query that fails, a malformed public key, a rejected
 * signature. Written with explicit EVP_PKEY_free() calls, each of those paths
 * is a separate opportunity to leak, and a leaked EVP_PKEY holding an ML-DSA
 * private key is 4 KB of secret material stranded on the heap with no
 * destructor left to cleanse it. With unique_ptr the unwinding is automatic
 * and uniform, so adding an error branch cannot introduce a leak.
 *
 * The deleters are empty structs rather than function pointers so that
 * sizeof(EvpPkeyPtr) == sizeof(EVP_PKEY*): a function-pointer deleter would
 * double the size of every handle for no benefit.
 */

#pragma once

#include <openssl/evp.h>

#include <memory>

namespace chess {
namespace crypto {

struct EvpPkeyDeleter {
    void operator()(EVP_PKEY* p) const noexcept { EVP_PKEY_free(p); }
};

struct EvpPkeyCtxDeleter {
    void operator()(EVP_PKEY_CTX* p) const noexcept { EVP_PKEY_CTX_free(p); }
};

using EvpPkeyPtr    = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
using EvpPkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter>;

} // namespace crypto
} // namespace chess
