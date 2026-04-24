"""Configuration parsing and startup-validation tests.

These verify that ardos honours its `want-*` flags and rejects obviously bad
config without crashing the runner.
"""
import subprocess
import time
from pathlib import Path

import pytest

from tests.common import config as cfg
from tests.common.ardos import Daemon, MDConnection, locate_binary


def _make_config(tmp_path: Path, **kwargs) -> Path:
    return cfg.generate_config(tmp_path / "cfg", **kwargs)


def test_md_only_boots(ardos):
    """Minimal config: only MD enabled. Port must open, nothing else."""
    daemon = ardos(md=True)
    conn = MDConnection("127.0.0.1", 7100)
    conn.close()
    daemon.stop()


def test_rejects_missing_dc_files(tmp_path: Path, external_services):
    """Ardos must bail with non-zero rc when dc-files is missing."""
    bad_cfg = tmp_path / "bad.yml"
    bad_cfg.write_text(
        "log-level: warn\n"
        "want-state-server: false\n"
        "want-client-agent: false\n"
        "want-database: false\n"
        "want-db-state-server: false\n"
        "want-metrics: false\n"
        "want-web-panel: false\n"
        "message-director:\n"
        "  host: 127.0.0.1\n"
        "  port: 7199\n"
        f"  rabbitmq-host: {cfg.RABBITMQ_HOST}\n"
        f"  rabbitmq-port: {cfg.RABBITMQ_PORT}\n"
        f"  rabbitmq-user: {cfg.RABBITMQ_USER}\n"
        f"  rabbitmq-password: {cfg.RABBITMQ_PASS}\n"
    )
    rc = subprocess.run(
        [str(locate_binary()), "--config", str(bad_cfg)],
        capture_output=True, timeout=10,
    ).returncode
    assert rc != 0


def test_all_roles_boot_together(ardos):
    """Full config (MD + SS + CA + DB + DBSS) must come up clean."""
    daemon = ardos(md=True, ss=True, ca=True, db=True, dbss=True)
    # Both the MD and CA ports should be open.
    MDConnection("127.0.0.1", 7100).close()
    MDConnection("127.0.0.1", 6667).close()
    daemon.stop()


def test_uberdog_config_loads(ardos):
    """UberDOG entries should be honoured — no DC class -> startup failure."""
    daemon = ardos(
        md=True, ss=True,
        uberdogs=[{"id": 4665, "class": "AuthManager", "anonymous": True}],
    )
    MDConnection("127.0.0.1", 7100).close()
    daemon.stop()
