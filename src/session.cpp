#include "session.hpp"

#include <chrono>
#include <cstdio>
#include <stdexcept>

#include <fcntl.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <sys/wait.h>

namespace neko {
namespace {

constexpr int handled_signals[] = {SIGCHLD, SIGWINCH, SIGHUP, SIGINT, SIGQUIT, SIGTERM,
                                   SIGTSTP, SIGTTOU};

} // namespace

Signals::Signals() {
    sigset_t set;
    sigemptyset(&set);
    for (int number : handled_signals) {
        sigaddset(&set, number);
    }
    if (sigprocmask(SIG_BLOCK, &set, &original_) < 0) {
        throw os_error("block relay signals");
    }
    fd_.reset(signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC));
    if (fd() < 0) {
        const int error = errno;
        sigprocmask(SIG_SETMASK, &original_, nullptr);
        throw os_error("signalfd", error);
    }
}

Signals::~Signals() { sigprocmask(SIG_SETMASK, &original_, nullptr); }

void Signals::reset_for_child() const {
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    auto mask = original_;
    for (int number : handled_signals) {
        sigaction(number, &action, nullptr);
        sigdelset(&mask, number);
    }
    sigprocmask(SIG_SETMASK, &mask, nullptr);
}

std::string default_shell() {
    errno = 0;
    if (const auto* info = getpwuid(getuid())) {
        if (info->pw_shell != nullptr && info->pw_shell[0] != '\0') {
            return info->pw_shell;
        }
    } else if (errno != 0) {
        throw os_error("look up account shell");
    }
    return "/bin/sh";
}

Session::Session(const std::string& shell, const termios& settings, const winsize& size,
                 const Signals& signals) {
    int master = -1;
    auto attrs = settings;
    auto dimensions = size;
    child_ = forkpty(&master, nullptr, &attrs, &dimensions);
    if (child_ < 0) {
        throw os_error("forkpty");
    }
    if (child_ == 0) {
        signals.reset_for_child();
        char interactive[] = "-i";
        char* const args[] = {const_cast<char*>(shell.c_str()), interactive, nullptr};
        execv(shell.c_str(), args);
        const int error = errno;
        std::perror("nekotee: exec shell");
        _exit(error == ENOENT ? 127 : 126);
    }

    master_.reset(master);
    const int flags = fcntl(master, F_GETFL);
    if (flags < 0 || fcntl(master, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(master, F_SETFD, FD_CLOEXEC) < 0) {
        const auto error = os_error("configure PTY master fd");
        cleanup();
        throw error;
    }
}

Session::~Session() { cleanup(); }

void Session::resize(const winsize& value) {
    if (fd() >= 0 && ioctl(fd(), TIOCSWINSZ, &value) < 0 && errno != EIO) {
        throw os_error("resize child PTY");
    }
}

std::optional<int> Session::status() {
    if (child_ > 0) {
        int raw = 0;
        pid_t result;
        do {
            result = waitpid(child_, &raw, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == child_) {
            if (WIFEXITED(raw) || WIFSIGNALED(raw)) {
                status_ = WIFEXITED(raw) ? WEXITSTATUS(raw) : 128 + WTERMSIG(raw);
                child_ = -1;
            }
        } else if (result < 0 && errno == ECHILD) {
            status_ = 1;
            child_ = -1;
        } else if (result < 0) {
            throw os_error("waitpid for shell");
        }
    }
    return status_;
}

void Session::cleanup() noexcept {
    close_master();
    if (child_ <= 0) {
        return;
    }
    kill(child_, SIGHUP);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    for (;;) {
        int raw = 0;
        const pid_t result = waitpid(child_, &raw, WNOHANG);
        if (result == child_ || (result < 0 && errno != EINTR)) {
            break;
        }
        if (std::chrono::steady_clock::now() >= end) {
            kill(child_, SIGKILL);
            while (waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {
            }
            break;
        }
        usleep(5'000);
    }
    child_ = -1;
}

} // namespace neko
