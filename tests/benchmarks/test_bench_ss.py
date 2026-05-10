"""State Server benchmarks — object churn + location changes.

Covers the avatar position tracking path from commit 3da188b.
"""

import pytest

from tests.common.ardos import Datagram
from tests.common.dc import class_id
from tests.common.msgtypes import (
    STATESERVER_CREATE_OBJECT_WITH_REQUIRED,
    STATESERVER_OBJECT_DELETE_RAM,
    STATESERVER_OBJECT_SET_LOCATION,
)

pytestmark = pytest.mark.benchmark(group="ss")

SS_CHANNEL = 1000


@pytest.fixture
def ss(ardos):
    return ardos(md=True, ss=True)


def test_create_and_delete_churn(ss, channel_conn, benchmark):
    sender = channel_conn()
    counter = [1_500_000]
    cls = class_id("test.dc", "DistributedTestObject1")

    def step():
        do_id = counter[0]
        counter[0] += 1
        dg = Datagram.create(
            [SS_CHANNEL], sender=5, msgtype=STATESERVER_CREATE_OBJECT_WITH_REQUIRED
        )
        dg.add_uint32(do_id).add_uint32(0).add_uint32(0).add_uint16(cls).add_uint32(1)
        sender.send(dg)
        sender.send(
            Datagram.create(
                [do_id], sender=5, msgtype=STATESERVER_OBJECT_DELETE_RAM
            ).add_uint32(do_id)
        )

    benchmark(step)


def test_set_location_churn(ss, channel_conn, benchmark):
    """Move a single object rapidly between zones."""
    sender = channel_conn()
    cls = class_id("test.dc", "DistributedTestObject1")
    do_id = 1_400_001
    dg = Datagram.create(
        [SS_CHANNEL], sender=5, msgtype=STATESERVER_CREATE_OBJECT_WITH_REQUIRED
    )
    dg.add_uint32(do_id).add_uint32(0).add_uint32(0).add_uint16(cls).add_uint32(1)
    sender.send(dg)

    toggle = [0]

    def step():
        zone = 100 + (toggle[0] & 1)
        toggle[0] += 1
        dg = Datagram.create([do_id], sender=5, msgtype=STATESERVER_OBJECT_SET_LOCATION)
        dg.add_uint32(5).add_uint32(zone)
        sender.send(dg)

    benchmark(step)
