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

    // BeginString:Sender->Target
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

    std::vector<SessionConfig> sessions;
};

// Throws std::runtime_error on a malformed file.
AppConfig load_config(const std::string& path);

}  // namespace fixmon
