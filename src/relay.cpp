#include "relay.hpp"

#include "mirror.hpp"
#include "session.hpp"
#include "terminal.hpp"
#include "transcript.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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

class BytePacer {
public:
    explicit BytePacer(double rate)
        : rate_(rate), chunk_(std::clamp(static_cast<std::size_t>(rate * 0.005),
                                        std::size_t{1}, std::size_t{8192})), credit_(chunk_) {}

    std::size_t chunk() const { return chunk_; }

    std::size_t ready(std::size_t wanted, Clock::time_point now, int& timeout) {
        if (!wanted) {
            return 0;
        }
        refill(now);
        const auto minimum = std::min(wanted, chunk_);
        if (credit_ < minimum) {
            const auto wait = std::chrono::duration<double>((minimum - credit_) / rate_);
            shorten_timeout(timeout, now + std::chrono::ceil<Clock::duration>(wait));
            return 0;
        }
        return std::min(wanted, static_cast<std::size_t>(credit_));
    }

    void charge(std::size_t bytes, Clock::time_point now) {
        refill(now);
        credit_ -= bytes;
    }

private:
    void refill(Clock::time_point now) {
        const auto elapsed = std::chrono::duration<double>(now - last_).count();
        credit_ = std::min(2.0 * chunk_, credit_ + rate_ * elapsed);
        last_ = now;
    }

    double rate_;
    std::size_t chunk_;
    double credit_;
    Clock::time_point last_ = Clock::now();
};

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
    bool expired_driver = false;
    bool exit_reported = false;
    Progress exit_progress;
    auto last_pty_output = Clock::now();

    const bool unbuffered = mirror.capacity() == 0;
    const auto resume_at = static_cast<std::size_t>(
        static_cast<std::uint64_t>(mirror.capacity()) * options.resume_percent / 100);
    const auto rate = options.byte_rate(mirror.wire_bytes_per_second());
    // Apply the same rate to admission and transmission. Without admission pacing,
    // the reserve becomes a large standing queue; without TX pacing, LF can burst.
    BytePacer admission(rate);
    BytePacer transmission(rate);
    bool paused = false;
    Progress pause_progress;
    // One read can yield more transcript bytes than it consumed (e.g. at a long line's LF).
    // Keep only that batch's unwritten suffix outside the fixed-capacity queue.
    std::string staged;
    std::size_t staged_offset = 0;

    auto backlog = [&] { return mirror.pending_bytes() + staged.size() - staged_offset; };
    auto staged_bytes = [&] { return std::string_view(staged).substr(staged_offset); };
    auto advance_staged = [&](std::size_t count) {
        staged_offset += count;
        if (staged_offset == staged.size()) {
            staged.clear();
            staged_offset = 0;
        }
    };
    auto discard_mirror = [&] {
        mirror.discard_pending(staged.size() - staged_offset);
        staged.clear();
        staged_offset = 0;
    };
    auto interrupt_wait = [&] {
        discard_mirror();
        mirror.discard_driver_output();
        return 128 + SIGINT;
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
                expired_driver = closed_at && !expired_bytes && mirror.driver_output().busy;
                discard_mirror();
            } else {
                shorten_timeout(timeout, deadline);
            }
        }
        if (mirror.fd() < 0 && !staged.empty()) {
            discard_mirror();
        } else if (!unbuffered && !staged.empty()) {
            advance_staged(mirror.append(staged_bytes()));
        }

        const bool pacing = options.limit && mirror.fd() >= 0 && !wait_expired;
        if (pacing && !unbuffered && !paused && mirror.pending_bytes() == mirror.capacity()) {
            paused = true;
            pause_progress = {};
        } else if (paused && (!pacing || (staged.empty() && mirror.pending_bytes() <= resume_at))) {
            paused = false;
            if (!ending_at) {
                notice(pacing ? (options.chinese ? "已恢复读取 PTY。" : "PTY reads resumed.")
                              : (options.chinese ? "镜像断开，已恢复读取 PTY。"
                                                 : "Mirror disconnected; PTY reads resumed."));
            }
        }

        const auto queued = backlog();
        const auto driver = closed_at && !queued && !wait_expired
                                ? mirror.driver_output() : Mirror::DriverOutput{};
        const auto draining = queued + driver.queued;
        if (!ending_at && paused) {
            if (pause_progress.due(queued, now, timeout)) {
                const auto left = std::to_string(queued - resume_at);
                if (notice(options.chinese ? "PTY 读取已暂停；距离恢复还需发送 " + left + " 字节。"
                                           : "PTY reads paused; " + left + " bytes to resume.")) {
                    pause_progress.shown(queued, now);
                }
            }
        } else if (ending_at && (draining || driver.busy)) {
            // Let the final PTY output through before inserting a local-only notice.
            const auto quiet_at = std::max(*ending_at, last_pty_output) + 250ms;
            if (!closed_at && !paused && now < quiet_at) {
                shorten_timeout(timeout, quiet_at);
            } else if (exit_progress.due(draining, now, timeout)) {
                const auto count = std::to_string(draining);
                const auto shown = draining
                    ? notice(options.chinese ? "等待镜像；剩余待发 " + count + " 字节。"
                                             : "Waiting for mirror; " + count + " bytes queued.")
                    : notice(options.chinese ? "等待镜像发送器排空。" : "Waiting for mirror transmitter.");
                if (shown) {
                    exit_progress.shown(draining, now);
                }
            }
        }
        if (closed_at) {
            if (driver.busy) {
                // Keep draining interruptible even when the driver has no poll event for it.
                shorten_timeout(timeout, now + 20ms);
            } else if (!queued && !exit_reported) {
                const auto count = std::to_string(expired_bytes);
                if (expired_bytes) {
                    exit_reported = notice(options.chinese ? "镜像等待到时；丢弃剩余 " + count + " 字节。"
                                                           : "Mirror wait expired; " + count + " queued bytes discarded.");
                } else if (expired_driver) {
                    exit_reported = notice(options.chinese ? "镜像等待到时；发送器可能仍有数据。"
                                                           : "Mirror wait expired; transmitter may still have data.");
                } else {
                    exit_reported = notice(options.chinese ? "镜像剩余待发：0 字节。" : "Mirror: 0 bytes queued.");
                }
            }
            if (to_terminal.empty() && exit_reported && (child_status || now >= *closed_at + 200ms)) {
                return child_status.value_or(0);
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
        if (session.fd() >= 0 && pacing) {
            if (paused || !staged.empty()) {
                read_size = 0;
            } else {
                read_size = admission.ready(std::min(read_size, admission.chunk()), now, timeout);
            }
        }
        const auto write_size = !mirror.can_submit() ? 0
                              : pacing ? transmission.ready(queued, now, timeout) : queued;
        if (session.fd() >= 0 && (child_status || hung_up) && read_size) {
            timeout = 0;
        }

        enum { signal_fd, local_fd, child_fd, mirror_fd };
        std::array<pollfd, 4> pollfds{
            interest(signals.fd(), true, false),
            interest(terminal.fd(), ending_at || (input_open && to_pty.free()),
                     !to_terminal.empty(), true),
            interest(session.fd(), read_size != 0, !to_pty.empty(), !hung_up),
            interest(mirror.fd(), mirror.software_flow(), write_size != 0, true),
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
                case SIGINT:
                    return interrupt_wait();
                case SIGHUP:
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
                    terminal.enter_raw();
                    if (!closed_at) {
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
                std::size_t sent;
                if (unbuffered) {
                    sent = mirror.submit(staged_bytes().substr(0, write_size));
                    advance_staged(sent);
                } else {
                    sent = mirror.flush(write_size);
                }
                if (pacing && sent) {
                    transmission.charge(sent, Clock::now());
                }
            }
        }

        if (local & POLLIN) {
            const auto limit = ending_at ? bytes.size() : std::min(bytes.size(), to_pty.free());
            const auto count = read(terminal.fd(), bytes.data(), limit);
            if (count > 0) {
                if (ending_at) {
                    const auto intr = terminal.settings().c_cc[VINTR];
                    const std::string_view input(bytes.data(), static_cast<std::size_t>(count));
                    if (input.find('\x03') != input.npos ||
                        (intr != _POSIX_VDISABLE && input.find(static_cast<char>(intr)) != input.npos)) {
                        return interrupt_wait();
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
                last_pty_output = Clock::now();
                to_terminal.append(output);
                if (!write_pending(terminal.fd(), to_terminal, "write current tty")) {
                    discard_mirror();
                    return 128 + SIGHUP;
                }
                if (mirror.fd() >= 0) {
                    auto text = transcript.feed(output);
                    if (wait_expired) {
                        mirror.discard_pending(text.size());
                        expired_bytes += text.size();
                    } else if (pacing && !text.empty()) {
                        admission.charge(text.size(), last_pty_output);
                        staged = std::move(text);
                    } else if (!pacing) {
                        mirror.offer_best_effort(text);
                    }
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
