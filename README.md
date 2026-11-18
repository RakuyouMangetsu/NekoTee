# NekoTee

NekoTee mirrors visible terminal text to another TTY, PTY, or serial port on Linux.<br>
NekoTee 是一个 Linux 终端镜像工具，将终端中的可见文字发送到另一个 TTY、PTY 或串口。

It starts an interactive shell in a new pseudoterminal (PTY) and connects it to your current window. The mirror is write-only; it cannot send input back to the shell.<br>
它在新的伪终端（PTY）中启动交互式 shell，并复用当前窗口。镜像端只写不读，无法向 shell 回传输入。

## Build from Source
**从源码构建**

You need Linux, CMake 3.16 or later, and C99 and C++17 compilers. The vendored libvterm builds with the project; no separate installation or network download is needed during the build.<br>
需要 Linux、CMake 3.16 或更高版本，以及支持 C99 和 C++17 的编译器。内置的 libvterm 随项目一起构建，构建时无需单独安装或联网下载依赖。

```sh
git clone https://github.com/RakuyouMangetsu/NekoTee.git
cd NekoTee
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Optionally install the binary and license notices under your home directory:<br>
也可以将程序和许可证安装到自己的主目录：

```sh
cmake --install build --prefix "$HOME/.local"
```

## Usage
**使用方法**

Run NekoTee in the foreground of your current terminal. Replace each example device path with a writable device dedicated to the mirror.<br>
请在当前终端的前台运行，并将示例中的设备路径替换为可写的专用镜像设备。

```sh
./build/nekotee
./build/nekotee --mirror /dev/pts/12
./build/nekotee --mirror /dev/ttyUSB0 --baud 115200
./build/nekotee --mirror /dev/ttyACM0 --baud 115200 --mirror-priority --buffer-kbytes 256
```

- `--mirror DEVICE`: Write to another terminal, an existing PTY slave, or a serial device such as `ttyS`, `ttyUSB`, or `ttyACM`. Writing to a PTY slave sends text to its master. The current terminal and `/dev/ptmx` are not valid destinations.<br>
  `--mirror DEVICE`：写入另一个终端、已有的 PTY 从端，或 `ttyS`、`ttyUSB`、`ttyACM` 等串口设备。写入 PTY 从端时，文字由其主端接收。不能将当前终端或 `/dev/ptmx` 设为目标。
- `--shell PATH`: Override the account's configured shell with an executable path. If the account has no configured shell, NekoTee uses `/bin/sh` by default.<br>
  `--shell PATH`：指定可执行文件路径，覆盖账户配置的 shell。账户未配置 shell 时，默认使用 `/bin/sh`。
- `--baud RATE`: Set the serial baud rate and use 8N1 with software and hardware flow control disabled. Omit it to keep the device's current baud rate, framing, and flow control.<br>
  `--baud RATE`：设定串口波特率，使用 8N1 并关闭软、硬件流控。不指定则保留设备现有的波特率、帧格式和流控设置。
- `-h`, `--help`, or no arguments: Show help without opening a session. The first nonempty value of `LC_ALL`, `LC_MESSAGES`, and `LANG` selects Chinese for `zh` locales and English otherwise.<br>
  `-h`、`--help` 或不加参数：只显示帮助，不启动会话。依次读取 `LC_ALL`、`LC_MESSAGES`、`LANG`，以第一个非空值为准；`zh` 区域显示中文，其余显示英文。

Window size changes are forwarded to the new PTY. Normal shell job control (`Ctrl-C`, `Ctrl-Z`, `fg`) stays in that session. Type `exit` to return to the original shell.<br>
窗口尺寸变化会同步到新 PTY。`Ctrl-C`、`Ctrl-Z` 和 `fg` 等作业控制仍由新会话处理。输入 `exit` 返回原 shell。

The current terminal displays a message when the PTY starts and ends. An invalid or inaccessible mirror path is reported before the shell starts.<br>
当前终端会显示 PTY 会话的起止提示。镜像设备路径无效或无法访问时，程序会在启动 shell 前报错。

NekoTee temporarily disables output processing on the mirror device to send its text unchanged and restores the device's terminal settings on normal exit.<br>
NekoTee 会临时关闭镜像设备的输出转换，让文本原样写入，并在正常退出时恢复设备的终端设置。

## What Gets Mirrored
**镜像内容**

The original terminal displays output as it arrives. Each PTY line feed (LF) sends the visible text of that line to the mirror as UTF-8 with a CRLF ending. A shell prompt and the command entered at it appear together, for example `PS> ls`.<br>
原终端即时显示输出。PTY 输出换行符（LF）时，镜像端收到该行当时的 UTF-8 可见文字，以 CRLF 结尾。提示符和在其后输入的命令会一起发送，例如 `PS> ls`。

Raw keystrokes, input typed with echo disabled, terminal control sequences, and characters with the SGR conceal attribute are not copied. An application that prints a secret can still expose it on the mirror. Use a destination and receiver you trust.<br>
原始按键、关闭回显时输入的内容、终端控制序列，以及带有 SGR 隐藏属性的字符不会复制。应用主动输出的敏感内容仍可能出现在镜像中，请使用可信的目标设备和接收端。

Backspaces, suggestion redraws, and carriage returns (CR) update the pending line until it is sent. A suggestion cleared before LF stays out of the transcript. Text that never reaches a line feed, such as an idle prompt, a CR-only progress display, or an unterminated final line, can be seen on the original terminal only.<br>
退格、补全重绘和回车符（CR）会更新待发送的行。换行前清除的补全建议不会进入转录。空闲提示符、只用 CR 刷新的进度条以及退出时仍未换行的末行只显示在原终端。

Soft wraps are joined into one line. If a single line scrolls out of the main screen before its LF, the transcript keeps up to 64 KiB of its most recent off-screen text.<br>
终端自动折行的内容会合并为一行。单行内容在换行前滚出主屏幕时，转录最多保留最近滚出的 64 KiB。

## Buffering and Backpressure
**缓冲与背压**

- **Default** `0 kbytes`. Attempt a nonblocking write as soon as a line is complete and drop bytes the device cannot accept. The local session does not wait for the mirror.<br>
  **默认** `0 kbytes`。行一完成就尝试非阻塞写入，丢弃设备暂时收不下的字节。本地会话不等待镜像。
- `--buffer-kbytes`: Enable a `128 kbytes` buffer and keep local priority.<br>
  `--buffer-kbytes`：启用 `128 kbytes` 缓冲，仍优先保障本地会话。
- `--buffer-kbytes N` or `--buffer-kbytes=N`: Set the capacity to `0`–`65536` kbytes (`1 kbyte = 1024` bytes).<br>
  `--buffer-kbytes N` 或 `--buffer-kbytes=N`：将容量设为 `0` 到 `65536` kbytes（`1 kbyte = 1024` 字节）。
- `--mirror-priority`: Use `128 kbytes` unless a size is specified. Pause PTY output reads at 75% full and resume at 50%. An explicit size of zero is invalid in this mode.<br>
  `--mirror-priority`：未指定大小时使用 `128 kbytes`。缓冲达到 75% 时暂停读取 PTY 输出，降到 50% 时恢复。此模式不能显式设为零缓冲。

A full buffer drops the oldest unsent bytes. Mirror priority can slow the local session, but does not guarantee lossless delivery. It applies backpressure to the PTY rather than injecting flow-control characters.<br>
缓冲满时丢弃最旧的未发送字节。镜像优先可能降低本地会话的输出速度，但不保证无损传输。限速通过给 PTY 施加背压实现，不会注入流控字符。

After the shell exits, local priority allows up to 500 ms to write any queued mirror data. Mirror priority waits for queued writes; press `Ctrl-C` to cancel the wait. A successful driver write does not guarantee that the far end received the data.<br>
shell 退出后，本地优先模式最多花 500 毫秒写出待发镜像数据。镜像优先模式会等待写完队列，可按 `Ctrl-C` 取消等待。驱动接受数据不代表远端已成功接收。

Dropped bytes and device errors are reported at exit. If the shell succeeds but mirror-priority mode encounters either problem, NekoTee returns exit status `1`.<br>
丢弃字节和设备错误会在退出时报告。如果 shell 成功退出，但镜像优先模式出现上述任一问题，NekoTee 返回退出码 `1`。

## Testing
**测试**

The integration suite uses Python 3, Bash, and `seq`. With `socat` installed, it also creates a virtual serial pair and tests 115200 baud, 8N1, text filtering, one-way forwarding, and restoration of terminal settings.<br>
集成测试使用 Python 3、Bash 和 `seq`。安装 `socat` 后，测试还会创建一对虚拟串口，验证 115200 波特率、8N1、文本过滤、单向转发以及终端设置恢复。

```sh
ctest --test-dir build --output-on-failure
```

To run only the virtual serial test:<br>
单独运行虚拟串口测试：

```sh
python3 tests/integration.py ./build/nekotee RelayTests.test_virtual_serial
```

## License
**许可证**

NekoTee is available under the [MIT License](LICENSE). The bundled [libvterm](third_party/libvterm/UPSTREAM.md) retains its [upstream MIT license and copyright notice](third_party/libvterm/LICENSE).<br>
NekoTee 采用 [MIT 许可证](LICENSE)。随附的 [libvterm](third_party/libvterm/UPSTREAM.md) 保留其[上游 MIT 许可证和版权声明](third_party/libvterm/LICENSE)。
