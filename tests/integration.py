"""Run integration tests against a built nekotee: python3 tests/integration.py ./build/nekotee."""

import errno
import fcntl
import os
from pathlib import Path
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest


BINARY = os.path.abspath(sys.argv.pop(1))


class Session:
    def __init__(self, *options, device=None, peer=None):
        self.options = options
        self.device = device
        self.peer = peer
        self.local = bytearray()
        self.mirror = bytearray()

    def __enter__(self):
        self.home = tempfile.TemporaryDirectory()
        self.local_master, self.local_slave = os.openpty()
        self.mirror_master, self.mirror_slave = os.openpty() if self.peer is None else (self.peer, None)
        self.resize(24, 132)
        self.original = termios.tcgetattr(self.local_slave)
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
                 "PS1": "PS> ", "PROMPT_COMMAND": "", "LC_ALL": "C.UTF-8"},
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

    def finish(self, command=b"exit\n", read_mirror=True):
        if command:
            self.send(command)
        deadline = time.monotonic() + 5
        while self.process.poll() is None:
            if time.monotonic() >= deadline:
                raise AssertionError(f"process did not exit: {self.local[-400:]!r}")
            self.pump(read_mirror=read_mirror)
        self.pump(read_mirror=read_mirror)
        return self.process.returncode


# Enough output to fill the Linux PTY queue even when its peer is not read.
def burst(count):
    return (f"seq -f 'ROW-%06.0f abcdefghijklmnopqrstuvwxyz' 1 {count}; "
            "printf '\\nLOCAL_\\x44ONE\\n'\n").encode()


class RelayTests(unittest.TestCase):
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
                self.assertEqual(noargs, subprocess.check_output([BINARY, "--help"], env=env))

    def test_session_messages_stay_on_local_terminal(self):
        with Session() as s:
            s.start()
            start = b"[nekotee] PTY session started."
            end = b"[nekotee] PTY session ended (exit status 37)."
            self.assertIn(start, s.local)
            self.assertLess(s.local.index(start), s.local.index(b"PS> "))
            self.assertEqual(s.finish(b"exit 37\n"), 37)
            self.assertIn(end, s.local)
            self.assertNotIn(start, s.mirror)
            self.assertNotIn(end, s.mirror)

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

    def test_default_never_waits_for_mirror(self):
        with Session() as s:
            s.start()
            s.send(burst(9000))
            s.until(b"\r\nLOCAL_DONE\r\n", read_mirror=False)
            self.assertEqual(s.finish(read_mirror=False), 0)
            s.pump()
            self.assertIn(b"dropped", s.local)
            self.assertIn(b"ROW-000001", s.mirror)
            self.assertNotIn(b"ROW-009000", s.mirror)

    def test_full_buffer_keeps_newest_data(self):
        with Session("--buffer-kbytes=1") as s:
            s.start()
            s.send(burst(9000))
            s.until(b"\r\nLOCAL_DONE\r\n", read_mirror=False)
            s.until(b"ROW-009000", mirror=True)
            self.assertNotIn(b"ROW-004500", s.mirror)
            self.assertEqual(s.finish(), 0)
            self.assertIn(b"dropped", s.local)

    def test_buffer_flag_without_size(self):
        with Session("--buffer-kbytes") as s:
            s.start()
            s.send(burst(1600))
            s.until(b"\r\nLOCAL_DONE\r\n", read_mirror=False)
            s.until(b"ROW-001600", mirror=True)
            self.assertIn(b"ROW-000800", s.mirror)
            self.assertEqual(s.finish(), 0)
            self.assertNotIn(b"dropped", s.local)

    def test_mirror_priority_applies_backpressure(self):
        with Session("--mirror-priority") as s:
            s.start()
            s.send(burst(9000))
            s.until(b"ROW-001000", read_mirror=False)
            deadline = time.monotonic() + 0.20
            while time.monotonic() < deadline:
                s.pump(read_mirror=False)
            self.assertNotIn(b"ROW-009000", s.local)

            s.until(b"\r\nLOCAL_DONE\r\n", mirror=True)
            mirrored_rows = [ln.split(b" ", 1)[0] for ln in s.mirror.splitlines() if ln.startswith(b"ROW-")]
            self.assertEqual(mirrored_rows, [f"ROW-{i:06}".encode() for i in range(1, 9001)])
            self.assertEqual(s.finish(), 0)

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
                finally:
                    os.close(port)
                    os.close(peer)
            finally:
                cable.terminate()
                cable.wait(timeout=3)


if __name__ == "__main__":
    unittest.main(verbosity=2)
