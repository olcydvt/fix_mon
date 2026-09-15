#include "fixmon/fix_parser.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>

namespace fixmon {

namespace {

inline bool is_separator(char c) { return c == '\x01' || c == '|'; }

inline int to_int(std::string_view sv) {
    int  v   = 0;
    bool any = false;
    for (char c : sv) {
        if (c < '0' || c > '9') break;
        v = v * 10 + (c - '0');
        any = true;
    }
    return any ? v : -1;
}

}  // namespace

bool parse_fix_fields(std::string_view body, TagMap& out) {
    out.clear();
    size_t i = 0;
    const size_t n = body.size();
    bool found_any = false;

    while (i < n) {
        // tag
        size_t eq = body.find('=', i);
        if (eq == std::string_view::npos) break;

        int tag = to_int(body.substr(i, eq - i));
        if (tag < 0) {
            // Not a tag=value pair; skip to next separator and continue.
            size_t sep = i;
            while (sep < n && !is_separator(body[sep])) ++sep;
            if (sep >= n) break;
            i = sep + 1;
            continue;
        }

        size_t vstart = eq + 1;
        size_t vend   = vstart;
        while (vend < n && !is_separator(body[vend])) ++vend;

        out.emplace(tag, body.substr(vstart, vend - vstart));
        found_any = true;

        if (vend >= n) break;
        i = vend + 1;
    }
    return found_any;
}

int64_t parse_fix_timestamp(std::string_view ts) {
    // Expected: YYYYMMDD-HH:MM:SS[.fff[fff[fff]]]
    if (ts.size() < 17) return 0;
    if (ts[8] != '-' || ts[11] != ':' || ts[14] != ':') return 0;

    auto num = [&](size_t pos, size_t len) -> int {
        int v = 0;
        for (size_t k = pos; k < pos + len; ++k) {
            char c = ts[k];
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return v;
    };

    int year = num(0, 4), mon = num(4, 2), day = num(6, 2);
    int hour = num(9, 2), min = num(12, 2), sec = num(15, 2);
    if (year < 0 || mon < 0 || day < 0 || hour < 0 || min < 0 || sec < 0) return 0;

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon  = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min  = min;
    tm.tm_sec  = sec;

#if defined(_WIN32)
    std::time_t epoch = _mkgmtime(&tm);
#else
    std::time_t epoch = timegm(&tm);
#endif
    if (epoch == static_cast<std::time_t>(-1)) return 0;

    int64_t ns = static_cast<int64_t>(epoch) * 1000000000LL;

    // Fractional seconds: FIX allows 3, 6 or 9 digits.
    if (ts.size() > 18 && ts[17] == '.') {
        int64_t frac = 0;
        int     digits = 0;
        for (size_t k = 18; k < ts.size() && digits < 9; ++k) {
            char c = ts[k];
            if (c < '0' || c > '9') break;
            frac = frac * 10 + (c - '0');
            ++digits;
        }
        for (int k = digits; k < 9; ++k) frac *= 10;
        ns += frac;
    }
    return ns;
}

void split_log_line(std::string_view line, int64_t& ts_ns, std::string_view& payload) {
    ts_ns   = 0;
    payload = line;

    // QuickFIX writes "<timestamp> : <payload>".
    size_t sep = line.find(" : ");
    if (sep == std::string_view::npos || sep > 32) {
        // Some builds omit the space padding.
        sep = line.find(':');
        if (sep == std::string_view::npos) return;
    }

    std::string_view head = line.substr(0, sep);
    // Trim trailing whitespace from the candidate timestamp.
    while (!head.empty() && (head.back() == ' ' || head.back() == '\t')) {
        head.remove_suffix(1);
    }

    int64_t parsed = parse_fix_timestamp(head);
    if (parsed == 0) return;  // no recognisable prefix; leave payload as-is

    ts_ns = parsed;
    size_t body_start = sep + 3 <= line.size() ? sep + 3 : line.size();
    payload = line.substr(body_start);
    while (!payload.empty() && (payload.front() == ' ' || payload.front() == '\t')) {
        payload.remove_prefix(1);
    }
}

int64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

const char* msg_type_name(std::string_view t) {
    if (t == "0")  return "Heartbeat";
    if (t == "1")  return "TestRequest";
    if (t == "2")  return "ResendRequest";
    if (t == "3")  return "Reject";
    if (t == "4")  return "SequenceReset";
    if (t == "5")  return "Logout";
    if (t == "A")  return "Logon";
    if (t == "D")  return "NewOrderSingle";
    if (t == "F")  return "OrderCancelRequest";
    if (t == "G")  return "OrderCancelReplaceRequest";
    if (t == "8")  return "ExecutionReport";
    if (t == "9")  return "OrderCancelReject";
    if (t == "j")  return "BusinessMessageReject";
    if (t == "V")  return "MarketDataRequest";
    if (t == "W")  return "MarketDataSnapshot";
    if (t == "X")  return "MarketDataIncrementalRefresh";
    if (t == "Y")  return "MarketDataRequestReject";
    if (t == "AB") return "NewOrderMultileg";
    if (t == "AE") return "TradeCaptureReport";
    return "Other";
}

}  // namespace fixmon
