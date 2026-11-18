#pragma once

#include "io.hpp"

#include <cstdint>
#include <optional>
#include <string>

#include <termios.h>

namespace neko {

class Mirror {
public:
    Mirror(const std::string& device, std::size_t capacity, bool priority);
    ~Mirror();

    void configure(int local_tty, std::optional<speed_t> baud);
    int fd() const { return fd_.get(); }
    bool queued() const { return !pending_.empty(); }
    bool priority() const { return priority_; }
    bool throttled() const { return throttled_; }
    std::size_t read_budget(std::size_t maximum) const;
    void offer(std::string_view bytes);
    void flush();
    void fail(int error);
    void discard_pending();
    int error() const { return error_; }
    std::uint64_t dropped() const { return dropped_; }

private:
    std::size_t write_once(std::string_view bytes);
    void enqueue(std::string_view bytes);
    void update_throttle();

    Fd fd_;
    termios original_{};
    bool configured_ = false;
    ByteQueue pending_;
    const bool priority_;
    bool throttled_ = false;
    int error_ = 0;
    std::uint64_t dropped_ = 0;
};

speed_t serial_baud(unsigned int rate);

} // namespace neko
