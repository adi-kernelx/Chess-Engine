/**
 * username.h — the whitelist that closes several downstream vectors at the door.
 *
 * A username is a string that ends up in HTML pages, JSON responses, log lines,
 * database rows, and eventually chat messages. Each of those has a different
 * escaping rule and a different set of characters that are "dangerous". Rather
 * than escape correctly at every site, we take the smaller decision at the
 * source: allow only `[A-Za-z0-9_]`, length 3..20.
 *
 *   HTML/XSS         : no `<`, `>`, `&`, `"`, `'`  — a username can never be
 *                      injected into a template unescaped.
 *   JSON             : no `"`, `\`, control bytes — no way to close a string
 *                      and forge a field.
 *   Path/URL         : no `/`, `?`, `#`, `%`, ` `  — the username is safe as a
 *                      path segment in a future spectator link.
 *   Log injection    : no CR/LF                      — no way to fake a log line.
 *   Case-fold safety : all ASCII                     — `lower()` and the DB
 *                                                     collation agree, so
 *                                                     `username_ci` matches
 *                                                     what the code computes.
 *   Homograph attack : no Unicode                    — no Cyrillic 'а' looking
 *                                                     like Latin 'a'.
 *
 * The password does NOT go through this filter, on purpose: passwords must not
 * have their character set restricted. See password.h.
 */

#pragma once

#include <cstddef>
#include <string>

namespace chess {
namespace auth {

constexpr size_t USERNAME_MIN_LEN = 3;
constexpr size_t USERNAME_MAX_LEN = 20;

/// True iff `u` is 3-20 ASCII chars from `[A-Za-z0-9_]`.
bool valid_username(const std::string& u);

/// ASCII-only lowercase. Safe because valid_username() forbids anything else.
std::string to_lower_ascii(const std::string& u);

} // namespace auth
} // namespace chess
