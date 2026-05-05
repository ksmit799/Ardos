"""Generate ardos YAML configs for individual tests.

Each Daemon fixture calls `generate_config(...)` to build a minimal YAML with
only the roles the test needs. Everything else (metrics, web panel) is off by
default. Overrides merge deep so a test can tweak a nested key without
re-specifying the whole block.
"""
from __future__ import annotations

import copy
import os
from pathlib import Path
from typing import Any, Dict, List, Optional

import yaml

RABBITMQ_HOST = os.environ.get("ARDOS_TEST_RABBITMQ_HOST", "127.0.0.1")
RABBITMQ_PORT = int(os.environ.get("ARDOS_TEST_RABBITMQ_PORT", "5672"))
RABBITMQ_USER = os.environ.get("ARDOS_TEST_RABBITMQ_USER", "guest")
RABBITMQ_PASS = os.environ.get("ARDOS_TEST_RABBITMQ_PASS", "guest")
MONGODB_URI = os.environ.get(
    "ARDOS_TEST_MONGODB_URI", "mongodb://127.0.0.1:27017/ardos_test"
)


def _deep_merge(base: Dict[str, Any], over: Dict[str, Any]) -> Dict[str, Any]:
    out = copy.deepcopy(base)
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = _deep_merge(out[k], v)
        else:
            out[k] = v
    return out


def generate_config(
    out_dir: Path,
    *,
    dc_files: Optional[List[str]] = None,
    md: bool = True,
    ss: bool = False,
    ca: bool = False,
    db: bool = False,
    dbss: bool = False,
    log_level: str = "debug",
    md_port: int = 7100,
    ca_port: int = 6667,
    ss_channel: int = 1000,
    db_channel: int = 4003,
    uberdogs: Optional[List[Dict[str, Any]]] = None,
    overrides: Optional[Dict[str, Any]] = None,
) -> Path:
    """Write a YAML config into out_dir/config.yml and return the path.

    The returned path's parent also contains the DC files (copied from
    tests/files/) so the binary's cwd can resolve relative DC file paths.
    """
    from . import dc as dc_mod

    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if dc_files is None:
        dc_files = ["test.dc"]

    # Copy DC files into out_dir so `dc-files: [test.dc]` resolves when ardos
    # runs with cwd=out_dir.
    for name in dc_files:
        src = dc_mod.DC_DIR / name
        (out_dir / name).write_bytes(src.read_bytes())

    cfg: Dict[str, Any] = {
        "log-level": log_level,
        "dc-files": dc_files,
        "want-state-server": ss,
        "want-client-agent": ca,
        "want-database": db,
        "want-db-state-server": dbss,
        "want-metrics": False,
        "want-web-panel": False,
        "message-director": {
            "host": "127.0.0.1",
            "port": md_port,
            "rabbitmq-host": RABBITMQ_HOST,
            "rabbitmq-port": RABBITMQ_PORT,
            "rabbitmq-user": RABBITMQ_USER,
            "rabbitmq-password": RABBITMQ_PASS,
        },
    }

    if ss:
        cfg["state-server"] = {"channel": ss_channel}

    if ca:
        cfg["client-agent"] = {
            "host": "127.0.0.1",
            "port": ca_port,
            "version": "dev",
            "heartbeat-interval": 0,  # disable — tests drive heartbeats explicitly
            "auth-timeout": 45000,
            "historical-objects-ttl": 5000,
            "relocate-allowed": True,
            "channels": {"min": 1000000000, "max": 1009999999},
            "interest": {"client": "disabled"},
        }

    if db:
        cfg["database-server"] = {
            "channel": db_channel,
            "mongodb-uri": MONGODB_URI,
            "generate": {"min": 100000000, "max": 399999999},
        }

    if dbss:
        cfg["db-state-server"] = {
            "database": db_channel,
            "ranges": {"min": 100000000, "max": 399999999},
        }

    if uberdogs:
        cfg["uberdogs"] = uberdogs

    if overrides:
        cfg = _deep_merge(cfg, overrides)

    cfg_path = out_dir / "config.yml"
    cfg_path.write_text(yaml.safe_dump(cfg, sort_keys=False))
    return cfg_path


def expected_ports(
    *, md: bool = True, ca: bool = False, md_port: int = 7100, ca_port: int = 6667
) -> List[int]:
    """Ports a Daemon should wait on, given the role flags."""
    out: List[int] = []
    if md:
        out.append(md_port)
    if ca:
        out.append(ca_port)
    return out
