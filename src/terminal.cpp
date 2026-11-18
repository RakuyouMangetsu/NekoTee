#include "terminal.hpp"

#include <stdexcept>

#include <fcntl.h>
#include <sys/stat.h>

namespace neko {

Terminal::Terminal() : fd_(open("/dev/tty", O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)) {
    if (fd() < 0) {
        throw os_error("open controlling terminal (/dev/tty)");
    }
    if (!foreground()) {
        throw std::runtime_error("nekotee must be run in the foreground of its terminal");
    }
    if (tcgetattr(fd(), &original_) < 0) {
        throw os_error("read controlling terminal settings");
    }
}

Terminal::~Terminal() { restore(); }

bool Terminal::foreground() const { return tcgetpgrp(fd()) == getpgrp(); }

winsize Terminal::size() const {
    winsize size{};
    if (ioctl(fd(), TIOCGWINSZ, &size) < 0) {
        throw os_error("read controlling terminal size");
    }
    return size;
}

void Terminal::enter_raw() {
    auto raw = original_;
    cfmakeraw(&raw);
    set_termios(fd(), raw);
    raw_ = true;
}

void Terminal::restore() noexcept {
    if (raw_) {
        restore_termios(fd(), original_);
        raw_ = false;
    }
}

bool same_tty(int a, int b) {
    // Resolve tty aliases such as /dev/tty, which have different stat device numbers.
    unsigned int first = 0;
    unsigned int second = 0;
    if (ioctl(a, TIOCGDEV, &first) == 0 && ioctl(b, TIOCGDEV, &second) == 0) {
        return first == second;
    }

    struct stat sa {}, sb {};
    return fstat(a, &sa) == 0 && fstat(b, &sb) == 0 && S_ISCHR(sa.st_mode) &&
           S_ISCHR(sb.st_mode) && sa.st_rdev == sb.st_rdev;
}

void set_termios(int fd, const termios& value) {
    int result;
    do {
        result = tcsetattr(fd, TCSANOW, &value);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        throw os_error("set terminal attributes");
    }
}

void restore_termios(int fd, const termios& value) noexcept {
    while (tcsetattr(fd, TCSANOW, &value) < 0 && errno == EINTR) {
    }
}

} // namespace neko
