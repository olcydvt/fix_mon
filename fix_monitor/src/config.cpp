#include "fixmon/config.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

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

}  // namespace

std::string SessionConfig::session_id() const {
    return begin_string + ":" + sender_comp_id + "->" + target_comp_id;
}

AppConfig load_config(const std::string& path) {
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
            else if (key == "heartbeat_interval") current.heartbeat_interval = std::stoi(val);
        } else {
            if      (key == "db_path")          cfg.db_path        = val;
            else if (key == "metrics_port")     cfg.metrics_port   = static_cast<uint16_t>(std::stoi(val));
            else if (key == "queue_size")       cfg.queue_capacity = next_pow2(std::stoul(val));
            else if (key == "batch_size")       cfg.batch_size     = std::stoul(val);
            else if (key == "batch_flush_ms")   cfg.batch_flush_ms = std::stoi(val);
            else if (key == "poll_interval_ms") cfg.poll_interval_ms = std::stoi(val);
            else if (key == "from_beginning")   cfg.from_beginning = to_bool(val);
            else if (key == "stale_after_multiple") cfg.stale_after_multiple = std::stoi(val);
            else if (key == "snapshot_interval_s")  cfg.snapshot_interval_s  = std::stoi(val);
        }
    }
    commit_session();

    if (cfg.sessions.empty()) throw std::runtime_error("no [session] blocks in config");
    return cfg;
}

}  // namespace fixmon
