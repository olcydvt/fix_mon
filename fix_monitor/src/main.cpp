#include <atomic>

#ifndef FIXMON_VERSION
#define FIXMON_VERSION "dev"
#endif
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fixmon/adapters.hpp"
#include "fixmon/config.hpp"
#include "fixmon/event_store.hpp"
#include "fixmon/fix_parser.hpp"
#include "fixmon/http_server.hpp"
#include "fixmon/metrics.hpp"
#include "fixmon/mpmc_queue.hpp"
#include "fixmon/redact.hpp"
#include "fixmon/session_state.hpp"

using namespace fixmon;

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

int state_code(LinkState s) {
    switch (s) {
        case LinkState::Disconnected: return 1;
        case LinkState::Connecting:   return 2;
        case LinkState::LogonPending: return 3;
        case LinkState::LoggedOn:     return 4;
        case LinkState::Stale:        return 5;
        default:                      return 0;
    }
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) out += ' ';
                else                                      out += c;
        }
    }
    return out;
}

std::string sessions_json(const SessionRegistry& reg) {
    auto snaps = reg.snapshot();
    std::ostringstream os;
    os << "{\"sessions\":[";
    bool first = true;
    for (const auto& s : snaps) {
        if (!first) os << ",";
        first = false;
        os << "{"
           << "\"session_id\":\""  << json_escape(s.session_id) << "\","
           << "\"state\":\""       << to_string(s.state) << "\","
           << "\"msgs_in\":"       << s.msgs_in << ","
           << "\"msgs_out\":"      << s.msgs_out << ","
           << "\"next_expected_in\":" << s.next_expected_in << ","
           << "\"last_outgoing_seq\":" << s.last_outgoing_seq << ","
           << "\"seq_gaps\":"      << s.seq_gaps << ","
           << "\"seq_gaps_reported\":" << s.seq_gaps_reported << ","
           << "\"seq_too_low\":"   << s.seq_too_low << ","
           << "\"resend_requests\":" << s.resend_requests << ","
           << "\"heartbeat_timeouts\":" << s.heartbeat_timeouts << ","
           << "\"disconnects\":"   << s.disconnects << ","
           << "\"reconnect_attempts\":" << s.reconnect_attempts << ","
           << "\"logon_rejects\":" << s.logon_rejects << ","
           << "\"session_rejects\":" << s.session_rejects << ","
           << "\"business_rejects\":" << s.business_rejects << ","
           << "\"exec_rejects\":"  << s.exec_rejects << ","
           << "\"last_incoming_ts_ns\":" << s.last_incoming_ts_ns << ","
           << "\"last_disconnect_reason\":\"" << json_escape(s.last_disconnect_reason) << "\","
           << "\"last_reject_text\":\"" << json_escape(s.last_reject_text) << "\""
           << "}";
    }
    os << "]}";
    return os.str();
}

void inc(MetricRegistry& r, const char* name, const Labels& labels, uint64_t n = 1) {
    if (Counter* c = r.counter(name, labels)) c->inc(n);
}

void set(MetricRegistry& r, const char* name, const Labels& labels, double v) {
    if (Gauge* g = r.gauge(name, labels)) g->set(v);
}

void mirror(MetricRegistry& r, const char* name, const Labels& labels, uint64_t v) {
    if (Counter* c = r.counter(name, labels)) c->set(v);
}

// Applies one event to the metric registry. Label sets are deliberately narrow:
// session, direction, msg_type, reason. Nothing per-order ever reaches here.
void update_metrics(MetricRegistry& reg, const Event& ev, uint64_t gap) {
    const std::string& sid = ev.session_id;

    if (ev.event_class == EventClass::FixMessage) {
        inc(reg, "fixmon_messages_total",
            {{"session", sid},
             {"direction", to_string(ev.direction)},
             {"msg_type", ev.msg_type},
             {"msg_type_name", msg_type_name(ev.msg_type)}});

        const std::string reason =
            ev.reject_reason >= 0 ? std::to_string(ev.reject_reason) : "none";

        if (ev.msg_type == "3") {
            inc(reg, "fixmon_session_rejects_total",
                {{"session", sid}, {"reason", reason},
                 {"ref_tag", ev.ref_tag_id >= 0 ? std::to_string(ev.ref_tag_id) : "none"}});
        } else if (ev.msg_type == "j") {
            inc(reg, "fixmon_business_rejects_total", {{"session", sid}, {"reason", reason}});
        } else if ((ev.msg_type == "8" && ev.ord_status == "8") || ev.msg_type == "9") {
            inc(reg, "fixmon_exec_rejects_total", {{"session", sid}, {"reason", reason}});
        } else if (ev.msg_type == "2" && ev.direction == Direction::Outgoing) {
            inc(reg, "fixmon_resend_requests_total", {{"session", sid}, {"src", "message_log"}});
        }
    } else {
        inc(reg, "fixmon_session_events_total",
            {{"session", sid}, {"type", to_string(ev.session_event)}});

        switch (ev.session_event) {
            case SessionEventType::SeqNumTooLow:
                inc(reg, "fixmon_seq_too_low_total", {{"session", sid}});
                break;
            case SessionEventType::ResendRequested:
                inc(reg, "fixmon_resend_requests_total", {{"session", sid}, {"src", "event_log"}});
                break;
            case SessionEventType::HeartbeatTimeout:
                inc(reg, "fixmon_heartbeat_timeouts_total", {{"session", sid}});
                break;
            case SessionEventType::Disconnected:
            case SessionEventType::LogoutReceived:
            case SessionEventType::LogoutSent:
                inc(reg, "fixmon_disconnects_total", {{"session", sid}});
                break;
            case SessionEventType::ReconnectAttempt:
                inc(reg, "fixmon_reconnect_attempts_total", {{"session", sid}});
                break;
            case SessionEventType::LogonRejected:
                inc(reg, "fixmon_logon_rejects_total", {{"session", sid}});
                break;
            default:
                break;
        }
    }

    // Both the message stream and the event log can reveal the same gap.
    // Label by source rather than summing, so a dashboard picks one and a
    // divergence between the two stays visible.
    if (gap > 0) {
        Labels l{{"session", sid}, {"src", to_string(ev.source)}};
        inc(reg, "fixmon_seq_gaps_total", l);
        inc(reg, "fixmon_seq_gap_messages_total", l, gap);
    }
}

void publish_state(MetricRegistry& reg, const SessionRegistry& sessions, int64_t now) {
    for (const auto& s : sessions.snapshot()) {
        Labels l{{"session", s.session_id}};
        set(reg, "fixmon_session_up", l, s.state == LinkState::LoggedOn ? 1.0 : 0.0);
        set(reg, "fixmon_session_state", l, state_code(s.state));
        set(reg, "fixmon_next_expected_seq_num", l, static_cast<double>(s.next_expected_in));
        set(reg, "fixmon_last_outgoing_seq_num", l, static_cast<double>(s.last_outgoing_seq));

        int64_t last = s.last_incoming_ts_ns != 0 ? s.last_incoming_ts_ns : s.last_event_ts_ns;
        double age = last != 0 ? static_cast<double>(now - last) / 1e9 : -1.0;
        set(reg, "fixmon_last_message_age_seconds", l, age);
    }
}

// Static description of a session, published once at startup.
//
// Every label here is chosen by hand. The engine config these sessions come
// from also carries hosts, ports and store paths, and the outright secret
// settings never made it into SessionConfig at all. What is left is enough to
// group dashboards by protocol version and by initiator/acceptor role.
void publish_session_info(MetricRegistry& reg, const SessionConfig& sc, int adapters) {
    const std::string sid = sc.session_id();

    set(reg, "fixmon_session_info",
        {{"session", sid},
         {"begin_string", sc.begin_string},
         {"connection_type", sc.connection_type.empty() ? "unknown" : sc.connection_type},
         {"source", sc.from_quickfix ? "quickfix_config" : "fixmon_ini"}},
        1.0);

    Labels l{{"session", sid}};
    // Zero here means the session is configured but nothing is being tailed -
    // the one failure mode where every other metric would look perfectly fine.
    set(reg, "fixmon_session_log_sources", l, adapters);
    set(reg, "fixmon_session_config_redacted", l,
        static_cast<double>(sc.redacted_settings.size()));
}

void print_session(const SessionConfig& sc, int adapters) {
    std::cout << "session: " << sc.session_id() << "  hb=" << sc.heartbeat_interval << "s";
    if (!sc.connection_type.empty()) std::cout << "  " << sc.connection_type;
    if (sc.from_quickfix) std::cout << "  <- " << sc.origin;
    std::cout << "\n";

    if (!sc.message_log_path.empty()) std::cout << "    messages: " << sc.message_log_path << "\n";
    if (!sc.event_log_path.empty())   std::cout << "    events  : " << sc.event_log_path << "\n";
    if (!sc.start_time.empty() || !sc.end_time.empty()) {
        std::cout << "    schedule: " << sc.start_time << " - " << sc.end_time << "\n";
    }
    if (adapters == 0) {
        std::cout << "    WARNING: no log files resolved, this session will stay silent\n";
    }
    if (!sc.redacted_settings.empty()) {
        // Names only. Saying they were seen and dropped is the point; silence
        // would leave an operator wondering whether we read them at all.
        std::cout << "    credentials ignored:";
        for (const auto& key : sc.redacted_settings) {
            std::cout << " " << key << "=" << kRedacted;
        }
        std::cout << "\n";
    }
}

void print_usage() {
    std::cerr << "usage: fixmon <config.ini> [--quickfix <session.cfg>]...\n"
                 "       fixmon --quickfix <session.cfg>...   (collector defaults)\n"
                 "       fixmon --print-config <...>          (resolve and exit)\n"
                 "       fixmon --version\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string              ini_path;
    std::vector<std::string> quickfix_paths;
    bool                     print_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            std::cout << "fixmon " << FIXMON_VERSION << "\n";
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        if (arg == "--quickfix" || arg == "-q") {
            if (++i >= argc) {
                std::cerr << "--quickfix needs a path to a QuickFIX cfg file\n";
                return 2;
            }
            quickfix_paths.emplace_back(argv[i]);
        } else if (arg == "--print-config") {
            print_only = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "unknown option: " << arg << "\n";
            print_usage();
            return 2;
        } else if (ini_path.empty()) {
            ini_path = arg;
        } else {
            std::cerr << "unexpected argument: " << arg << "\n";
            print_usage();
            return 2;
        }
    }

    if (ini_path.empty() && quickfix_paths.empty()) {
        print_usage();
        return 2;
    }

    // Unbuffered: otherwise startup and shutdown lines sit in the buffer and
    // show up late (or not at all) in docker logs and journald.
    std::cout << std::unitbuf;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    AppConfig cfg;
    try {
        cfg = ini_path.empty() ? config_from_quickfix(quickfix_paths)
                               : load_config(ini_path, quickfix_paths);
    } catch (const std::exception& e) {
        std::cerr << "config error: " << e.what() << "\n";
        return 2;
    }

    // Discovery is never silent: every import decision, and every log file we
    // could not find, gets said out loud before anything starts.
    for (const auto& note : cfg.notes) std::cout << "config: " << note << "\n";

    MetricRegistry registry;
    declare_all_metrics(registry);

    SessionRegistry sessions(cfg.stale_after_multiple);

    // --print-config resolves everything, shows what would be watched, exits.
    // Lets an engine cfg import be checked without touching the database or
    // binding a port.
    if (print_only) {
        for (const auto& sc : cfg.sessions) {
            int attached = static_cast<int>(!sc.message_log_path.empty()) +
                           static_cast<int>(!sc.event_log_path.empty());
            print_session(sc, attached);
        }
        std::cout << "event store: " << cfg.db_path << " (not opened)\n"
                  << "metrics port: " << cfg.metrics_port << " (not bound)\n";
        return 0;
    }

    EventStore  store;
    std::string err;
    if (!store.open(cfg.db_path, err)) {
        std::cerr << "event store: " << err << "\n";
        return 1;
    }
    std::cout << "event store: " << cfg.db_path << "\n";

    MpmcQueue<Event>      queue(cfg.queue_capacity);
    std::atomic<uint64_t> dropped{0};

    auto sink = [&queue, &dropped](Event&& ev) {
        if (!queue.try_push(std::move(ev))) {
            dropped.fetch_add(1, std::memory_order_relaxed);
        }
    };

    // Two adapters per session, each on its own thread, both feeding one queue.
    std::vector<std::unique_ptr<ISourceAdapter>> adapters;
    for (const auto& sc : cfg.sessions) {
        sessions.register_session(sc.session_id(), sc.heartbeat_interval);

        int attached = 0;
        if (!sc.message_log_path.empty()) {
            adapters.push_back(std::make_unique<MessageLogAdapter>(
                sc, cfg.from_beginning, cfg.poll_interval_ms));
            ++attached;
        }
        if (!sc.event_log_path.empty()) {
            adapters.push_back(std::make_unique<EventLogAdapter>(
                sc, cfg.from_beginning, cfg.poll_interval_ms));
            ++attached;
        }

        print_session(sc, attached);
        publish_session_info(registry, sc, attached);
    }
    for (auto& a : adapters) a->start(sink);

    HttpServer http(cfg.metrics_port,
                    [&registry] { return registry.expose(); },
                    [&sessions] { return sessions_json(sessions); });
    if (!http.start(err)) {
        std::cerr << "http: " << err << "\n";
        store.close();
        return 1;
    }
    std::cout << "metrics: http://0.0.0.0:" << cfg.metrics_port << "/metrics\n";

    // ---- pipeline: single consumer, so no locking on the hot path ----
    auto   last_flush    = std::chrono::steady_clock::now();
    auto   last_snapshot = last_flush;
    Event  ev;

    while (!g_stop.load(std::memory_order_relaxed)) {
        bool did_work = false;

        while (queue.try_pop(ev)) {
            did_work = true;
            uint64_t gap = sessions.apply(ev);
            update_metrics(registry, ev, gap);
            store.stage(ev);

            if (store.staged() >= cfg.batch_size) {
                store.flush();  // counters are mirrored from the store below
            }
        }

        auto now_tp = std::chrono::steady_clock::now();

        if (std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_flush).count()
                >= cfg.batch_flush_ms) {
            store.flush();
            last_flush = now_tp;

            int64_t now = now_ns();
            sessions.evaluate_staleness(now);
            publish_state(registry, sessions, now);

            set(registry, "fixmon_queue_depth", {}, static_cast<double>(queue.size_approx()));
            set(registry, "fixmon_metric_series", {}, static_cast<double>(registry.series_count()));

            // Self-monitoring. These mirror counters owned by the store, the
            // queue and the adapters. A collector that silently stops keeping
            // up is worse than no collector, so these get alert rules.
            mirror(registry, "fixmon_events_dropped_total", {}, dropped.load());
            mirror(registry, "fixmon_store_rows_written_total", {}, store.rows_written());
            mirror(registry, "fixmon_store_write_errors_total", {}, store.write_errors());
            mirror(registry, "fixmon_metric_series_rejected_total", {},
                   registry.rejected_series());
            // Should stay at zero forever. Movement means some code path tried
            // to label a series with a credential name and the registry stopped
            // it - a defect, not a capacity problem.
            mirror(registry, "fixmon_metric_series_redacted_total", {},
                   registry.redacted_series());

            for (auto& a : adapters) {
                Labels al{{"adapter", a->name()}, {"session", a->session_id()}};
                mirror(registry, "fixmon_lines_read_total", al, a->lines_read());
                mirror(registry, "fixmon_parse_failures_total", al, a->parse_errors());
            }
        }

        // Periodic state snapshot so an incident can be reconstructed later
        // without replaying the whole event table.
        if (std::chrono::duration_cast<std::chrono::seconds>(now_tp - last_snapshot).count()
                >= cfg.snapshot_interval_s) {
            int64_t now = now_ns();
            for (const auto& s : sessions.snapshot()) store.write_snapshot(s, now);
            last_snapshot = now_tp;
        }

        if (!did_work) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::cout << "\nshutting down...\n";
    for (auto& a : adapters) a->stop();

    while (queue.try_pop(ev)) {
        uint64_t gap = sessions.apply(ev);
        update_metrics(registry, ev, gap);
        store.stage(ev);
    }
    store.flush();

    int64_t now = now_ns();
    for (const auto& s : sessions.snapshot()) store.write_snapshot(s, now);

    mirror(registry, "fixmon_events_dropped_total", {}, dropped.load());
    mirror(registry, "fixmon_store_rows_written_total", {}, store.rows_written());
    mirror(registry, "fixmon_store_write_errors_total", {}, store.write_errors());

    http.stop();
    store.close();

    std::cout << "rows written: " << store.rows_written()
              << "  write errors: " << store.write_errors()
              << "  dropped: " << dropped.load() << "\n";
    for (auto& a : adapters) {
        std::cout << "  " << a->name() << ": lines=" << a->lines_read()
                  << " unmatched=" << a->parse_errors() << "\n";
    }
    return 0;
}
