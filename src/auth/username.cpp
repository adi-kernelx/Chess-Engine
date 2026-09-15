#include "auth/username.h"

namespace chess {
namespace auth {

bool valid_username(const std::string& u) {
    if (u.size() < USERNAME_MIN_LEN || u.size() > USERNAME_MAX_LEN) return false;
    for (unsigned char c : u) {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_';
        if (!ok) return false;
    }
    return true;
}

std::string to_lower_ascii(const std::string& u) {
    std::string out;
    out.reserve(u.size());
    for (unsigned char c : u) {
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + ('a' - 'A'));
        out.push_back(static_cast<char>(c));
    }
    return out;
}

} // namespace auth
} // namespace chess
