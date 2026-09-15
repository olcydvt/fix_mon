#pragma once
//
// SQLite event store.
//
// This is the full-fidelity record: every message and every session event, with
// the raw line preserved. Prometheus holds aggregates for dashboards; this
// holds the evidence. It is also the table a future agent reads - hence the
// flat, self-describing schema with text enums rather than opaque integers.
//
// Write path is batched inside explicit transactions with WAL enabled. Single
// row inserts on a synchronous journal are roughly three orders of magnitude
// slower and will not keep up with a busy session.
//
#include <cstdint>
#include <string>
#include <vector>

#include "fixmon/event.hpp"
#include "fixmon/session_state.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace fixmon {

class EventStore {
public:
    EventStore();
    ~EventStore();

    EventStore(const EventStore&)            = delete;
    EventStore& operator=(const EventStore&) = delete;

    bool open(const std::string& path, std::string& err);
    void close();

    // Buffers a row. Flushed by flush() or when the batch is full.
    void stage(const Event& ev);
    size_t staged() const { return staged_.size(); }

    // Commits staged rows in one transaction. Returns rows written.
    size_t flush();

    // Periodic session state snapshot, so the agent can answer
    // "what did this session look like at 14:32" without replaying events.
    void write_snapshot(const SessionSnapshot& snap, int64_t ts_ns);

    uint64_t rows_written() const { return rows_written_; }
    uint64_t write_errors() const { return write_errors_; }

private:
    bool exec(const char* sql, std::string& err);
    bool create_schema(std::string& err);

    sqlite3*      db_        = nullptr;
    sqlite3_stmt* insert_ev_ = nullptr;
    sqlite3_stmt* insert_sn_ = nullptr;

    std::vector<Event> staged_;
    uint64_t           rows_written_ = 0;
    uint64_t           write_errors_ = 0;
};

}  // namespace fixmon
