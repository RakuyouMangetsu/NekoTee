#include "mirror.hpp"
#include "terminal.hpp"

#include <stdexcept>

#include <fcntl.h>

namespace neko {

speed_t serial_baud(unsigned int rate) {
    switch (rate) {
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 500000: return B500000;
    case 576000: return B576000;
    case 921600: return B921600;
    case 1000000: return B1000000;
    default: throw std::invalid_argument("unsupported baud rate");
    }
}

Mirror::Mirror(const std::string& device, int local_tty, std::optional<speed_t> baud,
               std::size_t capacity, bool priority)
    : pending_(capacity), priority_(priority) {
    fd_.reset(open(device.c_str(), O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
    if (fd() < 0) {
        throw std::system_error(errno, std::generic_category(), "open mirror device " + device);
    }
    unsigned int pty_number = 0;
    if (ioctl(fd(), TIOCGPTN, &pty_number) == 0) {
        throw std::invalid_argument("mirror must be an existing tty or PTY slave, not ptmx");
    }
    if (tcgetattr(fd(), &original_) < 0) {
        throw os_error("read mirror device settings (the mirror must be a tty)");
    }
    if (same_tty(local_tty, fd())) {
        throw std::invalid_argument("mirror device must not be the current terminal");
    }

    auto attrs = original_;
    attrs.c_oflag &= ~OPOST;
    if (baud) {
        attrs.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
        attrs.c_cflag |= CS8 | CLOCAL;
        attrs.c_iflag &= ~(IXON | IXOFF | IXANY);
        if (cfsetispeed(&attrs, *baud) < 0 || cfsetospeed(&attrs, *baud) < 0) {
            throw os_error("configure mirror baud rate");
        }
    }
    set_termios(fd(), attrs);
    configured_ = true;
}

Mirror::~Mirror() {
    if (configured_) {
        restore_termios(fd(), original_);
    }
}

std::size_t Mirror::write_once(std::string_view bytes) {
    const ssize_t count = write(fd(), bytes.data(), bytes.size());
    if (count > 0) {
        return static_cast<std::size_t>(count);
    }
    if (count == 0 || !retry_io(errno)) {
        fail(count == 0 ? EIO : errno);
    }
    return 0;
}

void Mirror::offer(std::string_view bytes) {
    if (bytes.empty() || fd() < 0) {
        return;
    }
    if (pending_.empty()) {
        bytes.remove_prefix(write_once(bytes));
    }
    if (fd() < 0 || pending_.capacity() == 0) {
        dropped_ += bytes.size();
    } else {
        enqueue(bytes);
    }
}

void Mirror::enqueue(std::string_view bytes) {
    if (bytes.size() > pending_.free()) {
        const auto excess = bytes.size() - pending_.free();
        const auto old = std::min(excess, pending_.size());
        pending_.consume(old);
        bytes.remove_prefix(excess - old);
        dropped_ += excess;
    }
    pending_.append(bytes);
    update_throttle();
}

void Mirror::flush() {
    if (queued()) {
        const auto count = write_once(pending_.front());
        if (count > 0) {
            pending_.consume(count);
        }
        update_throttle();
    }
}

void Mirror::fail(int error) {
    if (configured_) {
        restore_termios(fd(), original_);
        configured_ = false;
    }
    error_ = error;
    discard_pending();
    fd_.reset();
}

void Mirror::discard_pending() {
    dropped_ += pending_.size();
    pending_.clear();
    throttled_ = false;
}

std::size_t Mirror::read_budget(std::size_t maximum) const {
    if (!priority_ || fd() < 0) {
        return maximum;
    }
    if (throttled_) {
        return 0;
    }
    return std::min(maximum, pending_.capacity() * 3 / 4 - pending_.size());
}

void Mirror::update_throttle() {
    if (priority_) {
        throttled_ = pending_.size() > pending_.capacity() / 2 &&
                     (throttled_ || pending_.size() >= pending_.capacity() * 3 / 4);
    }
}

} // namespace neko
