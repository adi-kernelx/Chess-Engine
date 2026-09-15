#include "crypto/random.h"

#include <openssl/err.h>
#include <openssl/rand.h>

#include <cstdio>
#include <cstdlib>

namespace chess {
namespace crypto {

void secure_random_bytes(uint8_t* out, size_t len) {
    if (len == 0) return;

    // RAND_bytes takes an int; guard the conversion rather than truncating.
    if (len > static_cast<size_t>(INT32_MAX)) {
        std::fprintf(stderr,
                     "[FATAL] secure_random_bytes: request of %zu bytes is too large\n", len);
        std::abort();
    }

    if (RAND_bytes(out, static_cast<int>(len)) != 1) {
        // Deliberately fatal. See the failure policy in random.h — a system
        // without working entropy must not be allowed to serve traffic.
        unsigned long err = ERR_get_error();
        char err_buf[256] = {0};
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        std::fprintf(stderr, "[FATAL] RAND_bytes failed: %s\n", err_buf);
        std::abort();
    }
}

SecureBuffer secure_random_buffer(size_t len) {
    SecureBuffer buf(len);
    secure_random_bytes(buf.data(), buf.size());
    return buf;
}

} // namespace crypto
} // namespace chess
