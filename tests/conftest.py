"""Pytest configuration and fixtures for the Ardos test suite."""
from __future__ import annotations

import base64
import json
import os
import socket
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Callable, Iterator, List, Optional

import pytest

from tests.common import config as cfg
from tests.common.ardos import (
    AIConnection,
    ChannelConnection,
    ClientConnection,
    Daemon,
    MDConnection,
)
from tests.common.msg_coverage import tracker

LOG_DIR = Path(__file__).resolve().parent / "logs"


# ---------------------------------------------------------------------------
# External services (rabbitmq + mongo)
# ---------------------------------------------------------------------------

def _tcp_open(host: str, port: int, timeout: float = 0.5) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def _wait_tcp(host: str, port: int, timeout: float, name: str) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if _tcp_open(host, port):
            return
        time.sleep(0.5)
    pytest.exit(f"{name} not reachable at {host}:{port} after {timeout}s", 1)


@pytest.fixture(scope="session")
def external_services() -> None:
    """Verify RabbitMQ and MongoDB are reachable. Purge between sessions."""
    _wait_tcp(cfg.RABBITMQ_HOST, cfg.RABBITMQ_PORT, 30, "RabbitMQ")
    _wait_tcp("127.0.0.1", 27017, 30, "MongoDB")
    # Drop the test database once per session so a stale mongo doesn't
    # contaminate freshly-built tests.
    try:
        from pymongo import MongoClient

        client = MongoClient(cfg.MONGODB_URI, serverSelectionTimeoutMS=5000)
        client.drop_database("ardos_test")
    except Exception as e:  # pragma: no cover
        pytest.exit(f"failed to reach MongoDB: {e}", 1)


def _rabbit_mgmt_request(path: str, timeout: float = 1.0) -> Optional[object]:
    """GET against RabbitMQ's management HTTP API. Returns parsed JSON, or
    None if the API isn't available (no management plugin, wrong creds,
    etc) — callers fall back to a short sleep.
    """
    auth = base64.b64encode(
        f"{cfg.RABBITMQ_USER}:{cfg.RABBITMQ_PASS}".encode()
    ).decode()
    req = urllib.request.Request(
        f"http://{cfg.RABBITMQ_HOST}:15672/api/{path}",
        headers={"Authorization": f"Basic {auth}"},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read())
    except (urllib.error.URLError, OSError, ValueError):
        return None


def _wait_rabbit_drained(timeout: float = 2.0) -> None:
    """Wait until the previous daemon's auto-generated exclusive queues are
    reaped by RabbitMQ. Replaces a blind sleep with a real signal — the
    queues vanish from the management view shortly after the daemon's
    AMQP connection drops on SIGTERM.

    Falls back to a short sleep if the management API isn't reachable
    (e.g. running against a vanilla rabbitmq:3 image).
    """
    deadline = time.monotonic() + timeout
    first = True
    while time.monotonic() < deadline:
        queues = _rabbit_mgmt_request("queues")
        if queues is None:
            if first:
                # Management plugin not available — one short sleep is the
                # best we can do without it.
                time.sleep(0.2)
            return
        first = False
        # Ardos's per-process queue is declared with no name (server-
        # generated, prefix `amq.gen-`). Wait until none of those linger.
        stragglers = [q for q in queues if q.get("name", "").startswith("amq.gen-")]
        if not stragglers:
            return
        time.sleep(0.05)


@pytest.fixture(autouse=True)
def _purge_between_tests(external_services) -> Iterator[None]:
    """Between each test: drop the mongo db + wait for RabbitMQ to reap any
    exclusive queues the just-stopped daemon left behind. Ardos uses
    per-node exclusive queues, so once those are gone the next daemon
    boots into a known-clean state."""
    yield
    try:
        from pymongo import MongoClient

        MongoClient(cfg.MONGODB_URI).drop_database("ardos_test")
    except Exception:
        pass
    _wait_rabbit_drained()


# ---------------------------------------------------------------------------
# Daemon factory
# ---------------------------------------------------------------------------

@pytest.fixture
def ardos(tmp_path: Path, request) -> Iterator[Callable[..., Daemon]]:
    """Yields a factory that spawns a configured ardos process.

    Usage:
        def test_foo(ardos):
            daemon = ardos(md=True, ss=True)
            ...
    """
    started: List[Daemon] = []

    def _factory(**kwargs) -> Daemon:
        md = kwargs.pop("md", True)
        ss = kwargs.pop("ss", False)
        ca = kwargs.pop("ca", False)
        db = kwargs.pop("db", False)
        dbss = kwargs.pop("dbss", False)
        uberdogs = kwargs.pop("uberdogs", None)
        overrides = kwargs.pop("overrides", None)
        dc_files = kwargs.pop("dc_files", None)
        md_port = kwargs.pop("md_port", 7100)
        ca_port = kwargs.pop("ca_port", 6667)

        out_dir = tmp_path / f"ardos-{len(started)}"
        config_path = cfg.generate_config(
            out_dir,
            md=md, ss=ss, ca=ca, db=db, dbss=dbss,
            md_port=md_port, ca_port=ca_port,
            dc_files=dc_files, uberdogs=uberdogs,
            overrides=overrides,
        )
        log_path = LOG_DIR / f"{request.node.name}-{len(started)}.log"
        ports = cfg.expected_ports(md=md, ca=ca, md_port=md_port, ca_port=ca_port)
        daemon = Daemon(
            config_path=config_path,
            log_path=log_path,
            ports=ports,
            md_port=md_port if md else None,
        )
        daemon.start()
        started.append(daemon)
        return daemon

    yield _factory

    failed = hasattr(request.node, "rep_call") and request.node.rep_call.failed
    for d in started:
        d.stop()
    # On failure, keep the logs; on success, drop them to avoid clutter.
    if not failed:
        for d in started:
            try:
                d.log_path.unlink(missing_ok=True)
            except OSError:
                pass


# ---------------------------------------------------------------------------
# Connection helpers
# ---------------------------------------------------------------------------

@pytest.fixture
def md_conn() -> Iterator[Callable[..., MDConnection]]:
    conns: List[MDConnection] = []

    def _factory(host: str = "127.0.0.1", port: int = 7100) -> MDConnection:
        c = MDConnection(host, port)
        conns.append(c)
        return c

    yield _factory
    for c in conns:
        try:
            c.close()
        except Exception:
            pass


@pytest.fixture
def channel_conn() -> Iterator[Callable[..., ChannelConnection]]:
    conns: List[ChannelConnection] = []

    def _factory(*channels: int, host: str = "127.0.0.1", port: int = 7100) -> ChannelConnection:
        c = ChannelConnection(host, port, *channels)
        conns.append(c)
        return c

    yield _factory
    for c in conns:
        try:
            c.close()
        except Exception:
            pass


@pytest.fixture
def ai_conn() -> Iterator[Callable[..., AIConnection]]:
    """Factory for "dummy AI" connections used to drive avatar ownership,
    interest, field updates, and session objects from the internal side."""
    conns: List[AIConnection] = []

    def _factory(
        *,
        ai_channel: int = AIConnection.DEFAULT_AI_CHANNEL,
        ss_channel: int = AIConnection.DEFAULT_SS_CHANNEL,
        host: str = "127.0.0.1",
        port: int = 7100,
    ) -> AIConnection:
        c = AIConnection(host, port, ai_channel=ai_channel, ss_channel=ss_channel)
        conns.append(c)
        return c

    yield _factory
    for c in conns:
        try:
            c.close()
        except Exception:
            pass


@pytest.fixture
def client_conn() -> Iterator[Callable[..., ClientConnection]]:
    conns: List[ClientConnection] = []

    def _factory(host: str = "127.0.0.1", port: int = 6667) -> ClientConnection:
        c = ClientConnection(host, port)
        conns.append(c)
        return c

    yield _factory
    for c in conns:
        try:
            c.close()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Report hooks: save failure status for cleanup, print coverage at end.
# ---------------------------------------------------------------------------

@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    setattr(item, f"rep_{call.when}", outcome.get_result())


def pytest_sessionfinish(session, exitstatus):
    # Write msg coverage report regardless of pass/fail.
    report = tracker.report()
    terminalreporter = session.config.pluginmanager.getplugin("terminalreporter")
    if terminalreporter:
        terminalreporter.write(report + "\n")
    coverage_file = LOG_DIR / "message-coverage.txt"
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    coverage_file.write_text(report)

    threshold = float(os.environ.get("ARDOS_MSG_COVERAGE_THRESHOLD", "0"))
    ratio = tracker.ratio()
    if threshold and ratio < threshold and exitstatus == 0:
        session.exitstatus = 1
        if terminalreporter:
            terminalreporter.write_line(
                f"message coverage {ratio:.0%} below threshold {threshold:.0%}",
                red=True,
            )
