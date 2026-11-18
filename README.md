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

Run NekoTee in the foreground of your current terminal. Replace each example device path with a writable device dedicated to the mirror.<br>
请在当前终端的前台运行，并将示例中的设备路径替换为可写的专用镜像设备。

```sh
./build/nekotee
./build/nekotee --mirror /dev/pts/12
./build/nekotee --mirror /dev/ttyUSB0 --baud 115200 --frame 8N1
./build/nekotee --mirror /dev/ttyACM0 --baud 115200 --limit --resume 50 --wait -1
./build/nekotee --mirror /dev/ttyUSB0 --baud 115200 --limit --buffer 0
```

| Option<br>选项 | Default<br>默认值 | Description<br>说明 |
| --- | --- | --- |
| `--mirror DEVICE` | Required<br>必填 | Another TTY, an existing PTY slave, or a serial device such as `ttyS`, `ttyUSB`, or `ttyACM`. A PTY slave forwards to its master. The current terminal and `/dev/ptmx` are not valid.<br>另一 TTY、已有的 PTY 从端，或 `ttyS`、`ttyUSB`、`ttyACM` 等串口。写入 PTY 从端时由其主端接收。不能指定当前终端或 `/dev/ptmx`。 |
| `--shell PATH` | Account shell<br>账户 shell | Shell executable path; falls back to `/bin/sh` when none is configured.<br>Shell 可执行文件路径；账户未配置时用 `/bin/sh`。 |
| `--baud RATE` | Device setting<br>设备原设置 | Positive decimal integer without a unit, e.g. `--baud 115200`. The device and driver determine which rates are supported; NekoTee has no baud-rate list.<br>不带单位的十进制正整数，如 `--baud 115200`。可用波特率由设备和驱动决定，NekoTee 不设白名单。 |
| `--frame FORMAT` | Device setting; 8N1 with `--baud`<br>设备原设置；指定 `--baud` 时为 8N1 | E.g. `7E1`: 5–8 data bits, `N` (none), `E` (even), or `O` (odd) parity, and 1 or 2 stop bits.<br>例如 `7E1`：数据位 5–8、校验 `N` 无／`E` 偶／`O` 奇、停止位 1 或 2。 |
| `--buffer [N]` | 128 kbytes | Send queue capacity, 0–65536 kbytes (1 kbyte = 1024 bytes). No `N` means 128; also accepts `--buffer=N`.<br>待发队列容量，0–65536 kbytes（1 kbyte = 1024 字节）。省略 `N` 时为 128；也可写 `--buffer=N`。 |
| `--wait MS` | 500 ms | Wait for queued bytes when the PTY exits. Use `0` to skip the wait or `-1` for no deadline.<br>PTY 退出后的队列等待时间。`0` 不等待，`-1` 不限时。 |
| `--limit` | Off<br>关闭 | Pace PTY reads using the mirror link rate and available queue space. With `--buffer 0`, use a fixed rate.<br>根据镜像链路速率和队列余量限制原端读取。`--buffer 0` 时使用固定速率。 |
| `--flow` | See below<br>见下方 | Use `--flow soft` for XON/XOFF or `--flow hard` for RTS/CTS. Sets the mirror link independently of PTY pacing. `soft` also requires read access to the device.<br>`--flow soft` 使用 XON/XOFF，`--flow hard` 使用 RTS/CTS。此选项设置镜像链路，与是否启用原端限速独立。`soft` 还需要设备的读权限。 |
| `--resume PCT` | 50% | Resume level for `--limit` with a nonzero queue, as an integer from 0 to 99 without `%`. The pause level is always 100%.<br>有队列时 `--limit` 的恢复水位，填 0 到 99 的整数，不带 `%`。暂停水位固定为 100%。 |
| `-h`, `--help` | Also with no arguments<br>无参数时同样显示 | Show help without starting a session. The first nonempty of `LC_ALL`, `LC_MESSAGES`, and `LANG` selects Chinese for `zh` locales and English otherwise.<br>只显示帮助，不启动会话。按顺序取 `LC_ALL`、`LC_MESSAGES`、`LANG` 的第一个非空值；`zh` 区域用中文，其余用英文。 |

Without `--flow`, `--limit` selects `soft`. Without `--limit` or `--flow`, `--baud` disables serial flow control; otherwise the device's flow control is kept.<br>
未指定 `--flow` 时，`--limit` 默认选 `soft`。两者都未指定时，若指定了 `--baud` 则关闭串口流控，否则保留设备原设置。

If the driver rejects the requested baud rate or reports a different rate after configuration, NekoTee exits before starting the shell. A matching readback cannot verify the hardware timing or the receiver's ability to use that rate.<br>
如果驱动拒绝设置，或回读的波特率与请求不符，NekoTee 会在启动 Shell 前报错。回读一致不能验证实际线路速率，也不能保证接收端支持该速率。

Window size changes are forwarded to the new PTY. Normal shell job control (`Ctrl-C`, `Ctrl-Z`, `fg`) stays in that session. Type `exit` to return to the original shell.<br>
窗口尺寸变化会同步到新 PTY。`Ctrl-C`、`Ctrl-Z` 和 `fg` 等作业控制仍由新会话处理。输入 `exit` 返回原 shell。

Startup shows the mirror path, shell, baud and framing read back from the driver, link flow control, queue size, exit wait and PTY pacing (including the queue watermarks or the fixed rate when applicable). The startup, progress and exit notices are not mirrored. An invalid or inaccessible target, or rejected serial configuration, is reported before the shell starts.<br>
启动时显示镜像路径、Shell、从驱动回读的波特率和帧格式、串口流控类型、队列大小、退出等待和原端限速（适用时显示队列水位或固定速率）。起止和进度提示不进入镜像。目标无效、无法访问或驱动不接受串口设置时，会在启动 Shell 前报错。

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

## Mirror Send Queue
**镜像发送队列**

- `--buffer N` or `--buffer=N`: Set the mirror send queue capacity to `0`–`65536` kbytes (`1 kbyte = 1024` bytes). Omitting the option or giving `--buffer` without a number uses `128` kbytes. `--buffer 0` disables the send queue.<br>
  `--buffer N` 或 `--buffer=N`：将镜像待发队列容量设为 `0` 到 `65536` kbytes（`1 kbyte = 1024` 字节）。不传选项或只传 `--buffer` 都使用 `128` kbytes。`--buffer 0` 禁用待发队列。

Without `--limit`, the mirror never makes PTY reads wait. With no backlog, try a nonblocking write immediately; queue anything the driver cannot accept and send it in order. When full, drop the oldest unsent bytes, which can leave a partial line or split a UTF-8 character. `--buffer 0` disables queuing and drops bytes that cannot be written immediately.<br>
未启用 `--limit` 时，镜像不拖慢原端读取。没有积压就立即尝试非阻塞写入；未写入驱动的字节排队并按顺序发送。队列满时淘汰最旧的待发字节，可能留下残行或截断 UTF-8 字符。`--buffer 0` 不排队，直接丢弃写不进的字节。

With `--limit`, the nominal link byte rate is the reported baud divided by the frame length in bits, including start, parity and stop bits. If the baud is unknown or zero, the pacing estimate uses 115200. Pacing does not change the port's baud.<br>
启用 `--limit` 时，用回读的波特率除以帧位数（含起始位、校验位和停止位）估算链路字节速率。波特率未知或为 0 时按 115200 估算。限速不会改变串口波特率。

With a nonzero queue, the PTY read limit is the link byte rate times `1 + 7 × free/capacity`, so it tapers as the queue fills. At 100%, PTY reads pause instead of dropping data, then resume at or below `--resume`. If a read produces more transcript bytes than fit, for example at the end of a long line, its unwritten suffix is held until it can be queued and is counted in the pause notice.<br>
有队列时，PTY 读取上限为链路字节速率乘 `1 + 7 × 剩余容量 / 总容量`，队列越满越慢。达到 100% 就暂停读取而不丢数据，降到 `--resume` 或以下再恢复。如果一次产生的镜像文本超过队列余量（例如长行换行时），未入队的尾段会暂存并计入暂停提示。

With `--limit --buffer 0`, the PTY read limit is fixed at 99% of the nominal link byte rate (about 11,405 bytes/s at 115200 baud, 8N1). If a write cannot complete, NekoTee holds only the rest of the current batch and waits for the port before reading more. There are no queue watermarks; `--resume` has no effect. In either limited mode, a disconnected mirror no longer limits the PTY.<br>
`--limit --buffer 0` 将 PTY 读取上限固定为理论链路字节速率的 99%（115200 波特、8N1 时约为 11,405 字节/秒）。当前批次若未写完，只暂存它的尾段，等串口接收后再读取下一批。此时没有队列水位，`--resume` 不生效。两种限速模式下，镜像设备断开后均不再限制原端。

The PTY backpressure is local in both cases. In `soft`, NekoTee stops submitting writes on peer XOFF (`0x13`) and resumes on XON (`0x11`). Bytes already handed to the driver may still arrive. NekoTee discards all other incoming bytes and does not send XON/XOFF when its own queue fills. `hard` enables RTS/CTS on the mirror port and needs compatible hardware and wiring; a PTY is not a hardware-flow destination.<br>
两种握手设置都由本地暂停读取 PTY 来产生背压。`soft` 收到对端 XOFF（`0x13`）后停止提交新数据，收到 XON（`0x11`）后继续；已交给驱动的字节仍可能到达。其余输入全部丢弃，本地队列满时也不会主动发送 XON/XOFF。`hard` 启用镜像串口的 RTS/CTS，需要设备支持并接线；PTY 不是硬件流控目标。

When the shell exits or the PTY closes, NekoTee displays the remaining backlog and applies `--wait`. At the deadline it discards the backlog rather than delaying exit further; press `Ctrl-C` to cancel an ongoing wait. Dropped bytes and device errors are reported without changing the shell's normal exit status. The count excludes bytes already accepted by the driver; a successful write does not confirm receipt at the far end.<br>
Shell 退出或 PTY 关闭时，程序显示剩余积压并按 `--wait` 等待。到时丢弃积压，不再为队列延迟退出；等待期间可按 `Ctrl-C` 取消。丢弃字节和设备错误会报告，但不修改 Shell 的正常退出码。计数不含已写入驱动的字节，写入成功也不代表远端已收到。

## Testing
**测试**

The integration suite uses Python 3, Bash, and `cat`. It checks startup settings, filtering, fast numbered output, pause and resume, and exit deadlines. With `socat` installed, it also tests serial settings and one-way forwarding on a virtual pair.<br>
集成测试使用 Python 3、Bash 和 `cat`，检查启动配置、内容过滤、带行号的突发输出、暂停与恢复和退出等待。安装 `socat` 后，还会在虚拟串口对上验证串口设置和单向转发。

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
    RelayTests.test_hardware_serial_rtscts_configuration
```

## License
**许可证**

NekoTee is available under the [MIT License](LICENSE). The bundled [libvterm](third_party/libvterm/UPSTREAM.md) retains its [upstream MIT license and copyright notice](third_party/libvterm/LICENSE).<br>
NekoTee 采用 [MIT 许可证](LICENSE)。随附的 [libvterm](third_party/libvterm/UPSTREAM.md) 保留其[上游 MIT 许可证和版权声明](third_party/libvterm/LICENSE)。
