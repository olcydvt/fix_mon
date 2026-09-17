#include "fixmon/event_store.hpp"

#include <sqlite3.h>

#include <cstring>

namespace fixmon {

namespace {

constexpr const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS events (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    ts_ns           INTEGER NOT NULL,   -- engine clock, falls back to ingest
    ingest_ts_ns    INTEGER NOT NULL,
    capture_ts_ns   INTEGER,            -- NULL for log sources, set by pcap later
    session_id      TEXT    NOT NULL,
    event_class     TEXT    NOT NULL,   -- fix_message | session_event
    source          TEXT    NOT NULL,   -- message_log | event_log | pcap
    direction       TEXT,               -- in | out
    msg_type        TEXT,
    msg_type_name   TEXT,
    msg_seq_num     INTEGER,
    poss_dup        INTEGER,
    poss_resend     INTEGER,
    session_event   TEXT,
    expected_seq    INTEGER,
    received_seq    INTEGER,
    reject_reason   INTEGER,
    ref_tag_id      INTEGER,
    ref_seq_num     INTEGER,
    ord_status      TEXT,
    text            TEXT,
    raw             TEXT
);

-- Time-ranged lookups per session are the dominant query shape, both for a
-- human digging into an incident and for anything reading this later.
CREATE INDEX IF NOT EXISTS idx_events_session_ts ON events(session_id, ts_ns);
CREATE INDEX IF NOT EXISTS idx_events_ts         ON events(ts_ns);
CREATE INDEX IF NOT EXISTS idx_events_msgtype    ON events(session_id, msg_type, ts_ns);
CREATE INDEX IF NOT EXISTS idx_events_sessevent  ON events(session_id, session_event, ts_ns);

CREATE TABLE IF NOT EXISTS session_snapshots (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    ts_ns               INTEGER NOT NULL,
    session_id          TEXT    NOT NULL,
    state               TEXT    NOT NULL,
    last_incoming_ts_ns INTEGER,
    last_outgoing_ts_ns INTEGER,
    next_expected_in    INTEGER,
    last_outgoing_seq   INTEGER,
    msgs_in             INTEGER,
    msgs_out            INTEGER,
    seq_gaps            INTEGER,
    seq_gaps_reported   INTEGER,
    seq_too_low         INTEGER,
    resend_requests     INTEGER,
    heartbeat_timeouts  INTEGER,
    disconnects         INTEGER,
    reconnect_attempts  INTEGER,
    logon_rejects       INTEGER,
    session_rejects     INTEGER,
    business_rejects    INTEGER,
    exec_rejects        INTEGER,
    last_disconnect_reason TEXT,
    last_reject_text       TEXT
);
CREATE INDEX IF NOT EXISTS idx_snap_session_ts ON session_snapshots(session_id, ts_ns);
)SQL";

constexpr const char* kInsertEvent = R"SQL(
INSERT INTO events (ts_ns, ingest_ts_ns, capture_ts_ns, session_id, event_class,
                    source, direction, msg_type, msg_type_name, msg_seq_num,
                    poss_dup, poss_resend, session_event, expected_seq,
                    received_seq, reject_reason, ref_tag_id, ref_seq_num,
                    ord_status, text, raw)
VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21)
)SQL";

constexpr const char* kInsertSnapshot = R"SQL(
INSERT INTO session_snapshots (ts_ns, session_id, state, last_incoming_ts_ns,
                               last_outgoing_ts_ns, next_expected_in,
                               last_outgoing_seq, msgs_in, msgs_out, seq_gaps,
                               seq_gaps_reported, seq_too_low, resend_requests, heartbeat_timeouts,
                               disconnects, reconnect_attempts, logon_rejects,
                               session_rejects, business_rejects, exec_rejects,
                               last_disconnect_reason, last_reject_text)
VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22)
)SQL";

void bind_text(sqlite3_stmt* s, int idx, const std::string& v) {
    if (v.empty()) sqlite3_bind_null(s, idx);
    else           sqlite3_bind_text(s, idx, v.c_str(), -1, SQLITE_TRANSIENT);
}

}  // namespace

const char* msg_type_name(std::string_view);  // from fix_parser

EventStore::EventStore() = default;

EventStore::~EventStore() { close(); }

bool EventStore::exec(const char* sql, std::string& err) {
    char* msg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        err = msg ? msg : "sqlite exec failed";
        if (msg) sqlite3_free(msg);
        return false;
    }
    return true;
}

bool EventStore::create_schema(std::string& err) { return exec(kSchema, err); }

bool EventStore::open(const std::string& path, std::string& err) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        err = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        return false;
    }

    // WAL plus NORMAL sync is the combination that makes this keep up with a
    // busy session while still surviving a process crash. FULL sync would cost
    // an fsync per commit; OFF would risk the file on a machine power loss.
    if (!exec("PRAGMA journal_mode=WAL;", err))        return false;
    if (!exec("PRAGMA synchronous=NORMAL;", err))      return false;
    if (!exec("PRAGMA temp_store=MEMORY;", err))       return false;
    if (!exec("PRAGMA cache_size=-65536;", err))       return false;  // 64 MB
    if (!exec("PRAGMA busy_timeout=5000;", err))       return false;

    if (!create_schema(err)) return false;

    if (sqlite3_prepare_v2(db_, kInsertEvent, -1, &insert_ev_, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_prepare_v2(db_, kInsertSnapshot, -1, &insert_sn_, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

void EventStore::close() {
    if (!db_) return;
    flush();
    if (insert_ev_) { sqlite3_finalize(insert_ev_); insert_ev_ = nullptr; }
    if (insert_sn_) { sqlite3_finalize(insert_sn_); insert_sn_ = nullptr; }
    sqlite3_close(db_);
    db_ = nullptr;
}

void EventStore::stage(const Event& ev) { staged_.push_back(ev); }

size_t EventStore::flush() {
    if (!db_ || staged_.empty()) return 0;

    std::string err;
    if (!exec("BEGIN IMMEDIATE;", err)) {
        ++write_errors_;
        staged_.clear();
        return 0;
    }

    size_t written = 0;
    for (const Event& ev : staged_) {
        sqlite3_reset(insert_ev_);
        sqlite3_clear_bindings(insert_ev_);

        int64_t ts = ev.engine_ts_ns != 0 ? ev.engine_ts_ns : ev.ingest_ts_ns;
        sqlite3_bind_int64(insert_ev_, 1, ts);
        sqlite3_bind_int64(insert_ev_, 2, ev.ingest_ts_ns);
        if (ev.capture_ts_ns != 0) sqlite3_bind_int64(insert_ev_, 3, ev.capture_ts_ns);
        else                       sqlite3_bind_null(insert_ev_, 3);

        bind_text(insert_ev_, 4, ev.session_id);
        sqlite3_bind_text(insert_ev_, 5, to_string(ev.event_class), -1, SQLITE_STATIC);
        sqlite3_bind_text(insert_ev_, 6, to_string(ev.source), -1, SQLITE_STATIC);

        if (ev.direction != Direction::Unknown) {
            sqlite3_bind_text(insert_ev_, 7, to_string(ev.direction), -1, SQLITE_STATIC);
        } else {
            sqlite3_bind_null(insert_ev_, 7);
        }

        if (!ev.msg_type.empty()) {
            bind_text(insert_ev_, 8, ev.msg_type);
            sqlite3_bind_text(insert_ev_, 9, msg_type_name(ev.msg_type), -1, SQLITE_STATIC);
        } else {
            sqlite3_bind_null(insert_ev_, 8);
            sqlite3_bind_null(insert_ev_, 9);
        }

        if (ev.msg_seq_num) sqlite3_bind_int64(insert_ev_, 10, static_cast<int64_t>(ev.msg_seq_num));
        else                sqlite3_bind_null(insert_ev_, 10);

        sqlite3_bind_int(insert_ev_, 11, ev.poss_dup    ? 1 : 0);
        sqlite3_bind_int(insert_ev_, 12, ev.poss_resend ? 1 : 0);

        if (ev.event_class == EventClass::SessionEvent) {
            sqlite3_bind_text(insert_ev_, 13, to_string(ev.session_event), -1, SQLITE_STATIC);
        } else {
            sqlite3_bind_null(insert_ev_, 13);
        }

        if (ev.expected_seq_num) sqlite3_bind_int64(insert_ev_, 14, (int64_t)ev.expected_seq_num);
        else                     sqlite3_bind_null(insert_ev_, 14);
        if (ev.received_seq_num) sqlite3_bind_int64(insert_ev_, 15, (int64_t)ev.received_seq_num);
        else                     sqlite3_bind_null(insert_ev_, 15);

        if (ev.reject_reason >= 0) sqlite3_bind_int(insert_ev_, 16, ev.reject_reason);
        else                       sqlite3_bind_null(insert_ev_, 16);
        if (ev.ref_tag_id >= 0)    sqlite3_bind_int(insert_ev_, 17, ev.ref_tag_id);
        else                       sqlite3_bind_null(insert_ev_, 17);
        if (ev.ref_seq_num)        sqlite3_bind_int64(insert_ev_, 18, (int64_t)ev.ref_seq_num);
        else                       sqlite3_bind_null(insert_ev_, 18);

        bind_text(insert_ev_, 19, ev.ord_status);
        bind_text(insert_ev_, 20, ev.text);
        bind_text(insert_ev_, 21, ev.raw);

        if (sqlite3_step(insert_ev_) == SQLITE_DONE) {
            ++written;
        } else {
            ++write_errors_;
        }
    }

    if (!exec("COMMIT;", err)) {
        ++write_errors_;
        exec("ROLLBACK;", err);
        written = 0;
    }

    staged_.clear();
    rows_written_ += written;
    return written;
}

void EventStore::write_snapshot(const SessionSnapshot& s, int64_t ts_ns) {
    if (!db_) return;
    sqlite3_reset(insert_sn_);
    sqlite3_clear_bindings(insert_sn_);

    sqlite3_bind_int64(insert_sn_, 1, ts_ns);
    bind_text(insert_sn_, 2, s.session_id);
    sqlite3_bind_text(insert_sn_, 3, to_string(s.state), -1, SQLITE_STATIC);
    sqlite3_bind_int64(insert_sn_, 4, s.last_incoming_ts_ns);
    sqlite3_bind_int64(insert_sn_, 5, s.last_outgoing_ts_ns);
    sqlite3_bind_int64(insert_sn_, 6, (int64_t)s.next_expected_in);
    sqlite3_bind_int64(insert_sn_, 7, (int64_t)s.last_outgoing_seq);
    sqlite3_bind_int64(insert_sn_, 8, (int64_t)s.msgs_in);
    sqlite3_bind_int64(insert_sn_, 9, (int64_t)s.msgs_out);
    sqlite3_bind_int64(insert_sn_, 10, (int64_t)s.seq_gaps);
    sqlite3_bind_int64(insert_sn_, 11, (int64_t)s.seq_gaps_reported);
    sqlite3_bind_int64(insert_sn_, 12, (int64_t)s.seq_too_low);
    sqlite3_bind_int64(insert_sn_, 13, (int64_t)s.resend_requests);
    sqlite3_bind_int64(insert_sn_, 14, (int64_t)s.heartbeat_timeouts);
    sqlite3_bind_int64(insert_sn_, 15, (int64_t)s.disconnects);
    sqlite3_bind_int64(insert_sn_, 16, (int64_t)s.reconnect_attempts);
    sqlite3_bind_int64(insert_sn_, 17, (int64_t)s.logon_rejects);
    sqlite3_bind_int64(insert_sn_, 18, (int64_t)s.session_rejects);
    sqlite3_bind_int64(insert_sn_, 19, (int64_t)s.business_rejects);
    sqlite3_bind_int64(insert_sn_, 20, (int64_t)s.exec_rejects);
    bind_text(insert_sn_, 21, s.last_disconnect_reason);
    bind_text(insert_sn_, 22, s.last_reject_text);

    if (sqlite3_step(insert_sn_) != SQLITE_DONE) ++write_errors_;
}

size_t EventStore::purge_before(int64_t cutoff_ns, std::string& err) {
    if (!db_) return 0;

    // Staged rows have no timestamp filter applied to them yet and could be
    // older than the cutoff. Committing first keeps "what is in the table"
    // and "what survived the purge" the same answer.
    flush();

    size_t removed = 0;
    auto del = [&](const char* sql) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
            err = sqlite3_errmsg(db_);
            return;
        }
        sqlite3_bind_int64(st, 1, cutoff_ns);
        if (sqlite3_step(st) == SQLITE_DONE) {
            removed += static_cast<size_t>(sqlite3_changes(db_));
        } else {
            err = sqlite3_errmsg(db_);
        }
        sqlite3_finalize(st);
    };

    // One transaction: a purge interrupted halfway would otherwise leave
    // snapshots pointing at events that are already gone.
    std::string ignored;
    exec("BEGIN IMMEDIATE", ignored);
    del("DELETE FROM events WHERE ts_ns < ?");
    del("DELETE FROM session_snapshots WHERE ts_ns < ?");
    exec("COMMIT", ignored);

    // No VACUUM. It rewrites the whole file while holding a write lock, which
    // on a busy store is a stall the collector cannot afford. The freed pages
    // are reused by the next inserts; the file stops growing, which is the
    // part that actually matters.

    rows_purged_ += removed;
    return removed;
}

}  // namespace fixmon
