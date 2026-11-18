#include "mirror.hpp"
#include "relay.hpp"
#include "session.hpp"
#include "terminal.hpp"

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr unsigned int default_buffer = 128;
constexpr unsigned int max_buffer = 64 * 1024;

bool chinese_help() {
    for (const auto* variable : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(variable);
        if (value && *value) {
            const std::string_view locale(value);
            return locale == "zh" || locale.substr(0, 3) == "zh_" ||
                   locale.substr(0, 3) == "zh-" || locale.substr(0, 3) == "zh." ||
                   locale.substr(0, 3) == "zh@";
        }
    }
    return false;
}

void help() {
    std::cout << (chinese_help() ? R"HELP(用法：nekotee --mirror DEVICE [选项]

  --mirror DEVICE       镜像目标：现有 tty、PTY 从端、ttyS、ttyUSB 或 ttyACM
  --shell PATH          覆盖当前账户配置的 shell；未配置时使用 /bin/sh
  --baud RATE           设置串口速率及 8N1（不启用软、硬件流控）
  --buffer-kbytes [N]    启用镜像缓冲，默认 128 kbytes；1 kbyte = 1024 字节
                        也可用 --buffer-kbytes=N，范围 0 到 65536
  --mirror-priority     镜像优先；未指定大小时启用 128 kbytes 缓冲
  -h, --help            显示帮助，不启动会话

工作机制：
  当前窗口接入一个新 PTY，运行交互 shell。输入只发给该 shell；镜像设备只写不读。
  镜像发送 UTF-8 纯文本（CRLF 换行），包含可见提示符、命令回显和输出。
  不发送原始按键、关闭回显的输入、样式或控制序列。主动打印的敏感文字仍会镜像。
  移动光标和清屏不会在镜像端重演；全屏应用显示为文本转录，不是画面复制。
  默认缓冲为 0：立即尝试写镜像，写不进就丢弃，原终端不等待镜像。
  启用缓冲后满时丢最旧的字节。镜像优先时，缓冲到 75% 暂停读取 PTY 输出，
  降到 50% 再继续。缓冲填满时仍丢最旧的数据。输入 exit 返回原 shell。

示例：nekotee --mirror /dev/ttyUSB0 --baud 115200 --mirror-priority
)HELP" : R"HELP(Usage: nekotee --mirror DEVICE [OPTIONS]

  --mirror DEVICE       Existing tty, PTY slave, ttyS, ttyUSB or ttyACM
  --shell PATH          Override the account shell (fallback: /bin/sh)
  --baud RATE           Set serial baud rate and 8N1 with no serial flow control
  --buffer-kbytes [N]    Enable the mirror buffer (default size: 128 kbytes)
                        Also accepts --buffer-kbytes=N; 0 to 65536, 1024 bytes each
  --mirror-priority     Throttle PTY output for the mirror; default buffer: 128 kbytes
  -h, --help            Show help without starting a session

How it works:
  This window connects to an interactive shell on a new PTY. Input goes only
  to that PTY; the mirror device is write-only. The mirror receives visible
  prompts, echoed commands, and output as UTF-8 text with CRLF line breaks.
  Raw keystrokes, no-echo input, styles, and control sequences are not copied.
  A secret deliberately printed by an application is still visible and mirrored.
  Cursor movement and screen clears are not replayed on the mirror; full-screen
  applications are transcribed as text, not reproduced as a duplicate display.
  The default is zero buffering: write immediately, drop what cannot be written,
  and never wait for the mirror during the session. A full buffer drops its oldest
  bytes. Mirror priority pauses PTY reads at 75% full and resumes at 50%.
  It still drops the oldest bytes if the buffer fills. Type exit to return.

Example: nekotee --mirror /dev/ttyUSB0 --baud 115200 --mirror-priority
)HELP");
}

struct Options {
    std::string device;
    std::string shell;
    std::optional<speed_t> baud;
    std::optional<unsigned int> buffer;
    bool priority = false;
    bool show_help = false;
};

unsigned int parse_number(std::string_view text, const char* option) {
    unsigned int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::invalid_argument(std::string(option) + " requires a nonnegative integer");
    }
    return value;
}

Options parse(int argc, char** argv) {
    Options options;
    options.show_help = argc == 1;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&]() -> std::string_view {
            if (i + 1 >= argc || std::string_view(argv[i + 1]).substr(0, 1) == "-") {
                throw std::invalid_argument(std::string(arg) + " requires a value");
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            options.show_help = true;
            return options;
        } else if (arg == "--mirror") {
            options.device = value();
        } else if (arg == "--shell") {
            options.shell = value();
        } else if (arg == "--baud") {
            options.baud = neko::serial_baud(parse_number(value(), "--baud"));
        } else if (arg == "--mirror-priority") {
            options.priority = true;
        } else if (arg == "--buffer-kbytes") {
            options.buffer = i + 1 < argc && std::string_view(argv[i + 1]).substr(0, 1) != "-"
                                 ? parse_number(argv[++i], "--buffer-kbytes") : default_buffer;
        } else if (arg.substr(0, 16) == "--buffer-kbytes=") {
            options.buffer = parse_number(arg.substr(16), "--buffer-kbytes");
        } else {
            throw std::invalid_argument("unknown option: " + std::string(arg));
        }
    }
    if (options.buffer && *options.buffer > max_buffer) {
        throw std::invalid_argument("--buffer-kbytes must not exceed 65536");
    }
    if (options.priority && options.buffer == 0) {
        throw std::invalid_argument("--mirror-priority needs a nonzero mirror buffer");
    }
    if (!options.show_help && options.device.empty()) {
        throw std::invalid_argument("--mirror DEVICE is required");
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    try {
        options = parse(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "nekotee: " << error.what() << " (see --help)\n";
        return 2;
    }
    if (options.show_help) {
        help();
        return 0;
    }

    try {
        const auto capacity = options.buffer.value_or(options.priority ? default_buffer : 0) *
                              std::size_t{1024};
        const auto shell = options.shell.empty() ? neko::default_shell() : options.shell;
        const neko::Signals signals;
        neko::Terminal terminal;
        neko::Mirror mirror(options.device, terminal.fd(), options.baud, capacity, options.priority);
        neko::Session session(shell, terminal.settings(), terminal.size(), signals);
        const int status = neko::relay(terminal, session, signals, mirror);
        terminal.restore();
        if (mirror.error()) {
            std::cerr << "nekotee: mirror disabled: " << std::generic_category().message(mirror.error())
                      << '\n';
        }
        if (mirror.dropped()) {
            std::cerr << "nekotee: dropped " << mirror.dropped() << " mirror bytes\n";
        }
        return status == 0 && mirror.priority() && (mirror.error() || mirror.dropped()) ? 1 : status;
    } catch (const std::exception& error) {
        std::cerr << "nekotee: " << error.what() << '\n';
        return 1;
    }
}
