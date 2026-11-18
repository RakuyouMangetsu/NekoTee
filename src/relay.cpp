#include "relay.hpp"

#include "mirror.hpp"
#include "session.hpp"
#include "terminal.hpp"
#include "transcript.hpp"

#include <array>
#include <chrono>
#include <csignal>

#include <poll.h>

namespace neko {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr short failures = POLLERR | POLLHUP | POLLNVAL;

pollfd interest(int fd, bool input, bool output, bool detect_hangup = false) {
    const auto events = static_cast<short>((input ? POLLIN : 0) | (output ? POLLOUT : 0));
    return {events || detect_hangup ? fd : -1, events, 0};
}

bool write_pending(int fd, ByteQueue& queue, const char* operation) {
    const auto bytes = queue.front();
    const auto count = write(fd, bytes.data(), bytes.size());
    if (count > 0) {
        queue.consume(static_cast<std::size_t>(count));
    } else if (count == 0 || errno == EIO || errno == ENXIO) {
        return false;
    } else if (!retry_io(errno)) {
        throw os_error(operation);
    }
    return true;
}

void shorten_timeout(int& timeout, Clock::time_point deadline) {
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
    const int milliseconds = static_cast<int>(std::max(remaining.count(), decltype(remaining.count()){0}));
    timeout = timeout < 0 ? milliseconds : std::min(timeout, milliseconds);
}

} // namespace

int relay(Terminal& terminal, Session& session, const Signals& signals, Mirror& mirror) {
    const auto size = terminal.size();
    Transcript transcript(size.ws_row, size.ws_col);
    ByteQueue to_terminal(64 * 1024);
    ByteQueue to_pty(16 * 1024);
    std::array<char, 8192> bytes{};
    std::optional<int> child_status;
    // Cap reads after our shell exits in case a background job still owns the slave.
    std::size_t exit_tail = 256 * 1024;
    bool input_open = true;
    std::optional<Clock::time_point> closed_at;

    auto close_source = [&] {
        session.close_master();
        to_pty.clear();
        closed_at = Clock::now();
    };
    auto sync_size = [&] {
        const auto current = terminal.size();
        session.resize(current);
        if (mirror.fd() >= 0) {
            transcript.resize(current.ws_row, current.ws_col);
        }
    };

    terminal.enter_raw();
    sync_size();
    for (;;) {
        child_status = session.status();
        if (child_status) {
            to_pty.clear();
        }

        int timeout = -1;
        if (closed_at) {
            const auto now = Clock::now();
            if (!mirror.priority() && mirror.queued()) {
                const auto deadline = *closed_at + 500ms;
                if (now >= deadline) {
                    mirror.discard_pending();
                } else {
                    shorten_timeout(timeout, deadline);
                }
            }
            if (to_terminal.empty()) {
                // While the mirror drains, the normal terminal can handle Ctrl-C.
                terminal.restore();
                if (!mirror.queued() && (child_status || now >= *closed_at + 200ms)) {
                    return child_status.value_or(0);
                }
            }
            if (!child_status && now < *closed_at + 200ms) {
                shorten_timeout(timeout, *closed_at + 200ms);
            }
        }

        const auto read_size = mirror.read_budget(std::min(
            {bytes.size(), to_terminal.free(), child_status ? exit_tail : bytes.size()}));
        if (session.fd() >= 0 && child_status && !exit_tail) {
            close_source();
            continue;
        }
        if (session.fd() >= 0 && child_status && read_size) {
            timeout = 0;
        }

        enum { signal_fd, local_fd, child_fd, mirror_fd };
        std::array<pollfd, 4> pollfds{
            interest(signals.fd(), true, false),
            interest(terminal.fd(), !closed_at && !child_status && input_open && to_pty.free(),
                     !to_terminal.empty()),
            interest(session.fd(), read_size != 0, !to_pty.empty()),
            interest(mirror.fd(), false, mirror.queued(), true),
        };
        if (poll(pollfds.data(), pollfds.size(), timeout) < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw os_error("poll session");
        }

        if (pollfds[signal_fd].revents & POLLIN) {
            signalfd_siginfo event{};
            while (read(signals.fd(), &event, sizeof(event)) == sizeof(event)) {
                switch (event.ssi_signo) {
                case SIGWINCH:
                    if (!closed_at) {
                        sync_size();
                    }
                    break;
                case SIGHUP:
                case SIGINT:
                case SIGQUIT:
                case SIGTERM:
                    mirror.discard_pending();
                    return 128 + static_cast<int>(event.ssi_signo);
                case SIGTSTP:
                    terminal.restore();
                    raise(SIGSTOP);
                    if (!terminal.foreground()) {
                        mirror.discard_pending();
                        return 128 + SIGTSTP;
                    }
                    if (!closed_at) {
                        terminal.enter_raw();
                        sync_size();
                    }
                    break;
                default:
                    // SIGCHLD is reaped at the top of the event loop.
                    break;
                }
            }
        }

        const auto local = pollfds[local_fd].revents;
        if (local & failures) {
            mirror.discard_pending();
            return 128 + SIGHUP;
        }
        if ((local & POLLOUT) && !write_pending(terminal.fd(), to_terminal, "write current tty")) {
            mirror.discard_pending();
            return 128 + SIGHUP;
        }

        const auto target = pollfds[mirror_fd].revents;
        if (target & failures) {
            mirror.fail(EIO);
        } else if (target & POLLOUT) {
            mirror.flush();
        }

        if (local & POLLIN) {
            const auto count = read(terminal.fd(), bytes.data(), std::min(bytes.size(), to_pty.free()));
            if (count > 0) {
                to_pty.append({bytes.data(), static_cast<std::size_t>(count)});
            } else if (count == 0 || (count < 0 && (errno == EIO || errno == ENXIO))) {
                mirror.discard_pending();
                return 128 + SIGHUP;
            } else if (!retry_io(errno)) {
                throw os_error("read current tty");
            }
        }

        const auto master = pollfds[child_fd].revents;
        if (!to_pty.empty() && ((master & POLLOUT) || (local & POLLIN))) {
            if (!write_pending(session.fd(), to_pty, "write child PTY")) {
                input_open = false;
                to_pty.clear();
            }
        }
        if (master & failures) {
            input_open = false;
            to_pty.clear();
        }

        if (session.fd() >= 0 && (child_status || (master & (POLLIN | failures)))) {
            const auto limit = mirror.read_budget(std::min(
                {bytes.size(), to_terminal.free(), child_status ? exit_tail : bytes.size()}));
            if (limit == 0) {
                continue;
            }
            const auto count = read(session.fd(), bytes.data(), limit);
            if (count > 0) {
                const std::string_view output(bytes.data(), static_cast<std::size_t>(count));
                to_terminal.append(output);
                if (!write_pending(terminal.fd(), to_terminal, "write current tty")) {
                    mirror.discard_pending();
                    return 128 + SIGHUP;
                }
                if (mirror.fd() >= 0) {
                    mirror.offer(transcript.feed(output));
                }
                if (child_status) {
                    exit_tail -= output.size();
                }
            } else if (count == 0 || errno == EIO || (child_status && errno == EAGAIN)) {
                close_source();
            } else if (!retry_io(errno)) {
                throw os_error("read child PTY");
            }
        }
    }
}

} // namespace neko
