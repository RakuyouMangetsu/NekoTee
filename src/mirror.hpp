#pragma once

#include "io.hpp"

#include <cstdint>
#include <optional>
#include <string>

#include <termios.h>

namespace neko {

enum class Flow { soft, hard };
enum class SendPolicy { drop_oldest, backpressure, immediate };

class Mirror {
public:
    Mirror(const std::string& device, std::size_t capacity, std::optional<Flow> flow);
    ~Mirror();

    void configure(int local_tty, std::optional<speed_t> baud, std::optional<tcflag_t> frame);
    int fd() const { return fd_.get(); }
    bool queued() const { return !pending_.empty(); }
    std::size_t pending_bytes() const { return pending_.size(); }
    std::size_t capacity() const { return pending_.capacity(); }
    const termios& settings() const { return actual_; }
    double wire_bytes_per_second() const;
    bool software_flow() const { return flow_ == Flow::soft; }
    bool can_submit() const { return fd() >= 0 && !peer_paused_; }
    void read_flow();
    // When retaining old data, return how much of the new batch was consumed.
    std::size_t offer(std::string_view bytes, SendPolicy policy);
    void flush();
    void fail(int error);
    void discard_pending(std::size_t staged = 0);
    int error() const { return error_; }
    std::uint64_t dropped() const { return dropped_; }

private:
    std::size_t write_once(std::string_view bytes);
    void enqueue(std::string_view bytes);

    Fd fd_;
    termios original_{};
    termios actual_{};
    bool configured_ = false;
    std::optional<Flow> flow_;
    bool peer_paused_ = false;
    ByteQueue pending_;
    int error_ = 0;
    std::uint64_t dropped_ = 0;
};

speed_t serial_baud(unsigned int rate);
std::optional<unsigned int> baud_rate(speed_t baud);
tcflag_t serial_frame(std::string_view format);
std::string format_serial_frame(tcflag_t flags);

} // namespace neko
