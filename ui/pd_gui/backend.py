"""Non-GTK helpers for the payload-dumper GUI.

The module owns everything that talks to the CLI: locating the binary,
listing partitions, assembling the argument vector and streaming the
machine-readable progress output.  It has no PyGObject dependency so the
logic can be tested on its own.
"""

from __future__ import annotations

import os
import re
import shutil
import signal
import subprocess
import threading
import time
from collections import deque
from dataclasses import dataclass
from typing import Callable, Iterable, Optional, Union

# <repo>/ui/pd_gui/backend.py -> <repo>
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

_PROGRESS_RE = re.compile(r"^([^:]+):(\d+)$")
_HUMAN_ENTRY_RE = re.compile(r"([^\s,()]+) \(([^()]*)\)")
_HUMAN_SIZE_RE = re.compile(r"^([0-9]+(?:\.[0-9]+)?) ?([kKMGTP]?B)$")

_UNITS = {"B": 0, "kB": 1, "MB": 2, "GB": 3, "TB": 4, "PB": 5}


class DumperError(Exception):
    """A failure that should be shown to the user as-is."""


@dataclass(frozen=True)
class Partition:
    name: str
    size: int
    delta: bool = False


@dataclass(frozen=True)
class Progress:
    partition: str
    percent: int


@dataclass(frozen=True)
class LogLine:
    text: str
    stream: str = "stderr"


@dataclass(frozen=True)
class Finished:
    returncode: int
    cancelled: bool
    elapsed: float
    error: Optional[str] = None


Event = Union[Progress, LogLine, Finished]


def find_binary(explicit: Optional[str] = None) -> Optional[str]:
    """Return the first usable payload-dumper executable, or None."""
    candidates = []
    if explicit:
        candidates.append(explicit)
    env_binary = os.environ.get("PD_BIN")
    if env_binary:
        candidates.append(env_binary)
    candidates.extend(
        [
            os.path.join(REPO_ROOT, "build", "release", "payload-dumper"),
            os.path.join(REPO_ROOT, "build", "payload-dumper"),
        ]
    )
    on_path = shutil.which("payload-dumper")
    if on_path:
        candidates.append(on_path)

    for candidate in candidates:
        path = os.path.abspath(os.path.expanduser(candidate))
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    return None


def format_size(num_bytes: int) -> str:
    """Format bytes using decimal units, matching the CLI's human output."""
    value = float(num_bytes)
    unit = 0
    while value >= 1000.0 and unit < 5:
        value /= 1000.0
        unit += 1
    names = ("B", "kB", "MB", "GB", "TB", "PB")
    if unit == 0:
        return f"{int(value)} B"
    if value < 10.0:
        return f"{value:.1f} {names[unit]}"
    return f"{value:.0f} {names[unit]}"


def _parse_human_size(text: str) -> int:
    match = _HUMAN_SIZE_RE.match(text.strip())
    if not match:
        return 0
    value = float(match.group(1))
    unit = match.group(2)
    # Normalise kB/kB etc. to decimal 1000-based units.
    exponent = _UNITS.get(unit) or _UNITS.get(unit[0].upper() + "B", 0)
    return int(value * (1000 ** exponent))


def _error_from_stderr(stderr: str, returncode: int) -> str:
    lines = [line.strip() for line in (stderr or "").splitlines() if line.strip()]
    for line in reversed(lines):
        if line.startswith("Error:"):
            return line[len("Error:") :].strip() or line
    if lines:
        # Prefer the beginning of the message over a trailing backtrace.
        return " ".join(lines[:4])
    return f"payload-dumper 退出码 {returncode}"


def _run_list(binary: str, input_path: str, machine: bool, timeout: float):
    args = [binary, "-l"]
    if machine:
        args.append("-m")
    args += ["--", input_path]
    return subprocess.run(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        check=False,
    )


def list_partitions(
    binary: str, input_path: str, timeout: float = 180.0
) -> list[Partition]:
    """List partitions using `-l -m`, with a fallback to the human listing."""
    input_path = os.path.abspath(os.path.expanduser(input_path))
    if not os.path.isfile(input_path):
        raise DumperError(f"输入文件不存在：{input_path}")

    machine = _run_list(binary, input_path, True, timeout)
    if machine.returncode != 0:
        raise DumperError(_error_from_stderr(machine.stderr, machine.returncode))

    sizes: dict[str, int] = {}
    order: list[str] = []
    for line in machine.stdout.splitlines():
        line = line.strip()
        match = _PROGRESS_RE.match(line)
        if not match:
            continue
        name = match.group(1)
        sizes[name] = int(match.group(2)) * 1024
        order.append(name)

    delta_names: set[str] = set()
    try:
        human = _run_list(binary, input_path, False, timeout)
        if human.returncode == 0:
            for match in _HUMAN_ENTRY_RE.finditer(human.stdout):
                if "delta" in match.group(2).lower():
                    delta_names.add(match.group(1))
    except (OSError, subprocess.SubprocessError):
        # Delta detection is a nicety; listing still works without it.
        pass

    if not order:
        # Very old builds may not support -l -m; try the human output.
        return _fallback_human_listing(binary, input_path, timeout)

    return [Partition(name, sizes.get(name, 0), name in delta_names) for name in order]


def _fallback_human_listing(binary: str, input_path: str, timeout: float) -> list[Partition]:
    human = _run_list(binary, input_path, False, timeout)
    if human.returncode != 0:
        raise DumperError(_error_from_stderr(human.stderr, human.returncode))
    parts = []
    for match in _HUMAN_ENTRY_RE.finditer(human.stdout):
        name = match.group(1)
        details = match.group(2)
        size_text = details.split(",")[0].strip()
        parts.append(Partition(name, _parse_human_size(size_text), "delta" in details.lower()))
    return parts


def default_output_dir(base_dir: Optional[str] = None) -> str:
    base = os.path.abspath(os.path.expanduser(base_dir or os.getcwd()))
    if not os.access(base, os.W_OK):
        # An app launched from a menu can have an unwritable cwd.
        base = os.path.expanduser("~")
    stamp = time.strftime("%Y%m%d_%H%M%S")
    candidate = os.path.join(base, f"extracted_{stamp}")
    suffix = 2
    while os.path.exists(candidate):
        candidate = os.path.join(base, f"extracted_{stamp}_{suffix}")
        suffix += 1
    return candidate


def build_extract_command(
    binary: str,
    input_path: str,
    output_dir: Optional[str] = None,
    workers: int = 0,
    partitions: Optional[Iterable[str]] = None,
    old_dir: Optional[str] = None,
    no_verify: bool = False,
    no_fec: bool = False,
) -> list[str]:
    """Build the argv for an extraction run.

    Machine-readable output is always on: it is what feeds the progress bars.
    """
    command = [binary, "-m"]
    if workers and workers > 0:
        command += ["-c", str(workers)]
    if output_dir:
        command += ["-o", output_dir]
    if old_dir:
        command += ["-old", old_dir]
    selected = [name for name in (partitions or []) if name]
    if selected:
        command += ["-p", ",".join(selected)]
    if no_verify:
        command.append("-no-verify")
    if no_fec:
        command.append("-no-fec")
    command += ["--", os.path.abspath(os.path.expanduser(input_path))]
    return command


class Cancellation:
    """Sends SIGINT to the running child, optionally escalating to SIGKILL."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._process: Optional[subprocess.Popen] = None
        self.requested = False

    def attach(self, process: subprocess.Popen) -> None:
        with self._lock:
            self._process = process
            requested = self.requested
        if requested and process.poll() is None:
            self._signal(process, signal.SIGINT)

    def request(self) -> None:
        with self._lock:
            self.requested = True
            process = self._process
        if process is not None and process.poll() is None:
            self._signal(process, signal.SIGINT)

    def force_kill(self) -> None:
        with self._lock:
            self.requested = True
            process = self._process
        if process is not None and process.poll() is None:
            self._signal(process, signal.SIGKILL)

    @staticmethod
    def _signal(process: subprocess.Popen, sig: int) -> None:
        try:
            process.send_signal(sig)
        except (ProcessLookupError, OSError):
            pass


def run_extraction(
    command: list[str],
    cwd: Optional[str],
    emit: Callable[[Event], None],
    cancellation: Optional[Cancellation] = None,
) -> None:
    """Run the CLI and report Progress/LogLine/Finished events.

    Every event is emitted from the calling thread (the caller normally wraps
    `emit` to marshal onto the GTK main loop).  Exactly one Finished event is
    emitted, after both output pipes have drained.
    """
    started = time.monotonic()
    try:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
    except OSError as exc:
        emit(Finished(-1, False, 0.0, f"无法启动 payload-dumper：{exc}"))
        return

    if cancellation is not None:
        cancellation.attach(process)

    stderr_tail: deque[str] = deque(maxlen=40)

    def read_stdout() -> None:
        assert process.stdout is not None
        for raw in process.stdout:
            line = raw.rstrip("\n")
            match = _PROGRESS_RE.match(line)
            if match:
                percent = max(0, min(100, int(match.group(2))))
                emit(Progress(match.group(1), percent))
            elif line.strip():
                emit(LogLine(line, "stdout"))

    def read_stderr() -> None:
        assert process.stderr is not None
        for raw in process.stderr:
            line = raw.rstrip("\n")
            if line.strip():
                stderr_tail.append(line)
                emit(LogLine(line, "stderr"))

    readers = [
        threading.Thread(target=read_stdout, name="pd-stdout", daemon=True),
        threading.Thread(target=read_stderr, name="pd-stderr", daemon=True),
    ]
    for reader in readers:
        reader.start()

    returncode = process.wait()
    for reader in readers:
        reader.join(timeout=5.0)
    elapsed = time.monotonic() - started
    cancelled = bool(cancellation is not None and cancellation.requested)

    error: Optional[str] = None
    if returncode != 0 and not cancelled:
        error = _error_from_stderr("\n".join(stderr_tail), returncode)

    emit(Finished(returncode, cancelled, elapsed, error))
