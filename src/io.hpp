#pragma once

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace neko {

inline std::system_error os_error(const char* operation, int code = errno) {
    return std::system_error(code, std::generic_category(), operation);
}

class Fd {
public:
    explicit Fd(int fd = -1) noexcept : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    int get() const noexcept { return fd_; }
    void reset(int fd = -1) noexcept {
        const int old = std::exchange(fd_, fd);
        if (old >= 0) {
            const int saved = errno;
            close(old);
            errno = saved;
        }
    }

private:
    int fd_;
};

class ByteQueue {
public:
    explicit ByteQueue(std::size_t capacity) : data_(capacity) {}

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return data_.size(); }
    std::size_t free() const { return capacity() - size_; }
    bool empty() const { return size_ == 0; }
    void clear() { head_ = size_ = 0; }

    std::size_t append(std::string_view bytes) {
        const auto count = std::min(bytes.size(), free());
        if (count == 0) {
            return 0;
        }
        const auto end = (head_ + size_) % capacity();
        const auto first = std::min(count, capacity() - end);
        std::memcpy(data_.data() + end, bytes.data(), first);
        std::memcpy(data_.data(), bytes.data() + first, count - first);
        size_ += count;
        return count;
    }

    std::string_view front() const {
        return {data_.data() + head_, std::min(size_, capacity() - head_)};
    }

    void consume(std::size_t count) {
        head_ = (head_ + count) % capacity();
        size_ -= count;
    }

private:
    std::vector<char> data_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
};

inline bool retry_io(int error) {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
}

} // namespace neko
