# NekoTee

NekoTee mirrors visible terminal text to another TTY, PTY, or serial port on Linux.<br>
NekoTee 是一个 Linux 终端镜像工具，将终端中的可见文字发送到另一个 TTY、PTY 或串口。

It starts an interactive shell in a new pseudoterminal (PTY) and connects it to your current window. The mirror cannot send input back to the shell.<br>
它在新的伪终端（PTY）中启动交互式 shell，并复用当前窗口。镜像端无法向 shell 回传输入。

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

Run NekoTee in the foreground of your current terminal. Replace the example device path with a dedicated mirror device. The default software flow control needs both read and write access.<br>
请在当前终端的前台运行，并将示例路径替换为专用镜像设备。默认的软件流控需要对该设备有读写权限。

```sh
./build/nekotee
./build/nekotee --mirror /dev/pts/12
./build/nekotee --mirror /dev/ttyUSB0 --baud 115200 --frame 8N1
./build/nekotee --mirror /dev/ttyUSB0 --baud 115200 --rate 95 --buffer 8 --resume 75
./build/nekotee --mirror /dev/ttyUSB0 --baud 115200 --flow none --buffer 0
./build/nekotee --mirror /dev/ttyUSB0 --baud 115200 --no-limit --wait 500
```

| Option<br>选项 | Default<br>默认值 | Description<br>说明 |
| --- | --- | --- |
| `--mirror DEVICE` | Required<br>必填 | Another TTY, an existing PTY slave, or a serial device such as `ttyS`, `ttyUSB`, or `ttyACM`. A PTY slave forwards to its master. The current terminal and `/dev/ptmx` are not valid.<br>另一 TTY、已有的 PTY 从端，或 `ttyS`、`ttyUSB`、`ttyACM` 等串口。写入 PTY 从端时由其主端接收。不能指定当前终端或 `/dev/ptmx`。 |
| `--shell PATH` | Account shell<br>账户 shell | Shell executable path; falls back to `/bin/sh` when none is configured.<br>Shell 可执行文件路径；账户未配置时用 `/bin/sh`。 |
| `--baud RATE` | Device setting<br>设备原设置 | Positive decimal integer without a unit, e.g. `--baud 115200`. The device and driver determine which rates are supported; NekoTee has no baud-rate list.<br>不带单位的十进制正整数，如 `--baud 115200`。可用波特率由设备和驱动决定，NekoTee 不设白名单。 |
| `--frame FORMAT` | Device setting; 8N1 with `--baud`<br>设备原设置；指定 `--baud` 时为 8N1 | E.g. `7E1`: 5–8 data bits, `N` (none), `E` (even), or `O` (odd) parity, and 1 or 2 stop bits.<br>例如 `7E1`：数据位 5–8、校验 `N` 无／`E` 偶／`O` 奇、停止位 1 或 2。 |
| `--buffer [N]` | 128 KiB | Send queue capacity in KiB (1 KiB = 1024 bytes). Range: 0–65536. `0` disables the queue. No `N` means 128; also accepts `--buffer=N`.<br>待发队列容量，单位 KiB（1 KiB = 1024 字节），范围 0–65536，`0` 不设队列。省略 `N` 时为 128；也可写 `--buffer=N`。 |
| `--limit` / `--no-limit` | `--limit` | Enable pacing and PTY backpressure, or favor the original terminal and allow mirror drops. The last of these two flags wins.<br>开启限速和背压，或优先保证原端速度并允许丢弃镜像数据。两者同时指定时以最后一个为准。 |
| `--rate PCT` | 99% | With pacing enabled, set the target percentage of the nominal link byte rate. Integer 1–100, without `%`.<br>限速开启时，设置目标速率为标称链路字节速率的百分比。填 1–100 的整数，不带 `%`。 |
| `--flow MODE` | `soft` with pacing<br>默认限速时为 `soft` | `soft` honors XON/XOFF, `hard` uses RTS/CTS, and `none` turns off serial handshaking. This setting is independent of PTY backpressure. `soft` also needs read access.<br>`soft` 响应 XON/XOFF，`hard` 使用 RTS/CTS，`none` 关闭串口握手。此选项不影响 PTY 背压；`soft` 还需要读权限。 |
| `--resume PCT` | 50% | With pacing and a nonzero queue, resume at or below this occupancy; the pause level is always 100%. Integer 0–99, without `%`.<br>限速开启且有队列时，降到此水位或以下再恢复；暂停水位固定为 100%。填 0–99 的整数，不带 `%`。 |
| `--wait MS` | `-1` (unlimited)<br>`-1`（不限时） | Exit drain deadline in milliseconds. `0` skips the wait, `-1` disables the deadline, and Ctrl+C cancels a wait.<br>退出排空的期限，单位毫秒。`0` 不等待，`-1` 不限时，等待期间可按 Ctrl+C 取消。 |
| `-h`, `--help` | Also with no arguments<br>无参数时同样显示 | Show help without starting a session. The first nonempty of `LC_ALL`, `LC_MESSAGES`, and `LANG` selects Chinese for `zh` locales and English otherwise.<br>只显示帮助，不启动会话。按顺序取 `LC_ALL`、`LC_MESSAGES`、`LANG` 的第一个非空值；`zh` 区域用中文，其余用英文。 |

With `--no-limit` and no explicit `--flow`, `--baud` disables serial flow control; otherwise the device's flow-control settings are kept. Use `--no-limit --wait 500` for the previous default terminal-first behavior.<br>
使用 `--no-limit` 且未指定 `--flow` 时，若指定了 `--baud` 则关闭串口流控，否则保留设备设置。可用 `--no-limit --wait 500` 恢复此前的原端优先行为。

If the driver rejects the requested baud rate or reports a different rate after configuration, NekoTee exits before starting the shell. A matching readback cannot verify the hardware timing or the receiver's ability to use that rate.<br>
如果驱动拒绝设置，或回读的波特率与请求不符，NekoTee 会在启动 Shell 前报错。回读一致不能验证实际线路速率，也不能保证接收端支持该速率。

Window size changes are forwarded to the new PTY. Normal shell job control (`Ctrl-C`, `Ctrl-Z`, `fg`) stays in that session. Type `exit` to return to the original shell.<br>
窗口尺寸变化会同步到新 PTY。`Ctrl-C`、`Ctrl-Z` 和 `fg` 等作业控制仍由新会话处理。输入 `exit` 返回原 shell。

Startup shows the mirror path, shell, baud and framing read back from the driver, link flow control, queue size, exit wait, and pacing target in bytes per second and percent, with watermarks when applicable. Startup, progress and exit notices are not mirrored. An invalid or inaccessible target, or rejected serial configuration, is reported before the shell starts.<br>
启动时显示镜像路径、shell、驱动回读的波特率和帧格式、串口流控、队列大小、退出等待，以及目标限速的字节/秒、百分比和适用的水位。启动、进度和退出提示不进入镜像。目标无效、无法访问或串口配置被拒绝时，会在启动 shell 前报错。

NekoTee temporarily disables terminal output processing and input echo on the mirror device and restores its settings on exit.<br>
NekoTee 会临时关闭镜像设备的输出转换和输入回显，并在退出时恢复设置。

## What Gets Mirrored
**镜像内容**

The original terminal displays output as it arrives. When an LF from the PTY starts a new line, NekoTee captures the visible text of the cursor's line before the LF and produces UTF-8 text ending in CRLF for the mirror. A shell prompt and the visible command appear together, for example `PS> ls`.<br>
原终端即时显示输出。PTY 输出 LF 并真正换行时，NekoTee 提取换行前光标所在行的可见文字，生成以 CRLF 结尾的 UTF-8 文本供镜像发送。提示符和可见的命令文字会出现在同一行，例如 `PS> ls`。

Raw keystrokes, input typed with echo disabled, terminal control sequences, and characters with the SGR conceal attribute are not copied. An application that prints a secret can still expose it on the mirror. Use a destination and receiver you trust.<br>
原始按键、关闭回显时输入的内容、终端控制序列，以及带有 SGR 隐藏属性的字符不会复制。应用主动输出的敏感内容仍可能出现在镜像中，请使用可信的目标设备和接收端。

Backspaces, suggestion redraws, and carriage returns (CR) update the pending line until it is sent. A suggestion cleared before LF stays out of the transcript. Text that never reaches a line feed, such as an idle prompt, a CR-only progress display, or an unterminated final line, can be seen on the original terminal only.<br>
退格、补全重绘和回车符（CR）会更新待发送的行。换行前清除的补全建议不会进入转录。空闲提示符、只用 CR 刷新的进度条以及退出时仍未换行的末行只显示在原终端。

Soft wraps are joined into one line. If a single line scrolls out of the main screen before its LF, the transcript keeps up to 64 KiB of its most recent off-screen text.<br>
终端自动折行的内容会合并为一行。单行内容在换行前滚出主屏幕时，转录最多保留最近滚出的 64 KiB。

## Pacing and Backpressure
**限速与背压**

By default, NekoTee paces both the production and submission of mirror text according to the number of filtered bytes, including CRLF. The target is the reported baud divided by the number of frame bits (including start, parity, and stop bits), multiplied by `--rate / 100`. For 115200 baud and 8N1, the default 99% is about 11,405 bytes/s. A missing or zero baud falls back to a 115200 estimate for pacing only. This target does not measure the actual wire rate or change the port's baud.<br>
默认按过滤后的镜像字节数（含 CRLF）限制镜像文本的产生和提交。目标速率为回读波特率除以帧位数（含起始位、校验位和停止位），再乘 `--rate / 100`。115200 波特、8N1 下，默认 99% 约为 11,405 字节/秒。波特率未知或为 0 时只在估算限速时按 115200 处理。此设置不测量实际线速，也不改变端口波特率。

The 128 KiB queue is reserve space for stalls, not permission to run ahead at a higher rate. NekoTee submits data as soon as the rate budget and the port allow. When the queue fills, it pauses PTY reads instead of discarding data and reports the bytes to send to reach `--resume`. A batch larger than the remaining space retains its unsent suffix in order. With `--buffer 0`, only that batch is retained and there are no watermarks. A disconnected mirror stops holding up the PTY.<br>
128 KiB 队列用于应对短时阻塞，不用于提高原端限速。有数据、字节预算和写入空间时就发送；队列满时暂停读取 PTY，不丢弃数据，并显示降到 `--resume` 还需发送的字节数。一次产生的文本若超出剩余空间，未入队尾段会按顺序暂存。`--buffer 0` 时只保留当前批次，不设水位。镜像端断开后不再拖慢 PTY。

With `--no-limit`, the mirror does not slow PTY reads. The program tries a nonblocking write immediately when there is no backlog, then queues any remainder. A full queue drops the oldest unsent bytes, which can leave a partial line or split a UTF-8 character. With `--no-limit --buffer 0`, bytes that cannot be written immediately are dropped.<br>
使用 `--no-limit` 时，镜像不拖慢 PTY 读取。没有积压时立即尝试非阻塞写入，写不完的部分入队。队列满时丢弃最旧的待发字节，可能留下残行或截断 UTF-8 字符。`--no-limit --buffer 0` 则直接丢弃不能立即写入的字节。

PTY backpressure is separate from serial handshaking. In `soft`, NekoTee stops submitting writes on peer XOFF (`0x13`) and resumes on XON (`0x11`); bytes already in the driver can still arrive. Other incoming bytes are ignored, and a full local queue does not send XOFF to the peer. `hard` requires compatible RTS/CTS hardware and wiring; `none` disables handshaking but does not disable pacing.<br>
PTY 背压与串口握手互相独立。`soft` 收到对端 XOFF（`0x13`）后停止提交，收到 XON（`0x11`）后继续；已交给驱动的字节仍可能到达。其余输入会被忽略，本地队列满时也不会主动向对端发送 XOFF。`hard` 需要支持 RTS/CTS 的硬件和接线；`none` 关闭握手，但不关闭限速。

On shell or PTY exit, NekoTee reports pending bytes and waits for its queue and any output the driver can report. The default has no deadline. Press Ctrl+C to discard the remaining queue, ask the driver to discard unsent output, and exit with status 130. A finite `--wait` stops submission at the deadline, reports dropped application bytes, and preserves the shell's exit code. Bytes already accepted by the driver may still be in flight. Neither a successful write nor an empty reported queue confirms receipt at the far end.<br>
Shell 退出或 PTY 关闭时，程序显示待发字节，并等待应用队列和驱动能报告的待发数据。默认不限时。按 Ctrl+C 会丢弃应用队列、请求驱动丢弃尚未发出的数据，并以 130 退出。设置有限的 `--wait` 时，到期停止提交并报告应用侧丢弃的字节，保留 shell 的退出码。已交给驱动的字节可能仍在传输；写入成功或队列回报为空都不代表对端已确认收到。

## Testing
**测试**

The integration suite uses Python 3, Bash, and `cat`. It checks startup settings, filtering, pacing, pause and resume, and timed or interruptible exit waits. With `socat` installed, it also tests serial settings and one-way forwarding on a virtual pair.<br>
集成测试使用 Python 3、Bash 和 `cat`，检查启动配置、内容过滤、限速、暂停与恢复，以及有期限或可取消的退出等待。安装 `socat` 后，还会在虚拟串口对上验证串口设置和单向转发。

```sh
ctest --test-dir build --output-on-failure
```

To run only the virtual serial test:<br>
单独运行虚拟串口测试：

```sh
python3 tests/integration.py ./build/nekotee RelayTests.test_virtual_serial
```

For physical tests, specify two idle ports connected in both TX/RX directions that you can read and write. The tests compare intact lines, fragments, and byte drops at 115200 baud, 8N1 and exercise peer XOFF/XON. The RTS/CTS test checks driver configuration, not hardware handshakes on the wire. Use `/dev/serial/by-id/` paths if device numbers change.<br>
测试实体串口时，请指定两个已双向连接 TX/RX、未被占用且当前用户可读写的设备。测试在 115200 波特率、8N1 下比较完整行、残片和丢弃字节，并验证对端发送 XOFF/XON 的行为。RTS/CTS 用例只检查驱动配置，不验证线路握手。设备编号会变时可用 `/dev/serial/by-id/` 下的路径。

```sh
NEKOTEE_SERIAL_TX=/dev/ttyUSB0 NEKOTEE_SERIAL_RX=/dev/ttyACM0 \
  python3 tests/integration.py ./build/nekotee \
    RelayTests.test_hardware_serial_cat_burst \
    RelayTests.test_hardware_serial_zero_buffer_pacing \
    RelayTests.test_hardware_serial_soft_flow \
    RelayTests.test_hardware_serial_interrupt_exit_wait \
    RelayTests.test_hardware_serial_rtscts_configuration
```

See the [default pacing regression report](Report/serial-default-pacing-2026-11-18.log). Older reports in `Report/` describe the defaults in effect when each test was run.<br>
参见[默认限速回归测试报告](Report/serial-default-pacing-2026-11-18.log)。`Report/` 中的旧报告记录的是各次测试时的默认行为。

## License
**许可证**

NekoTee is available under the [MIT License](LICENSE). The bundled [libvterm](third_party/libvterm/UPSTREAM.md) retains its [upstream MIT license and copyright notice](third_party/libvterm/LICENSE).<br>
NekoTee 采用 [MIT 许可证](LICENSE)。随附的 [libvterm](third_party/libvterm/UPSTREAM.md) 保留其[上游 MIT 许可证和版权声明](third_party/libvterm/LICENSE)。
