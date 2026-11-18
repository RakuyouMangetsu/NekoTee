#include "mirror.hpp"
#include "relay.hpp"
#include "session.hpp"
#include "terminal.hpp"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr unsigned int default_buffer = 128;
constexpr unsigned int max_buffer = 64 * 1024;

bool use_chinese() {
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

void session_end_message(int status) {
    if (use_chinese()) {
        std::cerr << "\n[nekotee] PTY 会话已结束，退出码 " << status << "。\n";
    } else {
        std::cerr << "\n[nekotee] PTY session ended (exit status " << status << ").\n";
    }
}

void help() {
    std::cout << (use_chinese() ? R"HELP(用法：nekotee --mirror DEVICE [选项]

  选项              | 默认值       | 说明
  ------------------+--------------+-----------------------------------------------
  --mirror DEVICE   | 必填         | 另一 tty、PTY 从端或串口；soft 也需读权限
  --shell PATH      | 账户 shell   | 指定 shell 路径；账户未配置时用 /bin/sh
  --baud RATE       | 设备原设置   | 十进制正整数，不带单位，如 115200
  --frame FORMAT    | 设备原设置*  | 如 8N1 或 7E1；数据位 5–8、校验 N/E/O、
                    |              | 停止位 1 或 2，N/E/O 分别为无/偶/奇校验
  --buffer [N]      | 128 kbytes   | 待发队列；0–65536，1 kbyte = 1024 字节
                    |              | 不带 N 使用 128；也可写 --buffer=N
  --wait MS         | 500 ms       | PTY 退出后的队列等待时间；0 不等，-1 不限时
  --limit           | 关闭         | 按串口速率和队列余量限制原端；0 队列时固定限速
  --flow soft|hard  | 见下方*      | 镜像串口握手：soft 为 XON/XOFF，hard 为 RTS/CTS
  --resume PCT      | 50%          | 有队列时 --limit 的恢复水位（0–99，不带 %）
  -h, --help        |              | 显示帮助，不启动会话；无参数同理

* 指定 --baud 但不指定 --frame 时用 8N1。未指定 --flow 时：有 --limit 则用
  soft；否则，指定 --baud 时关闭串口流控，未指定 --baud 时保留设备设置。
  可用波特率由设备和驱动决定。驱动拒绝或回读速率不符时，不启动会话。
  回读一致只表示驱动报告该设置，不保证线路能按该速率正常通信。

工作机制：
  会话：复用当前终端窗口，在新 PTY 中运行 shell。启动时显示实际配置，目标无效则报错。
        结束时输入 exit 返回；镜像端不能给 shell 发送输入。程序提示不会镜像。
  取样：LF 真正换行时，提取光标所在行的可见文字（含提示符、命令，合并自动折行），
        输出 UTF-8 和 CRLF。不复制换行前清除的内容、原始按键、无回显输入、样式、
        控制序列或未换行的末行。主动打印的敏感文字仍可能出现。
  发送：无积压立即尝试写入。不启用 --limit 时，队列满则丢最旧的数据；
        此时 --buffer 0 不排队，写不进的字节直接丢弃。
  限速：--limit 有队列时，队列越满越慢，达到 100% 暂停读取 PTY，降到恢复
        水位后继续；暂停时显示还需处理的字节数。--limit --buffer 0 不设水位，
        按链路字节速率（波特率除以帧位数）的 99% 读取，当前批次写不完则等候，
        不读下一批。--flow 只设置镜像串口握手，soft 响应对端 XOFF/XON，
        hard 需要接通 RTS/CTS，PTY 本身没有这些引脚。
  退出：显示剩余待发字节；--wait 到时丢弃剩余数据，也可按 Ctrl-C 取消等待。
        正常退出保留 shell 的退出码。待发字节不包括驱动已接受的数据，
        写入成功不代表远端已收到。

示例：nekotee --mirror /dev/ttyUSB0 --baud 115200 --frame 8N1 --limit --wait -1
)HELP" : R"HELP(Usage: nekotee --mirror DEVICE [OPTIONS]

  Option            | Default       | Description
  ------------------+---------------+---------------------------------------------
  --mirror DEVICE   | required      | Another tty, PTY slave, or serial port
                    |               | Soft flow also needs read permission
  --shell PATH      | account shell | Shell path (fallback: /bin/sh)
  --baud RATE       | device        | Positive decimal baud, no unit, e.g. 115200
  --frame FORMAT    | device*       | E.g. 8N1 or 7E1: 5-8 data bits, N/E/O parity
                    |               | (none/even/odd), 1 or 2 stop bits
  --buffer [N]      | 128 kbytes    | Send queue; 0-65536, 1 kbyte = 1024 bytes
                    |               | No N means 128. Also: --buffer=N
  --wait MS         | 500 ms        | Queue wait at PTY exit; 0 skips, -1 is unlimited
  --limit           | off           | Pace PTY reads; fixed rate with --buffer 0
  --flow soft|hard  | see below*    | Mirror handshake: XON/XOFF or RTS/CTS
  --resume PCT      | 50%           | Buffered pacing resume level (0-99, no % sign)
  -h, --help        |               | Show help without a session (also with no args)

* --baud without --frame uses 8N1. Without --flow, --limit selects soft;
  otherwise --baud disables serial flow control, or the device setting is kept.
  Available baud rates depend on the device and driver. A rejected setting or a
  different rate reported by the driver prevents startup. Readback does not
  verify that the link works at that rate.

How it works:
  Session: Run the shell in a PTY in the current window. Check the effective settings
           at startup; an invalid destination fails before the shell starts. Type exit
           to return. The mirror cannot send input to the shell; notices stay local.
  Capture: On LF, send the visible text of the cursor's line (including prompts and
           commands), joining soft wraps and ending in CRLF. No erased suggestions,
           raw keys, no-echo input, styles, controls, or unterminated last line.
           This is a text transcript; secrets printed by a program can still appear.
  Sending: Try writes immediately when the queue is empty. Without --limit, a full
           queue drops the oldest unsent bytes, or with --buffer 0 drops bytes that
           cannot be written at once.
  Pacing:  With --limit and a queue, read more slowly as it fills. At 100%, pause
           until the resume level and report the bytes left to get there. With
           --limit --buffer 0, read at 99% of the link byte rate (baud / frame bits)
           and finish writing each batch before reading more. There are no watermarks.
           --flow sets the mirror link: soft honors peer XOFF/XON, hard needs RTS/CTS.
  Exit:    Report queued bytes, then drop any remainder at the --wait deadline.
           Ctrl-C cancels the wait. Preserve the shell status on normal exit. Bytes
           already accepted by the driver are not in the count or confirmed delivered.

Example: nekotee --mirror /dev/ttyUSB0 --baud 115200 --frame 8N1 --limit --wait -1
)HELP");
}

struct Options {
    std::string device;
    std::string shell;
    std::optional<std::uint32_t> baud;
    std::optional<unsigned int> frame;
    std::optional<neko::Flow> flow;
    neko::RelayOptions relay;
    unsigned int buffer = default_buffer;
    bool show_help = false;
};

void session_start_message(const Options& options, const std::string& shell,
                           const neko::Mirror& mirror) {
    const bool zh = options.relay.chinese;
    std::cerr << (zh ? "[nekotee] PTY 会话已启动。输入 exit 返回原 shell。\n"
                     : "[nekotee] PTY session started. Type exit to return.\n")
              << (zh ? "  镜像：" : "  Mirror: ") << options.device
              << (zh ? "；Shell：" : "; shell: ") << shell << '\n'
              << (zh ? "  串口：" : "  Link: ")
              << mirror.baud() << " baud, " << mirror.frame() << "; ";
    const bool hard = mirror.hardware_flow();
    const bool send = mirror.kernel_ixon();
    const bool receive = mirror.kernel_ixoff();
    if (hard) {
        std::cerr << "RTS/CTS" << ((send || receive) ? " + " : "");
    }
    if (mirror.software_flow()) {
        std::cerr << (zh ? "XON/XOFF (发送，程序处理)" : "XON/XOFF (TX, user space)");
    } else if (send || receive) {
        std::cerr << "XON/XOFF (" << (send && receive ? "TX/RX" : send ? "TX" : "RX") << ')';
    } else if (!hard) {
        std::cerr << (zh ? "无串口流控" : "no serial flow control");
    }
    std::cerr << '\n' << (zh ? "  队列：" : "  Queue: ") << options.buffer << " KiB"
              << (zh ? "；退出等待：" : "; exit wait: ");
    if (options.relay.wait) {
        std::cerr << options.relay.wait->count() << " ms";
    } else {
        std::cerr << (zh ? "不限时" : "unlimited");
    }
    std::cerr << '\n' << (zh ? "  原端限速：" : "  PTY pacing: ");
    if (options.relay.limit && options.buffer == 0) {
        std::cerr << (zh ? "开启，固定 " : "on, fixed at ")
                  << mirror.wire_bytes_per_second() * neko::unbuffered_rate_fraction
                  << (zh ? " 字节/秒，无队列水位" : " B/s, no queue watermarks");
    } else if (options.relay.limit) {
        std::cerr << (zh ? "开启，100% 暂停，" : "on, pause at 100%, resume at ")
                  << options.relay.resume_percent << '%' << (zh ? " 恢复" : "");
    } else {
        std::cerr << (zh ? "关闭" : "off");
    }
    std::cerr << '\n';
}

unsigned int parse_number(std::string_view text, const char* option) {
    unsigned int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::invalid_argument(std::string(option) + " requires a nonnegative integer");
    }
    return value;
}

std::uint32_t parse_baud(std::string_view text) {
    std::uint32_t baud = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), baud);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !baud) {
        throw std::invalid_argument("--baud requires a positive decimal integer (1–4294967295)");
    }
    return baud;
}

std::optional<std::chrono::milliseconds> parse_wait(std::string_view text) {
    int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value < -1) {
        throw std::invalid_argument("--wait requires -1 or an integer from 0 to 2147483647 ms");
    }
    return value == -1 ? std::nullopt : std::optional{std::chrono::milliseconds{value}};
}

Options parse(int argc, char** argv) {
    constexpr std::string_view buffer_value_prefix = "--buffer=";
    Options options;
    options.show_help = argc == 1;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&](bool signed_number = false) -> std::string_view {
            if (i + 1 >= argc || (!signed_number && argv[i + 1][0] == '-')) {
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
            options.baud = parse_baud(value(true));
        } else if (arg == "--frame") {
            options.frame = neko::serial_frame(value());
        } else if (arg == "--wait") {
            options.relay.wait = parse_wait(value(true));
        } else if (arg == "--limit") {
            options.relay.limit = true;
        } else if (arg == "--flow") {
            const auto flow = value();
            if (flow == "soft") {
                options.flow = neko::Flow::soft;
            } else if (flow == "hard") {
                options.flow = neko::Flow::hard;
            } else {
                throw std::invalid_argument("--flow must be soft or hard");
            }
        } else if (arg == "--resume") {
            options.relay.resume_percent = parse_number(value(), "--resume");
            if (options.relay.resume_percent >= 100) {
                throw std::invalid_argument("--resume must be between 0 and 99");
            }
        } else if (arg == "--buffer") {
            options.buffer = i + 1 < argc && std::string_view(argv[i + 1]).substr(0, 1) != "-"
                                 ? parse_number(argv[++i], "--buffer") : default_buffer;
        } else if (arg.substr(0, buffer_value_prefix.size()) == buffer_value_prefix) {
            options.buffer = parse_number(arg.substr(buffer_value_prefix.size()), "--buffer");
        } else {
            throw std::invalid_argument("unknown option: " + std::string(arg));
        }
    }
    if (options.buffer > max_buffer) {
        throw std::invalid_argument("--buffer must not exceed 65536");
    }
    if (options.relay.limit && !options.flow) {
        options.flow = neko::Flow::soft;
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
    options.relay.chinese = use_chinese();

    bool started = false;
    try {
        const auto capacity = options.buffer * std::size_t{1024};
        const neko::Signals signals;
        neko::Mirror mirror(options.device, capacity, options.flow);
        neko::Terminal terminal;
        mirror.configure(terminal.fd(), options.baud, options.frame);
        const auto shell = options.shell.empty() ? neko::default_shell() : options.shell;
        int status;
        {
            neko::Session session(shell, terminal.settings(), terminal.size(), signals);
            started = true;
            session_start_message(options, shell, mirror);
            status = neko::relay(terminal, session, signals, mirror, options.relay);
            terminal.restore();
        }
        if (mirror.error()) {
            std::cerr << "nekotee: mirror disabled: " << std::generic_category().message(mirror.error())
                      << '\n';
        }
        if (mirror.dropped()) {
            std::cerr << "nekotee: dropped " << mirror.dropped() << " mirror bytes\n";
        }
        session_end_message(status);
        return status;
    } catch (const std::exception& error) {
        std::cerr << "nekotee: " << error.what() << '\n';
        if (started) {
            session_end_message(1);
        }
        return 1;
    }
}
