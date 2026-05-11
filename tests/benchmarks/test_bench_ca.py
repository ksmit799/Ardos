"""Client Agent throughput benchmarks.

Each benchmark parametrizes over ``population`` — the number of CA-connected,
authed clients with interest opened in a shared test zone — and exercises one
CA hot path:

  - ``test_object_churn_throughput``  — AI creates and deletes K objects per
    step; every client observes K entries + K leaves.
  - ``test_field_update_throughput``  — K pre-existing objects receive a
    broadcast field update per step; every client observes K updates.
  - ``test_location_switch_throughput`` — K pre-existing objects relocate
    between two watched zones per step; every client observes K location
    updates.

Population scaling is the primary axis; ``ACTIVE`` (operations per step) is
held fixed so that per-step time stays in CodSpeed's useful sampling window
across the sweep.

Implementation notes:

  - Fixture brings up MD+SS+CA once and connects ``n_pop`` clients. Each
    client gets a deterministic CA channel via the configured channel range
    (CA allocates sequentially from ``min``).
  - SET_STATE + ADD_INTEREST are pipelined: we fire every command upfront,
    then drain client inboxes until each has seen the matching DONE_INTEREST_RESP.
    That implicitly confirms the prior SET_STATE completed (FIFO on the
    per-client channel).
  - The timed step uses a ``selectors``-backed parallel drain so we wait on
    many sockets at once instead of recv'ing each in sequence. Without this,
    Python recv overhead dominates the measurement at large ``n_pop``.
"""

from __future__ import annotations

import os
import selectors
import socket
import time
from typing import List, Sequence, Set

import pytest

from tests.common.ardos import (
    AIConnection,
    AUTH_STATE_ESTABLISHED,
    ClientConnection,
    Datagram,
)
from tests.common.dc import class_id, dc_hash, field_id
from tests.common.msgtypes import (
    CLIENT_DONE_INTEREST_RESP,
    CLIENT_ENTER_OBJECT_REQUIRED,
    CLIENT_ENTER_OBJECT_REQUIRED_OTHER,
    CLIENT_OBJECT_LEAVING,
    CLIENT_OBJECT_LOCATION,
    CLIENT_OBJECT_SET_FIELD,
    STATESERVER_OBJECT_SET_LOCATION,
)

pytestmark = pytest.mark.benchmark(group="ca")

POPULATION_SIZES = [256, 1024]
# Operations per step. Held constant so per-step time scales primarily with
# population.
ACTIVE = 4
# pop=4096 and pop=10000 were attempted but the daemon stalls part-way through
# the second timed-step iteration: iter-1 broadcasts complete end-to-end, but
# at some point libuv stops processing AI messages and no further broadcasts
# go out. The CA throughput indexes are fine -- iter 1 confirms routing
# delivers to all 4096 subscribers. The hang is somewhere in the
# uvw/AMQP-CPP/event-loop interaction at high N. Bump this back up once the
# stall is diagnosed.

CLIENT_CHANNEL_BASE = 1_000_000_000

TEST_PARENT = 1
TEST_ZONE = 10
ALT_ZONE = 11

# DoId pools — partitioned so churn (creates/deletes per iteration) never
# collides with the pre-created field/location objects.
CHURN_DOID_BASE = 2_000_000  # incremented per iteration to avoid SS collisions
FIELD_DOID_BASE = 3_000_000
LOC_DOID_BASE = 4_000_000

ENTRY_MSGS: Set[int] = {
    CLIENT_ENTER_OBJECT_REQUIRED,
    CLIENT_ENTER_OBJECT_REQUIRED_OTHER,
}


# ---------------------------------------------------------------------------
# Parallel drain — wait on N client sockets at once
# ---------------------------------------------------------------------------


def _await_counts(
    clients: Sequence[ClientConnection],
    per_client: int,
    accept_msgs: Set[int],
    *,
    timeout: float,
) -> None:
    """Block until every client has received ``per_client`` framed datagrams
    whose msgtype is in ``accept_msgs``. Other datagrams are silently drained.

    Uses ``selectors`` to wait on all sockets in parallel; without this,
    sequential recv-then-parse at N=10k turns Python into the bottleneck
    rather than the CA.
    """
    sel = selectors.DefaultSelector()
    counts = [0] * len(clients)
    socks: List[socket.socket] = [c.sock for c in clients]
    # Pull whatever's already buffered on the MDConnection into our local
    # parse buffer; clear the connection's buffer so we own draining.
    buffers: List[bytearray] = []
    prev_blocking: List[bool] = []
    for i, c in enumerate(clients):
        buf = bytearray(c._rx)
        c._rx.clear()
        buffers.append(buf)
        prev_blocking.append(socks[i].getblocking())
        socks[i].setblocking(False)
        sel.register(socks[i], selectors.EVENT_READ, data=i)

    def _parse(buf: bytearray, idx: int) -> None:
        pos = 0
        n_buf = len(buf)
        # Inline parse: [uint16 LE length][uint16 LE msgtype][...]
        while n_buf - pos >= 2:
            length = buf[pos] | (buf[pos + 1] << 8)
            if n_buf - pos < 2 + length:
                break
            if length >= 2:
                mt = buf[pos + 2] | (buf[pos + 3] << 8)
                if mt in accept_msgs:
                    counts[idx] += 1
            pos += 2 + length
        if pos:
            del buf[:pos]

    # First pass: anything already buffered from setup phase may already
    # satisfy some clients.
    for i, buf in enumerate(buffers):
        if buf:
            _parse(buf, i)

    try:
        deadline = time.monotonic() + timeout
        while any(c < per_client for c in counts):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                under = [i for i, c in enumerate(counts) if c < per_client]
                raise TimeoutError(
                    f"drain: {len(under)}/{len(clients)} clients short "
                    f"(min={min(counts)} want={per_client})"
                )
            events = sel.select(remaining)
            for key, _mask in events:
                i = key.data
                try:
                    chunk = socks[i].recv(65536)
                except BlockingIOError:
                    continue
                if not chunk:
                    raise ConnectionError(f"client {i}: peer closed")
                buffers[i].extend(chunk)
                _parse(buffers[i], i)
    finally:
        for i, s in enumerate(socks):
            try:
                sel.unregister(s)
            except (KeyError, ValueError):
                pass
            s.setblocking(prev_blocking[i])
            # Hand any leftover bytes back to the connection's buffer so
            # post-benchmark teardown can still parse them.
            if buffers[i]:
                clients[i]._rx.extend(buffers[i])
        sel.close()


# ---------------------------------------------------------------------------
# Fixture
# ---------------------------------------------------------------------------


def _required_payload() -> bytes:
    """DistributedTestObject1.setRequired1 — single uint32."""
    return Datagram().add_uint32(78).bytes()


def _send_set_location(ai: AIConnection, do_id: int, parent: int, zone: int) -> None:
    ai.send(
        Datagram.create(
            [do_id], sender=ai.ai_channel, msgtype=STATESERVER_OBJECT_SET_LOCATION
        )
        .add_uint32(parent)
        .add_uint32(zone)
    )


@pytest.fixture(params=POPULATION_SIZES, ids=lambda n: f"pop={n}")
def populated_cluster(request, ardos, ai_conn, client_conn):
    """MD+SS+CA with ``request.param`` connected, authed, interested clients.

    Interest covers TEST_ZONE and ALT_ZONE so the location benchmark can
    relocate between the two without disabling visibility.
    """
    n_pop = request.param

    ardos(
        md=True,
        ss=True,
        ca=True,
        overrides={
            # Default to warn-level logging so per-message trace writes don't
            # skew the measurement (trace emits one line per dispatch/subscribe
            # /publish, which at pop=10k adds hundreds of ms of syscalls per
            # step). Set ARDOS_BENCH_LOG_LEVEL=trace to crank it up for
            # diagnostic runs.
            "log-level": os.environ.get("ARDOS_BENCH_LOG_LEVEL", "warn"),
            "client-agent": {
                "channels": {
                    "min": CLIENT_CHANNEL_BASE,
                    "max": CLIENT_CHANNEL_BASE + n_pop - 1,
                },
                "interest": {
                    "client": "disabled",
                    "mode": "whitelist",
                    "zones": [TEST_ZONE, ALT_ZONE],
                },
            },
        },
    )

    ai = ai_conn()

    # Connect + hello every client. Sequential because CA allocates channels
    # in connect order — client[i] gets CLIENT_CHANNEL_BASE + i.
    clients: List[ClientConnection] = []
    for _ in range(n_pop):
        c = client_conn()
        c.hello(dc_hash("test.dc"), "dev")
        c.expect_hello_resp()
        clients.append(c)

    # Pipeline SET_STATE + two ADD_INTEREST (TEST_ZONE and ALT_ZONE) for every
    # client. Per-channel FIFO guarantees SET_STATE lands before ADD_INTEREST.
    for i in range(n_pop):
        ch = CLIENT_CHANNEL_BASE + i
        ai.set_client_state(ch, AUTH_STATE_ESTABLISHED, wait=False)
        ai.add_interest(ch, interest_id=1, parent=TEST_PARENT, zone=TEST_ZONE)
        ai.add_interest(ch, interest_id=2, parent=TEST_PARENT, zone=ALT_ZONE)

    # Wait until every client has seen two DONE_INTEREST_RESP. That confirms
    # both ADD_INTERESTs (and therefore the prior SET_STATE) have committed.
    _await_counts(
        clients,
        per_client=2,
        accept_msgs={CLIENT_DONE_INTEREST_RESP},
        timeout=max(60.0, n_pop * 0.02),
    )

    return ai, clients


# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------


def test_object_churn_throughput(populated_cluster, benchmark):
    """AI creates ACTIVE objects in TEST_ZONE, then deletes them. Each client
    must observe ACTIVE entry messages and ACTIVE leave messages.
    """
    ai, clients = populated_cluster
    dclass = class_id("test.dc", "DistributedTestObject1")
    required = _required_payload()
    counter = [CHURN_DOID_BASE]

    def step():
        base = counter[0]
        counter[0] += ACTIVE
        for k in range(ACTIVE):
            ai.create_object(
                do_id=base + k,
                parent=TEST_PARENT,
                zone=TEST_ZONE,
                dclass_id=dclass,
                required=required,
            )
        _await_counts(
            clients,
            per_client=ACTIVE,
            accept_msgs=ENTRY_MSGS,
            timeout=60.0,
        )
        for k in range(ACTIVE):
            ai.delete_object(base + k)
        _await_counts(
            clients,
            per_client=ACTIVE,
            accept_msgs={CLIENT_OBJECT_LEAVING},
            timeout=60.0,
        )

    benchmark(step)


def test_field_update_throughput(populated_cluster, benchmark):
    """AI fires ACTIVE field updates on pre-existing objects in TEST_ZONE.
    Every client (interest in TEST_ZONE) must see ACTIVE CLIENT_OBJECT_SET_FIELD.

    Uses ``setBR1`` (string broadcast ram on DistributedTestObject1).
    """
    ai, clients = populated_cluster
    dclass = class_id("test.dc", "DistributedTestObject1")
    fid = field_id("test.dc", "DistributedTestObject1", "setBR1")
    required = _required_payload()

    # Pre-create ACTIVE objects in TEST_ZONE that will receive updates.
    for k in range(ACTIVE):
        ai.create_object(
            do_id=FIELD_DOID_BASE + k,
            parent=TEST_PARENT,
            zone=TEST_ZONE,
            dclass_id=dclass,
            required=required,
        )
    # Drain the resulting entry-fanout so the timed step starts clean.
    _await_counts(
        clients,
        per_client=ACTIVE,
        accept_msgs=ENTRY_MSGS,
        timeout=60.0,
    )

    payload = Datagram().add_string("u").bytes()

    def step():
        for k in range(ACTIVE):
            ai.set_field(FIELD_DOID_BASE + k, fid, payload)
        _await_counts(
            clients,
            per_client=ACTIVE,
            accept_msgs={CLIENT_OBJECT_SET_FIELD},
            timeout=60.0,
        )

    benchmark(step)


def test_location_switch_throughput(populated_cluster, benchmark):
    """AI relocates ACTIVE pre-existing objects between TEST_ZONE and
    ALT_ZONE. Each client (interested in both zones) sees ACTIVE
    CLIENT_OBJECT_LOCATION updates per step.
    """
    ai, clients = populated_cluster
    dclass = class_id("test.dc", "DistributedTestObject1")
    required = _required_payload()

    for k in range(ACTIVE):
        ai.create_object(
            do_id=LOC_DOID_BASE + k,
            parent=TEST_PARENT,
            zone=TEST_ZONE,
            dclass_id=dclass,
            required=required,
        )
    _await_counts(
        clients,
        per_client=ACTIVE,
        accept_msgs=ENTRY_MSGS,
        timeout=60.0,
    )

    state = {"zone": TEST_ZONE}

    def step():
        target = ALT_ZONE if state["zone"] == TEST_ZONE else TEST_ZONE
        for k in range(ACTIVE):
            _send_set_location(ai, LOC_DOID_BASE + k, TEST_PARENT, target)
        state["zone"] = target
        _await_counts(
            clients,
            per_client=ACTIVE,
            accept_msgs={CLIENT_OBJECT_LOCATION},
            timeout=60.0,
        )

    benchmark(step)
