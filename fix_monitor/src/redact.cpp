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

// Words that routinely follow a credential noun without being its value:
// "invalid username or password for user X" must mask X, not "or" and not
// "for". Without this the redaction would eat the sentence it is protecting.
constexpr std::array<std::string_view, 34> kStopWords{
    "or", "and", "for", "the", "a", "an", "is", "was", "not", "no", "in", "of",
    "to", "with", "from", "invalid", "incorrect", "bad", "missing", "required",
    "expired", "mismatch", "error", "failed", "failure", "does", "did", "must",
    "cannot", "empty", "null", "unknown", "rejected", "wrong",
};

bool is_stop_word(std::string_view folded) {
    for (std::string_view w : kStopWords) {
        if (folded == w) return true;
    }
    return false;
}

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Trims non-alphanumeric characters from both ends so "password:" and "(user"
// reach is_sensitive_key as the word they actually are.
std::string_view strip_punct(std::string_view s) {
    while (!s.empty() && !std::isalnum(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && !std::isalnum(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

inline bool is_fix_separator(char c) { return c == '\x01' || c == '|'; }

// Tags whose value is prose a human typed. Too useful to diagnosis to drop -
// "MsgSeqNum too low, expecting 5 but received 2" is the whole answer - so they
// are scrubbed word by word rather than replaced wholesale.
bool is_free_text_tag(int tag) {
    switch (tag) {
        case 58:   // Text
        case 147:  // Subject
        case 148:  // Headline
            return true;
        default:
            return false;
    }
}

// The same prose in a foreign character set. We cannot scrub what we cannot
// reliably decode, and it mirrors a field we know carries login material, so it
// goes entirely regardless of policy.
bool is_opaque_text_tag(int tag) {
    switch (tag) {
        case 355:  // EncodedText
        case 349:  // EncodedIssuer
        case 351:  // EncodedSecurityDesc
            return true;
        default:
            return false;
    }
}

// Length-prefixed pairs: the value of `data` may contain the SOH separator, so
// its end has to come from the declared length rather than from a scan.
int data_tag_for_length_tag(int tag) {
    switch (tag) {
        case 90:  return 91;   // SecureDataLen  -> SecureData
        case 95:  return 96;   // RawDataLen     -> RawData
        case 354: return 355;  // EncodedTextLen -> EncodedText
        default:  return -1;
    }
}

int parse_tag(std::string_view sv) {
    if (sv.empty()) return -1;
    int v = 0;
    for (char c : sv) {
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (c - '0');
        if (v > 100000) return -1;
    }
    return v;
}

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

bool is_credential_tag(int tag) {
    switch (tag) {
        case 553:   // Username
        case 554:   // Password
        case 925:   // NewPassword
        case 1400:  // EncryptedPasswordMethod
        case 1401:  // EncryptedPasswordLen
        case 1402:  // EncryptedPassword
        case 1403:  // EncryptedNewPasswordLen
        case 1404:  // EncryptedNewPassword
        case 90:    // SecureDataLen
        case 91:    // SecureData
        case 95:    // RawDataLen   - FIX 4.2 logons carry the password here
        case 96:    // RawData
            return true;
        default:
            return false;
    }
}

bool is_business_tag(int tag) {
    switch (tag) {
        // ---- who ----
        case 1:    // Account
        case 109:  // ClientID
        case 76:   // ExecBroker
        case 448:  // PartyID
        // ---- which order ----
        case 11:   // ClOrdID
        case 17:   // ExecID
        case 37:   // OrderID
        case 41:   // OrigClOrdID
        case 198:  // SecondaryOrderID
        case 526:  // SecondaryClOrdID
        // ---- what instrument ----
        case 48:   // SecurityID
        case 55:   // Symbol
        case 65:   // SymbolSfx
        // ---- what position ----
        case 54:   // Side
        case 38:   // OrderQty
        case 44:   // Price
        case 99:   // StopPx
        case 110:  // MinQty
        case 152:  // CashOrderQty
        case 210:  // MaxShow
        // ---- what was filled ----
        case 6:    // AvgPx
        case 14:   // CumQty
        case 31:   // LastPx
        case 32:   // LastQty
        case 151:  // LeavesQty
            return true;
        default:
            return false;
    }
}

void mask_fix_body(std::string& body, bool mask_business) {
    if (body.empty()) return;

    std::string out;
    out.reserve(body.size());

    bool         changed = false;
    const size_t n       = body.size();
    size_t       i       = 0;

    // Set by a length field (90/95) for the data field that must follow it.
    int    expect_tag = -1;
    size_t expect_len = 0;

    // Walk field by field rather than searching for '=' across the whole body:
    // a stray '=' inside free text would otherwise shift the tag boundary and
    // let the next real field slip past unmasked.
    while (i < n) {
        size_t eq = i;
        while (eq < n && body[eq] != '=' && !is_fix_separator(body[eq])) ++eq;

        const bool has_eq = (eq < n && body[eq] == '=');
        const int  tag    = has_eq ? parse_tag(std::string_view(body.data() + i, eq - i)) : -1;

        size_t end;
        if (tag > 0 && tag == expect_tag && eq + 1 + expect_len <= n) {
            // SecureData(91) and RawData(96) declare their length and are
            // allowed to contain the SOH byte itself. Trusting the separator
            // scan here would cut such a value in half and copy the tail
            // through unmasked - the exact leak this function exists to stop.
            end = eq + 1 + expect_len;
        } else {
            end = has_eq ? eq : i;
            while (end < n && !is_fix_separator(body[end])) ++end;
        }
        expect_tag = -1;

        bool masked = false;
        if (tag > 0 && end > eq + 1) {
            const std::string_view value(body.data() + eq + 1, end - eq - 1);
            const std::string_view label(body.data() + i, eq + 1 - i);

            if (is_credential_tag(tag)) {
                out.append(label);
                out.append(kRedacted);
                masked  = true;
                changed = true;
            } else if (is_opaque_text_tag(tag)) {
                out.append(label);
                out.append(kMasked);
                masked  = true;
                changed = true;
            } else if (is_free_text_tag(tag)) {
                // Tag 58 is where an engine echoes a failed login back at you,
                // and it is neither a credential tag nor a business tag - which
                // is exactly how a password walked into the stored body while
                // the derived text column next to it was clean.
                const std::string scrubbed = redact_free_text(value);
                out.append(label);
                out.append(scrubbed);
                masked = true;
                if (std::string_view(scrubbed) != value) changed = true;
            } else if (mask_business && is_business_tag(tag)) {
                out.append(label);
                out.append(kMasked);
                masked  = true;
                changed = true;
            }
        }
        if (!masked) out.append(body, i, end - i);

        // Read the declared length off the original body - out may already hold
        // the masked form - so the next field knows how far its value runs.
        if (end > eq + 1) {
            const int next = data_tag_for_length_tag(tag);
            if (next > 0) {
                const int len = parse_tag(std::string_view(body.data() + eq + 1, end - eq - 1));
                if (len > 0) {
                    expect_tag = next;
                    expect_len = static_cast<size_t>(len);
                }
            }
        }

        if (end < n) out.push_back(body[end]);
        i = end + 1;
    }

    if (changed) body.swap(out);
}

std::string redact_free_text(std::string_view text) {
    if (text.empty()) return std::string(text);

    // How many tokens after a credential noun may still be its value.
    // "password rejected for TRADER01" puts two connector words in between, so
    // a window of one - clearing on the first of them - reads the sentence as
    // safe and lets the login name through. Bounded rather than open-ended: a
    // noun near the start of a long line must not redact its way to the end.
    constexpr int kWindow = 4;

    std::string out;
    out.reserve(text.size());

    bool         changed = false;
    int          pending = 0;  // tokens left in which a value may still appear
    const size_t n       = text.size();
    size_t       i       = 0;

    while (i < n) {
        size_t s = i;
        while (s < n && is_space(text[s])) ++s;
        out.append(text.substr(i, s - i));  // whitespace kept verbatim
        if (s >= n) break;

        size_t e = s;
        while (e < n && !is_space(text[e])) ++e;
        std::string_view tok = text.substr(s, e - s);
        i = e;

        // "password=hunter2" / "Username: trader01"
        size_t sep = tok.find_first_of("=:");
        if (sep != std::string_view::npos && sep > 0 && sep + 1 < tok.size() &&
            is_sensitive_key(strip_punct(tok.substr(0, sep)))) {
            out.append(tok.substr(0, sep + 1));
            out.append(kRedacted);
            changed = true;
            pending = 0;
            continue;
        }

        std::string_view bare = strip_punct(tok);

        // A lone separator between the noun and its value: "password = x".
        if (bare.empty()) {
            out.append(tok);
            continue;
        }

        // Checked before the window, so "password for user TRADER01" keeps the
        // word "user" readable instead of redacting the noun and exposing the
        // name that follows it.
        if (is_sensitive_key(bare)) {
            pending = kWindow;
            out.append(tok);
            continue;
        }

        if (pending > 0) {
            if (is_stop_word(fold(bare))) {
                --pending;
            } else {
                out.append(kRedacted);
                changed = true;
                pending = 0;
                continue;
            }
        }

        out.append(tok);
    }

    return changed ? out : std::string(text);
}

}  // namespace fixmon

