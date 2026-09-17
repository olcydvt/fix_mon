#include "fixmon/quickfix_config.hpp"

#include "fixmon/redact.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <istream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace fixmon {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && lower(a) == lower(b);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        if (p == std::string::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
}

// The two names QuickFIX C++ and quickfix/j give the same file. Order matters:
// ".current.log" is checked first because that is the C++ engine, which is the
// one that reads this cfg dialect.
const char* const kLogSuffixes[] = {".current.log", ".log"};

bool file_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) && fs::is_regular_file(p, ec);
}

// Replaces ".messages." with ".event." or the other way round, so that finding
// one of the pair is enough to know what the other is called.
std::string swap_log_kind(const std::string& path, const std::string& from, const std::string& to) {
    const std::string needle = "." + from + ".";
    size_t p = path.rfind(needle);
    if (p == std::string::npos) return {};
    return path.substr(0, p) + "." + to + "." + path.substr(p + needle.size());
}

// Scores a file name against a session identity using exact '-' separated
// tokens. Substring matching would let BROKER1-VENUEX2 answer for VENUEX.
int identity_score(const std::string& stem, const SessionConfig& sc) {
    const std::vector<std::string> toks = split(stem, '-');
    auto has_token = [&](const std::string& want) {
        if (want.empty()) return false;
        return std::any_of(toks.begin(), toks.end(),
                           [&](const std::string& t) { return iequals(t, want); });
    };
    int score = 0;
    if (has_token(sc.sender_comp_id))    ++score;
    if (has_token(sc.target_comp_id))    ++score;
    if (has_token(sc.begin_string))      ++score;
    if (has_token(sc.session_qualifier)) ++score;
    return score;
}

// Last resort when no canonical name matched: look at what is actually in the
// directory. Requires at least sender and target to line up before claiming a
// file belongs to this session.
std::string scan_for_log(const std::string& dir, const SessionConfig& sc, const std::string& kind) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};

    const std::string marker = "." + kind + ".";
    std::string best;
    int         best_score = 0;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        const std::string name = entry.path().filename().string();
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".log") != 0) continue;

        size_t m = name.find(marker);
        if (m == std::string::npos) continue;

        int score = identity_score(name.substr(0, m), sc);
        if (score > best_score) {
            best_score = score;
            best       = entry.path().string();
        }
    }
    return best_score >= 2 ? best : std::string{};
}

// FileLogPath is relative to the engine's working directory, which is not
// necessarily ours. Try it as given first, then relative to the cfg file, which
// is what a relative path in a config almost always means to a human.
std::string resolve_log_dir(const std::string& configured, const fs::path& cfg_dir) {
    std::error_code ec;
    if (fs::is_directory(configured, ec)) return configured;

    if (!cfg_dir.empty()) {
        fs::path alt = cfg_dir / configured;
        if (fs::is_directory(alt, ec)) return alt.string();
    }
    return configured;
}

void attach_logs(SessionConfig& sc, const std::string& dir, std::vector<std::string>* notes) {
    const std::vector<std::string> prefixes = quickfix_log_prefixes(sc);

    sc.message_log_path = resolve_quickfix_log(dir, prefixes, "messages");
    if (sc.message_log_path.empty()) sc.message_log_path = scan_for_log(dir, sc, "messages");

    sc.event_log_path = resolve_quickfix_log(dir, prefixes, "event");
    if (sc.event_log_path.empty()) sc.event_log_path = scan_for_log(dir, sc, "event");

    // Found one of the pair: the other one is the same name with the kind
    // swapped, whether or not it exists yet.
    if (sc.event_log_path.empty() && !sc.message_log_path.empty()) {
        sc.event_log_path = swap_log_kind(sc.message_log_path, "messages", "event");
    } else if (sc.message_log_path.empty() && !sc.event_log_path.empty()) {
        sc.message_log_path = swap_log_kind(sc.event_log_path, "event", "messages");
    }

    if (sc.message_log_path.empty() && sc.event_log_path.empty() && !prefixes.empty()) {
        // Nothing on disk yet - a session that has not connected since the logs
        // were rotated away, or an engine that has not started. Watch the
        // canonical names; the tailer opens them as soon as they appear.
        sc.message_log_path = (fs::path(dir) / (prefixes.front() + ".messages.current.log")).string();
        sc.event_log_path   = (fs::path(dir) / (prefixes.front() + ".event.current.log")).string();
        if (notes) {
            notes->push_back("no log files under " + dir + " for " + sc.session_id() +
                             " yet; watching " + sc.message_log_path);
        }
    }
}

// Acceptors are often configured with '*' for the counterparty. The engine
// still writes one log pair per real comp id, so the actual names are the only
// place the session list exists. Read them off the directory.
std::vector<std::pair<std::string, std::string>>
discover_wildcard_comp_ids(const std::string& dir, const SessionConfig& sc) {
    std::vector<std::pair<std::string, std::string>> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;

    const bool sender_wild = sc.sender_comp_id == "*";
    const bool target_wild = sc.target_comp_id == "*";
    if (sender_wild && target_wild) return out;  // nothing left to anchor on

    const std::string marker = ".messages.";
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        const std::string name = entry.path().filename().string();
        size_t m = name.find(marker);
        if (m == std::string::npos) continue;

        std::string stem = name.substr(0, m);
        // Strip the BeginString when the file carries one.
        if (!sc.begin_string.empty() && stem.rfind(sc.begin_string + "-", 0) == 0) {
            stem = stem.substr(sc.begin_string.size() + 1);
        }

        std::string sender, target;
        if (target_wild) {
            const std::string head = sc.sender_comp_id + "-";
            if (stem.rfind(head, 0) != 0) continue;
            sender = sc.sender_comp_id;
            target = stem.substr(head.size());
        } else {
            const std::string tail = "-" + sc.target_comp_id;
            if (stem.size() <= tail.size() ||
                stem.compare(stem.size() - tail.size(), tail.size(), tail) != 0) {
                continue;
            }
            sender = stem.substr(0, stem.size() - tail.size());
            target = sc.target_comp_id;
        }

        if (sender.empty() || target.empty()) continue;
        if (std::find(out.begin(), out.end(), std::make_pair(sender, target)) == out.end()) {
            out.emplace_back(sender, target);
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------

bool CaseInsensitiveLess::operator()(const std::string& a, const std::string& b) const {
    return std::lexicographical_compare(
        a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) {
            return std::tolower(x) < std::tolower(y);
        });
}

bool QuickFixSessionBlock::has(const std::string& key) const {
    return values.find(key) != values.end();
}

std::string QuickFixSessionBlock::get(const std::string& key, const std::string& fallback) const {
    auto it = values.find(key);
    return it == values.end() || it->second.empty() ? fallback : it->second;
}

int QuickFixSessionBlock::get_int(const std::string& key, int fallback) const {
    auto it = values.find(key);
    if (it == values.end()) return fallback;
    try {
        return std::stoi(it->second);
    } catch (...) {
        return fallback;
    }
}

TriState QuickFixSessionBlock::get_bool(const std::string& key) const {
    auto it = values.find(key);
    if (it == values.end()) return TriState::Unknown;
    return parse_tristate(it->second);
}

QuickFixSettings parse_quickfix_settings(std::istream& in, const std::string& origin) {
    QuickFixSettings out;
    out.path = origin;

    QuickFixDictionary       defaults;
    std::vector<std::string> default_redacted;

    QuickFixDictionary       current;
    std::vector<std::string> current_redacted;

    enum class Sec { None, Default, Session };
    Sec sec = Sec::None;

    auto remember_redacted = [](std::vector<std::string>& v, const std::string& key) {
        if (std::find_if(v.begin(), v.end(), [&](const std::string& k) {
                return iequals(k, key);
            }) == v.end()) {
            v.push_back(key);
        }
    };

    auto commit = [&] {
        if (sec != Sec::Session) return;
        QuickFixSessionBlock block;
        block.values = defaults;
        for (const auto& [k, v] : current) block.values[k] = v;  // session wins

        block.redacted_keys = default_redacted;
        for (const auto& k : current_redacted) remember_redacted(block.redacted_keys, k);

        out.sessions.push_back(std::move(block));
        current.clear();
        current_redacted.clear();
    };

    std::string line;
    int         lineno = 0;

    while (std::getline(in, line)) {
        ++lineno;
        const std::string t = trim(line);

        // Only a whole-line comment. QuickFIX values may contain '#', and
        // silently truncating a path at one would be worse than keeping it.
        if (t.empty() || t[0] == '#') continue;

        if (t.front() == '[' && t.back() == ']') {
            commit();
            const std::string name = trim(t.substr(1, t.size() - 2));
            if (iequals(name, "DEFAULT")) {
                sec = Sec::Default;
            } else if (iequals(name, "SESSION")) {
                sec = Sec::Session;
            } else {
                sec = Sec::None;
                out.warnings.push_back(origin + ":" + std::to_string(lineno) +
                                       ": ignoring unknown section [" + name + "]");
            }
            continue;
        }

        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            out.warnings.push_back(origin + ":" + std::to_string(lineno) +
                                   ": no '=' on this line, ignored");
            continue;
        }

        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        if (key.empty()) continue;

        if (is_sensitive_key(key)) {
            // Dropped here, at the edge. The value is not copied anywhere, so
            // no later layer can leak what it never received.
            remember_redacted(sec == Sec::Default ? default_redacted : current_redacted, key);
            std::fill(val.begin(), val.end(), '*');
            continue;
        }

        if (sec == Sec::Default) {
            defaults[key] = val;
        } else if (sec == Sec::Session) {
            current[key] = val;
        } else {
            out.warnings.push_back(origin + ":" + std::to_string(lineno) + ": '" + key +
                                   "' sits outside [DEFAULT] and [SESSION], ignored");
        }
    }
    commit();

    out.defaults = std::move(defaults);
    if (out.sessions.empty()) {
        out.warnings.push_back(origin + ": no [SESSION] blocks found");
    }
    return out;
}

QuickFixSettings load_quickfix_settings(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open quickfix config: " + path);
    return parse_quickfix_settings(in, path);
}

std::vector<std::string> quickfix_log_prefixes(const SessionConfig& sc) {
    std::vector<std::string> out;
    auto add = [&out](std::string p) {
        if (p.empty()) return;
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(std::move(p));
    };

    const std::string& b = sc.begin_string;
    const std::string& s = sc.sender_comp_id;
    const std::string& t = sc.target_comp_id;
    const std::string& q = sc.session_qualifier;

    // quickfix/j folds sub and location ids into the name with '_'.
    std::string sj = s;
    if (!sc.sender_sub_id.empty())      sj += "_" + sc.sender_sub_id;
    if (!sc.sender_location_id.empty()) sj += "_" + sc.sender_location_id;
    std::string tj = t;
    if (!sc.target_sub_id.empty())      tj += "_" + sc.target_sub_id;
    if (!sc.target_location_id.empty()) tj += "_" + sc.target_location_id;

    if (!q.empty()) {
        add(b + "-" + sj + "-" + tj + "-" + q);
        add(b + "-" + s + "-" + t + "-" + q);
        add(s + "-" + t + "-" + q);
    }
    add(b + "-" + sj + "-" + tj);  // QuickFIX C++ FileLog::generatePrefix
    add(b + "-" + s + "-" + t);
    add(s + "-" + t);              // deployments that trim the BeginString
    return out;
}

std::string resolve_quickfix_log(const std::string& dir,
                                 const std::vector<std::string>& prefixes,
                                 const std::string& kind) {
    for (const std::string& prefix : prefixes) {
        for (const char* suffix : kLogSuffixes) {
            fs::path candidate = fs::path(dir) / (prefix + "." + kind + suffix);
            if (file_exists(candidate)) return candidate.string();
        }
    }
    return {};
}

std::vector<SessionConfig> sessions_from_quickfix(const QuickFixSettings& settings,
                                                  std::vector<std::string>* notes) {
    std::vector<SessionConfig> out;
    auto note = [&](std::string m) {
        if (notes) notes->push_back(std::move(m));
    };
    for (const auto& w : settings.warnings) note(w);

    const fs::path cfg_dir = fs::path(settings.path).parent_path();

    for (const QuickFixSessionBlock& block : settings.sessions) {
        SessionConfig sc;
        sc.begin_string   = block.get("BeginString", "FIX.4.4");
        sc.sender_comp_id = block.get("SenderCompID");
        sc.target_comp_id = block.get("TargetCompID");

        if (sc.sender_comp_id.empty() || sc.target_comp_id.empty()) {
            note(settings.path + ": [SESSION] without SenderCompID/TargetCompID, skipped");
            continue;
        }

        sc.session_qualifier  = block.get("SessionQualifier");
        sc.sender_sub_id      = block.get("SenderSubID");
        sc.sender_location_id = block.get("SenderLocationID");
        sc.target_sub_id      = block.get("TargetSubID");
        sc.target_location_id = block.get("TargetLocationID");

        sc.heartbeat_interval = block.get_int("HeartBtInt", 30);
        if (sc.heartbeat_interval <= 0) sc.heartbeat_interval = 30;

        sc.connection_type    = lower(block.get("ConnectionType"));
        sc.reconnect_interval = block.get_int("ReconnectInterval", 0);
        sc.start_time         = block.get("StartTime");
        sc.end_time           = block.get("EndTime");
        sc.file_store_path    = block.get("FileStorePath");
        sc.file_log_path      = block.get("FileLogPath");

        // The settings that decide what a sequence mismatch means. Read rather
        // than assumed: the engine is the only thing that knows, and a default
        // invented here would be handed on as fact.
        sc.reset_on_logon      = block.get_bool("ResetOnLogon");
        sc.reset_on_logout     = block.get_bool("ResetOnLogout");
        sc.reset_on_disconnect = block.get_bool("ResetOnDisconnect");
        sc.persist_messages    = block.get_bool("PersistMessages");

        sc.origin             = settings.path;
        sc.from_quickfix      = true;
        sc.redacted_settings  = block.redacted_keys;

        if (sc.file_log_path.empty()) {
            // File logging is off, or the engine logs somewhere we were not
            // told about. Keep the session so it still shows up as configured,
            // but say plainly that nothing will be read for it.
            note(settings.path + ": " + sc.session_id() +
                 " has no FileLogPath - file logging is off, nothing to tail");
            out.push_back(std::move(sc));
            continue;
        }

        const std::string dir = resolve_log_dir(sc.file_log_path, cfg_dir);
        sc.file_log_path      = dir;

        if (sc.sender_comp_id == "*" || sc.target_comp_id == "*") {
            auto pairs = discover_wildcard_comp_ids(dir, sc);
            if (pairs.empty()) {
                note(settings.path + ": wildcard session " + sc.session_id() +
                     " matched no logs under " + dir +
                     "; add an explicit [session] block once the counterparty is known");
                continue;
            }
            for (const auto& [sender, target] : pairs) {
                SessionConfig one   = sc;
                one.sender_comp_id  = sender;
                one.target_comp_id  = target;
                attach_logs(one, dir, notes);
                note(settings.path + ": wildcard expanded to " + one.session_id());
                out.push_back(std::move(one));
            }
            continue;
        }

        attach_logs(sc, dir, notes);
        out.push_back(std::move(sc));
    }
    return out;
}

}  // namespace fixmon

