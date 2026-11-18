"""Run integration tests against a built nekotee: python3 tests/integration.py ./build/nekotee."""

from contextlib import contextmanager
import errno
import fcntl
import os
from pathlib import Path
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import tty
import unittest


BINARY = os.path.abspath(sys.argv.pop(1))
SERIAL_TX = os.environ.get("NEKOTEE_SERIAL_TX")
SERIAL_RX = os.environ.get("NEKOTEE_SERIAL_RX")
CAT_COMMAND = b"cat burst.txt\n"
CAT_END = b"BURST_END\r\n"
EXIT_TEXT = b"PS> exit\r\nexit\r\n"


class Session:
    def __init__(self, *options, device=None, peer=None, locale="C.UTF-8"):
        self.options = options
        self.device = device
        self.peer = peer
        self.locale = locale
        self.local = bytearray()
        self.mirror = bytearray()

    def __enter__(self):
        self.home = tempfile.TemporaryDirectory()
        self.local_master, self.local_slave = os.openpty()
        self.mirror_master, self.mirror_slave = os.openpty() if self.peer is None else (self.peer, None)
        self.resize(24, 132)
        self.original = termios.tcgetattr(self.local_slave)
        self.mirror_original = termios.tcgetattr(self.mirror_slave) if self.mirror_slave is not None else None
        os.set_blocking(self.local_master, False)
        os.set_blocking(self.mirror_master, False)
        device = self.device or os.ttyname(self.mirror_slave)
        if device == "self":
            device = os.ttyname(self.local_slave)

        def controlling_tty():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)

        self.process = subprocess.Popen(
            [BINARY, "--mirror", device, "--shell", "/bin/bash", *self.options],
            stdin=self.local_slave, stdout=self.local_slave, stderr=self.local_slave,
            preexec_fn=controlling_tty, cwd=self.home.name,
            env={**os.environ, "TERM": "xterm-256color", "HOME": self.home.name,
                 "PS1": "PS> ", "PROMPT_COMMAND": "", "LC_ALL": self.locale},
        )
        return self

    def __exit__(self, *exc):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        os.close(self.local_master)
        os.close(self.local_slave)
        if self.peer is None and self.mirror_master >= 0:
            os.close(self.mirror_master)
        if self.mirror_slave is not None:
            os.close(self.mirror_slave)
        self.home.cleanup()

    def resize(self, rows, columns):
        fcntl.ioctl(self.local_slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))

    def send(self, data):
        os.write(self.local_master, data)

    def pump(self, timeout=0.02, read_mirror=True):
        devices = {self.local_master: self.local}
        if read_mirror and self.mirror_master >= 0:
            devices[self.mirror_master] = self.mirror
        for fd in select.select(list(devices), [], [], timeout)[0]:
            try:
                devices[fd].extend(os.read(fd, 65536))
            except BlockingIOError:
                pass
            except OSError as error:
                if error.errno != errno.EIO:
                    raise

    def until(self, marker, *, mirror=False, read_mirror=True, since=0, timeout=5):
        output = self.mirror if mirror else self.local
        deadline = time.monotonic() + timeout
        while marker not in output[since:]:
            if time.monotonic() >= deadline:
                raise AssertionError(f"did not see {marker!r}: {output[-400:]!r}")
            self.pump(read_mirror=read_mirror)

    def start(self):
        self.until(b"PS> ")

    def finish(self, command=b"exit\n", read_mirror=True, timeout=5):
        if command:
            self.send(command)
        deadline = time.monotonic() + timeout
        while self.process.poll() is None:
            if time.monotonic() >= deadline:
                raise AssertionError(f"process did not exit: {self.local[-400:]!r}")
            self.pump(read_mirror=read_mirror)
        self.pump(read_mirror=read_mirror)
        return self.process.returncode


@contextmanager
def serial_115200(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        original = termios.tcgetattr(fd)
        try:
            tty.setraw(fd, termios.TCSANOW)
            attrs = termios.tcgetattr(fd)
            attrs[0] &= ~(termios.IXON | termios.IXOFF | termios.IXANY)
            attrs[2] &= ~(termios.CSIZE | termios.PARENB | termios.PARODD |
                          termios.CSTOPB | termios.CRTSCTS | getattr(termios, "CMSPAR", 0))
            attrs[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
            attrs[4] = attrs[5] = termios.B115200
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            yield fd
        finally:
            termios.tcsetattr(fd, termios.TCSANOW, original)
    finally:
        os.close(fd)


class RelayTests(unittest.TestCase):
    def write_burst(self, s, count):
        rows = [f"BURST-{i:06d} {i * 2654435761:016x} abcdefghijklmnopqrstuvwxyz0123456789"
                .encode("ascii") for i in range(1, count + 1)]
        Path(s.home.name, "burst.txt").write_bytes(b"\n".join(rows) + b"\nBURST_END\n")
        return b"\r\n".join(rows) + b"\r\n" + CAT_END

    def cat_burst(self, s, count=3500, *, read_mirror=False, timeout=5):
        s.start()
        output = self.write_burst(s, count)
        started = time.monotonic()
        s.send(CAT_COMMAND)
        s.until(CAT_END, read_mirror=read_mirror, timeout=timeout)
        elapsed = time.monotonic() - started
        self.assertIn(output, s.local)
        return output, elapsed

    def check_cat_mirror(self, s, output, *, label=None):
        expected = b"PS> " + CAT_COMMAND.rstrip(b"\n") + b"\r\n" + output + EXIT_TEXT
        received = bytes(s.mirror)
        drop_report = re.search(rb"nekotee: dropped (\d+) mirror bytes", s.local)
        dropped = int(drop_report[1]) if drop_report else 0
        numbers = {row: i for i, row in enumerate(output.split(b"\r\n")[:-2])}
        rows = received.split(b"\r\n")
        intact = [numbers[row] for row in rows[:-1] if row in numbers]
        wanted = set(expected.split(b"\r\n"))
        fragments = sum(row not in wanted for row in rows[:-1]) + bool(rows[-1])
        if label:
            print(f"{label}: intact rows {len(intact)}/{len(numbers)}, "
                  f"partial/malformed records {fragments}, received {len(received)}/{len(expected)} "
                  f"bytes, reported drops {dropped}, exit {s.process.returncode}", flush=True)
        self.assertNotIn(b"mirror disabled", s.local)
        self.assertEqual(len(expected) - len(received), dropped, "unexplained byte count")
        self.assertEqual(intact, sorted(set(intact)), "reordered or duplicate rows")
        if not dropped:
            self.assertEqual(received, expected)
        else:
            # With drops, fragments may join, but the remaining bytes cannot move or change.
            offset = 0
            for byte in received:
                offset = expected.find(byte, offset) + 1
                if not offset:
                    self.fail(f"mirror is not an ordered subsequence ({label})")
        return dropped

    def test_help_locales(self):
        for overrides, expected in [
            ({"LC_ALL": "zh_CN.UTF-8", "LANG": "en_US.UTF-8"}, "工作机制"),
            ({"LC_ALL": "", "LC_MESSAGES": "zh_TW.UTF-8", "LANG": "C"}, "工作机制"),
            ({"LC_ALL": "C", "LANG": "zh_CN.UTF-8"}, "How it works"),
        ]:
            with self.subTest(overrides=overrides):
                env = {**os.environ, **overrides}
                noargs = subprocess.check_output([BINARY], env=env)
                self.assertIn(expected.encode(), noargs)
                for option in (b"--buffer [N]", b"--frame FORMAT", b"--baud 115200",
                               b"--wait MS", b"--limit", b"--no-limit", b"--rate PCT",
                               b"--flow MODE", b"--resume PCT"):
                    self.assertIn(option, noargs)
                self.assertNotIn(b"--mirror-priority", noargs)
                self.assertEqual(noargs, subprocess.check_output([BINARY, "--help"], env=env))

    def test_reject_invalid_serial_settings(self):
        for flag, value in (("--baud", "115200bps"), ("--baud", "0"), ("--baud", "-1"),
                            ("--baud", "4294967296"),
                            ("--frame", "9N1"), ("--frame", "8X1"), ("--frame", "8N3")):
            with self.subTest(flag=flag, value=value):
                result = subprocess.run([BINARY, "--mirror", "/dev/null", flag, value],
                                        capture_output=True)
                self.assertEqual(result.returncode, 2)
                self.assertIn(b"see --help", result.stderr)

    def test_numeric_baud_settings(self):
        for baud in (300, 600, 12345):
            with self.subTest(baud=baud):
                with Session("--baud", str(baud), "--buffer", "0", "--limit") as s:
                    s.start()
                    self.assertIn(f"Link: {baud} baud, 8N1".encode(), s.local)
                    self.assertIn(f"PTY pacing: on, {baud / 10 * 0.99:g} B/s".encode(), s.local)
                    self.assertEqual(s.finish(), 0)
                    s.until(EXIT_TEXT, mirror=True)
                    self.assertEqual(s.mirror, EXIT_TEXT)
                    self.assertEqual(termios.tcgetattr(s.mirror_slave), s.mirror_original)

    def test_reject_invalid_queue_settings(self):
        for args in (("--wait", "-2"), ("--wait", "2147483648"), ("--wait", "1s"),
                     ("--flow", "invalid"), ("--rate", "0"), ("--rate", "101"), ("--rate", "99%"),
                     ("--resume", "-1"), ("--resume", "100"), ("--mirror-priority",)):
            with self.subTest(args=args):
                result = subprocess.run([BINARY, "--mirror", "/dev/null", *args], capture_output=True)
                self.assertEqual(result.returncode, 2)
                self.assertIn(b"see --help", result.stderr)

    def test_session_messages_stay_on_local_terminal(self):
        with Session() as s:
            s.start()
            start = b"[nekotee] PTY session started."
            end = b"[nekotee] PTY session ended (exit status 37)."
            self.assertIn(start, s.local)
            self.assertLess(s.local.index(start), s.local.index(b"PS> "))
            self.assertIn(os.fsencode(os.ttyname(s.mirror_slave)) + b"; shell: /bin/bash", s.local)
            self.assertIn(b"XON/XOFF (TX, user space)", s.local)
            self.assertIn(b"Queue: 128 KiB; exit wait: unlimited", s.local)
            self.assertIn(b"99% of nominal link rate), pause at 100%, resume at 50%", s.local)
            self.assertEqual(s.finish(b"exit 37\n"), 37)
            self.assertIn(end, s.local)
            self.assertNotIn(start, s.mirror)
            self.assertNotIn(end, s.mirror)
            self.assertNotIn(b"Queue:", s.mirror)
            self.assertNotIn(b"bytes queued", s.mirror)

    def test_startup_reports_effective_settings_in_chinese(self):
        with Session("--baud", "115200", "--frame", "8N2", "--rate", "80", "--wait", "-1",
                     "--resume", "25", "--buffer", "1", locale="zh_CN.UTF-8") as s:
            s.start()
            for text in ("PTY 会话已启动", "115200 baud, 8N2; XON/XOFF (发送，程序处理)",
                         "队列：1 KiB；退出等待：不限时",
                         "原端限速：开启，8378.18 字节/秒（标称线速的 80%），100% 暂停，25% 恢复"):
                self.assertIn(text.encode(), s.local)
                self.assertNotIn(text.encode(), s.mirror)
            self.assertEqual(s.finish(), 0)

    def test_flow_none_does_not_disable_pacing(self):
        with Session("--flow", "none", "--no-limit", "--limit") as s:
            s.start()
            self.assertIn(b"no serial flow control", s.local)
            self.assertIn(b"PTY pacing: on", s.local)
            os.write(s.mirror_master, b"\x13")
            self.assertEqual(s.finish(), 0)
            s.until(EXIT_TEXT, mirror=True)
            self.assertEqual(s.mirror, EXIT_TEXT)

    def test_hardware_flow_rejects_a_pty(self):
        with Session("--limit", "--flow", "hard") as s:
            self.assertEqual(s.finish(None), 1)
            self.assertIn(b"--flow hard requires a mirror serial port", s.local)
            self.assertNotIn(b"PTY session started", s.local)

    def test_reject_missing_tty_before_starting_session(self):
        with tempfile.TemporaryDirectory() as directory:
            device = str(Path(directory, "tty-missing"))
            with Session(device=device) as s:
                self.assertEqual(s.finish(None), 1)
                self.assertIn(os.fsencode(device), s.local)
                self.assertNotIn(b"PTY session started", s.local)
                self.assertNotIn(b"PS> ", s.local)
                self.assertEqual(termios.tcgetattr(s.local_slave), s.original)

    def test_edited_command_is_mirrored_only_at_line_feed(self):
        with Session() as s:
            s.start()
            local_start, mirror_start = len(s.local), len(s.mirror)
            s.send(b"echo cmake")
            s.until(b"echo cmake", since=local_start)
            local_start = len(s.local)
            s.send(b"\x7f" * 3)
            s.until(b"\x08", since=local_start)
            s.pump()
            self.assertEqual(s.mirror[mirror_start:], b"")
            s.send(b"\n")
            s.until(b"PS> echo cm\r\ncm\r\n", mirror=True, since=mirror_start)
            self.assertEqual(s.mirror[mirror_start:], b"PS> echo cm\r\ncm\r\n")
            self.assertEqual(s.finish(), 0)

    def test_cr_redraw_and_erasable_suggestion_are_not_sent(self):
        with Session() as s:
            s.start()
            # Paint a hint, redraw with CR, then finalize a shorter line.
            s.send(b"printf 'Output: cm\\033[90m\\x61ke\\033[0m'; "
                   b"sleep 0.1; printf '\\r\\033[KOutput: cm\\n'\n")
            s.until(b"\x1b[90make\x1b[0m")
            s.until(b"\r\nOutput: cm\r\n", mirror=True)
            self.assertEqual(s.finish(), 0)
            self.assertNotIn(b"Output: cmake", s.mirror)

    def test_soft_wrapped_line_survives_scrolling_before_lf(self):
        with Session() as s:
            s.start()
            s.resize(5, 30)
            s.send(b"printf 'BEGIN%0400dEND\\n' 12345\n")
            line = b"BEGIN" + b"0" * 395 + b"12345END\r\n"
            s.until(line, mirror=True)
            self.assertEqual(s.finish(), 0)
            self.assertEqual(s.mirror.count(line), 1)

    def test_visible_commands_text_and_no_echo(self):
        with Session() as s:
            s.start()
            Path(s.home.name, "expected-listing").touch()
            s.send(b"ls\n")
            s.until(b"PS> ls\r\nexpected-listing\r\n", mirror=True)

            s.send("printf '\\033[31m红字\\033[0m\\n\\033[8mHID\\x44EN\\033[28m\\n'\n".encode())
            s.until("\r\n红字\r\n".encode(), mirror=True)
            s.until(b"HIDDEN", since=0)
            s.send(b"printf '\\033]0;OSC-\\x48IDDEN\\007\\033_GFX-\\x48IDDEN\\033\\\\\n'\n")
            s.until(b"OSC-HIDDEN")

            local_start, start = len(s.local), len(s.mirror)
            s.send(b"read -rs -p $'Pass\\x77ord: ' value; printf '\\naccepted=%s\\n' \"${#value}\"\n")
            s.until(b"Password: ", since=local_start)
            self.assertNotIn(b"Password: ", s.mirror[start:])
            secret = b"not-for-mirror-zz"
            s.send(secret + b"\n")
            s.until(f"\r\nPassword: \r\naccepted={len(secret)}\r\n".encode(), mirror=True, since=start)
            self.assertEqual(s.finish(), 0)
            for invisible in (b"\x1b", b"\x07", b"\x08", secret, b"HIDDEN", b"GFX-HIDDEN"):
                self.assertNotIn(invisible, s.mirror)
            self.assertEqual(termios.tcgetattr(s.local_slave), s.original)

    def test_size_signals_and_exit_code(self):
        with Session() as s:
            s.start()
            s.resize(36, 100)
            s.send(b"stty size\n")
            s.until(b"\r\n36 100\r\n", mirror=True)

            s.send(b"printf 'sleep-\\x72eady\\n'; sleep 20\n")
            s.until(b"\r\nsleep-ready\r\n", mirror=True)
            time.sleep(0.04)
            s.send(b"\x1a")
            s.until(b"Stopped", mirror=True)
            s.send(b"fg\n")
            start = len(s.local)
            s.until(b"sleep 20", since=start)
            s.send(b"\x03")
            s.until(b"PS> ", since=start)
            self.assertEqual(s.finish(b"exit 37\n"), 37)
            self.assertEqual(termios.tcgetattr(s.local_slave), s.original)

    def test_term_restores_terminal(self):
        with Session() as s:
            mirror_original = termios.tcgetattr(s.mirror_slave)
            s.start()
            s.process.terminate()
            self.assertEqual(s.finish(None), 128 + signal.SIGTERM)
            self.assertIn(b"PTY session ended (exit status 143)", s.local)
            self.assertEqual(termios.tcgetattr(s.local_slave), s.original)
            self.assertEqual(termios.tcgetattr(s.mirror_slave), mirror_original)

    def test_reject_same_tty_and_non_tty(self):
        for device in ("self", "/dev/tty", "/dev/null", "/dev/ptmx"):
            with self.subTest(device=device), Session(device=device) as s:
                self.assertEqual(s.finish(None), 1)
                self.assertIn(b"mirror", s.local)
                self.assertEqual(termios.tcgetattr(s.local_slave), s.original)

    def test_zero_buffer_does_not_keep_backlog(self):
        with Session("--no-limit", "--buffer", "0") as s:
            self.cat_burst(s)
            self.assertEqual(s.finish(read_mirror=False), 0)
            s.pump()
            self.assertIn(b"dropped", s.local)
            self.assertIn(b"BURST-000001", s.mirror)
            self.assertNotIn(b"BURST-003500", s.mirror)

    def test_cat_burst_keeps_newest_data(self):
        for size, buffer in ((128, ()), (1, ("--buffer=1",))):
            with self.subTest(size=size), Session("--no-limit", *buffer) as s:
                output, _ = self.cat_burst(s)  # The local cat must finish with no mirror reads.
                s.until(CAT_END, mirror=True)
                self.assertIn(output[-size * 1024:], s.mirror)
                self.assertEqual(s.finish(), 0)
                s.until(EXIT_TEXT, mirror=True)
                self.assertGreater(self.check_cat_mirror(s, output), 0)

    def test_buffer_flag_without_size(self):
        with Session("--no-limit", "--buffer") as s:
            output, _ = self.cat_burst(s, 1600)
            s.until(CAT_END, mirror=True)
            self.assertEqual(s.finish(), 0)
            s.until(EXIT_TEXT, mirror=True)
            self.assertEqual(self.check_cat_mirror(s, output), 0)

    def test_exit_wait_deadline_with_stalled_mirror(self):
        for wait in (0, 180, 500):
            with self.subTest(wait=wait), Session("--no-limit", "--wait", str(wait)) as s:
                self.cat_burst(s)
                start = time.monotonic()
                self.assertEqual(s.finish(b"exit 37\n", read_mirror=False), 37)
                elapsed = time.monotonic() - start
                self.assertGreaterEqual(elapsed, wait / 1000 - 0.03)
                self.assertLess(elapsed, wait / 1000 + 0.6)
                self.assertRegex(s.local, rb"Mirror wait expired; [1-9][0-9]* queued bytes discarded")
                self.assertIn(b"dropped", s.local)

    def test_unlimited_exit_wait_is_cancellable(self):
        with Session() as s:
            s.start()
            os.write(s.mirror_master, b"\x13")
            s.send(b"printf 'short-payload\\n'; exit\n")
            s.until(b"short-payload\r\n")
            s.until(b"Waiting for mirror;")
            time.sleep(0.65)
            self.assertIsNone(s.process.poll())
            self.assertEqual(s.finish(b"\x03", timeout=1), 128 + signal.SIGINT)
            self.assertIn(b"dropped", s.local)
            self.assertEqual(termios.tcgetattr(s.local_slave), s.original)
            self.assertEqual(termios.tcgetattr(s.mirror_slave), s.mirror_original)

    def test_unlimited_exit_wait_drains_when_mirror_returns(self):
        with Session("--baud", "1000000") as s:
            s.start()
            os.write(s.mirror_master, b"\x13")
            output, _ = self.cat_burst(s, 600)
            s.send(b"exit\n")
            s.until(b"Waiting for mirror;")
            time.sleep(0.65)
            self.assertIsNone(s.process.poll())
            self.assertEqual(s.mirror, b"")
            os.write(s.mirror_master, b"\x11")
            self.assertEqual(s.finish(None), 0)
            s.until(EXIT_TEXT, mirror=True)
            self.assertIn(b"Mirror: 0 bytes queued.", s.local)
            self.assertEqual(self.check_cat_mirror(s, output), 0)

    def test_pacing_uses_filtered_bytes_at_the_same_rate_with_or_without_queue(self):
        for buffer in (0, 128):
            with self.subTest(buffer=buffer), Session("--baud", "19200", "--rate", "50",
                                                      "--buffer", str(buffer)) as s:
                s.start()
                output = self.write_burst(s, 20)
                path = Path(s.home.name, "burst.txt")
                # 9 KiB of SGR before the first visible line does not consume the serial budget.
                path.write_bytes(b"\x1b[31m\x1b[0m" * 1024 + path.read_bytes())
                start = time.monotonic()
                s.send(CAT_COMMAND)
                s.until(CAT_END, timeout=3)
                local_elapsed = time.monotonic() - start
                s.until(CAT_END, mirror=True, timeout=1)
                mirror_elapsed = time.monotonic() - start
                nominal = len(output) / 960  # 19200 baud, 8N1, 50%.
                self.assertGreaterEqual(local_elapsed, nominal - 0.08)
                self.assertLess(mirror_elapsed, nominal + 0.75)
                self.assertLess(mirror_elapsed - local_elapsed, 0.25)
                self.assertNotIn(b"PTY reads paused;", s.local)
                self.assertEqual(s.finish(), 0)
                s.until(EXIT_TEXT, mirror=True)
                self.assertEqual(self.check_cat_mirror(s, output), 0)

    def test_pacing_preserves_a_line_larger_than_the_queue(self):
        with Session("--limit", "--buffer", "1", "--resume", "25", "--wait", "-1",
                     "--baud", "1000000") as s:
            s.start()
            self.assertIn(b"XON/XOFF (TX, user space)", s.local)
            os.write(s.mirror_master, b"\x13")
            line = b"BEGIN" + b"0" * 8160 + b"END\r\n"
            command = b"printf 'BEGIN%08160dEND\\n' 0\n"
            s.send(command)
            s.until(b"bytes to resume.", read_mirror=False)
            left = int(re.search(rb"PTY reads paused; (\d+) bytes to resume", s.local)[1])
            self.assertEqual(left, len(b"PS> " + command[:-1] + b"\r\n" + line) - 256)
            s.pump()
            self.assertEqual(s.mirror, b"")
            os.write(s.mirror_master, b"\x11")
            s.until(line, mirror=True)
            s.until(b"PTY reads resumed.")
            self.assertEqual(s.finish(), 0)
            s.until(EXIT_TEXT, mirror=True)
            self.assertEqual(s.mirror, b"PS> " + command[:-1] + b"\r\n" + line + EXIT_TEXT)
            self.assertNotIn(b"dropped", s.local)

    def test_finite_wait_even_if_child_exits_during_backpressure(self):
        with Session("--limit", "--buffer", "1", "--wait", "100", "--baud", "1000000") as s:
            s.start()
            os.write(s.mirror_master, b"\x13")
            s.send(b"printf '%01200d\\n' 0; exit 37\n")
            self.assertEqual(s.finish(None, read_mirror=False, timeout=2), 37)
            self.assertIn(b"Waiting for mirror;", s.local)
            self.assertIn(b"Mirror wait expired;", s.local)

    def test_disconnect_does_not_end_shell(self):
        with Session() as s:
            s.start()
            os.close(s.mirror_master)
            s.mirror_master = -1
            s.send(b"printf 'still-\\x61live\\n'\n")
            s.until(b"\rstill-alive\r\n")
            self.assertEqual(s.finish(), 0)
            self.assertIn(b"mirror disabled", s.local)

    @unittest.skipUnless(shutil.which("socat"), "socat required for virtual serial pair")
    def test_virtual_serial(self):
        with tempfile.TemporaryDirectory() as ports:
            sender = str(Path(ports, "ttyUSB0"))
            receiver = str(Path(ports, "ttyUSB1"))
            cable = subprocess.Popen(["socat", f"pty,raw,echo=0,link={sender}",
                                      f"pty,raw,echo=0,link={receiver}"])
            try:
                deadline = time.monotonic() + 4
                while not (os.path.exists(sender) and os.path.exists(receiver)):
                    if cable.poll() is not None or time.monotonic() >= deadline:
                        self.fail("socat could not create the virtual serial pair")
                    time.sleep(0.01)
                peer = os.open(receiver, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                port = os.open(sender, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                try:
                    original = termios.tcgetattr(port)
                    with Session("--baud", "115200", device=sender, peer=peer) as s:
                        s.start()
                        attrs = termios.tcgetattr(port)
                        self.assertEqual(attrs[4:6], [termios.B115200, termios.B115200])
                        self.assertEqual(attrs[2] & termios.CSIZE, termios.CS8)
                        self.assertFalse(attrs[2] & (termios.PARENB | termios.CSTOPB | termios.CRTSCTS))
                        os.write(peer, b"from-serial-must-not-reach-shell\r\n")
                        s.send(b"printf '\\033[32mvirtual-\\x73erial\\033[0m\\n'\n")
                        s.until(b"\r\nvirtual-serial\r\n", mirror=True)
                        self.assertEqual(s.finish(), 0)
                        self.assertNotIn(b"from-serial-must-not-reach-shell", s.local)
                        self.assertNotIn(b"\x1b", s.mirror)
                    self.assertEqual(termios.tcgetattr(port), original)
                    with Session("--frame", "8N2", "--no-limit", device=sender, peer=peer) as s:
                        s.start()
                        attrs = termios.tcgetattr(port)
                        self.assertEqual(attrs[4:6], original[4:6])
                        self.assertEqual(attrs[0], original[0])
                        self.assertEqual(attrs[2] & (termios.CSIZE | termios.PARENB | termios.CSTOPB),
                                         termios.CS8 | termios.CSTOPB)
                        self.assertEqual(s.finish(), 0)
                    self.assertEqual(termios.tcgetattr(port), original)
                finally:
                    os.close(port)
                    os.close(peer)
            finally:
                cable.terminate()
                cable.wait(timeout=3)

    @unittest.skipUnless(SERIAL_TX and SERIAL_RX, "set NEKOTEE_SERIAL_TX and NEKOTEE_SERIAL_RX")
    def test_hardware_serial_cat_burst(self):
        self.assertFalse(os.path.samefile(SERIAL_TX, SERIAL_RX), "use distinct serial ports")
        modes = (("default", (), 3500, True),
                 ("unpaced, zero buffer", ("--no-limit", "--buffer", "0"), 3500, False),
                 ("unpaced, no exit wait", ("--no-limit", "--buffer", "256", "--wait", "0"), 3500, False),
                 ("unpaced, 1500 ms wait", ("--no-limit", "--buffer", "256", "--wait", "1500"), 3500, False),
                 ("unpaced, unlimited wait", ("--no-limit", "--buffer", "256"), 3500, True),
                 ("paced, 8 KiB", ("--buffer", "8"), 1000, True))
        # Use the same speed and framing before, during, and after the process,
        # so restoring termios at exit cannot cut a byte in flight at a different rate.
        with serial_115200(SERIAL_TX), serial_115200(SERIAL_RX) as peer:
            received_counts = {}
            for label, flags, count, intact in modes:
                with self.subTest(mode=label):
                    termios.tcflush(peer, termios.TCIFLUSH)
                    with Session("--baud", "115200", "--frame", "8N1", *flags,
                                 device=SERIAL_TX, peer=peer) as s:
                        s.start()
                        self.assertIn(b"115200 baud, 8N1", s.local)
                        output = self.write_burst(s, count)
                        started = time.monotonic()
                        s.send(CAT_COMMAND)
                        s.until(CAT_END, timeout=35)
                        elapsed = time.monotonic() - started
                        self.assertIn(output, s.local)
                        status = s.finish(timeout=25)
                        self.collect_serial_tail(s)
                        dropped = self.check_cat_mirror(s, output, label=f"{label}, local {elapsed:.3f}s")
                        self.assertEqual(status, 0)
                        self.assertEqual(dropped == 0, intact)
                        received_counts[label] = len(s.mirror)
            if {"unpaced, 1500 ms wait", "unpaced, no exit wait"} <= received_counts.keys():
                self.assertGreater(received_counts["unpaced, 1500 ms wait"], received_counts["unpaced, no exit wait"])

    @unittest.skipUnless(SERIAL_TX and SERIAL_RX, "set NEKOTEE_SERIAL_TX and NEKOTEE_SERIAL_RX")
    def test_hardware_serial_zero_buffer_pacing(self):
        self.assertFalse(os.path.samefile(SERIAL_TX, SERIAL_RX), "use distinct serial ports")
        with serial_115200(SERIAL_TX), serial_115200(SERIAL_RX) as peer:
            termios.tcflush(peer, termios.TCIFLUSH)
            with Session("--baud", "115200", "--frame", "8N1", "--limit", "--buffer", "0",
                         device=SERIAL_TX, peer=peer) as s:
                s.start()
                self.assertIn(b"Queue: 0 KiB; exit wait: unlimited", s.local)
                self.assertIn(b"PTY pacing: on, 11404.8 B/s (99% of nominal link rate), no queue watermarks",
                              s.local)
                output = self.write_burst(s, 1000)
                started = time.monotonic()
                s.send(CAT_COMMAND)
                s.until(CAT_END, timeout=10)
                local_elapsed = time.monotonic() - started
                s.until(CAT_END, mirror=True, timeout=3)
                mirror_elapsed = time.monotonic() - started
                self.assertEqual(s.finish(), 0)
                self.collect_serial_tail(s)

                self.assertIn(output, s.local)
                self.assertNotIn(b"PTY reads paused;", s.local)
                self.assertEqual(self.check_cat_mirror(s, output, label="--limit --buffer 0"), 0)
                wire_rate = 115200 / 10  # 8N1 includes one start bit and one stop bit.
                for side, elapsed in (("PTY", local_elapsed), ("Serial", mirror_elapsed)):
                    rate = len(output) / elapsed
                    print(f"{side}: {len(output)} bytes in {elapsed:.3f}s, {rate:.0f} B/s "
                          f"({rate / wire_rate:.1%} of nominal)", flush=True)
                    # Allow for poll rounding, scheduling and USB delivery latency.
                    self.assertGreaterEqual(rate, 0.90 * wire_rate)
                    self.assertLessEqual(rate, 1.01 * wire_rate)

    def collect_serial_tail(self, s):
        deadline = time.monotonic() + 5
        quiet_until = time.monotonic() + 0.5
        while time.monotonic() < quiet_until:
            self.assertLess(time.monotonic(), deadline, "serial did not settle")
            before = len(s.mirror)
            s.pump(timeout=0.05)
            if len(s.mirror) != before:
                quiet_until = time.monotonic() + 0.5

    @unittest.skipUnless(SERIAL_TX and SERIAL_RX, "set NEKOTEE_SERIAL_TX and NEKOTEE_SERIAL_RX")
    def test_hardware_serial_soft_flow(self):
        self.assertFalse(os.path.samefile(SERIAL_TX, SERIAL_RX), "use distinct serial ports")
        with serial_115200(SERIAL_TX) as port, serial_115200(SERIAL_RX) as peer:
            termios.tcflush(peer, termios.TCIFLUSH)
            with Session("--baud", "115200", "--limit", "--buffer", "4", "--resume", "25",
                         "--wait", "-1", device=SERIAL_TX, peer=peer) as s:
                s.start()
                s.send(b"echo serial-ready\n")
                s.until(b"\r\nserial-ready\r\n", mirror=True)
                self.assertFalse(termios.tcgetattr(port)[0] & termios.IXON)
                self.assertIn(b"XON/XOFF (TX, user space)", s.local)
                self.assertIn(b"resume at 25%", s.local)
                s.mirror.clear()
                reverse = b"echo mirror-input-must-be-ignored\r\n\x13"
                self.assertEqual(os.write(peer, reverse), len(reverse))
                termios.tcdrain(peer)
                time.sleep(0.1)  # Allow the sender to consume XOFF before generating the burst.
                try:
                    output = self.write_burst(s, 600)
                    s.send(CAT_COMMAND)
                    s.until(b"bytes to resume.")
                    s.pump(timeout=0.2)
                    self.assertEqual(s.mirror, b"", "mirror sent data after peer XOFF")
                    self.assertNotIn(CAT_END, s.local)
                    report = re.search(rb"PTY reads paused; (\d+) bytes to resume", s.local)[1]
                    self.assertGreaterEqual(int(report), 3072)  # 100% - 25% of 4 KiB, plus any staged suffix.
                finally:
                    # Release the actual transmitter even if the assertions above fail.
                    os.write(peer, b"\x11")
                s.until(b"PTY reads resumed.", timeout=8)
                s.until(CAT_END, timeout=12)
                self.assertEqual(s.finish(timeout=12), 0)
                self.collect_serial_tail(s)
                self.assertNotIn(b"mirror-input-must-be-ignored", s.local)
                self.assertEqual(self.check_cat_mirror(s, output, label=f"XOFF/XON, {int(report)} bytes to resume"), 0)

    @unittest.skipUnless(SERIAL_TX and SERIAL_RX, "set NEKOTEE_SERIAL_TX and NEKOTEE_SERIAL_RX")
    def test_hardware_serial_interrupt_exit_wait(self):
        self.assertFalse(os.path.samefile(SERIAL_TX, SERIAL_RX), "use distinct serial ports")
        with serial_115200(SERIAL_TX) as port, serial_115200(SERIAL_RX) as peer:
            original = termios.tcgetattr(port)
            termios.tcflush(peer, termios.TCIFLUSH)
            with Session(device=SERIAL_TX, peer=peer) as s:
                s.start()
                s.send(b"echo serial-ready\n")
                s.until(b"\r\nserial-ready\r\n", mirror=True)
                s.mirror.clear()
                os.write(peer, b"\x13")
                termios.tcdrain(peer)
                time.sleep(0.1)
                s.send(b"printf 'short-payload\\n'; exit\n")
                s.until(b"Waiting for mirror;")
                time.sleep(0.65)
                self.assertIsNone(s.process.poll())
                started = time.monotonic()
                status = s.finish(b"\x03", timeout=1)
                self.assertEqual(status, 128 + signal.SIGINT)
                self.assertEqual(s.mirror, b"")
                self.assertIn(b"dropped", s.local)
                self.assertEqual(termios.tcgetattr(s.local_slave), s.original)
                self.assertEqual(termios.tcgetattr(port), original)
                print(f"Peer XOFF held through exit; Ctrl+C returned {status} in "
                      f"{time.monotonic() - started:.3f}s", flush=True)

    @unittest.skipUnless(SERIAL_TX and SERIAL_RX, "set NEKOTEE_SERIAL_TX and NEKOTEE_SERIAL_RX")
    def test_hardware_serial_rtscts_configuration(self):
        self.assertFalse(os.path.samefile(SERIAL_TX, SERIAL_RX), "use distinct serial ports")
        with serial_115200(SERIAL_TX) as port, serial_115200(SERIAL_RX) as peer:
            try:
                fcntl.ioctl(port, termios.TIOCMGET, struct.pack("i", 0))
            except OSError as error:
                self.skipTest(f"serial sender does not expose modem signals: {error}")
            original = termios.tcgetattr(port)
            # Verify driver configuration and exit, not RTS/CTS behavior on the wire.
            with Session("--baud", "115200", "--frame", "8N1", "--flow", "hard", "--limit",
                         "--wait", "0", device=SERIAL_TX, peer=peer) as s:
                s.start()
                self.assertIn(b"115200 baud, 8N1; RTS/CTS", s.local)
                attrs = termios.tcgetattr(port)
                self.assertTrue(attrs[2] & termios.CRTSCTS)
                self.assertFalse(attrs[0] & (termios.IXON | termios.IXOFF | termios.IXANY))
                self.assertEqual(s.finish(timeout=2), 0)
            self.assertEqual(termios.tcgetattr(port), original)


if __name__ == "__main__":
    unittest.main(verbosity=2)
