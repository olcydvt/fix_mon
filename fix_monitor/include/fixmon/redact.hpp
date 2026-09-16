#pragma once
//
// One place that decides what counts as a secret.
//
// Two very different call sites share this rule on purpose:
//   - the QuickFIX config reader, which drops the value before it is stored
//   - the metric registry, which refuses a label with such a name
//
// Keeping a single predicate means a credential cannot be secret in one layer
// and not in the other. A vendor-specific key we have never seen still gets
// caught, because the match is on the shape of the name rather than on a fixed
// list of known settings.
//
#include <string>
#include <string_view>

namespace fixmon {

// What an operator sees instead of a secret. The value itself is never kept.
inline constexpr const char* kRedacted = "<redacted>";

// True when a config key or metric label names something that must never leave
// this process: passwords, tokens, key material, login identity.
bool is_sensitive_key(std::string_view key);

// kRedacted for sensitive keys, the value untouched otherwise. Used for the
// few places that print a setting back to a human.
std::string redact_if_sensitive(std::string_view key, const std::string& value);

}  // namespace fixmon

