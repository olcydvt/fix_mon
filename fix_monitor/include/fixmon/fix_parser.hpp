#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace fixmon {

// Tag -> value view into the caller's buffer. Valid only while the source
// string is alive.
using TagMap = std::unordered_map<int, std::string_view>;

// Accepts SOH (\x01) or '|' as the field separator, so hand-edited and
// pipe-normalised logs parse the same as raw captures.
bool parse_fix_fields(std::string_view body, TagMap& out);

// "20240115-09:30:00.123" or "...:00.123456789" -> ns since epoch (UTC).
// Returns 0 if the string does not look like a QuickFIX timestamp.
int64_t parse_fix_timestamp(std::string_view ts);

// Splits "20240115-09:30:00.123 : <payload>" into timestamp and payload.
// If the line has no timestamp prefix, ts_ns is 0 and payload is the whole line.
void split_log_line(std::string_view line, int64_t& ts_ns, std::string_view& payload);

int64_t now_ns();

// Human-readable name for a FIX MsgType, for dashboards and the store.
const char* msg_type_name(std::string_view msg_type);

}  // namespace fixmon
