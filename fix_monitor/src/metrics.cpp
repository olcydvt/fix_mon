#include "fixmon/metrics.hpp"

#include "fixmon/redact.hpp"

#include <cstring>
#include <sstream>


namespace fixmon {

void Gauge::set(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    bits_.store(bits, std::memory_order_relaxed);
}

double Gauge::value() const {
    uint64_t bits = bits_.load(std::memory_order_relaxed);
    double   v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

MetricRegistry::MetricRegistry(size_t max_series) : max_series_(max_series) {}

// The values were already dropped by the config reader, so reaching here means
// some other code path built a label out of a secret. Refuse the whole series
// rather than the one label: a metric that silently loses a dimension is worse
// than one that never appears and shows up in the refusal counter.
bool MetricRegistry::refuse_sensitive(const Labels& labels) const {
    for (const auto& [key, value] : labels) {
        (void)value;
        if (is_sensitive_key(key)) {
            redacted_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

void MetricRegistry::declare_counter(const std::string& name, const std::string& help) {
    std::unique_lock lock(mutex_);
    auto& f = families_[name];
    f.help     = help;
    f.is_gauge = false;
}

void MetricRegistry::declare_gauge(const std::string& name, const std::string& help) {
    std::unique_lock lock(mutex_);
    auto& f = families_[name];
    f.help     = help;
    f.is_gauge = true;
}

std::string MetricRegistry::encode_labels(const Labels& labels) {
    if (labels.empty()) return {};
    std::string out = "{";
    bool first = true;
    for (const auto& [k, v] : labels) {
        if (!first) out += ",";
        first = false;
        out += k;
        out += "=\"";
        for (char c : v) {  // escape per exposition format
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (c == '\n')        { out += "\\n"; }
            else                       { out += c; }
        }
        out += "\"";
    }
    out += "}";
    return out;
}

Counter* MetricRegistry::counter(const std::string& name, const Labels& labels) {
    if (refuse_sensitive(labels)) return nullptr;

    const std::string key = encode_labels(labels);
    {
        std::shared_lock lock(mutex_);
        auto fit = families_.find(name);
        if (fit != families_.end()) {
            auto it = fit->second.counters.find(key);
            if (it != fit->second.counters.end()) return it->second.get();
        }
    }
    std::unique_lock lock(mutex_);
    auto& f  = families_[name];
    auto  it = f.counters.find(key);
    if (it != f.counters.end()) return it->second.get();

    size_t total = 0;
    for (const auto& [n, fam] : families_) total += fam.counters.size() + fam.gauges.size();
    if (total >= max_series_) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    auto* raw = new Counter();
    f.counters.emplace(key, std::unique_ptr<Counter>(raw));
    return raw;
}

Gauge* MetricRegistry::gauge(const std::string& name, const Labels& labels) {
    if (refuse_sensitive(labels)) return nullptr;

    const std::string key = encode_labels(labels);
    {
        std::shared_lock lock(mutex_);
        auto fit = families_.find(name);
        if (fit != families_.end()) {
            auto it = fit->second.gauges.find(key);
            if (it != fit->second.gauges.end()) return it->second.get();
        }
    }
    std::unique_lock lock(mutex_);
    auto& f  = families_[name];
    f.is_gauge = true;
    auto  it = f.gauges.find(key);
    if (it != f.gauges.end()) return it->second.get();

    size_t total = 0;
    for (const auto& [n, fam] : families_) total += fam.counters.size() + fam.gauges.size();
    if (total >= max_series_) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    auto* raw = new Gauge();
    f.gauges.emplace(key, std::unique_ptr<Gauge>(raw));
    return raw;
}

uint64_t MetricRegistry::series_count() const {
    std::shared_lock lock(mutex_);
    uint64_t total = 0;
    for (const auto& [n, f] : families_) total += f.counters.size() + f.gauges.size();
    return total;
}

std::string MetricRegistry::expose() const {
    std::shared_lock lock(mutex_);
    std::ostringstream os;
    os.setf(std::ios::fixed);

    for (const auto& [name, f] : families_) {
        if (f.counters.empty() && f.gauges.empty()) continue;
        if (!f.help.empty()) os << "# HELP " << name << " " << f.help << "\n";
        os << "# TYPE " << name << (f.is_gauge ? " gauge" : " counter") << "\n";

        for (const auto& [key, c] : f.counters) {
            os << name << key << " " << c->value() << "\n";
        }
        for (const auto& [key, g] : f.gauges) {
            os.precision(6);
            os << name << key << " " << g->value() << "\n";
        }
    }
    return os.str();
}

void declare_all_metrics(MetricRegistry& r) {
    // ---- application layer ----
    r.declare_counter("fixmon_messages_total",
                      "FIX application messages seen, by session, direction and type");
    r.declare_counter("fixmon_session_rejects_total",
                      "Session-level Reject (35=3) by reason");
    r.declare_counter("fixmon_business_rejects_total",
                      "BusinessMessageReject (35=j) by reason");
    r.declare_counter("fixmon_exec_rejects_total",
                      "Rejected ExecutionReports and cancel rejects by reason");

    // ---- session layer, mostly from the event log ----
    r.declare_counter("fixmon_session_events_total",
                      "Session-layer events narrated by the engine, by type");
    r.declare_counter("fixmon_seq_gaps_total",
                      "Inbound sequence gaps detected");
    r.declare_counter("fixmon_seq_gap_messages_total",
                      "Total messages missing across all detected gaps");
    r.declare_counter("fixmon_seq_too_low_total",
                      "MsgSeqNum-too-low occurrences, which normally require manual reset");
    r.declare_counter("fixmon_resend_requests_total", "ResendRequests observed");
    r.declare_counter("fixmon_heartbeat_timeouts_total", "Heartbeat timeouts");
    r.declare_counter("fixmon_disconnects_total", "Disconnects by session");
    r.declare_counter("fixmon_reconnect_attempts_total", "Reconnect attempts");
    r.declare_counter("fixmon_logon_rejects_total", "Rejected logons");

    // ---- state gauges ----
    r.declare_gauge("fixmon_session_up",
                    "1 when the session is logged on, 0 otherwise");
    r.declare_gauge("fixmon_session_state",
                    "Numeric LinkState: 1 disconnected 2 connecting 3 logon_pending 4 logged_on 5 stale");
    r.declare_gauge("fixmon_last_message_age_seconds",
                    "Seconds since the last inbound message on this session");
    r.declare_gauge("fixmon_next_expected_seq_num", "Next expected inbound MsgSeqNum");
    r.declare_gauge("fixmon_last_outgoing_seq_num", "Highest outgoing MsgSeqNum seen");

    // Signed: negative means the engine stamped a line in our future. Every
    // other time-based number here - staleness, message age, the latency of the
    // whole pipeline - is measured against the engine's own timestamps, so if
    // this one drifts they are all wrong together and none of them says so.
    r.declare_gauge("fixmon_engine_clock_skew_seconds",
                    "Ingest time minus the engine's timestamp on the last line of this session");

    // ---- what the session was configured as ----
    // Labels here are whitelisted by hand: identity and transport role only.
    // Hosts, ports, credentials and store paths are deliberately absent - this
    // series is joined against the others on `session`, so it needs nothing
    // else to be useful.
    r.declare_gauge("fixmon_session_info",
                    "Static session identity, always 1. Labels: begin_string, connection_type, "
                    "source, seq_reset_policy, resend_capability. Join against the sequence-gap "
                    "counters: a gap under reset_each_logon is routine, the same gap under "
                    "persistent is an incident");
    r.declare_gauge("fixmon_session_log_sources",
                    "Number of log adapters attached to this session (0 means nothing is being read)");
    r.declare_gauge("fixmon_session_config_redacted",
                    "Credential settings found in the engine config and dropped, by count");

    // ---- collector self-monitoring ----
    r.declare_counter("fixmon_lines_read_total", "Log lines read, by adapter");
    r.declare_counter("fixmon_parse_failures_total", "Lines the adapter could not map");
    r.declare_counter("fixmon_events_dropped_total",
                      "Events dropped because the internal queue was full");
    r.declare_counter("fixmon_queue_full_waits_total",
                      "Times a producer had to wait because the internal queue was full");
    r.declare_counter("fixmon_store_rows_written_total", "Rows committed to the event store");
    r.declare_counter("fixmon_store_write_errors_total", "Event store write errors");
    r.declare_counter("fixmon_store_rows_purged_total",
                      "Rows removed by retention. Flat at zero with retention on means "
                      "nothing has aged out yet, or the purge is not running");
    r.declare_gauge("fixmon_queue_depth", "Approximate internal queue depth");
    r.declare_gauge("fixmon_metric_series", "Number of exported series");
    r.declare_counter("fixmon_metric_series_rejected_total",
                      "Series refused because the cardinality cap was reached");
    r.declare_counter("fixmon_metric_series_redacted_total",
                      "Series refused because a label named a credential - should always be 0");
}

}  // namespace fixmon
