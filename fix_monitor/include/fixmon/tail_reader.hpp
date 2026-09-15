#pragma once
//
// Line-oriented file tailer with rotation detection.
//
// QuickFIX rotates message and event logs (daily, or on restart). We detect
// three cases:
//   - file shrank      -> truncated in place, restart from 0
//   - inode changed    -> rotated/replaced, restart from 0
//   - file disappeared -> wait for it to come back
//
// Partial trailing lines are held in a carry buffer until the newline arrives,
// so a line that is still being written is never handed downstream half-parsed.
//
#include <cstdint>
#include <fstream>
#include <functional>
#include <string>

namespace fixmon {

class TailReader {
public:
    TailReader(std::string path, bool from_beginning);
    ~TailReader();

    TailReader(const TailReader&)            = delete;
    TailReader& operator=(const TailReader&) = delete;

    // Reads whatever is currently available, invoking cb once per complete
    // line. Returns the number of lines delivered. Non-blocking.
    size_t poll(const std::function<void(const std::string&)>& cb);

    bool        is_open()  const { return stream_.is_open(); }
    uint64_t    rotations() const { return rotations_; }
    const std::string& path() const { return path_; }

private:
    bool open_stream(bool seek_to_end);
    bool detect_rotation();

    std::string   path_;
    std::ifstream stream_;
    std::string   carry_;
    bool          from_beginning_;
    uint64_t      inode_      = 0;
    uint64_t      last_size_  = 0;
    uint64_t      rotations_  = 0;
};

}  // namespace fixmon
