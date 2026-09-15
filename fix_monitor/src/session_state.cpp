#include "fixmon/session_state.hpp"

namespace fixmon {

const char* to_string(LinkState s) {
    switch (s) {
        case LinkState::Disconnected: return "disconnected";
        case LinkState::Connecting:   return "connecting";
        case LinkState::LogonPending: return "logon_pending";
        case LinkState::LoggedOn:     return "logged_on";
        case LinkState::Stale:        return "stale";
        default:                      return "unknown";
    }
}

SessionRegistry::SessionRegistry(int stale_after_multiple)
    : stale_multiple_(stale_after_multiple < 1 ? 2 : stale_after_multiple) {}

void SessionRegistry::register_session(const std::string& id, int hb) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& s = sessions_[id];
    s.session_id         = id;
    s.heartbeat_interval = hb > 0 ? hb : 30;
}

SessionSnapshot& SessionRegistry::get(const std::string& id) {
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        auto& s = sessions_[id];
        s.session_id = id;
        return s;
    }
    return it->second;
}

uint64_t SessionRegistry::apply(const Event& ev) {
    std::lock_guard<std::mutex> lock(mutex_);
    SessionSnapshot& s = get(ev.session_id);

    int64_t ts = ev.engine_ts_ns != 0 ? ev.engine_ts_ns : ev.ingest_ts_ns;
    if (ts > s.last_event_ts_ns) s.last_event_ts_ns = ts;

    uint64_t gap = 0;

    if (ev.event_class == EventClass::FixMessage) {
        if (ev.direction == Direction::Incoming) {
            ++s.msgs_in;
            s.last_incoming_ts_ns = ts;

            // Inbound gap detection straight off the message stream. This works
            // even when the engine did not narrate it, and cross-checks the
            // event log when it did.
            if (!ev.poss_dup && ev.msg_seq_num > 0) {
                if (s.next_expected_in != 0 && ev.msg_seq_num > s.next_expected_in) {
                    gap = ev.msg_seq_num - s.next_expected_in;
                    s.seq_gaps += 1;
                }
                s.next_expected_in = ev.msg_seq_num + 1;
            }
        } else if (ev.direction == Direction::Outgoing) {
            ++s.msgs_out;
            s.last_outgoing_ts_ns = ts;
            if (ev.msg_seq_num > s.last_outgoing_seq) s.last_outgoing_seq = ev.msg_seq_num;
        }

        // Session-layer transitions visible in the message stream itself.
        if (ev.msg_type == "A") {
            if (ev.direction == Direction::Incoming) {
                s.state       = LinkState::LoggedOn;
                s.logon_ts_ns = ts;
            } else if (s.state != LinkState::LoggedOn) {
                s.state = LinkState::LogonPending;
            }
        } else if (ev.msg_type == "5") {
            s.state = LinkState::Disconnected;
        } else if (ev.msg_type == "3") {
            ++s.session_rejects;
            if (!ev.text.empty()) s.last_reject_text = ev.text;
        } else if (ev.msg_type == "j") {
            ++s.business_rejects;
            if (!ev.text.empty()) s.last_reject_text = ev.text;
        } else if (ev.msg_type == "2") {
            if (ev.direction == Direction::Outgoing) ++s.resend_requests;
        } else if (ev.msg_type == "8" && ev.ord_status == "8") {
            ++s.exec_rejects;
            if (!ev.text.empty()) s.last_reject_text = ev.text;
        } else if (ev.msg_type == "9") {
            ++s.exec_rejects;
            if (!ev.text.empty()) s.last_reject_text = ev.text;
        }

        return gap;
    }

    // ---- session events ----
    switch (ev.session_event) {
        case SessionEventType::Connecting:
            s.state = LinkState::Connecting;
            break;
        case SessionEventType::ConnectFailed:
            s.state = LinkState::Disconnected;
            if (!ev.text.empty()) s.last_disconnect_reason = ev.text;
            break;
        case SessionEventType::LogonSent:
            s.state = LinkState::LogonPending;
            break;
        case SessionEventType::LogonReceived:
            s.state       = LinkState::LoggedOn;
            s.logon_ts_ns = ts;
            break;
        case SessionEventType::LogonRejected:
            ++s.logon_rejects;
            s.state = LinkState::Disconnected;
            if (!ev.text.empty()) s.last_disconnect_reason = ev.text;
            break;
        case SessionEventType::LogoutSent:
        case SessionEventType::LogoutReceived:
        case SessionEventType::Disconnected:
            ++s.disconnects;
            s.state = LinkState::Disconnected;
            if (!ev.text.empty()) s.last_disconnect_reason = ev.text;
            break;
        case SessionEventType::SeqNumTooLow:
            ++s.seq_too_low;
            break;
        case SessionEventType::SeqNumTooHigh:
            // Counted separately from the derived gap. If the two ever
            // disagree, that disagreement is itself a finding.
            ++s.seq_gaps_reported;
            if (ev.expected_seq_num && ev.received_seq_num > ev.expected_seq_num) {
                gap = ev.received_seq_num - ev.expected_seq_num;
            }
            break;
        case SessionEventType::ResendRequested:
            ++s.resend_requests;
            break;
        case SessionEventType::HeartbeatTimeout:
            ++s.heartbeat_timeouts;
            break;
        case SessionEventType::ReconnectAttempt:
            ++s.reconnect_attempts;
            s.state = LinkState::Connecting;
            break;
        default:
            break;
    }
    return gap;
}

void SessionRegistry::evaluate_staleness(int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, s] : sessions_) {
        if (s.state != LinkState::LoggedOn && s.state != LinkState::Stale) continue;

        int64_t tolerance =
            static_cast<int64_t>(s.heartbeat_interval) * stale_multiple_ * 1000000000LL;
        int64_t last = s.last_incoming_ts_ns != 0 ? s.last_incoming_ts_ns : s.last_event_ts_ns;
        if (last == 0) continue;

        if (now - last > tolerance) {
            s.state = LinkState::Stale;
        } else if (s.state == LinkState::Stale) {
            s.state = LinkState::LoggedOn;
        }
    }
}

std::vector<SessionSnapshot> SessionRegistry::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SessionSnapshot> out;
    out.reserve(sessions_.size());
    for (const auto& [id, s] : sessions_) out.push_back(s);
    return out;
}

}  // namespace fixmon
