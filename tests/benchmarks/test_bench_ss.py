"""State Server direct benchmarks — SS hot paths without a CA in the loop.

These pair with the CA-level benchmarks in ``test_bench_ca.py``:

  - ``test_ss_create_throughput``        ↔ CA ``test_object_churn_throughput``
  - ``test_ss_set_field_throughput``     ↔ CA ``test_field_update_throughput``
  - ``test_ss_set_location_throughput``  ↔ CA ``test_location_switch_throughput``

The CA benchmarks measure end-to-end fanout to N clients. The SS benchmarks
measure just the SS work — object creation, field broadcast emit, location
change emit. Comparing the two narrows down where a regression lives: if the
SS benchmark moves, blame the SS; if only the CA benchmark moves, blame the
CA fanout path.

Round-trip discipline (avoid measuring Python's send buffer):
  * Every step blocks on a real observable: GET_LOCATION_RESP for creates,
    or the broadcast emission on the location channel for field/location
    updates. Without this we'd just be timing socket buffer fills.
"""

from __future__ import annotations

import os

import pytest

from tests.common.ardos import AIConnection, Datagram, DatagramIterator
from tests.common.dc import class_id, field_id
from tests.common.msgtypes import (
    STATESERVER_OBJECT_CHANGING_LOCATION,
    STATESERVER_OBJECT_SET_FIELD,
    STATESERVER_OBJECT_SET_LOCATION,
)

pytestmark = pytest.mark.benchmark(group="ss")

SS_PARENT = 1
SS_ZONE = 10
ALT_ZONE = 11

# DoId pools, partitioned per benchmark so iterations never collide.
CREATE_DOID_BASE = 6_000_000
FIELD_DOID = 7_000_001
LOC_DOID = 7_000_002


def _location_channel(parent: int, zone: int) -> int:
    """Matches LocationAsChannel() in src/net/message_types.h (ZONE_BITS=32)."""
    return (parent << 32) | zone


def _required_payload() -> bytes:
    """DistributedTestObject1.setRequired1 — single uint32 with a default."""
    return Datagram().add_uint32(78).bytes()


@pytest.fixture
def ss(ardos):
    # warn-level logging by default so per-message trace writes don't skew
    # the measurement. Set ARDOS_BENCH_LOG_LEVEL=trace for diagnostic runs.
    return ardos(
        md=True,
        ss=True,
        overrides={"log-level": os.environ.get("ARDOS_BENCH_LOG_LEVEL", "warn")},
    )


def test_ss_create_throughput(ss, ai_conn, benchmark):
    """One CREATE_OBJECT_WITH_REQUIRED per step, round-tripped via
    GET_LOCATION_RESP so the timing reflects actual SS work (queue bind +
    object table insert + location channel publish), not just Python's
    socket buffer fill.

    Objects accumulate across iterations — that's fine, the SS hash map
    cost is O(1) per insert and there's no per-object cleanup work that
    grows the per-step cost.
    """
    ai = ai_conn()
    dclass = class_id("test.dc", "DistributedTestObject1")
    required = _required_payload()
    counter = [CREATE_DOID_BASE]

    def step():
        do_id = counter[0]
        counter[0] += 1
        ai.create_object(
            do_id=do_id,
            parent=SS_PARENT,
            zone=SS_ZONE,
            dclass_id=dclass,
            required=required,
        )
        ai.wait_object_alive(do_id, timeout=5.0)

    benchmark(step)


def test_ss_set_field_throughput(ss, ai_conn, channel_conn, benchmark):
    """Pre-existing object; per step the AI fires SET_FIELD on a broadcast
    field and a watcher subscribed to the object's location channel
    confirms the broadcast emitted by the SS. Measures just the SS's
    SET_FIELD handler + broadcast emit.
    """
    ai = ai_conn()
    dclass = class_id("test.dc", "DistributedTestObject1")
    fid = field_id("test.dc", "DistributedTestObject1", "setBR1")
    required = _required_payload()

    ai.create_object(
        do_id=FIELD_DOID,
        parent=SS_PARENT,
        zone=SS_ZONE,
        dclass_id=dclass,
        required=required,
    )
    ai.wait_object_alive(FIELD_DOID, timeout=5.0)

    watcher = channel_conn(_location_channel(SS_PARENT, SS_ZONE))
    watcher.flush()

    payload = Datagram().add_string("u").bytes()

    def step():
        ai.set_field(FIELD_DOID, fid, payload)
        # Drain until we see the broadcast — anything else on this channel
        # (there shouldn't be anything in this isolated fixture) is dropped.
        while True:
            dg = watcher.recv(timeout=5.0)
            it = DatagramIterator(dg)
            _, _, mt = it.read_header()
            if mt == STATESERVER_OBJECT_SET_FIELD:
                break

    benchmark(step)


def test_ss_set_location_throughput(ss, ai_conn, channel_conn, benchmark):
    """Pre-existing object; per step the AI toggles its location between
    SS_ZONE and ALT_ZONE. A watcher subscribed to both location channels
    confirms the SS emitted CHANGING_LOCATION. Measures the SS's
    SET_LOCATION handler + interest/membership update emit.
    """
    ai = ai_conn()
    dclass = class_id("test.dc", "DistributedTestObject1")
    required = _required_payload()

    ai.create_object(
        do_id=LOC_DOID,
        parent=SS_PARENT,
        zone=SS_ZONE,
        dclass_id=dclass,
        required=required,
    )
    ai.wait_object_alive(LOC_DOID, timeout=5.0)

    watcher = channel_conn(
        _location_channel(SS_PARENT, SS_ZONE),
        _location_channel(SS_PARENT, ALT_ZONE),
    )
    watcher.flush()

    state = {"zone": SS_ZONE}

    def step():
        target = ALT_ZONE if state["zone"] == SS_ZONE else SS_ZONE
        ai.send(
            Datagram.create(
                [LOC_DOID],
                sender=ai.ai_channel,
                msgtype=STATESERVER_OBJECT_SET_LOCATION,
            )
            .add_uint32(SS_PARENT)
            .add_uint32(target)
        )
        state["zone"] = target
        while True:
            dg = watcher.recv(timeout=5.0)
            it = DatagramIterator(dg)
            _, _, mt = it.read_header()
            if mt == STATESERVER_OBJECT_CHANGING_LOCATION:
                break

    benchmark(step)
