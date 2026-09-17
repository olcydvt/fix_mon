#include "fixmon/config.hpp"

#include "fixmon/quickfix_config.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace fixmon {

namespace {

// Strips an inline comment. "true   # replay on startup" would otherwise be
// compared verbatim against "true" and silently evaluate to false.
std::string strip_comment(const std::string& s) {
    size_t h = s.find('#');
    size_t c = s.find(';');
    size_t cut = std::min(h == std::string::npos ? s.size() : h,
                          c == std::string::npos ? s.size() : c);
    return s.substr(0, cut);
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool to_bool(const std::string& v) {
    std::string l = v;
    std::transform(l.begin(), l.end(), l.begin(), ::tolower);
    return l == "1" || l == "true" || l == "yes" || l == "y";
}

size_t next_pow2(size_t v) {
    size_t p = 2;
    while (p < v) p <<= 1;
    return p;
}

// A relative quickfix_config is relative to the ini that mentions it, not to
// wherever the collector happens to be started from.
std::string resolve_against(const std::string& path, const fs::path& base_dir) {
    std::error_code ec;
    if (fs::exists(path, ec)) return path;
    if (!base_dir.empty()) {
        fs::path alt = base_dir / path;
        if (fs::exists(alt, ec)) return alt.string();
    }
    return path;
}

// Fills the blanks of an explicit [session] block from what the engine config
// said about the same session. The operator's file wins wherever it spoke;
// everything it left out comes from the engine, which is the point of reading
// the engine config at all.
void fill_from_quickfix(SessionConfig& dst, const SessionConfig& src) {
    if (dst.message_log_path.empty())   dst.message_log_path   = src.message_log_path;
    if (dst.event_log_path.empty())     dst.event_log_path     = src.event_log_path;
    if (!dst.heartbeat_explicit)        dst.heartbeat_interval = src.heartbeat_interval;
    if (dst.session_qualifier.empty())  dst.session_qualifier  = src.session_qualifier;
    if (dst.sender_sub_id.empty())      dst.sender_sub_id      = src.sender_sub_id;
    if (dst.sender_location_id.empty()) dst.sender_location_id = src.sender_location_id;
    if (dst.target_sub_id.empty())      dst.target_sub_id      = src.target_sub_id;
    if (dst.target_location_id.empty()) dst.target_location_id = src.target_location_id;
    if (dst.connection_type.empty())    dst.connection_type    = src.connection_type;
    if (dst.file_log_path.empty())      dst.file_log_path      = src.file_log_path;
    if (dst.file_store_path.empty())    dst.file_store_path    = src.file_store_path;
    if (dst.start_time.empty())         dst.start_time         = src.start_time;
    if (dst.end_time.empty())           dst.end_time           = src.end_time;
    if (dst.reconnect_interval == 0)    dst.reconnect_interval = src.reconnect_interval;
    if (dst.origin.empty())             dst.origin             = src.origin;

    // Unknown means the [session] block said nothing, so the engine's answer is
    // the only one there is. An explicit yes/no in the ini still wins, which is
    // how an operator corrects an engine config they cannot edit.
    if (dst.reset_on_logon == TriState::Unknown)      dst.reset_on_logon      = src.reset_on_logon;
    if (dst.reset_on_logout == TriState::Unknown)     dst.reset_on_logout     = src.reset_on_logout;
    if (dst.reset_on_disconnect == TriState::Unknown) dst.reset_on_disconnect = src.reset_on_disconnect;
    if (dst.persist_messages == TriState::Unknown)    dst.persist_messages    = src.persist_messages;

    dst.from_quickfix = true;
    for (const auto& key : src.redacted_settings) {
        if (std::find(dst.redacted_settings.begin(), dst.redacted_settings.end(), key) ==
            dst.redacted_settings.end()) {
            dst.redacted_settings.push_back(key);
        }
    }
}

// Imports every configured engine cfg and merges the result into cfg.sessions.
void import_quickfix_configs(AppConfig& cfg, const fs::path& base_dir) {
    for (const std::string& raw_path : cfg.quickfix_configs) {
        const std::string path = resolve_against(raw_path, base_dir);

        QuickFixSettings settings = load_quickfix_settings(path);
        std::vector<SessionConfig> derived = sessions_from_quickfix(settings, &cfg.notes);

        for (SessionConfig& d : derived) {
            const std::string id = d.session_id();
            auto it = std::find_if(cfg.sessions.begin(), cfg.sessions.end(),
                                   [&](const SessionConfig& s) { return s.session_id() == id; });
            if (it == cfg.sessions.end()) {
                cfg.notes.push_back("imported " + id + " from " + path);
                cfg.sessions.push_back(std::move(d));
            } else {
                cfg.notes.push_back("merged engine settings into " + id + " from " + path);
                // The operator's [session] block wins on log paths. Say so
                // explicitly: reading the engine cfg may already have complained
                // that it found no logs under its FileLogPath, and without this
                // line that complaint reads like a failure long after it stopped
                // being true. Happens whenever the engine runs on a different
                // host or OS than the collector and its path means nothing here.
                if (!it->message_log_path.empty() &&
                    it->message_log_path != d.message_log_path) {
                    cfg.notes.push_back("using [session] log paths for " + id +
                                        "; FileLogPath from " + path +
                                        " was not usable from here and is ignored");
                }
                fill_from_quickfix(*it, d);
            }
        }
    }
}

void finalize(AppConfig& cfg, const fs::path& base_dir) {
    import_quickfix_configs(cfg, base_dir);

    if (cfg.sessions.empty()) {
        throw std::runtime_error(
            "no sessions: add a [session] block, or point quickfix_config at the "
            "engine's own cfg file");
    }
}

}  // namespace

std::string SessionConfig::session_id() const {
    std::string id = begin_string + ":" + sender_comp_id + "->" + target_comp_id;
    // A qualifier is what separates two sessions that share comp ids. Leaving
    // it out would collapse them onto one metric series and one state machine.
    if (!session_qualifier.empty()) id += ":" + session_qualifier;
    return id;
}

const char* to_string(TriState v) {
    switch (v) {
        case TriState::Yes: return "yes";
        case TriState::No:  return "no";
        default:            return "unknown";
    }
}

TriState parse_tristate(std::string_view v) {
    if (v.size() == 1) {
        switch (v[0]) {
            case 'Y': case 'y': case '1': return TriState::Yes;
            case 'N': case 'n': case '0': return TriState::No;
            default:                      return TriState::Unknown;
        }
    }
    std::string l;
    l.reserve(v.size());
    for (char c : v) l.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (l == "true"  || l == "yes") return TriState::Yes;
    if (l == "false" || l == "no")  return TriState::No;
    return TriState::Unknown;
}

const char* SessionConfig::seq_reset_policy() const {
    // Order matters. ResetOnLogon is the strongest of the three: if numbers
    // restart at every logon then what the other two do never becomes visible,
    // so reporting them instead would describe a case that cannot arise.
    if (reset_on_logon == TriState::Yes) return "reset_each_logon";
    if (reset_on_logout == TriState::Yes || reset_on_disconnect == TriState::Yes) {
        return "reset_each_logout";
    }
    // Only claim persistence once something actually told us so. All three
    // reading Unknown means we never saw an engine config, and "persistent" is
    // a statement about configuration, not a default to fall back on.
    if (reset_on_logon == TriState::No &&
        reset_on_logout == TriState::No &&
        reset_on_disconnect == TriState::No) {
        return "persistent";
    }
    return "unknown";
}

const char* SessionConfig::resend_capability() const {
    switch (persist_messages) {
        case TriState::Yes: return "full";
        case TriState::No:  return "gap_fill_only";
        default:            return "unknown";
    }
}

AppConfig load_config(const std::string& path,
                      const std::vector<std::string>& extra_quickfix_configs) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open config file: " + path);

    AppConfig     cfg;
    SessionConfig current;
    bool          in_session = false;
    std::string   line;
    int           lineno = 0;

    auto commit_session = [&] {
        if (!in_session) return;
        if (current.sender_comp_id.empty() || current.target_comp_id.empty()) {
            throw std::runtime_error("session block missing sender_comp_id/target_comp_id");
        }
        if (current.begin_string.empty()) current.begin_string = "FIX.4.4";
        cfg.sessions.push_back(current);
        current = SessionConfig{};
    };

    while (std::getline(in, line)) {
        ++lineno;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line.front() == '[' && line.back() == ']') {
            std::string section = line.substr(1, line.size() - 2);
            commit_session();
            in_session = (section == "session");
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            throw std::runtime_error("malformed line " + std::to_string(lineno) + ": " + line);
        }
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(strip_comment(line.substr(eq + 1)));

        if (in_session) {
            if      (key == "begin_string")       current.begin_string     = val;
            else if (key == "sender_comp_id")     current.sender_comp_id   = val;
            else if (key == "target_comp_id")     current.target_comp_id   = val;
            else if (key == "message_log")        current.message_log_path = val;
            else if (key == "event_log")          current.event_log_path   = val;
            else if (key == "session_qualifier")  current.session_qualifier = val;
            else if (key == "sender_sub_id")      current.sender_sub_id     = val;
            else if (key == "target_sub_id")      current.target_sub_id     = val;
            // Only worth setting by hand when the engine cfg is out of reach -
            // on another host, or in a format we were not pointed at. Getting
            // these wrong is worse than leaving them unknown, because a wrong
            // answer here is still delivered as an answer.
            else if (key == "reset_on_logon")      current.reset_on_logon      = parse_tristate(val);
            else if (key == "reset_on_logout")     current.reset_on_logout     = parse_tristate(val);
            else if (key == "reset_on_disconnect") current.reset_on_disconnect = parse_tristate(val);
            else if (key == "persist_messages")    current.persist_messages    = parse_tristate(val);
            else if (key == "heartbeat_interval") {
                current.heartbeat_interval = std::stoi(val);
                current.heartbeat_explicit = true;
            }
        } else {
            if      (key == "db_path")          cfg.db_path        = val;
            else if (key == "metrics_port")     cfg.metrics_port   = static_cast<uint16_t>(std::stoi(val));
            else if (key == "queue_size")       cfg.queue_capacity = next_pow2(std::stoul(val));
            else if (key == "queue_full_wait_ms") cfg.queue_full_wait_ms = std::stoi(val);
            else if (key == "batch_size")       cfg.batch_size     = std::stoul(val);
            else if (key == "batch_flush_ms")   cfg.batch_flush_ms = std::stoi(val);
            else if (key == "poll_interval_ms") cfg.poll_interval_ms = std::stoi(val);
            else if (key == "from_beginning")   cfg.from_beginning = to_bool(val);
            else if (key == "stale_after_multiple") cfg.stale_after_multiple = std::stoi(val);
            else if (key == "snapshot_interval_s")  cfg.snapshot_interval_s  = std::stoi(val);
            else if (key == "mask_message_bodies")  cfg.mask_message_bodies  = to_bool(val);
            else if (key == "retention_days")       cfg.retention_days       = std::stoi(val);
            // Repeatable: one engine cfg per line, or several comma separated.
            else if (key == "quickfix_config") {
                std::istringstream parts(val);
                std::string one;
                while (std::getline(parts, one, ',')) {
                    one = trim(one);
                    if (!one.empty()) cfg.quickfix_configs.push_back(one);
                }
            }
        }
    }
    commit_session();

    for (const std::string& p : extra_quickfix_configs) {
        if (!p.empty()) cfg.quickfix_configs.push_back(p);
    }

    finalize(cfg, fs::path(path).parent_path());
    return cfg;
}

AppConfig config_from_quickfix(const std::vector<std::string>& quickfix_configs) {
    AppConfig cfg;
    cfg.quickfix_configs = quickfix_configs;
    finalize(cfg, fs::path{});
    return cfg;
}

}  // namespace fixmon
