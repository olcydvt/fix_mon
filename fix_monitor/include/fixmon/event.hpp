#pragma once
//
// Normalized event schema.
//
// Every source adapter (message log, event log, and later pcap) produces this
// one struct. Downstream stages - session state, metrics, event store - only
// ever see Event, never a source-specific format. This is the seam that lets a
// pcap adapter be added later without touching anything below it.
//
#include <cstdint>
#include <string>

namespace fixmon {

// Where the event came from. Kept in the schema so the store can tell a
// log-derived timestamp from a wire-derived one.
enum class Source : uint8_t {
    Unknown    = 0,
    MessageLog = 1,
    EventLog   = 2,
    Pcap       = 3,  // reserved
    EngineHook = 4,  // reserved
};

enum class EventClass : uint8_t {
    Unknown      = 0,
    FixMessage   = 1,  // an actual FIX message off the wire / out of the log
    SessionEvent = 2,  // engine's own narration: logon, disconnect, gap, ...
};

enum class Direction : uint8_t {
    Unknown  = 0,
    Incoming = 1,
    Outgoing = 2,
};

// Session-layer event taxonomy. This is the normalized vocabulary; the event
// log adapter maps engine-specific wording onto it.
enum class SessionEventType : uint8_t {
    None              = 0,
    SessionCreated    = 1,
    Connecting        = 2,
    ConnectFailed     = 3,
    LogonSent         = 4,
    LogonReceived     = 5,
    LogonRejected     = 6,
    LogoutSent        = 7,
    LogoutReceived    = 8,
    Disconnected      = 9,
    SeqNumTooLow      = 10,  // received < expected -> real problem
    SeqNumTooHigh     = 11,  // received > expected -> gap, triggers resend
    ResendRequested   = 12,
    SequenceReset     = 13,
    HeartbeatTimeout  = 14,
    TestRequestSent   = 15,
    ReconnectAttempt  = 16,
    SessionScheduled  = 17,  // session start/end per schedule
    ConfigError       = 18,
    Unparsed          = 19,  // kept verbatim rather than dropped
};

const char* to_string(Source);
const char* to_string(EventClass);
const char* to_string(Direction);
const char* to_string(SessionEventType);

struct Event {
    EventClass  event_class = EventClass::Unknown;
    Source      source      = Source::Unknown;

    // BeginString:SenderCompID->TargetCompID
    std::string session_id;

    Direction   direction = Direction::Unknown;

    // Engine's own clock, from the log line prefix. Nanoseconds since epoch.
    int64_t engine_ts_ns = 0;
    // Wire clock. Always 0 for log sources; the pcap adapter fills it.
    int64_t capture_ts_ns = 0;
    // When this collector saw the line. Useful for measuring log write lag.
    int64_t ingest_ts_ns = 0;

    // ---- FixMessage fields ----
    std::string msg_type;          // tag 35
    uint64_t    msg_seq_num = 0;   // tag 34
    bool        poss_dup    = false;
    bool        poss_resend = false;

    // Derived from the message body, so downstream never re-parses.
    int32_t     reject_reason = -1;  // tag 373 (session) or 380 (business)
    int32_t     ref_tag_id    = -1;  // tag 371
    uint64_t    ref_seq_num   = 0;   // tag 45
    std::string ord_status;          // tag 39
    std::string text;                // tag 58 / engine reason text

    // ---- SessionEvent fields ----
    SessionEventType session_event = SessionEventType::None;
    uint64_t expected_seq_num = 0;
    uint64_t received_seq_num = 0;

    // Raw line, kept so the store has full fidelity and nothing is lost to a
    // parse rule that did not match.
    std::string raw;
};

}  // namespace fixmon
