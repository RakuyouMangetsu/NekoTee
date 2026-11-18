#include "relay.hpp"

#include "mirror.hpp"
#include "session.hpp"
#include "terminal.hpp"
#include "transcript.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <limits>

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
    using Count = std::chrono::milliseconds::rep;
    const auto milliseconds = static_cast<int>(std::clamp<Count>(
        remaining.count(), 0, std::numeric_limits<int>::max()));
    timeout = timeout < 0 ? milliseconds : std::min(timeout, milliseconds);
}

class Progress {
public:
    bool due(std::size_t count, Clock::time_point now, int& timeout) const {
        if (last_ == count) {
            return false;
        }
        if (now < next_) {
            shorten_timeout(timeout, next_);
            return false;
        }
        return true;
    }

    void shown(std::size_t count, Clock::time_point now) {
        last_ = count;
        next_ = now + 500ms;
    }

private:
    std::optional<std::size_t> last_;
    Clock::time_point next_ = Clock::time_point::min();
};

} // namespace

int relay(Terminal& terminal, Session& session, const Signals& signals, Mirror& mirror,
          const RelayOptions& options) {
    const auto size = terminal.size();
    Transcript transcript(size.ws_row, size.ws_col);
    ByteQueue to_terminal(64 * 1024);
    ByteQueue to_pty(16 * 1024);
    std::array<char, 8192> bytes{};
    std::optional<int> child_status;
    // Cap reads after our shell exits in case a background job still owns the slave.
    std::size_t exit_tail = 256 * 1024;
    bool input_open = true;
    bool hung_up = false;
    std::optional<Clock::time_point> ending_at;
    std::optional<Clock::time_point> closed_at;
    bool wait_expired = false;
    std::size_t expired_bytes = 0;
    bool exit_reported = false;
    Progress exit_progress;

    const auto resume_at = static_cast<std::size_t>(
        static_cast<std::uint64_t>(mirror.capacity()) * options.resume_percent / 100);
    const auto wire_rate = mirror.wire_bytes_per_second();
    auto next_read = Clock::now();
    bool paused = false;
    Progress pause_progress;
    // One read can yield more transcript bytes than it consumed (e.g. at a long line's LF).
    // Keep only that batch's unwritten suffix outside the fixed-capacity queue.
    std::string staged;
    std::size_t staged_offset = 0;

    auto backlog = [&] { return mirror.pending_bytes() + staged.size() - staged_offset; };
    auto discard_mirror = [&] {
        mirror.discard_pending(staged.size() - staged_offset);
        staged.clear();
        staged_offset = 0;
    };
    auto notice = [&](std::string text) {
        text = "\r\n[nekotee] " + text + "\r\n";
        if (text.size() > to_terminal.free()) {
            return false;
        }
        to_terminal.append(text);
        return true;
    };
    auto begin_ending = [&] {
        if (!ending_at) {
            ending_at = Clock::now();
        }
    };

    auto close_source = [&] {
        begin_ending();
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
            begin_ending();
            to_pty.clear();
        }

        int timeout = -1;
        const auto now = Clock::now();
        if (ending_at && options.wait && !wait_expired) {
            const auto deadline = *ending_at + *options.wait;
            if (now >= deadline) {
                wait_expired = true;
                expired_bytes = backlog();
                discard_mirror();
            } else {
                shorten_timeout(timeout, deadline);
            }
        }
        if (mirror.fd() < 0 && !staged.empty()) {
            discard_mirror();
        } else if (!staged.empty()) {
            staged_offset += mirror.offer(std::string_view(staged).substr(staged_offset),
                                          SendPolicy::backpressure);
            if (staged_offset == staged.size()) {
                staged.clear();
                staged_offset = 0;
            }
        }

        const bool pacing = options.limit && mirror.fd() >= 0 && !wait_expired;
        if (pacing && !paused && mirror.pending_bytes() == mirror.capacity()) {
            paused = true;
            pause_progress = {};
        } else if (paused && (!pacing || (staged.empty() && mirror.pending_bytes() <= resume_at))) {
            paused = false;
            next_read = now;
            if (!ending_at) {
                notice(pacing ? (options.chinese ? "已恢复读取 PTY。" : "PTY reads resumed.")
                              : (options.chinese ? "镜像断开，已恢复读取 PTY。"
                                                 : "Mirror disconnected; PTY reads resumed."));
            }
        }

        const auto queued = backlog();
        if (!ending_at && paused) {
            if (pause_progress.due(queued, now, timeout)) {
                const auto left = std::to_string(queued - resume_at);
                if (notice(options.chinese ? "PTY 读取已暂停；距离恢复还需发送 " + left + " 字节。"
                                           : "PTY reads paused; " + left + " bytes to resume.")) {
                    pause_progress.shown(queued, now);
                }
            }
        } else if (ending_at && queued && exit_progress.due(queued, now, timeout)) {
            const auto count = std::to_string(queued);
            if (notice(options.chinese ? "等待镜像；剩余待发 " + count + " 字节。"
                                       : "Waiting for mirror; " + count + " bytes queued.")) {
                exit_progress.shown(queued, now);
            }
        }
        if (closed_at) {
            if (!queued && !exit_reported) {
                const auto count = std::to_string(expired_bytes);
                if (expired_bytes) {
                    exit_reported = notice(options.chinese ? "镜像等待到时；丢弃剩余 " + count + " 字节。"
                                                           : "Mirror wait expired; " + count + " queued bytes discarded.");
                } else {
                    exit_reported = notice(options.chinese ? "镜像剩余待发：0 字节。" : "Mirror: 0 bytes queued.");
                }
            }
            if (to_terminal.empty()) {
                // While the mirror drains, the normal terminal can handle Ctrl-C.
                terminal.restore();
                if (exit_reported && (child_status || now >= *closed_at + 200ms)) {
                    return child_status.value_or(0);
                }
            }
            if (!child_status && now < *closed_at + 200ms) {
                shorten_timeout(timeout, *closed_at + 200ms);
            }
        }

        auto read_size = std::min(
            {bytes.size(), to_terminal.free(), child_status ? exit_tail : bytes.size()});
        if (session.fd() >= 0 && child_status && !exit_tail) {
            close_source();
            continue;
        }
        double read_rate = 0;
        if (session.fd() >= 0 && pacing) {
            if (paused || !staged.empty()) {
                read_size = 0;
            } else if (now < next_read) {
                read_size = 0;
                shorten_timeout(timeout, next_read);
            } else {
                // Allow bursts at up to 8x nominal wire speed, tapering to 1x near full.
                const auto free = mirror.capacity() - mirror.pending_bytes();
                read_rate = wire_rate * (1 + 7.0 * free / mirror.capacity());
                const auto chunk = static_cast<std::size_t>(std::ceil(read_rate * 0.02));
                read_size = std::min(read_size, chunk);
            }
        }
        if (session.fd() >= 0 && (child_status || hung_up) && read_size) {
            timeout = 0;
        }

        const bool cancel_wait = ending_at && !closed_at;
        enum { signal_fd, local_fd, child_fd, mirror_fd };
        std::array<pollfd, 4> pollfds{
            interest(signals.fd(), true, false),
            interest(terminal.fd(), cancel_wait || (!ending_at && input_open && to_pty.free()),
                     !to_terminal.empty(), true),
            interest(session.fd(), read_size != 0, !to_pty.empty(), !hung_up),
            interest(mirror.fd(), mirror.software_flow(), mirror.can_submit() && mirror.queued(), true),
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
                    discard_mirror();
                    return 128 + static_cast<int>(event.ssi_signo);
                case SIGTSTP:
                    terminal.restore();
                    raise(SIGSTOP);
                    if (!terminal.foreground()) {
                        discard_mirror();
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
            discard_mirror();
            return 128 + SIGHUP;
        }
        if ((local & POLLOUT) && !write_pending(terminal.fd(), to_terminal, "write current tty")) {
            discard_mirror();
            return 128 + SIGHUP;
        }

        const auto target = pollfds[mirror_fd].revents;
        if (target & failures) {
            mirror.fail(EIO);
        } else {
            if (target & POLLIN) {
                mirror.read_flow();
            }
            if (target & POLLOUT) {
                mirror.flush();
            }
        }

        if (local & POLLIN) {
            const auto limit = cancel_wait ? bytes.size() : std::min(bytes.size(), to_pty.free());
            const auto count = read(terminal.fd(), bytes.data(), limit);
            if (count > 0) {
                if (cancel_wait) {
                    const auto intr = terminal.settings().c_cc[VINTR];
                    const std::string_view input(bytes.data(), static_cast<std::size_t>(count));
                    if (intr != _POSIX_VDISABLE && input.find(static_cast<char>(intr)) != input.npos) {
                        discard_mirror();
                        return 128 + SIGINT;
                    }
                } else {
                    to_pty.append({bytes.data(), static_cast<std::size_t>(count)});
                }
            } else if (count == 0 || (count < 0 && (errno == EIO || errno == ENXIO))) {
                discard_mirror();
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
            begin_ending();
            hung_up = true;
            input_open = false;
            to_pty.clear();
        }

        if (session.fd() >= 0 && read_size && (child_status || hung_up || (master & POLLIN))) {
            const auto count = read(session.fd(), bytes.data(), read_size);
            if (count > 0) {
                const std::string_view output(bytes.data(), static_cast<std::size_t>(count));
                to_terminal.append(output);
                if (!write_pending(terminal.fd(), to_terminal, "write current tty")) {
                    discard_mirror();
                    return 128 + SIGHUP;
                }
                if (mirror.fd() >= 0) {
                    auto text = transcript.feed(output);
                    const auto policy = pacing ? SendPolicy::backpressure
                                               : wait_expired ? SendPolicy::immediate : SendPolicy::drop_oldest;
                    const auto sent = mirror.offer(text, policy);
                    if (sent < text.size()) {
                        staged = std::move(text);
                        staged_offset = sent;
                    }
                }
                if (read_rate && mirror.fd() >= 0) {
                    next_read = Clock::now() + std::chrono::ceil<Clock::duration>(
                        std::chrono::duration<double>(output.size() / read_rate));
                }
                if (child_status) {
                    exit_tail -= output.size();
                }
            } else if (count == 0 || errno == EIO || ((child_status || hung_up) && errno == EAGAIN)) {
                close_source();
            } else if (!retry_io(errno)) {
                throw os_error("read child PTY");
            }
        }
    }
}

} // namespace neko
