#pragma once
//
// QuickFIX engine configuration reader.
//
// The engine already knows every session it runs and where it writes their
// logs. Restating that in a second file is how the two drift apart: a session
// gets added to the engine, nobody updates the monitor, and the monitor stays
// green because it is not watching anything. So we read the engine's own cfg
// and take the session list from there.
//
// What we take:  BeginString, comp ids, qualifier, HeartBtInt, ConnectionType,
//                schedule, FileLogPath (to find the logs), FileStorePath.
// What we drop:  every credential, at parse time, before the value is stored.
//                Only the key name survives, so an operator can see it was
//                ignored on purpose. See redact.hpp.
//
// The file is opened read-only and never written back.
//
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

#include "fixmon/config.hpp"

namespace fixmon {

// QuickFIX setting names are conventionally CamelCase but deployments are not
// consistent about it, and a case mismatch silently losing FileLogPath would
// be a bad failure mode. Compare case-insensitively.
struct CaseInsensitiveLess {
    bool operator()(const std::string& a, const std::string& b) const;
};

using QuickFixDictionary = std::map<std::string, std::string, CaseInsensitiveLess>;

// One [SESSION] block with [DEFAULT] already merged underneath it.
struct QuickFixSessionBlock {
    QuickFixDictionary values;

    // Credential keys seen in this block or in [DEFAULT]. Names only.
    std::vector<std::string> redacted_keys;

    bool        has(const std::string& key) const;
    std::string get(const std::string& key, const std::string& fallback = {}) const;
    int         get_int(const std::string& key, int fallback) const;
};

struct QuickFixSettings {
    std::string                       path;      // origin, for messages
    QuickFixDictionary                defaults;  // [DEFAULT], credentials removed
    std::vector<QuickFixSessionBlock> sessions;
    std::vector<std::string>          warnings;
};

// Parses the QuickFIX ini dialect: [DEFAULT] / [SESSION] sections, key=value,
// '#' comments only at the start of a line (a value may legitimately contain
// one). Never throws; anything unexpected lands in warnings.
QuickFixSettings parse_quickfix_settings(std::istream& in, const std::string& origin);

// Throws std::runtime_error if the file cannot be opened.
QuickFixSettings load_quickfix_settings(const std::string& path);

// Turns parsed settings into fixmon sessions, resolving each session's message
// and event log under FileLogPath. Notes explain anything that could not be
// resolved. Appends to notes when given.
std::vector<SessionConfig> sessions_from_quickfix(const QuickFixSettings& settings,
                                                  std::vector<std::string>* notes = nullptr);

// ---- exposed for tests ----

// Candidate log file prefixes for a session, most specific first. QuickFIX C++
// and quickfix/j name their files differently, and some deployments trim the
// BeginString, so we try several rather than assume one.
std::vector<std::string> quickfix_log_prefixes(const SessionConfig& sc);

// First existing "<prefix>.<kind>.current.log" or "<prefix>.<kind>.log" under
// dir, or empty if none exist. kind is "messages" or "event".
std::string resolve_quickfix_log(const std::string& dir,
                                 const std::vector<std::string>& prefixes,
                                 const std::string& kind);

}  // namespace fixmon

