#include "fixmon/adapters.hpp"
#include "fixmon/fix_parser.hpp"
#include "fixmon/redact.hpp"

#include <chrono>
#include <cstdlib>

namespace fixmon {

namespace {

uint64_t to_u64(std::string_view sv) {
    uint64_t v = 0;
    for (char c : sv) {
        if (c < '0' || c > '9') break;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    return v;
}

int32_t to_i32(std::string_view sv) {
    if (sv.empty()) return -1;
    int32_t v = 0;
    for (char c : sv) {
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (c - '0');
    }
    return v;
}

}  // namespace

MessageLogAdapter::MessageLogAdapter(SessionConfig cfg, bool from_beginning, int poll_ms,
                                     bool mask_bodies)
    : cfg_(std::move(cfg)),
      from_beginning_(from_beginning),
      poll_ms_(poll_ms),
      mask_bodies_(mask_bodies) {
    session_id_ = cfg_.session_id();
}

MessageLogAdapter::~MessageLogAdapter() { stop(); }

bool MessageLogAdapter::parse_line(const std::string& line, Event& out) {
    int64_t          ts = 0;
    std::string_view payload;
    split_log_line(line, ts, payload);

    // A message log line must contain a FIX body. Anything else is noise.
    size_t begin = payload.find("8=FIX");
    if (begin == std::string_view::npos) return false;
    payload = payload.substr(begin);

    TagMap tags;
    if (!parse_fix_fields(payload, tags) || tags.find(35) == tags.end()) {
        return false;
    }

    out = Event{};
    out.event_class   = EventClass::FixMessage;
    out.source        = Source::MessageLog;
    out.session_id    = session_id_;
    out.engine_ts_ns  = ts;
    out.ingest_ts_ns  = now_ns();
    out.raw.assign(payload.data(), payload.size());

    out.msg_type.assign(tags[35].data(), tags[35].size());

    if (auto it = tags.find(34); it != tags.end()) out.msg_seq_num = to_u64(it->second);
    if (auto it = tags.find(43); it != tags.end()) out.poss_dup    = (it->second == "Y");
    if (auto it = tags.find(97); it != tags.end()) out.poss_resend = (it->second == "Y");

    // QuickFIX writes both sides to one file with no in/out marker, so we
    // derive direction by comparing tag 49 against our own SenderCompID.
    if (auto it = tags.find(49); it != tags.end()) {
        out.direction = (it->second == cfg_.sender_comp_id) ? Direction::Outgoing
                                                            : Direction::Incoming;
    }

    // Derive the fields the dashboards and the store care about, so nothing
    // downstream has to re-parse the body.
    if (auto it = tags.find(373); it != tags.end()) out.reject_reason = to_i32(it->second);
    if (auto it = tags.find(380); it != tags.end() && out.reject_reason < 0) {
        out.reject_reason = to_i32(it->second);
    }
    if (auto it = tags.find(371); it != tags.end()) out.ref_tag_id  = to_i32(it->second);
    if (auto it = tags.find(45);  it != tags.end()) out.ref_seq_num = to_u64(it->second);
    if (auto it = tags.find(39);  it != tags.end()) {
        out.ord_status.assign(it->second.data(), it->second.size());
    }
    if (auto it = tags.find(58); it != tags.end()) {
        // Tag 58 is free text written by the counterparty. It is the one place in a
        // FIX message where an operator can type anything at all, so it gets the
        // same token scrub as an engine log line rather than being trusted.
        out.text = redact_free_text(it->second);
    }
    // OrdRejReason / CxlRejReason when no session reject reason present.
    if (out.reject_reason < 0) {
        if (auto it = tags.find(103); it != tags.end()) out.reject_reason = to_i32(it->second);
        else if (auto it2 = tags.find(102); it2 != tags.end()) out.reject_reason = to_i32(it2->second);
    }

    // Mask last, after every field we care about has been lifted out of the body.
    // The derived columns above are structure (sequence numbers, reject codes,
    // status letters) and stay readable; only the body copy loses its values.
    // Doing it here rather than at query time means a sensitive value never
    // reaches the queue, the store, or the HTTP surface in the first place.
    mask_fix_body(out.raw, mask_bodies_);

    return true;
}

void MessageLogAdapter::start(EventSink sink) {
    if (running_.exchange(true)) return;
    sink_    = std::move(sink);
    reader_  = std::make_unique<TailReader>(cfg_.message_log_path, from_beginning_);
    thread_  = std::thread(&MessageLogAdapter::run, this);
}

void MessageLogAdapter::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    reader_.reset();
}

void MessageLogAdapter::run() {
    Event ev;
    while (running_.load(std::memory_order_relaxed)) {
        reader_->poll([&](const std::string& line) {
            lines_read_.fetch_add(1, std::memory_order_relaxed);
            if (parse_line(line, ev)) {
                sink_(std::move(ev));
            } else {
                parse_errors_.fetch_add(1, std::memory_order_relaxed);
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms_));
    }
}

}  // namespace fixmon
