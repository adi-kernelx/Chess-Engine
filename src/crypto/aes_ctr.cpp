#include "crypto/aes_ctr.h"

#include <openssl/crypto.h>

#include <cstring>

namespace chess {
namespace crypto {

void AesCtr::increment_counter(uint8_t counter[COUNTER_SIZE]) {
    // Big-endian increment: start at the least significant byte (the last one)
    // and propagate a carry leftwards. Stops as soon as a byte does not wrap.
    for (int i = static_cast<int>(COUNTER_SIZE) - 1; i >= 0; --i) {
        if (++counter[i] != 0) break;   // no carry out of this byte
    }
    // If every byte wrapped the counter returns to zero. That requires 2^128
    // blocks, which is not reachable.
}

void AesCtr::process(const uint8_t key[KEY_SIZE],
                     const uint8_t counter_block[COUNTER_SIZE],
                     const uint8_t* in, uint8_t* out, size_t len) {
    if (len == 0) return;

    const Aes256 cipher(key);

    uint8_t counter[COUNTER_SIZE];
    std::memcpy(counter, counter_block, COUNTER_SIZE);

    uint8_t keystream[BLOCK_SIZE];
    size_t offset = 0;

    while (offset < len) {
        cipher.encrypt_block(counter, keystream);

        // The final block is usually partial. CTR simply uses as much of the
        // keystream as it needs and discards the rest — this is why the
        // plaintext length is preserved and no padding is required.
        const size_t remaining = len - offset;
        const size_t chunk = (remaining < BLOCK_SIZE) ? remaining : BLOCK_SIZE;

        for (size_t i = 0; i < chunk; ++i) {
            out[offset + i] = static_cast<uint8_t>(in[offset + i] ^ keystream[i]);
        }

        offset += chunk;
        increment_counter(counter);
    }

    OPENSSL_cleanse(keystream, sizeof(keystream));
    OPENSSL_cleanse(counter, sizeof(counter));
}

SecureBuffer AesCtr::process(const SecureBuffer& key,
                             const uint8_t counter_block[COUNTER_SIZE],
                             const uint8_t* in, size_t len) {
    SecureBuffer out(len);
    if (key.size() != KEY_SIZE) {
        // Wrong key size is a programming error; return empty rather than
        // silently encrypting with a truncated or padded key.
        out.wipe();
        return out;
    }
    process(key.data(), counter_block, in, out.data(), len);
    return out;
}

} // namespace crypto
} // namespace chess
