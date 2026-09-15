#pragma once
//
// Per-session state, correlated across both adapters.
//
// The message log tells us traffic stopped. The event log tells us why. This
// class is where those two streams meet on session_id + time, which is exactly
// the correlation an on-call engineer needs and neither log gives alone.
//
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "fixmon/event.hpp"

namespace fixmon {

enum class LinkState : uint8_t {
    Unknown       = 0,
    Disconnected  = 1,
    Connecting    = 2,
    LogonPending  = 3,
    LoggedOn      = 4,
    Stale         = 5,  // logged on but silent past heartbeat tolerance
};

const char* to_string(LinkState);

struct SessionSnapshot {
    std::string session_id;
    LinkState   state = LinkState::Unknown;

    int64_t last_event_ts_ns    = 0;
    int64_t last_incoming_ts_ns = 0;
    int64_t last_outgoing_ts_ns = 0;
    int64_t logon_ts_ns         = 0;

    uint64_t next_expected_in  = 0;
    uint64_t last_outgoing_seq = 0;

    uint64_t msgs_in  = 0;
    uint64_t msgs_out = 0;

    uint64_t seq_gaps          = 0;  // derived from the message stream
    uint64_t seq_gaps_reported = 0;  // narrated by the engine's event log
    uint64_t seq_too_low       = 0;
    uint64_t resend_requests   = 0;
    uint64_t heartbeat_timeouts= 0;
    uint64_t disconnects       = 0;
    uint64_t reconnect_attempts= 0;
    uint64_t logon_rejects     = 0;
    uint64_t session_rejects   = 0;
    uint64_t business_rejects  = 0;
    uint64_t exec_rejects      = 0;

    std::string last_disconnect_reason;
    std::string last_reject_text;

    int heartbeat_interval = 30;
};

class SessionRegistry {
public:
    explicit SessionRegistry(int stale_after_multiple);

    void register_session(const std::string& session_id, int heartbeat_interval);

    // Applies one event. Returns the seq gap size if this event revealed an
    // inbound gap, otherwise 0. Called from the single pipeline thread.
    uint64_t apply(const Event& ev);

    // Recomputes Stale for sessions with no traffic inside the tolerance.
    void evaluate_staleness(int64_t now);

    std::vector<SessionSnapshot> snapshot() const;

private:
    SessionSnapshot& get(const std::string& session_id);

    mutable std::mutex                     mutex_;
    std::map<std::string, SessionSnapshot> sessions_;
    int                                    stale_multiple_;
};

}  // namespace fixmon
