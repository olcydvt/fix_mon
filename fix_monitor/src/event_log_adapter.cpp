#include "fixmon/adapters.hpp"
#include "fixmon/fix_parser.hpp"

#include <chrono>
#include <regex>
#include <vector>

namespace fixmon {

namespace {

// Rule table. Hardcoded for now; the shape is deliberately data-like so it can
// be lifted into an external file without touching this adapter's logic.
//
// Order matters: the first match wins, so the more specific patterns come
// first. Anything unmatched becomes SessionEventType::Unparsed and is stored
// verbatim rather than dropped - that is how new engine wording gets noticed.
struct Rule {
    std::regex       pattern;
    SessionEventType type;
    int              expected_group = 0;  // capture index, 0 = none
    int              received_group = 0;
    int              text_group     = 0;
};

const std::vector<Rule>& rules() {
    static const std::vector<Rule> table = [] {
        auto ci = std::regex::icase | std::regex::optimize;
        std::vector<Rule> t;

        t.push_back({std::regex(R"(MsgSeqNum too low.*expecting\s+(\d+).*received\s+(\d+))", ci),
                     SessionEventType::SeqNumTooLow, 1, 2, 0});
        t.push_back({std::regex(R"(MsgSeqNum too high.*expecting\s+(\d+).*received\s+(\d+))", ci),
                     SessionEventType::SeqNumTooHigh, 1, 2, 0});
        t.push_back({std::regex(R"((?:Sent|Sending|Received)\s+ResendRequest.*?(\d+))", ci),
                     SessionEventType::ResendRequested, 0, 0, 0});
        t.push_back({std::regex(R"(Sequence\s+reset)", ci),
                     SessionEventType::SequenceReset, 0, 0, 0});

        t.push_back({std::regex(R"((?:Test\s*Request\s+timed\s+out|Heartbeat\s+timeout|No\s+response\s+to\s+test\s+request))", ci),
                     SessionEventType::HeartbeatTimeout, 0, 0, 0});
        t.push_back({std::regex(R"(Sending\s+test\s+request)", ci),
                     SessionEventType::TestRequestSent, 0, 0, 0});

        t.push_back({std::regex(R"(Logon\s+(?:rejected|failed)|Error\s+during\s+logon|Invalid\s+logon)", ci),
                     SessionEventType::LogonRejected, 0, 0, 0});
        t.push_back({std::regex(R"((?:Initiated|Sending|Sent)\s+logon)", ci),
                     SessionEventType::LogonSent, 0, 0, 0});
        t.push_back({std::regex(R"(Received\s+logon)", ci),
                     SessionEventType::LogonReceived, 0, 0, 0});

        t.push_back({std::regex(R"((?:Sending|Sent)\s+logout)", ci),
                     SessionEventType::LogoutSent, 0, 0, 0});
        t.push_back({std::regex(R"(Received\s+logout)", ci),
                     SessionEventType::LogoutReceived, 0, 0, 0});

        // Capture the reason text - this is the single most useful field in the
        // whole event log and it exists nowhere in the message log.
        t.push_back({std::regex(R"(Disconnecting\s*:?\s*(.*))", ci),
                     SessionEventType::Disconnected, 0, 0, 1});
        t.push_back({std::regex(R"((?:Socket\s+(?:read|write)\s+error|Connection\s+reset|Disconnected))", ci),
                     SessionEventType::Disconnected, 0, 0, 0});

        t.push_back({std::regex(R"((?:Connection\s+failed|Failed\s+to\s+connect|Connect\s+failed)\s*:?\s*(.*))", ci),
                     SessionEventType::ConnectFailed, 0, 0, 1});
        t.push_back({std::regex(R"(Attempting\s+to\s+reconnect|Reconnect)", ci),
                     SessionEventType::ReconnectAttempt, 0, 0, 0});
        t.push_back({std::regex(R"(Connecting\s+to)", ci),
                     SessionEventType::Connecting, 0, 0, 0});

        t.push_back({std::regex(R"(Session\s+schedule|Session\s+state|Session\s+(?:start|end))", ci),
                     SessionEventType::SessionScheduled, 0, 0, 0});
        t.push_back({std::regex(R"(Created\s+session)", ci),
                     SessionEventType::SessionCreated, 0, 0, 0});
        t.push_back({std::regex(R"(Configuration\s+error|Invalid\s+configuration|Dictionary)", ci),
                     SessionEventType::ConfigError, 0, 0, 0});

        return t;
    }();
    return table;
}

uint64_t group_u64(const std::smatch& m, int idx) {
    if (idx <= 0 || idx >= static_cast<int>(m.size())) return 0;
    try {
        return std::stoull(m[idx].str());
    } catch (...) {
        return 0;
    }
}

}  // namespace

EventLogAdapter::EventLogAdapter(SessionConfig cfg, bool from_beginning, int poll_ms)
    : cfg_(std::move(cfg)),
      from_beginning_(from_beginning),
      poll_ms_(poll_ms) {
    session_id_ = cfg_.session_id();
}

EventLogAdapter::~EventLogAdapter() { stop(); }

bool EventLogAdapter::parse_line(const std::string& line, Event& out) {
    if (line.empty()) return false;

    int64_t          ts = 0;
    std::string_view payload;
    split_log_line(line, ts, payload);

    std::string body(payload);

    out = Event{};
    out.event_class  = EventClass::SessionEvent;
    out.source       = Source::EventLog;
    out.session_id   = session_id_;
    out.engine_ts_ns = ts;
    out.ingest_ts_ns = now_ns();
    out.raw          = line;
    out.text         = body;

    for (const Rule& r : rules()) {
        std::smatch m;
        if (!std::regex_search(body, m, r.pattern)) continue;

        out.session_event   = r.type;
        out.expected_seq_num = group_u64(m, r.expected_group);
        out.received_seq_num = group_u64(m, r.received_group);

        if (r.text_group > 0 && r.text_group < static_cast<int>(m.size())) {
            std::string reason = m[r.text_group].str();
            if (!reason.empty()) out.text = reason;
        }
        return true;
    }

    // No rule matched. Keep it rather than discard it: an unparsed line is a
    // signal that the engine's wording changed, and the raw text is still
    // useful to a human reading the store.
    out.session_event = SessionEventType::Unparsed;
    return true;
}

void EventLogAdapter::start(EventSink sink) {
    if (running_.exchange(true)) return;
    sink_   = std::move(sink);
    reader_ = std::make_unique<TailReader>(cfg_.event_log_path, from_beginning_);
    thread_ = std::thread(&EventLogAdapter::run, this);
}

void EventLogAdapter::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    reader_.reset();
}

void EventLogAdapter::run() {
    Event ev;
    while (running_.load(std::memory_order_relaxed)) {
        reader_->poll([&](const std::string& line) {
            lines_read_.fetch_add(1, std::memory_order_relaxed);
            if (parse_line(line, ev)) {
                if (ev.session_event == SessionEventType::Unparsed) {
                    parse_errors_.fetch_add(1, std::memory_order_relaxed);
                }
                sink_(std::move(ev));
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms_));
    }
}

}  // namespace fixmon
