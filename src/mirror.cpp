#include "mirror.hpp"

#include <array>
#include <stdexcept>

#include <asm/termbits.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

namespace neko {
namespace {

// Use the Linux ABI here, separately from libc's termios in Terminal and Session.
#ifdef TCGETS2
using DeviceTermios = termios2;
constexpr unsigned long get_settings = TCGETS2;
constexpr unsigned long set_settings = TCSETS2;
#else
using DeviceTermios = termios; // On e.g. PowerPC, the original ioctl carries numeric speeds.
constexpr unsigned long get_settings = TCGETS;
constexpr unsigned long set_settings = TCSETS;
#endif

#ifdef CMSPAR
constexpr tcflag_t stick_parity = CMSPAR;
#else
constexpr tcflag_t stick_parity = 0;
#endif
constexpr tcflag_t frame_mask = CSIZE | PARENB | PARODD | CSTOPB | stick_parity;
constexpr tcflag_t soft_flow_mask = IXON | IXOFF | IXANY;

int termios_ioctl(int fd, unsigned long request, DeviceTermios& attrs) noexcept {
    int result;
    do {
        result = ioctl(fd, request, &attrs);
    } while (result < 0 && errno == EINTR);
    return result;
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

unsigned int data_bits(tcflag_t flags) {
    switch (flags & CSIZE) {
    case CS5: return 5;
    case CS6: return 6;
    case CS7: return 7;
    default: return 8;
    }
}

} // namespace

struct Mirror::PortSettings {
    DeviceTermios original{};
    DeviceTermios actual{};
};

unsigned int serial_frame(std::string_view format) {
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

std::string Mirror::frame() const {
    const auto flags = port_->actual.c_cflag;
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
    : port_(std::make_unique<PortSettings>()), flow_(flow), pending_(capacity) {
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
    if (termios_ioctl(fd(), get_settings, port_->original) < 0) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(),
                                "read terminal settings for mirror device '" + device + "'");
    }
}

void Mirror::configure(int local_tty, std::optional<std::uint32_t> baud,
                       std::optional<unsigned int> frame) {
    if (same_tty(local_tty, fd())) {
        throw std::invalid_argument("mirror device must not be the current terminal");
    }

    auto attrs = port_->original;
    attrs.c_oflag &= ~OPOST;
    attrs.c_lflag &= ~(ECHO | ECHONL);
    if (baud || frame) {
        attrs.c_cflag = (attrs.c_cflag & ~frame_mask) | frame.value_or(CS8);
    }
    if (baud) {
        attrs.c_cflag &= ~(CBAUD | CIBAUD);
        attrs.c_cflag |= CLOCAL | BOTHER | (static_cast<tcflag_t>(BOTHER) << IBSHIFT);
        attrs.c_ispeed = attrs.c_ospeed = *baud;
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
    if (termios_ioctl(fd(), set_settings, attrs) < 0) {
        const int error = errno;
        if (baud) {
            throw std::system_error(error, std::generic_category(),
                                    "configure mirror for --baud " + std::to_string(*baud));
        }
        throw os_error("set mirror terminal settings", error);
    }
    configured_ = true;
    auto& actual = port_->actual;
    if (termios_ioctl(fd(), get_settings, actual) < 0) {
        throw os_error("read back mirror terminal settings");
    }
    // Linux uses the output rate when the input baud selector is B0.
    const auto rx_baud = actual.c_cflag & CIBAUD ? actual.c_ispeed : actual.c_ospeed;
    if (baud && (rx_baud != *baud || actual.c_ospeed != *baud)) {
        throw std::runtime_error("mirror device did not accept --baud " + std::to_string(*baud) +
                                 " (driver reports RX " + std::to_string(rx_baud) +
                                 ", TX " + std::to_string(actual.c_ospeed) + " baud)");
    }
    if ((baud || frame) && (actual.c_cflag & frame_mask) != frame.value_or(CS8)) {
        throw std::runtime_error("mirror device does not support the requested framing");
    }
    if (flow_ && (((attrs.c_iflag ^ actual.c_iflag) & soft_flow_mask) ||
                  ((attrs.c_cflag ^ actual.c_cflag) & CRTSCTS) ||
                  (software_flow() && ((actual.c_lflag & ICANON) || !(actual.c_cflag & CREAD))))) {
        throw std::runtime_error("mirror device does not support the requested --flow");
    }
}

std::uint32_t Mirror::baud() const { return port_->actual.c_ospeed; }
bool Mirror::hardware_flow() const { return port_->actual.c_cflag & CRTSCTS; }
bool Mirror::kernel_ixon() const { return port_->actual.c_iflag & IXON; }
bool Mirror::kernel_ixoff() const { return port_->actual.c_iflag & IXOFF; }

double Mirror::wire_bytes_per_second() const {
    const auto flags = port_->actual.c_cflag;
    const auto rate = baud();
    const auto bits = 1 + data_bits(flags) + ((flags & PARENB) != 0) + (flags & CSTOPB ? 2 : 1);
    return static_cast<double>(rate ? rate : 115200) / bits;
}

Mirror::~Mirror() { restore(); }

void Mirror::restore() noexcept {
    if (configured_) {
        termios_ioctl(fd(), set_settings, port_->original);
        configured_ = false;
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
    if (fd() >= 0 && policy == SendPolicy::backpressure) {
        return size - bytes.size() + pending_.append(bytes);
    }
    if (fd() >= 0 && pending_.capacity() && policy == SendPolicy::drop_oldest) {
        enqueue(bytes);
    } else {
        dropped_ += bytes.size();
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
    restore();
    error_ = error;
    discard_pending();
    fd_.reset();
}

void Mirror::discard_pending(std::size_t staged) {
    dropped_ += pending_.size() + staged;
    pending_.clear();
}

} // namespace neko
