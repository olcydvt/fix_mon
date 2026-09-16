#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fixmon {

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
