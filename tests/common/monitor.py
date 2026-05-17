"""Test-side handle for the background sampler in tests.tools.monitor.

Spawns the sampler as a subprocess against a Daemon's PID, and provides
`mark()` for the test to drop phase markers into a sibling JSONL. Use
via the `bench_monitor` fixture; this module exists so the bench files
don't have to know about subprocess plumbing.

Both files use ``time.time_ns()`` so post-hoc analysis can merge them by
timestamp without per-process clock offsets.
"""

from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Optional


class BenchMonitor:
    """Lifetime: created in a fixture, started once the daemon PID is known,
    stopped on fixture teardown. ``mark()`` is cheap enough to call from
    inside a benchmark step (single line append + optional flush)."""

    def __init__(
        self,
        out_dir: Path,
        *,
        enabled: bool = True,
        rabbit_host: str = "127.0.0.1",
        rabbit_user: str = "guest",
        rabbit_pass: str = "guest",
        amqp_port: int = 5672,
        interval: float = 0.5,
    ) -> None:
        self.out_dir = Path(out_dir)
        self.enabled = enabled
        self.rabbit_host = rabbit_host
        self.rabbit_user = rabbit_user
        self.rabbit_pass = rabbit_pass
        self.amqp_port = amqp_port
        self.interval = interval
        self._proc: Optional[subprocess.Popen] = None
        self._marks: Optional[Any] = None
        self.data_path = self.out_dir / "monitor-data.jsonl"
        self.marks_path = self.out_dir / "monitor-marks.jsonl"

    # ------------------------------------------------------------------ start

    def start(self, daemon_pid: int) -> None:
        if not self.enabled or self._proc is not None:
            return
        self.out_dir.mkdir(parents=True, exist_ok=True)
        self._marks = open(self.marks_path, "w", buffering=1)
        self._write_mark("monitor_started", pid=daemon_pid)

        # Run the sampler as a module so its imports resolve from the repo.
        env = os.environ.copy()
        env["PYTHONUNBUFFERED"] = "1"
        self._proc = subprocess.Popen(
            [
                sys.executable,
                "-m",
                "tests.tools.monitor",
                "--pid",
                str(daemon_pid),
                "--out",
                str(self.data_path),
                "--interval",
                str(self.interval),
                "--rabbit-host",
                self.rabbit_host,
                "--rabbit-user",
                self.rabbit_user,
                "--rabbit-pass",
                self.rabbit_pass,
                "--amqp-port",
                str(self.amqp_port),
            ],
            env=env,
            # Inherit stderr so any sampler crash is visible in CI logs.
            stdout=subprocess.DEVNULL,
        )

    # ------------------------------------------------------------------ mark

    def mark(self, event: str, **fields: Any) -> None:
        """Drop a phase marker into the marks JSONL. Safe to call before
        ``start()`` (no-op) and after ``stop()`` (no-op)."""
        if not self.enabled or self._marks is None:
            return
        self._write_mark(event, **fields)

    def _write_mark(self, event: str, **fields: Any) -> None:
        rec = {"t_ns": time.time_ns(), "src": "phase", "event": event}
        rec.update(fields)
        try:
            self._marks.write(json.dumps(rec, separators=(",", ":")))
            self._marks.write("\n")
        except (OSError, ValueError):
            pass

    # ------------------------------------------------------------------ stop

    def stop(self) -> None:
        if self._marks is not None:
            self._write_mark("monitor_stopping")
            try:
                self._marks.close()
            except OSError:
                pass
            self._marks = None
        if self._proc is None:
            return
        if self._proc.poll() is None:
            self._proc.send_signal(signal.SIGTERM)
            try:
                self._proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait(timeout=2.0)
        self._proc = None
