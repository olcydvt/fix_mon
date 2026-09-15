#include "fixmon/tail_reader.hpp"

#include <filesystem>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace fixmon {

namespace {

uint64_t file_id(const std::string& path) {
#if defined(_WIN32)
    // On Windows we fall back to (size, write time) as the identity proxy.
    std::error_code ec;
    auto t = fs::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<uint64_t>(t.time_since_epoch().count());
#else
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_ino);
#endif
}

uint64_t file_size(const std::string& path) {
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(sz);
}

}  // namespace

TailReader::TailReader(std::string path, bool from_beginning)
    : path_(std::move(path)), from_beginning_(from_beginning) {
    open_stream(!from_beginning_);
}

TailReader::~TailReader() {
    if (stream_.is_open()) stream_.close();
}

bool TailReader::open_stream(bool seek_to_end) {
    stream_.close();
    stream_.clear();
    stream_.open(path_, std::ios::in | std::ios::binary);
    if (!stream_.is_open()) return false;

    inode_ = file_id(path_);
    if (seek_to_end) {
        stream_.seekg(0, std::ios::end);
    }
    last_size_ = file_size(path_);
    carry_.clear();
    return true;
}

bool TailReader::detect_rotation() {
    if (!fs::exists(path_)) return false;

    uint64_t cur_id   = file_id(path_);
    uint64_t cur_size = file_size(path_);

    // Replaced (rotated) or truncated in place.
    if ((inode_ != 0 && cur_id != 0 && cur_id != inode_) || cur_size < last_size_) {
        ++rotations_;
        open_stream(false);  // new file: read it from the start
        return true;
    }
    last_size_ = cur_size;
    return false;
}

size_t TailReader::poll(const std::function<void(const std::string&)>& cb) {
    if (!stream_.is_open()) {
        if (!open_stream(!from_beginning_)) return 0;
    }

    detect_rotation();

    // Clear EOF so a previously exhausted stream picks up appended bytes.
    if (stream_.eof()) stream_.clear();

    size_t delivered = 0;
    char   buf[65536];

    while (stream_.read(buf, sizeof(buf)) || stream_.gcount() > 0) {
        std::streamsize got = stream_.gcount();
        if (got <= 0) break;

        carry_.append(buf, static_cast<size_t>(got));

        size_t start = 0;
        for (;;) {
            size_t nl = carry_.find('\n', start);
            if (nl == std::string::npos) break;

            size_t end = nl;
            if (end > start && carry_[end - 1] == '\r') --end;  // CRLF logs

            if (end > start) {
                cb(carry_.substr(start, end - start));
                ++delivered;
            }
            start = nl + 1;
        }
        if (start > 0) carry_.erase(0, start);

        if (stream_.eof()) {
            stream_.clear();
            break;
        }
    }

    if (stream_.fail() && !stream_.eof()) {
        stream_.clear();
    }
    return delivered;
}

}  // namespace fixmon
