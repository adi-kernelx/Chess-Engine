/**
 * sealed_registry.h — the opt-in layer that turns the envelope into a service.
 *
 * Decision O2: this must be reusable. A future private-chat feature should be
 * able to protect its messages with zero new cryptographic work — one line:
 *
 *     sealed_registry.require_sealed("send_message");
 *
 * That is the entire integration. The registry knows nothing about what it is
 * protecting; it sees a message type and a blob.
 *
 * WHERE IT SITS
 *
 *     incoming frame
 *        ├─ type not registered  → dispatch unchanged (every existing handler)
 *        ├─ type registered, arrived sealed   → open, replace payload, dispatch
 *        └─ type registered, arrived UNSEALED → REJECT, never dispatch
 *
 * The third branch is the one that carries weight. Without it, an attacker
 * strips the envelope and sends `{"type":"login","password":"…"}` in the clear,
 * and the whole construction is optional — which is to say, absent. A downgrade
 * that the server accepts is not a downgrade, it is the protocol.
 *
 * HANDLERS STAY OBLIVIOUS
 *
 * After a successful open, the plaintext payload is re-emitted as an ordinary
 * message with its `type` restored, and dispatched normally. handle_login()
 * receives exactly what it would have received unsealed and cannot tell how it
 * arrived. That is what makes this a service rather than a special case welded
 * onto the auth path.
 *
 * A note on `type` placement: websocket.cpp finds `type` by string-scanning
 * rather than parsing, and takes the FIRST match. The rewritten message is
 * therefore built with `"type"` first, by hand, rather than handed to
 * nlohmann — which sorts object keys and would bury it.
 *
 * LAYERING
 *
 * This class deliberately does not include anything from net/. It converts
 * strings to strings; main.cpp does the wiring. Keeping crypto/ free of a
 * dependency on the network layer is what allows test_sealed_envelope to
 * exercise the whole downgrade-rejection path without a socket.
 */

#pragma once

#include "crypto/sealed_key_store.h"
#include "crypto/signature.h"

#include <mutex>
#include <string>
#include <unordered_set>

namespace chess {
namespace crypto {

class SealedRegistry {
public:
    /**
     * @param identity  the server's long-term ML-DSA-65 key; must outlive this.
     * @param store     one-time key store; must outlive this.
     */
    SealedRegistry(const MlDsa65KeyPair& identity, SealedKeyStore& store);

    /// Opt a message type in. Call at start-up, before serving.
    void require_sealed(const std::string& type);
    bool is_required(const std::string& type) const;

    /**
     * Build the JSON reply to a `seal_request`.
     * Returns an empty string if the client is over its cap — the caller should
     * answer with a generic error, not with detail about the limit.
     */
    std::string handle_seal_request(const std::string& client_ip);

    enum class Outcome {
        NotSealed,   ///< type is not registered; dispatch the original message
        Opened,      ///< out_plaintext holds the rewritten message
        Rejected     ///< drop it; never dispatch
    };

    /**
     * Decide what to do with an incoming message.
     *
     * Every failure — missing envelope, malformed base64, unknown or expired
     * key id, bad tag — returns Rejected with nothing written. The caller must
     * not report which.
     */
    Outcome inspect(const std::string& type, const std::string& message,
                    std::string& out_plaintext);

private:
    const MlDsa65KeyPair& identity_;
    SealedKeyStore&       store_;

    mutable std::mutex             mutex_;
    std::unordered_set<std::string> required_;
};

} // namespace crypto
} // namespace chess
