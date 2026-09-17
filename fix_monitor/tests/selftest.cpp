#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fixmon/adapters.hpp"
#include "fixmon/config.hpp"
#include "fixmon/event_store.hpp"
#include "fixmon/fix_parser.hpp"
#include "fixmon/metrics.hpp"
#include "fixmon/mpmc_queue.hpp"
#include "fixmon/quickfix_config.hpp"
#include "fixmon/redact.hpp"
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

// Temp paths rather than a hardcoded /tmp: this suite has to run on the Windows
// side of the build too, and a test that cannot run is a test nobody trusts.
static std::filesystem::path temp_dir(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

static void write_file(const std::filesystem::path& p, const std::string& content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p);
    f << content;
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

static void test_redaction() {
    std::cout << "\n[redaction]\n";
    CHECK(is_sensitive_key("Password"), "Password is a secret");
    CHECK(is_sensitive_key("password"), "case does not matter");
    CHECK(is_sensitive_key("SocketKeyStorePassword"),
          "an SSL wrapper key we have never seen is still caught by fragment");
    CHECK(is_sensitive_key("private_key_file"), "separators folded away before matching");
    CHECK(is_sensitive_key("Username"),
          "login identity treated as a credential, not as a label");

    CHECK(!is_sensitive_key("SenderCompID"), "comp id is not a secret");
    CHECK(!is_sensitive_key("FileLogPath"), "log path is not a secret");
    CHECK(!is_sensitive_key("HeartBtInt"), "heartbeat is not a secret");
    CHECK(!is_sensitive_key("session"), "our own metric labels are not caught");
    CHECK(!is_sensitive_key("msg_type_name"), "nor the wordier ones");
    CHECK(!is_sensitive_key("source"), "nor the session_info labels");

    CHECK(redact_if_sensitive("Password", "hunter2") == std::string(kRedacted),
          "secret value replaced on the way to a human");
    CHECK(redact_if_sensitive("HeartBtInt", "30") == "30", "ordinary value passes through");

    // Engines narrate failures in prose, and that prose reaches /sessions and
    // the snapshot table verbatim unless something stops it here.
    const std::string prose = redact_free_text("Invalid password for user TRADER01");
    CHECK(prose.find("TRADER01") == std::string::npos,
          "the value following a credential word is masked");
    CHECK(prose.find("Invalid") != std::string::npos && prose.find("user") != std::string::npos,
          "the sentence stays readable - an operator still learns what failed");

    CHECK(redact_free_text("password=hunter2").find("hunter2") == std::string::npos,
          "key=value form caught as well as prose");
    CHECK(redact_free_text("Username: bob").find("bob") == std::string::npos,
          "colon form caught too");

    // Regression: two connector words between the noun and the value. A window
    // that closed on the first of them read this line as safe and let the login
    // name through to /sessions via last_disconnect_reason.
    CHECK(redact_free_text("password rejected for TRADER01").find("TRADER01")
              == std::string::npos,
          "a run of connector words does not close the credential window");
    CHECK(redact_free_text("Disconnecting: bad password supplied by user JSMITH")
              .find("JSMITH") == std::string::npos,
          "and the same holds for the wording an engine actually uses");

    // The other half of the job: not eating the diagnosis it is protecting.
    CHECK(redact_free_text("Received logout") == "Received logout",
          "ordinary prose is returned untouched");
    CHECK(redact_free_text("MsgSeqNum too low, expecting 5 but received 2")
              == "MsgSeqNum too low, expecting 5 but received 2",
          "a sequence gap message survives intact - it is the whole diagnosis");
    CHECK(redact_free_text("").empty(), "empty text does not throw");
}

static void test_body_masking() {
    std::cout << "\n[body masking]\n";

    // A Logon carries Username and Password in clear text, and the engine logs
    // it like any other message. This is what makes masking a correctness
    // requirement rather than a preference.
    std::string logon =
            "8=FIX.4.4|35=A|34=1|49=BROKER1|56=VENUEX|98=0|108=30|553=trader01|554=hunter2|10=1|";
    mask_fix_body(logon, true);
    CHECK(logon.find("hunter2") == std::string::npos, "password never survives into the body");
    CHECK(logon.find("trader01") == std::string::npos, "nor the username");
    CHECK(logon.find("554=") != std::string::npos,
          "the tag itself stays, so a reader knows the field was there");
    CHECK(logon.find("35=A") != std::string::npos, "message type untouched");
    CHECK(logon.find("34=1|") != std::string::npos, "sequence number untouched");
    CHECK(logon.find("108=30") != std::string::npos, "heartbeat interval untouched");

    // Business masking is policy, so both settings have to behave.
    const std::string order =
            "8=FIX.4.4|35=D|34=7|49=BROKER1|56=VENUEX|11=ORD-7|55=VOD.L|54=1|38=5000|44=123.45|10=9|";

    std::string masked = order;
    mask_fix_body(masked, true);
    CHECK(masked.find("ORD-7") == std::string::npos, "client order id masked");
    CHECK(masked.find("VOD.L") == std::string::npos, "symbol masked");
    CHECK(masked.find("123.45") == std::string::npos, "price masked");
    CHECK(masked.find("5000") == std::string::npos, "quantity masked");
    CHECK(masked.find(std::string("11=") + kMasked) != std::string::npos,
          "masked marker is distinct from redacted - policy, not a secret");
    CHECK(masked.find("34=7|") != std::string::npos,
          "sequence number survives: diagnosis runs on it");
    CHECK(masked.find("49=BROKER1") != std::string::npos,
          "comp ids survive: they are how a session is identified at all");

    std::string kept = order;
    mask_fix_body(kept, false);
    CHECK(kept == order, "business masking off leaves an ordinary order untouched");

    // ...but a credential is never a policy choice.
    std::string creds_only = "8=FIX.4.4|35=A|554=hunter2|11=ORD-7|";
    mask_fix_body(creds_only, false);
    CHECK(creds_only.find("hunter2") == std::string::npos,
          "credential masked even with business masking off");
    CHECK(creds_only.find("ORD-7") != std::string::npos, "business value left alone there");

    // RawData(96) declares its length and is allowed to contain SOH. Splitting
    // on the separator would copy the tail of the secret straight through.
    std::string raw = "8=FIX.4.4\x01" "35=A\x01" "95=11\x01" "96=AB\x01" "CD\x01" "EFGHI\x01"
                      "108=30\x01";
    mask_fix_body(raw, true);
    CHECK(raw.find("EFGHI") == std::string::npos,
          "a SOH inside RawData does not let the tail escape");
    CHECK(raw.find("108=30") != std::string::npos,
          "and the field after RawData is still found");

    // Degenerate input must not throw, hang, or corrupt what it cannot parse.
    std::string empty;
    mask_fix_body(empty, true);
    CHECK(empty.empty(), "empty body survives");

    std::string junk = "not a fix message at all";
    mask_fix_body(junk, true);
    CHECK(junk == "not a fix message at all", "non-FIX text passes through unchanged");

    std::string truncated = "8=FIX.4.4|554=";
    mask_fix_body(truncated, true);
    CHECK(truncated == "8=FIX.4.4|554=", "an empty credential value has nothing to mask");

    std::string no_sep = "8=FIX.4.4|58=price is 44=99 apparently|";
    mask_fix_body(no_sep, true);
    CHECK(no_sep.find("44=99") != std::string::npos,
          "an '=' inside free text does not shift the tag boundary");

    // Regression: tag 58 is neither a credential tag nor a business tag, so
    // both tiers skipped it and a password walked into the stored body while
    // the derived text column beside it was already clean.
    std::string reject = "8=FIX.4.4|35=3|34=3|45=2|58=Invalid password for user TRADER01|10=1|";
    mask_fix_body(reject, true);
    CHECK(reject.find("TRADER01") == std::string::npos,
          "free text inside the body is scrubbed, not skipped");
    CHECK(reject.find("Invalid password for user") != std::string::npos,
          "and the readable part of the reject survives");
    CHECK(reject.find("45=2") != std::string::npos, "ref seq num still there");

    std::string gap = "8=FIX.4.4|35=3|58=MsgSeqNum too low, expecting 5 but received 2|10=1|";
    const std::string gap_before = gap;
    mask_fix_body(gap, true);
    CHECK(gap == gap_before,
          "a sequence gap reject passes through whole - it is the whole diagnosis");

    // EncodedText mirrors Text in another charset. We cannot scrub what we
    // cannot decode, and it is length-prefixed, so it goes entirely.
    std::string enc = "8=FIX.4.4\x01" "35=3\x01" "354=8\x01" "355=pw\x01" "12345\x01" "34=9\x01";
    mask_fix_body(enc, true);
    CHECK(enc.find("12345") == std::string::npos,
          "encoded text is dropped whole, including past an embedded SOH");
    CHECK(enc.find("34=9") != std::string::npos, "and the field after it is still found");
}

// Masking has to happen where the Event is built, not at query time: a value
// that never enters the Event cannot leak from the store or the HTTP surface.
static void test_adapter_masks_on_ingest() {
    std::cout << "\n[masking on ingest]\n";

    MessageLogAdapter on(make_cfg(), true, 100, true);
    Event ev;
    CHECK(on.parse_line(
              "20240115-09:30:00.100 : 8=FIX.4.4|9=70|35=A|34=1|49=BROKER1|56=VENUEX|"
              "553=trader01|554=hunter2|98=0|108=30|10=1|",
              ev),
          "logon with credentials parses");
    CHECK(ev.raw.find("hunter2") == std::string::npos,
          "the password is gone before the event leaves the adapter");
    CHECK(ev.msg_type == "A" && ev.msg_seq_num == 1,
          "the fields diagnosis needs were lifted out before masking");

    MessageLogAdapter off(make_cfg(), true, 100, false);
    Event ev2;
    CHECK(off.parse_line(
              "20240115-09:30:00.200 : 8=FIX.4.4|9=70|35=D|34=2|49=BROKER1|56=VENUEX|"
              "11=ORD-7|554=hunter2|10=1|",
              ev2),
          "order parses with business masking off");
    CHECK(ev2.raw.find("hunter2") == std::string::npos,
          "credentials still masked when body masking is off");
    CHECK(ev2.raw.find("ORD-7") != std::string::npos,
          "business detail retained when the operator asked for it");

    Event ev3;
    CHECK(off.parse_line(
              "20240115-09:30:00.300 : 8=FIX.4.4|35=3|34=3|49=VENUEX|56=BROKER1|"
              "58=Invalid password for user TRADER01|10=1|",
              ev3),
          "reject with prose in tag 58 parses");
    CHECK(ev3.text.find("TRADER01") == std::string::npos,
          "tag 58 is free text and gets the same scrub as an engine log line");
}

static void test_metric_label_guard() {
    std::cout << "\n[metric label guard]\n";
    MetricRegistry reg;
    declare_all_metrics(reg);

    CHECK(reg.gauge("fixmon_session_info", {{"session", "s"}, {"password", "hunter2"}}) == nullptr,
          "a series labelled with a credential is refused outright");
    CHECK(reg.redacted_series() == 1, "the refusal is counted, not silently ignored");
    CHECK(reg.expose().find("hunter2") == std::string::npos,
          "the value never reaches the exposition format Prometheus scrapes");
    CHECK(reg.counter("fixmon_messages_total", {{"session", "s"}, {"Username", "bob"}}) == nullptr,
          "counters are guarded by the same rule as gauges");
    CHECK(reg.gauge("fixmon_session_info", {{"session", "s"}, {"begin_string", "FIX.4.4"}}) != nullptr,
          "ordinary labels still work");
    CHECK(reg.gauge("fixmon_session_info",
                    {{"session", "s2"},
                     {"seq_reset_policy", "persistent"},
                     {"resend_capability", "gap_fill_only"}}) != nullptr,
          "the sequence policy labels are not mistaken for credentials");
}

static void test_config() {
    std::cout << "\n[config]\n";
    const std::filesystem::path path = temp_dir("fixmon_cfg_test.ini");
    write_file(path,
               "[global]\n"
               "db_path        = ./x.db\n"
               "metrics_port   = 9200      # inline comment after a number\n"
               "from_beginning = true      # inline comment after a bool\n"
               "queue_size     = 1000      ; semicolon comment\n"
               "snapshot_interval_s = 7\n"
               "\n[session]\n"
               "begin_string   = FIX.4.2\n"
               "sender_comp_id = ME\n"
               "target_comp_id = YOU\n"
               "message_log    = m.log\n"
               "heartbeat_interval = 45\n");

    AppConfig c = load_config(path.string());
    CHECK(c.metrics_port == 9200, "numeric value parses past an inline comment");
    CHECK(c.from_beginning == true,
          "boolean parses past an inline comment (was silently false before)");
    CHECK(c.queue_capacity == 1024, "queue size rounded up to a power of two");
    CHECK(c.snapshot_interval_s == 7, "snapshot interval read");
    CHECK(c.sessions.size() == 1, "one session block");
    CHECK(c.sessions[0].session_id() == "FIX.4.2:ME->YOU", "session id composed");
    CHECK(c.sessions[0].heartbeat_interval == 45, "heartbeat interval read");
    CHECK(c.sessions[0].heartbeat_explicit, "heartbeat marked as explicitly set");
    CHECK(c.sessions[0].event_log_path.empty(), "omitted event log stays empty");
    std::filesystem::remove(path);
}

static void test_quickfix_settings() {
    std::cout << "\n[quickfix settings]\n";
    std::istringstream cfg(
        "# the engine's own file, read-only\n"
        "[DEFAULT]\n"
        "ConnectionType=initiator\n"
        "FileLogPath=/var/log/quickfix\n"
        "FileStorePath=/var/quickfix/store\n"
        "HeartBtInt=45\n"
        "SenderCompID=BROKER1\n"
        "StartTime=00:00:00\n"
        "EndTime=23:59:59\n"
        "Password=hunter2\n"
        "\n"
        "[SESSION]\n"
        "BeginString=FIX.4.4\n"
        "TargetCompID=VENUEX\n"
        "socketconnecthost=10.0.0.1\n"
        "\n"
        "[SESSION]\n"
        "BeginString=FIX.4.2\n"
        "TargetCompID=VENUEY\n"
        "HeartBtInt=60\n"
        "SSLPrivateKeyPassword=abc\n");

    QuickFixSettings s = parse_quickfix_settings(cfg, "test.cfg");
    CHECK(s.sessions.size() == 2, "two [SESSION] blocks read");
    CHECK(s.sessions[0].get("SenderCompID") == "BROKER1", "[DEFAULT] inherited by the session");
    CHECK(s.sessions[0].get_int("HeartBtInt", 0) == 45, "HeartBtInt inherited from [DEFAULT]");
    CHECK(s.sessions[1].get_int("HeartBtInt", 0) == 60, "session block overrides the default");
    CHECK(s.sessions[0].get("SOCKETCONNECTHOST") == "10.0.0.1",
          "keys compare case-insensitively - a case mismatch silently losing "
          "FileLogPath would be the worst failure this reader could have");

    CHECK(!s.sessions[0].has("Password"), "password never stored");
    CHECK(s.defaults.find("Password") == s.defaults.end(), "not kept in the defaults either");

    auto named = [](const std::vector<std::string>& v, const std::string& k) {
        return std::find(v.begin(), v.end(), k) != v.end();
    };
    CHECK(named(s.sessions[0].redacted_keys, "Password"),
          "credential recorded by name, so an operator sees it was ignored on purpose");
    CHECK(s.sessions[1].redacted_keys.size() == 2,
          "session-level credential added to the one inherited from [DEFAULT]");
}

// The four settings that decide what a sequence gap means. Everything here is
// about not answering when we were not told, because a confident wrong answer
// is the failure mode this whole feature exists to prevent.
static void test_sequence_settings() {
    std::cout << "\n[sequence settings]\n";

    CHECK(parse_tristate("Y") == TriState::Yes, "Y as QuickFIX writes it");
    CHECK(parse_tristate("N") == TriState::No, "N likewise");
    CHECK(parse_tristate("y") == TriState::Yes, "lower case accepted");
    CHECK(parse_tristate("1") == TriState::Yes && parse_tristate("0") == TriState::No,
          "1/0 accepted - deployments use them");
    CHECK(parse_tristate("true") == TriState::Yes && parse_tristate("false") == TriState::No,
          "true/false accepted too");
    CHECK(parse_tristate("") == TriState::Unknown, "an empty value is not a No");
    CHECK(parse_tristate("maybe") == TriState::Unknown, "and neither is junk");

    // ---- read from the engine's own file ----
    std::istringstream cfg(
        "[DEFAULT]\n"
        "ConnectionType=initiator\n"
        "SenderCompID=BROKER1\n"
        "FileLogPath=/nonexistent\n"
        "ResetOnLogon=Y\n"
        "PersistMessages=N\n"
        "[SESSION]\n"
        "BeginString=FIX.4.4\nTargetCompID=VENUEX\n"
        "[SESSION]\n"
        "BeginString=FIX.4.4\nTargetCompID=VENUEY\n"
        "ResetOnLogon=N\nResetOnLogout=N\nResetOnDisconnect=N\nPersistMessages=Y\n");

    QuickFixSettings s = parse_quickfix_settings(cfg, "seq.cfg");
    CHECK(s.sessions[0].get_bool("ResetOnLogon") == TriState::Yes,
          "ResetOnLogon inherited from [DEFAULT]");
    CHECK(s.sessions[1].get_bool("ResetOnLogon") == TriState::No,
          "the session block overrides it");
    CHECK(s.sessions[0].get_bool("ResetOnLogout") == TriState::Unknown,
          "a setting nobody wrote down stays unknown rather than defaulting to N");

    std::vector<SessionConfig> derived = sessions_from_quickfix(s, nullptr);
    CHECK(derived.size() == 2, "both sessions derived");
    CHECK(derived[0].reset_on_logon == TriState::Yes, "flag reaches SessionConfig");
    CHECK(derived[0].persist_messages == TriState::No, "and so does PersistMessages");

    // ---- what the flags add up to ----
    CHECK(std::string(derived[0].seq_reset_policy()) == "reset_each_logon",
          "ResetOnLogon=Y dominates: starting at 1 again is expected here");
    CHECK(std::string(derived[0].resend_capability()) == "gap_fill_only",
          "PersistMessages=N means a resend request can never be honoured");
    CHECK(std::string(derived[1].seq_reset_policy()) == "persistent",
          "all three reset flags off means a gap is a real gap");
    CHECK(std::string(derived[1].resend_capability()) == "full",
          "and this one can actually replay");

    SessionConfig bare;
    CHECK(std::string(bare.seq_reset_policy()) == "unknown",
          "a session with no engine config behind it says so");
    CHECK(std::string(bare.resend_capability()) == "unknown",
          "rather than claiming the QuickFIX default as fact");

    SessionConfig partial;
    partial.reset_on_logon = TriState::No;
    CHECK(std::string(partial.seq_reset_policy()) == "unknown",
          "one flag known and two missing is still not enough to claim persistence");

    SessionConfig on_logout;
    on_logout.reset_on_logon      = TriState::No;
    on_logout.reset_on_disconnect = TriState::Yes;
    CHECK(std::string(on_logout.seq_reset_policy()) == "reset_each_logout",
          "ResetOnDisconnect=Y is reported even when ResetOnLogon is off");
}

static void test_sequence_settings_merge() {
    std::cout << "\n[sequence settings merge]\n";
    namespace fs = std::filesystem;
    const fs::path root = temp_dir("fixmon_seq_merge");
    fs::remove_all(root);

    write_file(root / "logs" / "FIX.4.4-BROKER1-VENUEX.messages.current.log", "");
    write_file(root / "logs" / "FIX.4.4-BROKER1-VENUEY.messages.current.log", "");

    write_file(root / "engine.cfg",
               "[DEFAULT]\n"
               "ConnectionType=initiator\n"
               "SenderCompID=BROKER1\n"
               "FileLogPath=logs\n"
               "ResetOnLogon=Y\n"
               "ResetOnLogout=N\n"
               "ResetOnDisconnect=N\n"
               "PersistMessages=Y\n"
               "[SESSION]\nBeginString=FIX.4.4\nTargetCompID=VENUEX\n"
               "[SESSION]\nBeginString=FIX.4.4\nTargetCompID=VENUEY\n");

    // VENUEX corrects the engine; VENUEY says nothing and should inherit.
    write_file(root / "fixmon.ini",
               "[global]\n"
               "quickfix_config = engine.cfg\n"
               "[session]\n"
               "begin_string   = FIX.4.4\n"
               "sender_comp_id = BROKER1\n"
               "target_comp_id = VENUEX\n"
               "reset_on_logon = N\n");

    AppConfig c = load_config((root / "fixmon.ini").string());
    auto find_session = [&](const std::string& id) -> const SessionConfig* {
        for (const auto& s : c.sessions) {
            if (s.session_id() == id) return &s;
        }
        return nullptr;
    };
    const SessionConfig* x = find_session("FIX.4.4:BROKER1->VENUEX");
    const SessionConfig* y = find_session("FIX.4.4:BROKER1->VENUEY");
    CHECK(x != nullptr && y != nullptr, "both sessions present");

    CHECK(x && x->reset_on_logon == TriState::No,
          "an explicit ini value wins - how an operator corrects an engine cfg they cannot edit");
    CHECK(x && x->persist_messages == TriState::Yes,
          "the settings the ini stayed quiet about still come from the engine");
    CHECK(x && std::string(x->seq_reset_policy()) == "persistent",
          "and the derived policy follows the corrected flag, not the engine's");
    CHECK(y && y->reset_on_logon == TriState::Yes,
          "a session with no override takes the engine's answer");
    CHECK(y && std::string(y->seq_reset_policy()) == "reset_each_logon",
          "two sessions off one engine cfg can legitimately disagree");

    fs::remove_all(root);
}

static void test_quickfix_discovery() {
    std::cout << "\n[quickfix discovery]\n";
    namespace fs = std::filesystem;
    const fs::path root = temp_dir("fixmon_qf_discovery");
    fs::remove_all(root);

    // QuickFIX C++ naming for one session...
    write_file(root / "enginelogs" / "FIX.4.4-BROKER1-VENUEX.messages.current.log", "");
    write_file(root / "enginelogs" / "FIX.4.4-BROKER1-VENUEX.event.current.log", "");
    // ...and the trimmed layout some deployments use, for another.
    write_file(root / "enginelogs" / "BROKER1-VENUEY.messages.log", "");

    write_file(root / "engine.cfg",
               "[DEFAULT]\n"
               "ConnectionType=initiator\n"
               "SenderCompID=BROKER1\n"
               "FileLogPath=enginelogs\n"
               "HeartBtInt=45\n"
               "StartTime=08:00:00\n"
               "Password=hunter2\n"
               "[SESSION]\n"
               "BeginString=FIX.4.4\n"
               "TargetCompID=VENUEX\n"
               "[SESSION]\n"
               "BeginString=FIX.4.2\n"
               "TargetCompID=VENUEY\n");

    QuickFixSettings settings = load_quickfix_settings((root / "engine.cfg").string());
    std::vector<std::string> notes;
    std::vector<SessionConfig> sessions = sessions_from_quickfix(settings, &notes);

    CHECK(sessions.size() == 2, "both sessions derived from the engine config alone");
    CHECK(sessions[0].session_id() == "FIX.4.4:BROKER1->VENUEX", "session id built from the cfg");
    CHECK(sessions[0].heartbeat_interval == 45, "HeartBtInt becomes our heartbeat_interval");
    CHECK(sessions[0].connection_type == "initiator", "connection type carried over");
    CHECK(sessions[0].start_time == "08:00:00", "schedule carried over");
    CHECK(sessions[0].message_log_path.find("FIX.4.4-BROKER1-VENUEX.messages.current.log") !=
              std::string::npos,
          "QuickFIX C++ log name resolved under a relative FileLogPath");
    CHECK(sessions[0].event_log_path.find("FIX.4.4-BROKER1-VENUEX.event.current.log") !=
              std::string::npos,
          "matching event log resolved");

    CHECK(sessions[1].message_log_path.find("BROKER1-VENUEY.messages.log") != std::string::npos,
          "the trimmed naming layout is found too");
    CHECK(sessions[1].event_log_path.find("BROKER1-VENUEY.event.log") != std::string::npos,
          "the missing half of the pair is derived from the half that exists, so "
          "the tailer picks it up the moment the engine creates it");

    CHECK(!sessions[0].redacted_settings.empty(), "credentials reported by name");
    CHECK(sessions[0].from_quickfix, "session marked as imported");
    CHECK(sessions[0].origin.find("engine.cfg") != std::string::npos, "origin recorded");

    // Nothing in the derived config may carry the secret anywhere.
    bool leaked = false;
    for (const auto& sc : sessions) {
        for (const auto& key : sc.redacted_settings) {
            leaked |= key.find("hunter2") != std::string::npos;
        }
        leaked |= sc.message_log_path.find("hunter2") != std::string::npos;
        leaked |= sc.file_log_path.find("hunter2") != std::string::npos;
    }
    CHECK(!leaked, "the password value exists nowhere in the imported session config");

    fs::remove_all(root);
}

static void test_quickfix_merge() {
    std::cout << "\n[quickfix merge with fixmon.ini]\n";
    namespace fs = std::filesystem;
    const fs::path root = temp_dir("fixmon_qf_merge");
    fs::remove_all(root);

    write_file(root / "enginelogs" / "FIX.4.4-BROKER1-VENUEX.messages.current.log", "");
    write_file(root / "enginelogs" / "FIX.4.4-BROKER1-VENUEX.event.current.log", "");
    write_file(root / "enginelogs" / "FIX.4.4-BROKER1-VENUEZ.messages.current.log", "");
    write_file(root / "enginelogs" / "FIX.4.4-BROKER1-VENUEZ.event.current.log", "");

    write_file(root / "engine.cfg",
               "[DEFAULT]\n"
               "ConnectionType=acceptor\n"
               "SenderCompID=BROKER1\n"
               "FileLogPath=enginelogs\n"
               "HeartBtInt=45\n"
               "[SESSION]\nBeginString=FIX.4.4\nTargetCompID=VENUEX\n"
               "[SESSION]\nBeginString=FIX.4.4\nTargetCompID=VENUEZ\n");

    write_file(root / "fixmon.ini",
               "[global]\n"
               "quickfix_config = engine.cfg\n"
               "[session]\n"
               "begin_string   = FIX.4.4\n"
               "sender_comp_id = BROKER1\n"
               "target_comp_id = VENUEX\n"
               "heartbeat_interval = 10\n");

    AppConfig c = load_config((root / "fixmon.ini").string());
    CHECK(c.sessions.size() == 2,
          "the session the operator listed plus the one only the engine knew about");

    auto find_session = [&](const std::string& id) -> const SessionConfig* {
        for (const auto& s : c.sessions) {
            if (s.session_id() == id) return &s;
        }
        return nullptr;
    };
    const SessionConfig* x = find_session("FIX.4.4:BROKER1->VENUEX");
    const SessionConfig* z = find_session("FIX.4.4:BROKER1->VENUEZ");

    CHECK(x != nullptr && z != nullptr, "both sessions present after the merge");
    CHECK(x && x->heartbeat_interval == 10,
          "an explicit heartbeat wins over HeartBtInt from the engine");
    CHECK(z && z->heartbeat_interval == 45,
          "a session with nothing explicit takes HeartBtInt from the engine");
    CHECK(x && !x->message_log_path.empty(),
          "log path the operator never wrote down, filled in from the engine cfg");
    CHECK(x && x->connection_type == "acceptor", "connection type filled in as well");
    CHECK(!c.notes.empty(), "the import says out loud what it did");

    fs::remove_all(root);
}

static void test_quickfix_wildcard() {
    std::cout << "\n[quickfix wildcard acceptor]\n";
    namespace fs = std::filesystem;
    const fs::path root = temp_dir("fixmon_qf_wildcard");
    fs::remove_all(root);

    write_file(root / "enginelogs" / "FIX.4.4-BROKER1-CLIENTA.messages.current.log", "");
    write_file(root / "enginelogs" / "FIX.4.4-BROKER1-CLIENTB.messages.current.log", "");

    write_file(root / "engine.cfg",
               "[DEFAULT]\n"
               "ConnectionType=acceptor\n"
               "SenderCompID=BROKER1\n"
               "FileLogPath=enginelogs\n"
               "[SESSION]\n"
               "BeginString=FIX.4.4\n"
               "TargetCompID=*\n");

    QuickFixSettings settings = load_quickfix_settings((root / "engine.cfg").string());
    std::vector<std::string> notes;
    std::vector<SessionConfig> sessions = sessions_from_quickfix(settings, &notes);

    CHECK(sessions.size() == 2,
          "a wildcard acceptor expands to the counterparties the engine actually logged");
    bool a = false, b = false;
    for (const auto& s : sessions) {
        a |= s.session_id() == "FIX.4.4:BROKER1->CLIENTA";
        b |= s.session_id() == "FIX.4.4:BROKER1->CLIENTB";
    }
    CHECK(a && b, "both discovered comp ids became real sessions");

    fs::remove_all(root);
}

static void test_store() {
    std::cout << "\n[event store]\n";
    const std::filesystem::path path = temp_dir("fixmon_selftest.db");
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-wal");
    std::filesystem::remove(path.string() + "-shm");

    EventStore store;
    std::string err;
    CHECK(store.open(path.string(), err), "store opens");

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

    // ---- retention ----
    // Everything above is stamped now, so a cutoff of thirty days ago must
    // leave all of it alone and take only what we deliberately backdate.
    const int64_t day_ns = 86400LL * 1000000000LL;
    const int64_t now    = now_ns();

    Event old_ev;
    old_ev.event_class  = EventClass::FixMessage;
    old_ev.source       = Source::MessageLog;
    old_ev.session_id   = "FIX.4.4:BROKER1->VENUEX";
    old_ev.msg_type     = "0";
    old_ev.engine_ts_ns = now - 400 * day_ns;
    old_ev.ingest_ts_ns = old_ev.engine_ts_ns;
    store.stage(old_ev);
    store.flush();

    SessionSnapshot old_snap;
    old_snap.session_id = "FIX.4.4:BROKER1->VENUEX";
    old_snap.state      = LinkState::Disconnected;
    store.write_snapshot(old_snap, now - 400 * day_ns);

    std::string perr;
    const size_t purged = store.purge_before(now - 30 * day_ns, perr);
    CHECK(perr.empty(), "purge reports no sqlite error");
    CHECK(purged == 2, "the aged event and the aged snapshot both go");
    CHECK(store.rows_purged() == 2, "purge count exposed for the metric to mirror");

    const size_t again = store.purge_before(now - 30 * day_ns, perr);
    CHECK(again == 0, "a second pass finds nothing - recent rows are untouched");
    CHECK(store.write_errors() == 0, "purging is not a write error");

    store.close();
    std::cout << "  (db left at " << path.string() << " for inspection)\n";
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
    test_redaction();
    test_body_masking();
    test_adapter_masks_on_ingest();
    test_metric_label_guard();
    test_config();
    test_quickfix_settings();
    test_sequence_settings();
    test_sequence_settings_merge();
    test_quickfix_discovery();
    test_quickfix_merge();
    test_quickfix_wildcard();
    test_store();

    std::cout << "\n";
    if (g_failures) {
        std::cout << g_failures << " FAILURE(S)\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
