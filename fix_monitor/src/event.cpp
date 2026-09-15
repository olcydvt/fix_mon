#include "fixmon/event.hpp"

namespace fixmon {

const char* to_string(Source s) {
    switch (s) {
        case Source::MessageLog: return "message_log";
        case Source::EventLog:   return "event_log";
        case Source::Pcap:       return "pcap";
        case Source::EngineHook: return "engine_hook";
        default:                 return "unknown";
    }
}

const char* to_string(EventClass c) {
    switch (c) {
        case EventClass::FixMessage:   return "fix_message";
        case EventClass::SessionEvent: return "session_event";
        default:                       return "unknown";
    }
}

const char* to_string(Direction d) {
    switch (d) {
        case Direction::Incoming: return "in";
        case Direction::Outgoing: return "out";
        default:                  return "unknown";
    }
}

const char* to_string(SessionEventType t) {
    switch (t) {
        case SessionEventType::SessionCreated:   return "session_created";
        case SessionEventType::Connecting:       return "connecting";
        case SessionEventType::ConnectFailed:    return "connect_failed";
        case SessionEventType::LogonSent:        return "logon_sent";
        case SessionEventType::LogonReceived:    return "logon_received";
        case SessionEventType::LogonRejected:    return "logon_rejected";
        case SessionEventType::LogoutSent:       return "logout_sent";
        case SessionEventType::LogoutReceived:   return "logout_received";
        case SessionEventType::Disconnected:     return "disconnected";
        case SessionEventType::SeqNumTooLow:     return "seq_num_too_low";
        case SessionEventType::SeqNumTooHigh:    return "seq_num_too_high";
        case SessionEventType::ResendRequested:  return "resend_requested";
        case SessionEventType::SequenceReset:    return "sequence_reset";
        case SessionEventType::HeartbeatTimeout: return "heartbeat_timeout";
        case SessionEventType::TestRequestSent:  return "test_request_sent";
        case SessionEventType::ReconnectAttempt: return "reconnect_attempt";
        case SessionEventType::SessionScheduled: return "session_scheduled";
        case SessionEventType::ConfigError:      return "config_error";
        case SessionEventType::Unparsed:         return "unparsed";
        default:                                 return "none";
    }
}

}  // namespace fixmon
