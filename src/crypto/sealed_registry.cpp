#include "crypto/sealed_registry.h"

#include "crypto/base64.h"
#include "crypto/sealed_envelope.h"

#include <nlohmann/json.hpp>

namespace chess {
namespace crypto {

using json = nlohmann::json;

namespace {

/// Read a base64 string field of an exact expected length.
bool read_b64(const json& obj, const char* field, size_t expected_len,
              std::vector<uint8_t>& out) {
    auto it = obj.find(field);
    if (it == obj.end() || !it->is_string()) return false;
    return decode_base64(it->get<std::string>(), out, expected_len);
}

} // namespace

SealedRegistry::SealedRegistry(const MlDsa65KeyPair& identity,
                               SealedKeyStore& store)
    : identity_(identity), store_(store) {}

void SealedRegistry::require_sealed(const std::string& type) {
    std::lock_guard<std::mutex> lock(mutex_);
    required_.insert(type);
}

bool SealedRegistry::is_required(const std::string& type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return required_.count(type) != 0;
}

std::string SealedRegistry::handle_seal_request(const std::string& client_ip) {
    SealKeyOffer offer;
    if (!store_.issue(identity_, client_ip, offer)) return std::string();

    // Built by hand rather than via nlohmann so that "type" stays the first
    // key — see the header note about websocket.cpp's string scan.
    std::string out = "{\"type\":\"seal_key\"";
    // The identity public key travels with the offer so the browser can hash it
    // and compare against PINNED_KEYS. Sending the key rather than baking it
    // into config.js is what makes rotation a safe rollout: the frontend pins a
    // LIST of hashes, so a new key can be accepted before the old one is retired.
    // Sending it is not a trust decision — an unpinned key is refused.
    out += ",\"identity_pk\":\"" + encode_base64(identity_.public_key()) + "\"";
    out += ",\"key_id\":\""    + encode_base64(offer.key_id)    + "\"";
    out += ",\"kem_ek\":\""    + encode_base64(offer.kem_ek)    + "\"";
    out += ",\"x25519_pk\":\"" + encode_base64(offer.x25519_pk) + "\"";
    out += ",\"expires_in\":"  + std::to_string(offer.expires_in);
    out += ",\"signature\":\"" + encode_base64(offer.signature) + "\"";
    out += "}";
    return out;
}

SealedRegistry::Outcome SealedRegistry::inspect(const std::string& type,
                                                const std::string& message,
                                                std::string& out_plaintext) {
    out_plaintext.clear();
    if (!is_required(type)) return Outcome::NotSealed;

    // From here on the type is one that MUST be sealed, so every path that is
    // not a successful open ends in Rejected.
    json root = json::parse(message, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) return Outcome::Rejected;

    auto sealed_it = root.find("sealed");
    if (sealed_it == root.end() || !sealed_it->is_object()) {
        // The downgrade attempt: a required type that arrived in the clear.
        return Outcome::Rejected;
    }
    const json& s = *sealed_it;

    SealedEnvelope env;
    if (!read_b64(s, "key_id",    seal::KEY_ID_SIZE,             env.key_id))    return Outcome::Rejected;
    if (!read_b64(s, "kem_ct",    mlkem768::CIPHERTEXT_SIZE,     env.kem_ct))    return Outcome::Rejected;
    if (!read_b64(s, "x25519_pk", x25519::PUBLIC_KEY_SIZE,       env.x25519_pk)) return Outcome::Rejected;
    if (!read_b64(s, "iv",        seal::IV_SIZE,                 env.iv))        return Outcome::Rejected;
    if (!read_b64(s, "tag",       seal::TAG_SIZE,                env.tag))       return Outcome::Rejected;

    // ct is the one variable-length field, so it is decoded without a length.
    auto ct_it = s.find("ct");
    if (ct_it == s.end() || !ct_it->is_string()) return Outcome::Rejected;
    if (!decode_base64(ct_it->get<std::string>(), env.ct)) return Outcome::Rejected;

    // Consume the key BEFORE attempting to open. The entry is gone either way,
    // so a failed forgery burns the key rather than allowing a grind against it.
    SealKeyMaterial material;
    if (!store_.consume(env.key_id, material)) return Outcome::Rejected;

    SecureBuffer payload;
    if (!SealedEnvelopeService::open(material, env, payload)) return Outcome::Rejected;

    // The payload is attacker-influenced right up to the moment the tag
    // verified, so it is parsed, not trusted. Re-serialising through nlohmann
    // also guarantees the rewritten message is well-formed JSON no matter what
    // the plaintext contained.
    const std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
    json body = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded() || !body.is_object()) return Outcome::Rejected;

    // A sealed payload must not smuggle its own "type": the routing decision
    // was already made from the outer frame, and letting the inside override it
    // would mean one registered type could be opened and dispatched as another.
    body.erase("type");

    const std::string dumped = body.dump();          // "{...}", keys escaped
    out_plaintext = "{\"type\":" + json(type).dump();
    if (dumped.size() > 2) {
        out_plaintext += "," + dumped.substr(1);     // drop the leading '{'
    } else {
        out_plaintext += "}";
    }
    return Outcome::Opened;
}

} // namespace crypto
} // namespace chess
