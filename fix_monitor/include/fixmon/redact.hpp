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

// What replaces a business value in a stored FIX body. Distinct from kRedacted
// so a reader can tell "we refused to keep a secret" from "we kept the shape of
// an order but not its content".
inline constexpr const char* kMasked = "<masked>";

// True when a config key or metric label names something that must never leave
// this process: passwords, tokens, key material, login identity.
bool is_sensitive_key(std::string_view key);

// kRedacted for sensitive keys, the value untouched otherwise. Used for the
// few places that print a setting back to a human.
std::string redact_if_sensitive(std::string_view key, const std::string& value);

// ---------------------------------------------------------------------------
// FIX body masking
//
// The engine's message log contains the Logon message, and a Logon carries
// Username(553) and Password(554) in clear text. Redacting the engine *config*
// while copying those tags verbatim into the store would be a hole straight
// through the same rule, so the tag layer gets the same treatment.
//
// Two tiers, because they answer to different rules:
//   credential tags - never kept, regardless of configuration
//   business tags   - identity and commercial detail, masked by policy
//
// Free text (58 and friends) is a third case and neither rule fits it. It is
// the one field a human types, so it can contain anything - but it is also
// where the useful half of a reject lives, so dropping it would cost more than
// it saves. It gets scrubbed word by word instead.
// ---------------------------------------------------------------------------

// Login material carried inside a FIX message. Always masked.
bool is_credential_tag(int tag);

// Tags that identify a client or carry commercial detail: order ids, account,
// symbol, side, quantity, price. Masked when body masking is enabled.
// Deliberately excludes enums and session plumbing (35, 34, 49, 56, 52, 39,
// 45, 371, 373, 380), which is what diagnosis actually runs on.
bool is_business_tag(int tag);

// Rewrites sensitive values in a raw FIX body in place, keeping every tag
// present and every separator where it was. "11=ORD-7" becomes "11=<masked>":
// a reader still sees that a ClOrdID was there, which is the part that matters
// when reconstructing what the engine did.
void mask_fix_body(std::string& body, bool mask_business);

// Engines narrate rejects in prose - "Invalid password for user TRADER01" - and
// that prose reaches /sessions, the snapshot table and anything reading them.
// Masks the value following a credential word, leaving the sentence readable.
// Heuristic by nature: it is a second line of defence, not the first.
std::string redact_free_text(std::string_view text);

}  // namespace fixmon

