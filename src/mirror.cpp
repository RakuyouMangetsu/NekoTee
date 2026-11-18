#include "mirror.hpp"
#include "terminal.hpp"

#include <array>
#include <stdexcept>
#include <utility>

#include <fcntl.h>

namespace neko {
namespace {

#ifdef CMSPAR
constexpr tcflag_t stick_parity = CMSPAR;
#else
constexpr tcflag_t stick_parity = 0;
#endif
constexpr tcflag_t frame_mask = CSIZE | PARENB | PARODD | CSTOPB | stick_parity;
constexpr tcflag_t soft_flow_mask = IXON | IXOFF | IXANY;
constexpr std::pair<unsigned int, speed_t> baud_options[] = {
    {1200, B1200}, {2400, B2400}, {4800, B4800}, {9600, B9600}, {19200, B19200},
    {38400, B38400}, {57600, B57600}, {115200, B115200}, {230400, B230400},
    {460800, B460800}, {500000, B500000}, {576000, B576000}, {921600, B921600},
    {1000000, B1000000},
};

unsigned int data_bits(tcflag_t flags) {
    switch (flags & CSIZE) {
    case CS5: return 5;
    case CS6: return 6;
    case CS7: return 7;
    default: return 8;
    }
}

} // namespace

speed_t serial_baud(unsigned int rate) {
    for (const auto& [number, speed] : baud_options) {
        if (number == rate) {
            return speed;
        }
    }
    throw std::invalid_argument("unsupported baud rate");
}

std::optional<unsigned int> baud_rate(speed_t baud) {
    for (const auto& [number, speed] : baud_options) {
        if (speed == baud) {
            return number;
        }
    }
    // These standard speeds can also be inherited from an already configured port.
    switch (baud) {
    case B0: return 0;
    case B50: return 50;
    case B75: return 75;
    case B110: return 110;
    case B134: return 134;
    case B150: return 150;
    case B200: return 200;
    case B300: return 300;
    case B600: return 600;
    case B1800: return 1800;
    case B1152000: return 1152000;
    case B1500000: return 1500000;
    case B2000000: return 2000000;
    case B2500000: return 2500000;
    case B3000000: return 3000000;
    case B3500000: return 3500000;
    case B4000000: return 4000000;
    default: return std::nullopt;
    }
}

tcflag_t serial_frame(std::string_view format) {
    if (format.size() != 3 || format[0] < '5' || format[0] > '8' ||
        (format[1] != 'N' && format[1] != 'E' && format[1] != 'O') ||
        (format[2] != '1' && format[2] != '2')) {
        throw std::invalid_argument("invalid --frame (expected 5-8 data bits, N/E/O parity, "
                                    "1-2 stop bits, e.g. 8N1)");
    }
    constexpr tcflag_t data_bits[] = {CS5, CS6, CS7, CS8};
    auto flags = data_bits[format[0] - '5'];
    if (format[1] != 'N') {
        flags |= PARENB;
        if (format[1] == 'O') {
            flags |= PARODD;
        }
    }
    if (format[2] == '2') {
        flags |= CSTOPB;
    }
    return flags;
}

std::string format_serial_frame(tcflag_t flags) {
    char parity = 'N';
    if (flags & PARENB) {
        if (flags & stick_parity) {
            parity = flags & PARODD ? 'M' : 'S';
        } else {
            parity = flags & PARODD ? 'O' : 'E';
        }
    }
    return {static_cast<char>('0' + data_bits(flags)), parity, flags & CSTOPB ? '2' : '1'};
}

Mirror::Mirror(const std::string& device, std::size_t capacity, std::optional<Flow> flow)
    : flow_(flow), pending_(capacity) {
    const auto access = software_flow() ? O_RDWR : O_WRONLY;
    fd_.reset(open(device.c_str(), access | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
    if (fd() < 0) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(), "open mirror device '" + device + "'");
    }
    unsigned int pty_number = 0;
    if (ioctl(fd(), TIOCGPTN, &pty_number) == 0) {
        throw std::invalid_argument("mirror must be an existing tty or PTY slave, not ptmx");
    }
    if (tcgetattr(fd(), &original_) < 0) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(),
                                "read terminal settings for mirror device '" + device + "'");
    }
}

void Mirror::configure(int local_tty, std::optional<speed_t> baud, std::optional<tcflag_t> frame) {
    if (same_tty(local_tty, fd())) {
        throw std::invalid_argument("mirror device must not be the current terminal");
    }

    auto attrs = original_;
    attrs.c_oflag &= ~OPOST;
    attrs.c_lflag &= ~(ECHO | ECHONL);
    if (baud || frame) {
        attrs.c_cflag = (attrs.c_cflag & ~frame_mask) | frame.value_or(CS8);
    }
    if (baud) {
        attrs.c_cflag |= CLOCAL;
        if (cfsetispeed(&attrs, *baud) < 0 || cfsetospeed(&attrs, *baud) < 0) {
            throw os_error("configure mirror baud rate");
        }
    }
    if (baud || flow_) {
        attrs.c_cflag &= ~CRTSCTS;
        attrs.c_iflag &= ~soft_flow_mask;
    }
    if (software_flow()) {
        // Gate writes here: some serial drivers transmit even after the tty is stopped.
        attrs.c_iflag &= ~(BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
        attrs.c_iflag |= IGNBRK | IGNPAR;
        attrs.c_cflag |= CREAD;
        attrs.c_lflag &= ~(ICANON | ISIG | IEXTEN);
        attrs.c_cc[VMIN] = 1;
        attrs.c_cc[VTIME] = 0;
    } else if (flow_ == Flow::hard) {
        int modem_status = 0;
        if (ioctl(fd(), TIOCMGET, &modem_status) < 0) {
            throw os_error("--flow hard requires a mirror serial port with modem signals");
        }
        attrs.c_cflag |= CRTSCTS;
    }
    set_termios(fd(), attrs);
    configured_ = true;
    if (tcgetattr(fd(), &actual_) < 0) {
        throw os_error("read back mirror terminal settings");
    }
    if ((baud || frame) && (actual_.c_cflag & frame_mask) != frame.value_or(CS8)) {
        throw std::runtime_error("mirror device does not support the requested framing");
    }
    if (flow_ && (((attrs.c_iflag ^ actual_.c_iflag) & soft_flow_mask) ||
                  ((attrs.c_cflag ^ actual_.c_cflag) & CRTSCTS) ||
                  (software_flow() && ((actual_.c_lflag & ICANON) || !(actual_.c_cflag & CREAD))))) {
        throw std::runtime_error("mirror device does not support the requested --flow");
    }
}

double Mirror::wire_bytes_per_second() const {
    const auto flags = actual_.c_cflag;
    const auto rate = baud_rate(cfgetospeed(&actual_)).value_or(0);
    const auto bits = 1 + data_bits(flags) + ((flags & PARENB) != 0) + (flags & CSTOPB ? 2 : 1);
    return static_cast<double>(rate ? rate : 115200) / bits;
}

Mirror::~Mirror() {
    if (configured_) {
        restore_termios(fd(), original_);
    }
}

void Mirror::read_flow() {
    std::array<char, 1024> input;
    const auto count = read(fd(), input.data(), input.size());
    if (count > 0) {
        for (ssize_t i = 0; i < count; ++i) {
            if (input[i] == '\x13') {
                peer_paused_ = true;
            } else if (input[i] == '\x11') {
                peer_paused_ = false;
            }
        }
    } else if (count == 0 || !retry_io(errno)) {
        fail(count == 0 ? EIO : errno);
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

std::size_t Mirror::offer(std::string_view bytes, SendPolicy policy) {
    const auto size = bytes.size();
    if (bytes.empty() || fd() < 0) {
        return size;
    }
    if (can_submit() && pending_.empty()) {
        bytes.remove_prefix(write_once(bytes));
    }
    if (fd() < 0 || pending_.capacity() == 0 || policy == SendPolicy::immediate) {
        dropped_ += bytes.size();
    } else if (policy == SendPolicy::drop_oldest) {
        enqueue(bytes);
    } else {
        return size - bytes.size() + pending_.append(bytes);
    }
    return size;
}

void Mirror::enqueue(std::string_view bytes) {
    if (bytes.size() > pending_.free()) {
        const auto excess = bytes.size() - pending_.free();
        const auto old = std::min(excess, pending_.size());
        pending_.consume(old);
        // A single batch can exceed capacity even after dropping all queued bytes.
        bytes.remove_prefix(excess - old);
        dropped_ += excess;
    }
    pending_.append(bytes);
}

void Mirror::flush() {
    if (can_submit() && queued()) {
        const auto count = write_once(pending_.front());
        if (count > 0) {
            pending_.consume(count);
        }
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

void Mirror::discard_pending(std::size_t staged) {
    dropped_ += pending_.size() + staged;
    pending_.clear();
}

} // namespace neko
