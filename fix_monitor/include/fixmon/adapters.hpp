#pragma once
//
// Source adapters.
//
// Every adapter turns one source into normalized Events and hands them to a
// sink callback. Nothing above this layer knows what a QuickFIX log looks like.
// A PcapAdapter added later implements the same interface and fills
// capture_ts_ns instead of leaving it zero.
//
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "fixmon/config.hpp"
#include "fixmon/event.hpp"
#include "fixmon/tail_reader.hpp"

namespace fixmon {

using EventSink = std::function<void(Event&&)>;

class ISourceAdapter {
public:
    virtual ~ISourceAdapter() = default;
    virtual void        start(EventSink sink) = 0;
    virtual void        stop()                = 0;
    virtual const char* name() const          = 0;
    virtual uint64_t    lines_read()  const   = 0;
    virtual uint64_t    parse_errors() const  = 0;
    virtual const std::string& session_id() const = 0;
};

// ---------------------------------------------------------------------------
// Adapter 1: QuickFIX message log. Application layer.
//
// Direction is derived by comparing tag 49 against our configured
// SenderCompID, because QuickFIX writes both sides into the same file with no
// explicit in/out marker.
// ---------------------------------------------------------------------------
class MessageLogAdapter : public ISourceAdapter {
public:
    MessageLogAdapter(SessionConfig cfg, bool from_beginning, int poll_ms);
    ~MessageLogAdapter() override;

    void        start(EventSink sink) override;
    void        stop() override;
    const char* name() const override { return "message_log"; }
    uint64_t    lines_read()   const override { return lines_read_.load(); }
    uint64_t    parse_errors() const override { return parse_errors_.load(); }
    const std::string& session_id() const override { return session_id_; }

    // Exposed for unit testing without touching the filesystem.
    bool parse_line(const std::string& line, Event& out);

private:
    void run();

    SessionConfig         cfg_;
    std::string           session_id_;
    bool                  from_beginning_;
    int                   poll_ms_;
    EventSink             sink_;
    std::thread           thread_;
    std::atomic<bool>     running_{false};
    std::atomic<uint64_t> lines_read_{0};
    std::atomic<uint64_t> parse_errors_{0};
    std::unique_ptr<TailReader> reader_;
};

// ---------------------------------------------------------------------------
// Adapter 2: QuickFIX event log. Session layer.
//
// This is the log that says *why* something happened - disconnect reason,
// sequence mismatch, heartbeat timeout - none of which appears in the message
// log. Rules are hardcoded here for now; lifting them into an external table
// is the next step and does not change this interface.
// ---------------------------------------------------------------------------
class EventLogAdapter : public ISourceAdapter {
public:
    EventLogAdapter(SessionConfig cfg, bool from_beginning, int poll_ms);
    ~EventLogAdapter() override;

    void        start(EventSink sink) override;
    void        stop() override;
    const char* name() const override { return "event_log"; }
    uint64_t    lines_read()   const override { return lines_read_.load(); }
    uint64_t    parse_errors() const override { return parse_errors_.load(); }
    const std::string& session_id() const override { return session_id_; }

    bool parse_line(const std::string& line, Event& out);

private:
    void run();

    SessionConfig         cfg_;
    std::string           session_id_;
    bool                  from_beginning_;
    int                   poll_ms_;
    EventSink             sink_;
    std::thread           thread_;
    std::atomic<bool>     running_{false};
    std::atomic<uint64_t> lines_read_{0};
    std::atomic<uint64_t> parse_errors_{0};
    std::unique_ptr<TailReader> reader_;
};

}  // namespace fixmon
