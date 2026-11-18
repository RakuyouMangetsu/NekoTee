#pragma once

#include "io.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace neko {

enum class Flow { soft, hard, none };

class Mirror {
public:
    struct DriverOutput {
        std::size_t queued = 0;
        bool busy = false;
    };

    Mirror(const std::string& device, std::size_t capacity, std::optional<Flow> flow);
    ~Mirror();

    void configure(int local_tty, std::optional<std::uint32_t> baud, std::optional<unsigned int> frame);
    int fd() const { return fd_.get(); }
    bool queued() const { return !pending_.empty(); }
    std::size_t pending_bytes() const { return pending_.size(); }
    std::size_t capacity() const { return pending_.capacity(); }
    std::uint32_t baud() const;
    std::string frame() const;
    bool hardware_flow() const;
    bool kernel_ixon() const;
    bool kernel_ixoff() const;
    double wire_bytes_per_second() const;
    bool software_flow() const { return flow_ == Flow::soft; }
    bool can_submit() const { return fd() >= 0 && !peer_paused_; }
    void read_flow();
    std::size_t append(std::string_view bytes) { return pending_.append(bytes); }
    std::size_t submit(std::string_view bytes);
    void offer_best_effort(std::string_view bytes);
    std::size_t flush(std::size_t budget);
    DriverOutput driver_output() const;
    void discard_driver_output() noexcept;
    void fail(int error);
    void discard_pending(std::size_t staged = 0);
    int error() const { return error_; }
    std::uint64_t dropped() const { return dropped_; }

private:
    struct PortSettings;
    void restore() noexcept;
    void append_drop_oldest(std::string_view bytes);

    Fd fd_;
    std::unique_ptr<PortSettings> port_;
    bool configured_ = false;
    std::optional<Flow> flow_;
    bool peer_paused_ = false;
    ByteQueue pending_;
    int error_ = 0;
    std::uint64_t dropped_ = 0;
};

unsigned int serial_frame(std::string_view format);

} // namespace neko
