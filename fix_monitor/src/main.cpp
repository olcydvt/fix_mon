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
#include <thread>
#include <vector>

#include "fixmon/adapters.hpp"
#include "fixmon/config.hpp"
#include "fixmon/event_store.hpp"
#include "fixmon/fix_parser.hpp"
#include "fixmon/http_server.hpp"
#include "fixmon/metrics.hpp"
#include "fixmon/mpmc_queue.hpp"
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

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-v")) {
        std::cout << "fixmon " << FIXMON_VERSION << "\n";
        return 0;
    }
    if (argc < 2) {
        std::cerr << "usage: fixmon <config.ini>\n"
                     "       fixmon --version\n";
        return 2;
    }

    // Unbuffered: otherwise startup and shutdown lines sit in the buffer and
    // show up late (or not at all) in docker logs and journald.
    std::cout << std::unitbuf;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    AppConfig cfg;
    try {
        cfg = load_config(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << "config error: " << e.what() << "\n";
        return 2;
    }

    MetricRegistry registry;
    declare_all_metrics(registry);

    SessionRegistry sessions(cfg.stale_after_multiple);

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
        if (!sc.message_log_path.empty()) {
            adapters.push_back(std::make_unique<MessageLogAdapter>(
                sc, cfg.from_beginning, cfg.poll_interval_ms));
        }
        if (!sc.event_log_path.empty()) {
            adapters.push_back(std::make_unique<EventLogAdapter>(
                sc, cfg.from_beginning, cfg.poll_interval_ms));
        }
        std::cout << "session: " << sc.session_id()
                  << "  hb=" << sc.heartbeat_interval << "s\n";
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

            for (auto& a : adapters) {
                Labels al{{"adapter", a->name()}};
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
