#!/usr/bin/env python3
import argparse
import errno
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from collections.abc import Iterable, Iterator
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from .cases import TestCase, default_cases
from .scripts import ControlScript, ControlContext

ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_CAPTURE_DURATION = 2.0


def log(msg: str) -> None:
    print(msg, flush=True)


def run_command(cmd: Iterable[str], **kwargs) -> subprocess.CompletedProcess:
    log(f"$ {' '.join(cmd)}")
    return subprocess.run(list(cmd), check=True, **kwargs)



class EventlogCapture(threading.Thread):
    def __init__(self, sock: socket.socket, output_path: Path):
        super().__init__(daemon=True)
        self.sock = sock
        self.output_path = output_path
        self._stop_event = threading.Event()
        self._finished = threading.Event()

    def run(self) -> None:
        try:
            with open(self.output_path, "wb") as out:
                while not self._stop_event.is_set():
                    try:
                        chunk = self.sock.recv(4096)
                    except OSError:
                        break
                    if not chunk:
                        break
                    out.write(chunk)
                    out.flush()
        finally:
            self._finished.set()

    def stop(self) -> None:
        self._stop_event.set()
        try:
            self.sock.shutdown(socket.SHUT_RD)
        except OSError:
            pass
        self.join(timeout=10)


@dataclass
class ControlBridge:
    sock: socket.socket
    script: Optional[ControlScript]
    thread: Optional[threading.Thread] = field(init=False)
    stop_event: threading.Event = field(init=False)

    def __post_init__(self) -> None:
        self.thread = None
        self.stop_event = threading.Event()

    def start(self) -> None:
        def runner() -> None:
            if not self.script:
                return

            ctx = ControlContext(self.sock.sendall)
            try:
                self.script(ctx)
            except Exception as exc:
                if isinstance(exc, BrokenPipeError) or (
                    isinstance(exc, OSError) and getattr(exc, "errno", None) == errno.EPIPE
                ):
                    # Target exited and closed the control socket; ignore this so the test can finish.
                    return
                log(f"Control script crashed: {exc}")

        self.thread = threading.Thread(target=runner, daemon=True)
        self.thread.start()

    def stop(self) -> None:
        if self.thread:
            self.thread.join(timeout=10)

@dataclass
class TestRunner:
    testCase: TestCase
    control_script: Optional[ControlScript]
    keep_eventlogs: bool = False
    print_stdout: bool = False
    env: dict[str,str] = field(init=False)
    socket_path: Optional[Path] = field(init=False)
    tcp_host: str = field(init=False)
    tcp_port: int = field(init=False)
    capture_duration: float = field(init=False)
    app_stdout: Path = field(init=False)

    def __post_init__(self) -> None:
        self.env = os.environ.copy()
        self.socket_path = None
        self.tcp_host = self.env.get("EVENTLOG_TCP_HOST", "127.0.0.1")
        self.tcp_port = int(self.env.get("EVENTLOG_TCP_PORT", "4242"))
        self.capture_duration = float(self.env.get("RECONNECT_CAPTURE_DURATION", DEFAULT_CAPTURE_DURATION))
        self.app_stdout = Path(tempfile.gettempdir()) / f"{self.testCase.target}.stdout"

    def init_socket_env(self) -> None:
        target = self.testCase.target
        if self.testCase.socket_type == "unix":
            sock_path = Path(f"/tmp/{target}_eventlog.sock")
            self.socket_path = sock_path
            self.env["GHC_EVENTLOG_UNIX_PATH"] = str(sock_path)
            self.env["GHC_EVENTLOG_WAIT"] = "true"
        elif self.testCase.socket_type == "tcp":
            host = self.tcp_host
            port = str(self.tcp_port)
            self.env["GHC_EVENTLOG_INET_HOST"] = host
            self.env["GHC_EVENTLOG_INET_PORT"] = port
            self.env["GHC_EVENTLOG_WAIT"] = "true"
        else:
            raise ValueError(f"Unknown socket type {self.testCase.socket_type}")

    def build_target(self) -> None:
        run_command(["cabal", "build", self.testCase.target], env=self.env, cwd=ROOT_DIR)

    def cleanup_paths(self) -> None:
        if self.app_stdout.exists():
            self.app_stdout.unlink()
        if self.socket_path and self.socket_path.exists():
            self.socket_path.unlink()

    def dump_stdout(self) -> None:
        if not self.print_stdout:
            return
        if not self.app_stdout.exists():
            log(f"[stdout] {self.app_stdout} missing")
            return
        log(f"--- begin stdout ({self.testCase.target}) ---")
        with open(self.app_stdout, "r", encoding="utf-8", errors="replace") as handle:
            contents = handle.read()
            if contents:
                sys.stdout.write(contents)
                if not contents.endswith("\n"):
                    sys.stdout.write("\n")
                sys.stdout.flush()
            else:
                log("(stdout empty)")
        log(f"--- end stdout ({self.testCase.target}) ---")

    def launch_target(self) -> subprocess.Popen:
        eventlog_rts = []
        if self.testCase.mode == "reconnect":
            eventlog_rts = ["+RTS", "--eventlog-flush-interval=1", "-RTS"]

        args = ["cabal", "run", self.testCase.target, "-f+debug", "--", "+RTS", "-l", "-hT", "--no-automatic-heap-samples", "-RTS", *eventlog_rts, *self.testCase.args]
        stdout_file = open(self.app_stdout, "w", encoding="utf-8", errors="replace")
        proc = subprocess.Popen(args, stdout=stdout_file, stderr=subprocess.STDOUT, cwd=ROOT_DIR, env=self.env)
        self._stdout_handle = stdout_file
        log(f"Launched {self.testCase.target} (pid={proc.pid})")
        return proc

    def wait_for_socket(self) -> None:
        if self.testCase.socket_type == "unix":
            assert self.socket_path is not None
            for _ in range(40):
                if self.socket_path.exists():
                    log(f"Socket {self.socket_path} ready")
                    return
                time.sleep(0.25)
            raise RuntimeError(f"Timed out waiting for {self.socket_path}")
        else:
            log(f"Waiting for TCP listener on {self.tcp_host}:{self.tcp_port}")
            time.sleep(1)

    def connect_socket(self) -> socket.socket:
        if self.testCase.socket_type == "unix":
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            assert self.socket_path is not None
            sock.connect(str(self.socket_path))
        else:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            deadline = time.time() + 10
            while True:
                try:
                    sock.connect((self.tcp_host, self.tcp_port))
                    break
                except OSError:
                    if time.time() > deadline:
                        raise
                    time.sleep(0.25)
        return sock

    def summarize_eventlog(self, path: Path) -> Optional[str]:
        if not path.exists():
            raise RuntimeError(f"Eventlog {path} missing")
        result = subprocess.run(["ghc-events", "show", str(path)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if result.returncode != 0:
            log(f"ghc-events show failed for {path}: {result.stderr}")
            return None
        else:
            line_count = len(result.stdout.splitlines())
            log(f"ghc-events output for {path}: {line_count} lines")
            return result.stdout

    def run(self) -> None:
        log(f"=== {self.testCase.description} ===")
        self.init_socket_env()
        self.build_target()
        self.cleanup_paths()
        proc = self.launch_target()
        try:
            self.wait_for_socket()
            if self.testCase.mode == "reconnect":
                self.run_reconnect(proc)
            else:
                self.run_normal(proc)
        finally:
            proc.wait(timeout=None)
            self._stdout_handle.close()
            self.dump_stdout()

    @contextmanager
    def _temporary_eventlog(self, tag: str) -> Iterator[Path]:
        fd, name = tempfile.mkstemp(suffix=".eventlog", prefix=f"{self.testCase.target}_{tag}_")
        os.close(fd)
        path = Path(name)
        try:
            yield path
        finally:
            if not self.keep_eventlogs and path.exists():
                path.unlink()

    def run_normal(self, proc: subprocess.Popen) -> None:
        with self._temporary_eventlog("normal") as eventlog_path:
            sock = self.connect_socket()
            bridge = ControlBridge(sock, self.control_script)
            bridge.start()
            capture = EventlogCapture(sock, eventlog_path)
            capture.start()
            rc = proc.wait()
            capture.stop()
            bridge.stop()
            sock.close()
            if rc != 0:
                raise RuntimeError(f"{self.testCase.target} exited with {rc}")
            output = self.summarize_eventlog(eventlog_path)
            self.testCase.verify_eventlog(eventlog_path, output)

    def capture_for_duration(self, output: Path, duration: float) -> None:
        sock = self.connect_socket()
        bridge = ControlBridge(sock, self.control_script)
        bridge.start()
        capture = EventlogCapture(sock, output)
        capture.start()
        time.sleep(duration)
        capture.stop()
        bridge.stop()
        sock.close()
        if not output.exists() or output.stat().st_size == 0:
            raise RuntimeError(f"Capture file {output} is empty")

    def run_reconnect(self, proc: subprocess.Popen) -> None:
        with self._temporary_eventlog("first") as first_eventlog, self._temporary_eventlog("second") as second_eventlog:
            self.capture_for_duration(first_eventlog, self.capture_duration)
            if proc.poll() is not None:
                raise RuntimeError(f"{self.testCase.target} exited unexpectedly; see {self.app_stdout}")
            time.sleep(1)
            self.capture_for_duration(second_eventlog, self.capture_duration)
            proc.terminate()
            proc.wait()
            first_output = self.summarize_eventlog(first_eventlog)
            self.testCase.verify_eventlog(first_eventlog, first_output)
            second_output = self.summarize_eventlog(second_eventlog)
            self.testCase.verify_eventlog(second_eventlog, second_output)


def main() -> None:
    parser = argparse.ArgumentParser(description="Python test harness for ghc-eventlog-socket")
    parser.add_argument("--only", help="Run only test cases containing this substring", default=None)
    parser.add_argument(
        "--keep-eventlogs",
        action="store_true",
        help="Do not delete temporary eventlog captures after summarizing them",
    )
    parser.add_argument(
        "--print-stdout",
        action="store_true",
        help="Print captured application stdout after each test case",
    )
    args = parser.parse_args()

    cases = default_cases()

    if args.only:
        cases = [c for c in cases if args.only.lower() in c.description.lower()]
        if not cases:
            raise SystemExit(f"No test cases match {args.only}")

    for case in cases:
        runner = TestRunner(case, case.control_script, keep_eventlogs=args.keep_eventlogs, print_stdout=args.print_stdout)
        try:
            runner.run()
        except Exception as exc:
            log(f"Test '{case.description}' failed: {exc}")
            raise

    log("All Python test cases completed")


if __name__ == "__main__":
    main()
