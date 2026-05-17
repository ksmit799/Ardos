"""Background system sampler for diagnostic benchmark runs.

Spawned as a subprocess by the bench harness; samples broker, daemon
process, and socket state on a fixed interval and writes one JSONL
event per sample. Stops cleanly on SIGTERM.

Output is consumed post-hoc to correlate broker flow-state / queue depth
/ daemon RSS / AMQP socket Send-Q against phase markers emitted by the
test harness into a sibling file.

CLI:

    python -m tests.tools.monitor \
        --pid <daemon-pid> \
        --out  <path/to/monitor-data.jsonl> \
        --interval 0.5 \
        --rabbit-host 127.0.0.1 \
        --rabbit-user guest \
        --rabbit-pass guest \
        --amqp-port 5672

Each output line is a JSON object:

    {"t_ns": <int>, "src": "<source>", ...source-specific fields}

Sources:
  - "proc"      : daemon RSS, threads, ctx switches, utime/stime
  - "rmq_conn"  : per-connection state (look for state=="flow")
  - "rmq_queue" : per-queue messages/unacked/memory
  - "rmq_overview": cluster-wide message rates + memory + alarms
  - "amqp_sock" : ss output for the daemon's connection to the broker
  - "sys"       : /proc/loadavg, /proc/meminfo MemAvailable, steal %
  - "thread"    : per-thread wchan + kernel stack (where the daemon is parked)
  - "daemon_sock": per-port aggregate Send-Q/Recv-Q across all daemon-owned
                    TCP sockets, bucketed by daemon-side port (6667 CA accepts,
                    7100 MD accepts, 5672 outbound to broker, "other")
  - "error"     : sampler-side errors (so a missing tool doesn't kill the run)
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Dict, List, Optional

_running = True


def _stop(_signum, _frame) -> None:
    global _running
    _running = False


def _now_ns() -> int:
    # time.time_ns is wall-clock; comparable across processes (unlike
    # monotonic_ns, whose reference point is per-process per Python docs).
    return time.time_ns()


def _emit(out, src: str, **fields: Any) -> None:
    rec: Dict[str, Any] = {"t_ns": _now_ns(), "src": src}
    rec.update(fields)
    out.write(json.dumps(rec, separators=(",", ":")))
    out.write("\n")


# ---------------------------------------------------------------------------
# Samplers
# ---------------------------------------------------------------------------


def _sample_proc(out, pid: int) -> None:
    """Daemon process metrics from /proc."""
    try:
        with open(f"/proc/{pid}/status") as f:
            status = {}
            for line in f:
                k, _, v = line.partition(":")
                status[k.strip()] = v.strip()
        with open(f"/proc/{pid}/stat") as f:
            stat = f.read().split()
        # /proc/[pid]/stat fields (1-indexed, see proc(5)):
        # 14 utime, 15 stime, 22 starttime, 23 vsize, 24 rss (pages)
        utime = int(stat[13])
        stime = int(stat[14])
        num_threads = int(stat[19])

        def _kb(s: str) -> Optional[int]:
            try:
                return int(s.split()[0])
            except (ValueError, IndexError):
                return None

        _emit(
            out,
            "proc",
            pid=pid,
            rss_kb=_kb(status.get("VmRSS", "")),
            vm_kb=_kb(status.get("VmSize", "")),
            threads=num_threads,
            utime=utime,
            stime=stime,
            vol_ctxt=int(status.get("voluntary_ctxt_switches", 0) or 0),
            invol_ctxt=int(status.get("nonvoluntary_ctxt_switches", 0) or 0),
        )

        # fd count — separate try because /proc/[pid]/fd needs perms
        try:
            fd_count = len(os.listdir(f"/proc/{pid}/fd"))
            _emit(out, "proc_fd", pid=pid, fd_count=fd_count)
        except OSError:
            pass
    except FileNotFoundError:
        # Daemon process is gone — stop the loop.
        global _running
        _running = False
    except (OSError, ValueError) as e:
        _emit(out, "error", source="proc", err=str(e))


def _rmq_get(
    host: str, user: str, password: str, path: str, timeout: float = 0.8
) -> Optional[object]:
    auth = base64.b64encode(f"{user}:{password}".encode()).decode()
    req = urllib.request.Request(
        f"http://{host}:15672/api/{path}",
        headers={"Authorization": f"Basic {auth}"},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read())
    except (urllib.error.URLError, OSError, ValueError):
        return None


def _sample_rmq(out, host: str, user: str, password: str) -> None:
    """Broker connections, queues, overview."""
    overview = _rmq_get(host, user, password, "overview")
    if overview is not None and isinstance(overview, dict):
        _emit(
            out,
            "rmq_overview",
            object_totals=overview.get("object_totals"),
            queue_totals=overview.get("queue_totals"),
            message_stats=overview.get("message_stats"),
            cluster_name=overview.get("cluster_name"),
        )

    conns = _rmq_get(host, user, password, "connections")
    if isinstance(conns, list):
        for c in conns:
            _emit(
                out,
                "rmq_conn",
                name=c.get("name"),
                state=c.get("state"),
                channels=c.get("channels"),
                send_oct=c.get("send_oct"),
                recv_oct=c.get("recv_oct"),
                send_pend=c.get("send_pend"),
                send_cnt=c.get("send_cnt"),
                recv_cnt=c.get("recv_cnt"),
                # send_oct_details.rate is the publish byte rate
                send_oct_rate=(c.get("send_oct_details") or {}).get("rate"),
                recv_oct_rate=(c.get("recv_oct_details") or {}).get("rate"),
            )

    queues = _rmq_get(host, user, password, "queues")
    if isinstance(queues, list):
        for q in queues:
            _emit(
                out,
                "rmq_queue",
                name=q.get("name"),
                state=q.get("state"),
                messages=q.get("messages"),
                messages_ready=q.get("messages_ready"),
                messages_unack=q.get("messages_unacknowledged"),
                memory=q.get("memory"),
                consumers=q.get("consumers"),
            )


def _sample_amqp_socket(out, amqp_port: int) -> None:
    """ss snapshot of all TCP sockets to/from the broker port."""
    try:
        # -t TCP, -n numeric, -i info, -p process. Filter via state filter.
        r = subprocess.run(
            [
                "ss",
                "-tnpi",
                f"( sport = :{amqp_port} or dport = :{amqp_port} )",
            ],
            capture_output=True,
            text=True,
            timeout=2.0,
        )
        if r.returncode != 0:
            _emit(out, "error", source="amqp_sock", err=r.stderr.strip())
            return
        lines = r.stdout.strip().splitlines()
        if len(lines) <= 1:
            return
        # Header is line 0; data is line 1+ (and ss prints continuation lines
        # for the info section). Don't try to parse the info section here —
        # just stash the raw block per AMQP-direction socket.
        body = "\n".join(lines[1:])
        _emit(out, "amqp_sock", raw=body)
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError) as e:
        _emit(out, "error", source="amqp_sock", err=str(e))


def _sample_thread_stacks(out, pid: int) -> None:
    """Per-thread kernel stack + wchan. Tells us conclusively whether the
    libuv loop is parked in epoll_wait (idle), futex_wait (lock contention),
    tcp_sendmsg (write backpressure), or something else.

    /proc/PID/task/TID/stack requires CAP_SYS_PTRACE or
    kernel.yama.ptrace_scope=0 on Ubuntu. wchan is unrestricted and always
    available — fall back to that when stack is denied.
    """
    try:
        tids = os.listdir(f"/proc/{pid}/task")
    except FileNotFoundError:
        global _running
        _running = False
        return
    except OSError as e:
        _emit(out, "error", source="thread", err=str(e))
        return

    for tid in tids:
        base = f"/proc/{pid}/task/{tid}"
        try:
            with open(f"{base}/comm") as f:
                comm = f.read().strip()
            with open(f"{base}/wchan") as f:
                wchan = f.read().strip()
        except (FileNotFoundError, OSError):
            continue  # thread exited mid-sample, skip
        stack: Optional[str] = None
        try:
            with open(f"{base}/stack") as f:
                stack = f.read().strip()
        except (FileNotFoundError, PermissionError, OSError):
            pass  # ptrace_scope restriction — wchan still useful
        fields: Dict[str, Any] = {"tid": int(tid), "comm": comm, "wchan": wchan}
        if stack:
            # Cap stack size: typical kernel stacks are <2KB but pathological
            # cases can blow up the log. Truncate to first 32 frames.
            lines = stack.splitlines()[:32]
            fields["stack"] = "\n".join(lines)
        _emit(out, "thread", **fields)


def _sample_daemon_sockets(out, pid: int) -> None:
    """All TCP sockets owned by the daemon, bucketed per daemon-side port.
    Captures Send-Q/Recv-Q distribution so we can see:
      - Are client TCP writes backed up (Send-Q on 6667 sockets)?
      - Is the AI socket unread (Recv-Q on 7100 sockets)?
      - Is the broker connection clear (5672 outbound)?
    """
    try:
        r = subprocess.run(
            ["ss", "-tnp"],
            capture_output=True,
            text=True,
            timeout=3.0,
        )
        if r.returncode != 0:
            _emit(out, "error", source="daemon_sock", err=r.stderr.strip()[:200])
            return
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError) as e:
        _emit(out, "error", source="daemon_sock", err=str(e))
        return

    pid_marker = f"pid={pid},"
    # bucket -> list of (recv_q, send_q)
    buckets: Dict[str, List[tuple]] = {}
    for line in r.stdout.splitlines()[1:]:
        if pid_marker not in line:
            continue
        parts = line.split()
        if len(parts) < 5:
            continue
        try:
            rq, sq = int(parts[1]), int(parts[2])
        except ValueError:
            continue
        local, peer = parts[3], parts[4]

        def _port(ep: str) -> str:
            return ep.rsplit(":", 1)[-1] if ":" in ep else ""

        lport, pport = _port(local), _port(peer)
        # Categorise by daemon-side role. Accepted listeners: local port is
        # the daemon's listen port. Outbound: peer port is the broker.
        if lport == "6667":
            bucket = "ca_accept"  # daemon writing OBJECT_LEAVING etc. to client
        elif lport == "7100":
            bucket = "md_accept"  # AI socket lives here
        elif pport == "5672":
            bucket = "amqp_out"
        elif lport == "5672":
            bucket = "amqp_in"  # shouldn't happen but be defensive
        else:
            bucket = "other"
        buckets.setdefault(bucket, []).append((rq, sq))

    def _pct(arr: List[int], p: int) -> int:
        if not arr:
            return 0
        s = sorted(arr)
        i = min(len(s) - 1, (p * len(s)) // 100)
        return s[i]

    for bucket, samples in buckets.items():
        recvs = [s[0] for s in samples]
        sends = [s[1] for s in samples]
        _emit(
            out,
            "daemon_sock",
            bucket=bucket,
            count=len(samples),
            recv_total=sum(recvs),
            recv_max=max(recvs) if recvs else 0,
            recv_p50=_pct(recvs, 50),
            recv_p99=_pct(recvs, 99),
            send_total=sum(sends),
            send_max=max(sends) if sends else 0,
            send_p50=_pct(sends, 50),
            send_p99=_pct(sends, 99),
            # Count how many sockets have nonzero queues
            recv_nonzero=sum(1 for v in recvs if v > 0),
            send_nonzero=sum(1 for v in sends if v > 0),
        )


_LAST_CPU: Optional[List[int]] = None


def _sample_sys(out) -> None:
    """Loadavg + meminfo + cpu steal %."""
    global _LAST_CPU
    try:
        with open("/proc/loadavg") as f:
            la = f.read().split()
        with open("/proc/meminfo") as f:
            mem: Dict[str, int] = {}
            for line in f:
                k, _, v = line.partition(":")
                try:
                    mem[k.strip()] = int(v.split()[0])
                except (ValueError, IndexError):
                    pass

        # CPU steal: read /proc/stat cpu line, diff against previous sample.
        with open("/proc/stat") as f:
            cpu_line = f.readline().split()
        # cpu line: cpu user nice system idle iowait irq softirq steal guest guest_nice
        cur = [int(x) for x in cpu_line[1:11]]
        steal_pct = None
        total_pct = None
        if _LAST_CPU is not None and len(_LAST_CPU) == len(cur):
            delta = [c - p for c, p in zip(cur, _LAST_CPU)]
            total = sum(delta)
            if total > 0:
                steal_pct = 100.0 * delta[7] / total
                total_pct = 100.0 * (total - delta[3]) / total  # everything - idle
        _LAST_CPU = cur

        _emit(
            out,
            "sys",
            load1=float(la[0]),
            load5=float(la[1]),
            mem_available_kb=mem.get("MemAvailable"),
            mem_total_kb=mem.get("MemTotal"),
            steal_pct=steal_pct,
            cpu_pct=total_pct,
        )
    except (OSError, ValueError, IndexError) as e:
        _emit(out, "error", source="sys", err=str(e))


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--interval", type=float, default=0.5)
    ap.add_argument("--rabbit-host", default="127.0.0.1")
    ap.add_argument("--rabbit-user", default="guest")
    ap.add_argument("--rabbit-pass", default="guest")
    ap.add_argument("--amqp-port", type=int, default=5672)
    args = ap.parse_args(argv)

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    # Line buffering — every record flushed individually so a SIGKILL'd
    # monitor leaves a complete file up to the last sample.
    with open(args.out, "w", buffering=1) as out:
        _emit(
            out,
            "start",
            pid=args.pid,
            interval=args.interval,
            rabbit_host=args.rabbit_host,
            amqp_port=args.amqp_port,
        )
        # Heavier samplers (thread stacks, full ss) tick on every Nth interval
        # to keep monitor overhead under ~1% CPU.
        heavy_every = max(1, int(round(1.0 / args.interval)))  # ~1s cadence
        tick = 0
        while _running:
            t0 = time.monotonic()
            _sample_proc(out, args.pid)
            _sample_rmq(out, args.rabbit_host, args.rabbit_user, args.rabbit_pass)
            _sample_amqp_socket(out, args.amqp_port)
            _sample_sys(out)
            if tick % heavy_every == 0:
                _sample_thread_stacks(out, args.pid)
                _sample_daemon_sockets(out, args.pid)
            tick += 1
            # Sleep the remainder of the interval. Pin to wall-clock cadence
            # rather than fixed sleep so a slow sample doesn't bias the
            # series.
            elapsed = time.monotonic() - t0
            remaining = args.interval - elapsed
            if remaining > 0:
                # Sleep in small chunks so SIGTERM unblocks promptly.
                end = time.monotonic() + remaining
                while _running and time.monotonic() < end:
                    time.sleep(min(0.1, end - time.monotonic()))
        _emit(out, "stop")
    return 0


if __name__ == "__main__":
    sys.exit(main())
