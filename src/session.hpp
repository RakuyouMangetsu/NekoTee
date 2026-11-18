#pragma once

#include "io.hpp"

#include <optional>
#include <string>

#include <sys/signalfd.h>
#include <termios.h>

struct winsize;

namespace neko {

class Signals {
public:
    Signals();
    ~Signals();

    int fd() const { return fd_.get(); }
    void reset_for_child() const;

private:
    Fd fd_;
    sigset_t original_{};
};

class Session {
public:
    Session(const std::string& shell, const termios& settings, const winsize& size,
            const Signals& signals);
    ~Session();

    int fd() const { return master_.get(); }
    void resize(const winsize& value);
    void close_master() { master_.reset(); }
    std::optional<int> status();

private:
    void cleanup() noexcept;

    Fd master_;
    pid_t child_ = -1;
    std::optional<int> status_;
};

std::string default_shell();

} // namespace neko
