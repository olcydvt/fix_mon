#include "fixmon/redact.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace fixmon {

namespace {

// Lowercases and drops separators so PrivateKey, private_key, PRIVATE-KEY and
// "private key" all collapse onto the same token.
std::string fold(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '_' || c == '-' || c == '.' || c == ' ') continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// Whole-name matches. These are only secrets when they are the entire key;
// treating them as fragments would swallow unrelated settings.
constexpr std::array<std::string_view, 8> kExact{
    "username", "user", "userid", "login", "logonname", "account", "accountid", "authenticator",
};

// Substring matches. QuickFIX itself only defines a handful of these, but SSL
// wrappers and vendor forks invent their own names constantly, so match on the
// fragment instead of guessing the full list.
constexpr std::array<std::string_view, 14> kFragments{
    "password", "passwd", "passphrase", "secret",   "token",   "apikey",
    "credential", "privatekey", "keystore", "truststore", "certificatekey",
    "sharedkey", "hmac", "salt",
};

}  // namespace

bool is_sensitive_key(std::string_view key) {
    const std::string k = fold(key);
    if (k.empty()) return false;

    for (std::string_view e : kExact) {
        if (k == e) return true;
    }
    for (std::string_view f : kFragments) {
        if (k.find(f) != std::string::npos) return true;
    }
    return false;
}

std::string redact_if_sensitive(std::string_view key, const std::string& value) {
    return is_sensitive_key(key) ? std::string(kRedacted) : value;
}

}  // namespace fixmon

