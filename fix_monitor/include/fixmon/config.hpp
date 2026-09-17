#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fixmon {

// A QuickFIX Y/N setting, or the fact that we never saw one.
//
// Three states rather than a bool because "the operator set it to N" and "we
// have no idea" lead to opposite advice, and a monitor that cannot tell them
// apart will give the wrong one with the same confidence as the right one. A
// session described only in fixmon.ini has no engine config behind it, so
// Unknown is the honest answer for all of these.
enum class TriState { Unknown, No, Yes };

const char* to_string(TriState v);

// Y/N as QuickFIX writes it. true/false and 1/0 are also accepted because
// deployments use them. Anything else, including an empty value, is Unknown -
// guessing here would be indistinguishable from knowing.
TriState parse_tristate(std::string_view v);

struct SessionConfig {
    std::string begin_string;      // FIX.4.2 / FIX.4.4 / FIXT.1.1
    std::string sender_comp_id;    // our side
    std::string target_comp_id;    // counterparty
    std::string message_log_path;
    std::string event_log_path;
    int         heartbeat_interval = 30;  // seconds, from the session config

    // Set when a [session] block stated a heartbeat explicitly. Without it we
    // cannot tell "30 because the operator said so" from "30 because that is
    // the default", and a HeartBtInt imported from the engine config should
    // only win over the latter.
    bool heartbeat_explicit = false;

    // ---- QuickFIX session identity beyond the comp ids ----
    // These take part in the engine's log file naming, which is exactly why we
    // carry them: they are needed to find the right file, nothing else.
    std::string session_qualifier;
    std::string sender_sub_id;
    std::string sender_location_id;
    std::string target_sub_id;
    std::string target_location_id;

    // ---- session properties imported from a QuickFIX engine config ----
    // Non-secret only. Credentials are dropped at parse time and never reach
    // this struct; see redacted_settings.
    std::string connection_type;          // initiator | acceptor
    std::string file_log_path;            // FileLogPath the logs were found under
    std::string file_store_path;          // FileStorePath, kept for context
    std::string start_time;               // StartTime, HH:MM:SS
    std::string end_time;                 // EndTime, HH:MM:SS
    int         reconnect_interval = 0;   // ReconnectInterval, seconds
    std::string origin;                   // cfg file this session came from
    bool        from_quickfix = false;

    // Names of credential settings seen in the source config. Names only - the
    // values were never stored. Lets an operator confirm they were ignored on
    // purpose rather than missed.
    std::vector<std::string> redacted_settings;

    // ---- how this session handles sequence numbers ----
    //
    // These four decide what a sequence mismatch means, and therefore what to
    // do about it. Without them any diagnosis is a guess dressed up as an
    // answer: "expected 5, received 1" is correct behaviour under
    // ResetOnLogon=Y and a lost-message incident under ResetOnLogon=N, and the
    // observable evidence is identical in both cases.
    TriState reset_on_logon      = TriState::Unknown;
    TriState reset_on_logout     = TriState::Unknown;
    TriState reset_on_disconnect = TriState::Unknown;
    TriState persist_messages    = TriState::Unknown;

    // What the reset flags add up to. Derived in one place so the console, the
    // JSON endpoint and the metric labels cannot drift into disagreeing.
    //
    //   unknown            no engine config behind this session
    //   reset_each_logon   ResetOnLogon=Y - starting from 1 again is normal
    //   reset_each_logout  ResetOnLogout=Y or ResetOnDisconnect=Y - numbers
    //                      survive a logon but not a disconnect
    //   persistent         nothing resets; numbers carry across reconnects and
    //                      a gap is a real gap
    const char* seq_reset_policy() const;

    //   unknown        PersistMessages was not in the config we read
    //   full           outgoing messages are kept and can genuinely be replayed
    //   gap_fill_only  PersistMessages=N - a ResendRequest can only ever be
    //                  answered with a SequenceReset, never the real messages,
    //                  so "ask them to resend" is advice that cannot work
    const char* resend_capability() const;

    // BeginString:Sender->Target, plus :Qualifier when the engine uses one.
    std::string session_id() const;
};

struct AppConfig {
    std::string db_path        = "fixmon.db";
    uint16_t    metrics_port   = 9109;
    size_t      queue_capacity = 65536;   // power of two
    size_t      batch_size     = 500;     // rows per sqlite transaction
    int         batch_flush_ms = 200;     // flush even if batch not full
    int         poll_interval_ms = 100;   // log tail poll
    bool        from_beginning = false;   // replay existing log content
    int         stale_after_multiple = 2; // heartbeat*N with no traffic -> stale
    int         snapshot_interval_s  = 30; // how often session state is persisted
    int         queue_full_wait_ms = 250;

    // Store client identity and commercial detail as <masked> instead of the
    // value. On by default: a monitor answers "is the link healthy", and that
    // question never needs to know the price. Credential tags are masked
    // regardless of this setting.
    bool mask_message_bodies = true;

    // Days of history kept in the event store. 0 disables the purge, which is
    // a deliberate choice an operator has to make rather than the default:
    // an unbounded log of counterparty traffic is a liability that grows.
    int retention_days = 0;

    // QuickFIX engine config files to import sessions from. Read-only; we take
    // identity, heartbeat and log paths out of them and ignore everything that
    // belongs to the engine itself.
    std::vector<std::string> quickfix_configs;

    std::vector<SessionConfig> sessions;

    // What the import resolved and what it could not. Printed at startup so
    // discovery is never silent.
    std::vector<std::string> notes;
};

// Throws std::runtime_error on a malformed file.
// extra_quickfix_configs come from the command line and are imported alongside
// any quickfix_config keys found in the ini.
AppConfig load_config(const std::string& path,
                      const std::vector<std::string>& extra_quickfix_configs = {});

// Collector defaults plus every session taken from the engine's own config.
// The ini becomes optional once the engine config already describes the
// sessions.
AppConfig config_from_quickfix(const std::vector<std::string>& quickfix_configs);

}  // namespace fixmon
