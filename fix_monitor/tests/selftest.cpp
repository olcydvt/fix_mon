#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "fixmon/adapters.hpp"
#include "fixmon/config.hpp"
#include "fixmon/event_store.hpp"
#include "fixmon/fix_parser.hpp"
#include "fixmon/metrics.hpp"
#include "fixmon/mpmc_queue.hpp"
#include "fixmon/session_state.hpp"

using namespace fixmon;

static int g_failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::cerr << "FAIL: " << (msg) << "  [" << #cond << "] line "  \
                      << __LINE__ << "\n";                                 \
            ++g_failures;                                                  \
        } else {                                                           \
            std::cout << "  ok: " << (msg) << "\n";                        \
        }                                                                  \
    } while (0)

static SessionConfig make_cfg() {
    SessionConfig c;
    c.begin_string   = "FIX.4.4";
    c.sender_comp_id = "BROKER1";
    c.target_comp_id = "VENUEX";
    c.heartbeat_interval = 30;
    return c;
}

static void test_timestamps() {
    std::cout << "\n[timestamp]\n";
    int64_t ns = parse_fix_timestamp("20240115-09:30:00.123");
    CHECK(ns > 0, "millisecond timestamp parses");
    CHECK(ns % 1000000000LL == 123000000LL, "millisecond fraction scaled to ns");

    int64_t ns2 = parse_fix_timestamp("20240115-09:30:00.123456789");
    CHECK(ns2 % 1000000000LL == 123456789LL, "nanosecond fraction preserved");
    CHECK(parse_fix_timestamp("garbage") == 0, "junk rejected");
}

static void test_fix_fields() {
    std::cout << "\n[fix fields]\n";
    TagMap tags;
    std::string msg = "8=FIX.4.4|9=100|35=D|34=42|49=BROKER1|56=VENUEX|11=ORD1|10=123|";
    CHECK(parse_fix_fields(msg, tags), "pipe-delimited body parses");
    CHECK(tags[35] == "D", "tag 35 extracted");
    CHECK(tags[34] == "42", "tag 34 extracted");

    std::string soh = "8=FIX.4.4\x01" "35=8\x01" "39=8\x01" "58=Rejected by venue\x01";
    TagMap t2;
    CHECK(parse_fix_fields(soh, t2), "SOH-delimited body parses");
    CHECK(t2[39] == "8", "ord status extracted from SOH body");
    CHECK(t2[58] == "Rejected by venue", "text with spaces extracted");
}

static void test_message_adapter() {
    std::cout << "\n[message log adapter]\n";
    MessageLogAdapter a(make_cfg(), true, 100);
    Event ev;

    CHECK(a.parse_line(
              "20240115-09:30:00.100 : 8=FIX.4.4|9=70|35=A|34=1|49=BROKER1|56=VENUEX|98=0|108=30|10=1|",
              ev),
          "outgoing logon parses");
    CHECK(ev.direction == Direction::Outgoing, "direction derived from tag 49 = our sender");
    CHECK(ev.msg_type == "A", "msg type A");
    CHECK(ev.msg_seq_num == 1, "seq num 1");
    CHECK(ev.event_class == EventClass::FixMessage, "classified as fix message");

    CHECK(a.parse_line(
              "20240115-09:30:00.200 : 8=FIX.4.4|9=70|35=A|34=1|49=VENUEX|56=BROKER1|98=0|108=30|10=1|",
              ev),
          "incoming logon parses");
    CHECK(ev.direction == Direction::Incoming, "direction flips for counterparty sender");

    CHECK(a.parse_line(
              "20240115-09:31:00.000 : 8=FIX.4.4|35=3|34=9|49=VENUEX|56=BROKER1|45=7|371=11|373=1|58=Required tag missing|10=1|",
              ev),
          "session reject parses");
    CHECK(ev.reject_reason == 1, "tag 373 lifted into reject_reason");
    CHECK(ev.ref_tag_id == 11, "tag 371 lifted into ref_tag_id");
    CHECK(ev.ref_seq_num == 7, "tag 45 lifted into ref_seq_num");
    CHECK(ev.text == "Required tag missing", "tag 58 lifted into text");

    CHECK(a.parse_line(
              "20240115-09:32:00.000 : 8=FIX.4.4|35=8|34=10|49=VENUEX|56=BROKER1|39=8|103=3|58=Market closed|10=1|",
              ev),
          "rejected exec report parses");
    CHECK(ev.ord_status == "8", "ord status 8 captured");
    CHECK(ev.reject_reason == 3, "OrdRejReason used when no session reject reason");

    CHECK(!a.parse_line("20240115-09:33:00.000 : not a fix message at all", ev),
          "non-FIX line rejected");
}

static void test_event_adapter() {
    std::cout << "\n[event log adapter]\n";
    EventLogAdapter a(make_cfg(), true, 100);
    Event ev;

    a.parse_line("20240115-09:29:59.000 : Created session", ev);
    CHECK(ev.session_event == SessionEventType::SessionCreated, "created session");
    CHECK(ev.event_class == EventClass::SessionEvent, "classified as session event");

    a.parse_line("20240115-09:30:00.000 : Initiated logon request", ev);
    CHECK(ev.session_event == SessionEventType::LogonSent, "logon sent");

    a.parse_line("20240115-09:30:00.300 : Received logon response", ev);
    CHECK(ev.session_event == SessionEventType::LogonReceived, "logon received");

    a.parse_line("20240115-10:00:00.000 : MsgSeqNum too high, expecting 45 but received 52", ev);
    CHECK(ev.session_event == SessionEventType::SeqNumTooHigh, "seq too high detected");
    CHECK(ev.expected_seq_num == 45, "expected seq captured");
    CHECK(ev.received_seq_num == 52, "received seq captured");

    a.parse_line("20240115-10:00:01.000 : MsgSeqNum too low, expecting 45 but received 12", ev);
    CHECK(ev.session_event == SessionEventType::SeqNumTooLow, "seq too low detected");

    a.parse_line("20240115-10:05:00.000 : Disconnecting: Socket exception, connection reset by peer", ev);
    CHECK(ev.session_event == SessionEventType::Disconnected, "disconnect detected");
    CHECK(ev.text.find("Socket exception") != std::string::npos,
          "disconnect reason text captured - the field the message log never has");

    a.parse_line("20240115-10:06:00.000 : Test Request timed out. Session will be disconnected.", ev);
    CHECK(ev.session_event == SessionEventType::HeartbeatTimeout, "heartbeat timeout detected");

    a.parse_line("20240115-10:06:30.000 : Logon rejected: Invalid credentials for BROKER1", ev);
    CHECK(ev.session_event == SessionEventType::LogonRejected, "logon reject detected");

    a.parse_line("20240115-10:06:40.000 : Error during logon", ev);
    CHECK(ev.session_event == SessionEventType::LogonRejected, "alternate logon-failure wording");

    a.parse_line("20240115-10:06:50.000 : Attempting to reconnect in 5 seconds", ev);
    CHECK(ev.session_event == SessionEventType::ReconnectAttempt, "reconnect attempt detected");

    a.parse_line("20240115-10:07:00.000 : Some wording no rule knows about yet", ev);
    CHECK(ev.session_event == SessionEventType::Unparsed, "unknown wording kept as unparsed");
    CHECK(!ev.raw.empty(), "unparsed line keeps its raw text");
}

static void test_session_state() {
    std::cout << "\n[session state]\n";
    SessionRegistry reg(2);
    const std::string sid = "FIX.4.4:BROKER1->VENUEX";
    reg.register_session(sid, 30);

    auto msg = [&](const char* type, uint64_t seq, Direction d) {
        Event e;
        e.event_class  = EventClass::FixMessage;
        e.source       = Source::MessageLog;
        e.session_id   = sid;
        e.msg_type     = type;
        e.msg_seq_num  = seq;
        e.direction    = d;
        e.engine_ts_ns = now_ns();
        return reg.apply(e);
    };

    msg("A", 1, Direction::Incoming);
    CHECK(reg.snapshot()[0].state == LinkState::LoggedOn, "inbound logon moves to logged_on");

    msg("0", 2, Direction::Incoming);
    msg("0", 3, Direction::Incoming);
    uint64_t gap = msg("8", 9, Direction::Incoming);
    CHECK(gap == 5, "gap size computed from the message stream alone");
    CHECK(reg.snapshot()[0].seq_gaps == 1, "derived gap counted once, not twice");

    Event ev;
    ev.event_class   = EventClass::SessionEvent;
    ev.source        = Source::EventLog;
    ev.session_id    = sid;
    ev.session_event = SessionEventType::Disconnected;
    ev.text          = "Socket exception";
    ev.engine_ts_ns  = now_ns();
    reg.apply(ev);

    auto s = reg.snapshot()[0];
    CHECK(s.state == LinkState::Disconnected, "event log disconnect updates state");
    CHECK(s.last_disconnect_reason == "Socket exception",
          "reason from event log attached to session state");
    CHECK(s.msgs_in == 4, "inbound message count");
}

static void test_logon_reject_flow() {
    std::cout << "\n[logon reject flow]\n";
    SessionRegistry reg(2);
    const std::string sid = "FIX.4.4:BROKER1->VENUEX";
    reg.register_session(sid, 30);

    auto sev = [&](SessionEventType t, const char* text) {
        Event e;
        e.event_class   = EventClass::SessionEvent;
        e.source        = Source::EventLog;
        e.session_id    = sid;
        e.session_event = t;
        e.text          = text;
        e.engine_ts_ns  = now_ns();
        reg.apply(e);
    };

    sev(SessionEventType::Disconnected,     "Socket exception");
    sev(SessionEventType::ReconnectAttempt, "Attempting to reconnect in 5 seconds");
    sev(SessionEventType::LogonSent,        "Initiated logon request");
    sev(SessionEventType::LogonRejected,    "Invalid credentials for BROKER1");

    auto s = reg.snapshot()[0];
    CHECK(s.logon_rejects == 1, "logon reject counted");
    CHECK(s.reconnect_attempts == 1, "reconnect attempt counted");
    CHECK(s.state == LinkState::Disconnected, "rejected logon leaves session down");
    CHECK(s.last_disconnect_reason == "Invalid credentials for BROKER1",
          "reject reason kept - reconnect loop with no logon means credentials, "
          "certificate or schedule, not a network blip");
}

static void test_staleness() {
    std::cout << "\n[staleness]\n";
    SessionRegistry reg(2);
    const std::string sid = "FIX.4.4:BROKER1->VENUEX";
    reg.register_session(sid, 1);  // 1s heartbeat, tolerance 2s

    Event e;
    e.event_class  = EventClass::FixMessage;
    e.session_id   = sid;
    e.msg_type     = "A";
    e.direction    = Direction::Incoming;
    e.msg_seq_num  = 1;
    e.engine_ts_ns = now_ns() - 10LL * 1000000000LL;  // 10s ago
    reg.apply(e);

    reg.evaluate_staleness(now_ns());
    CHECK(reg.snapshot()[0].state == LinkState::Stale,
          "silent session past heartbeat tolerance goes stale");
}

static void test_queue() {
    std::cout << "\n[mpmc queue]\n";
    MpmcQueue<Event> q(8);
    for (int i = 0; i < 8; ++i) {
        Event e;
        e.session_id = "s";
        e.msg_seq_num = static_cast<uint64_t>(i);
        CHECK(q.try_push(std::move(e)) == true, i == 0 ? "push succeeds" : "push succeeds");
    }
    Event overflow;
    overflow.session_id = "s";
    CHECK(!q.try_push(std::move(overflow)), "bounded queue refuses when full instead of growing");

    Event out;
    CHECK(q.try_pop(out), "pop succeeds");
    CHECK(out.msg_seq_num == 0, "FIFO order preserved");

    // concurrent producers, single consumer
    MpmcQueue<Event> q2(1024);
    std::atomic<int> pushed{0};
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t) {
        producers.emplace_back([&] {
            for (int i = 0; i < 100; ++i) {
                Event e;
                e.session_id = "s";
                if (q2.try_push(std::move(e))) pushed.fetch_add(1);
            }
        });
    }
    for (auto& t : producers) t.join();
    int popped = 0;
    Event tmp;
    while (q2.try_pop(tmp)) ++popped;
    CHECK(popped == pushed.load(), "no events lost across 4 concurrent producers");
}

static void test_metrics() {
    std::cout << "\n[metrics]\n";
    MetricRegistry reg;
    declare_all_metrics(reg);

    reg.counter("fixmon_messages_total",
                {{"session", "FIX.4.4:A->B"}, {"direction", "in"}, {"msg_type", "D"}})->inc();
    reg.counter("fixmon_messages_total",
                {{"session", "FIX.4.4:A->B"}, {"direction", "in"}, {"msg_type", "D"}})->inc(4);
    reg.gauge("fixmon_session_up", {{"session", "FIX.4.4:A->B"}})->set(1.0);

    std::string out = reg.expose();
    CHECK(out.find("# TYPE fixmon_messages_total counter") != std::string::npos,
          "counter TYPE line emitted");
    CHECK(out.find("msg_type=\"D\"} 5") != std::string::npos, "counter accumulates to 5");
    CHECK(out.find("fixmon_session_up{session=\"FIX.4.4:A->B\"} 1") != std::string::npos,
          "gauge emitted");
    CHECK(reg.series_count() == 2, "two series registered");

    MetricRegistry capped(3);
    capped.declare_counter("x", "h");
    for (int i = 0; i < 10; ++i) capped.counter("x", {{"l", std::to_string(i)}});
    CHECK(capped.series_count() <= 3, "series cap holds against a runaway label");
    CHECK(capped.rejected_series() > 0, "rejected series counted rather than silently dropped");
}

static void test_config() {
    std::cout << "\n[config]\n";
    const char* path = "/tmp/fixmon_cfg_test.ini";
    {
        std::ofstream f(path);
        f << "[global]\n"
             "db_path        = ./x.db\n"
             "metrics_port   = 9200      # inline comment after a number\n"
             "from_beginning = true      # inline comment after a bool\n"
             "queue_size     = 1000      ; semicolon comment\n"
             "snapshot_interval_s = 7\n"
             "\n[session]\n"
             "begin_string   = FIX.4.2\n"
             "sender_comp_id = ME\n"
             "target_comp_id = YOU\n"
             "message_log    = /tmp/m.log\n"
             "heartbeat_interval = 45\n";
    }
    AppConfig c = load_config(path);
    CHECK(c.metrics_port == 9200, "numeric value parses past an inline comment");
    CHECK(c.from_beginning == true,
          "boolean parses past an inline comment (was silently false before)");
    CHECK(c.queue_capacity == 1024, "queue size rounded up to a power of two");
    CHECK(c.snapshot_interval_s == 7, "snapshot interval read");
    CHECK(c.sessions.size() == 1, "one session block");
    CHECK(c.sessions[0].session_id() == "FIX.4.2:ME->YOU", "session id composed");
    CHECK(c.sessions[0].heartbeat_interval == 45, "heartbeat interval read");
    CHECK(c.sessions[0].event_log_path.empty(), "omitted event log stays empty");
    std::remove(path);
}

static void test_store() {
    std::cout << "\n[event store]\n";
    const char* path = "/tmp/fixmon_selftest.db";
    std::remove(path);
    std::remove("/tmp/fixmon_selftest.db-wal");
    std::remove("/tmp/fixmon_selftest.db-shm");

    EventStore store;
    std::string err;
    CHECK(store.open(path, err), "store opens");

    for (int i = 0; i < 250; ++i) {
        Event e;
        e.event_class  = EventClass::FixMessage;
        e.source       = Source::MessageLog;
        e.session_id   = "FIX.4.4:BROKER1->VENUEX";
        e.direction    = Direction::Incoming;
        e.msg_type     = "8";
        e.msg_seq_num  = static_cast<uint64_t>(i + 1);
        e.engine_ts_ns = now_ns();
        e.ingest_ts_ns = e.engine_ts_ns;
        e.raw          = "8=FIX.4.4|35=8|";
        store.stage(e);
    }
    Event se;
    se.event_class   = EventClass::SessionEvent;
    se.source        = Source::EventLog;
    se.session_id    = "FIX.4.4:BROKER1->VENUEX";
    se.session_event = SessionEventType::Disconnected;
    se.text          = "Socket exception";
    se.engine_ts_ns  = now_ns();
    se.ingest_ts_ns  = se.engine_ts_ns;
    store.stage(se);

    size_t n = store.flush();
    CHECK(n == 251, "batch committed in one transaction");
    CHECK(store.write_errors() == 0, "no write errors");

    SessionSnapshot snap;
    snap.session_id = "FIX.4.4:BROKER1->VENUEX";
    snap.state      = LinkState::LoggedOn;
    snap.msgs_in    = 250;
    store.write_snapshot(snap, now_ns());
    CHECK(store.write_errors() == 0, "snapshot written");

    store.close();
    std::cout << "  (db left at " << path << " for inspection)\n";
}

int main() {
    std::cout << "fixmon selftest\n===============\n";
    test_timestamps();
    test_fix_fields();
    test_message_adapter();
    test_event_adapter();
    test_session_state();
    test_logon_reject_flow();
    test_staleness();
    test_queue();
    test_metrics();
    test_config();
    test_store();

    std::cout << "\n";
    if (g_failures) {
        std::cout << g_failures << " FAILURE(S)\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
