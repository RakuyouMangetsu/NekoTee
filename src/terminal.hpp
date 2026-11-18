#pragma once

#include "io.hpp"

#include <sys/ioctl.h>
#include <termios.h>

namespace neko {

class Terminal {
public:
    Terminal();
    ~Terminal();

    int fd() const { return fd_.get(); }
    const termios& settings() const { return original_; }
    winsize size() const;
    bool foreground() const;
    void enter_raw();
    void restore() noexcept;

private:
    Fd fd_;
    termios original_{};
    bool raw_ = false;
};

bool same_tty(int a, int b);
void set_termios(int fd, const termios& value);
void restore_termios(int fd, const termios& value) noexcept;

} // namespace neko
