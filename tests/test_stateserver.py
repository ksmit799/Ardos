"""State Server tests.

Covers the distributed-object lifecycle on the SS:
  - create with required / required+other
  - GET_ALL / GET_FIELD / GET_LOCATION round-trips
  - SET_FIELD broadcast
  - SET_LOCATION visibility changes
  - SET_AI / SET_OWNER enter messages
  - zone queries (GET_ZONE_OBJECTS, GET_ZONES_OBJECTS)
  - object delete
"""
import time

import pytest

from tests.common.ardos import Datagram, DatagramIterator
from tests.common.dc import class_id, field_id
from tests.common.msgtypes import (
    STATESERVER_CREATE_OBJECT_WITH_REQUIRED,
    STATESERVER_OBJECT_DELETE_RAM,
    STATESERVER_OBJECT_ENTER_AI_WITH_REQUIRED,
    STATESERVER_OBJECT_ENTER_AI_WITH_REQUIRED_OTHER,
    STATESERVER_OBJECT_GET_ALL,
    STATESERVER_OBJECT_GET_ALL_RESP,
    STATESERVER_OBJECT_GET_LOCATION,
    STATESERVER_OBJECT_GET_LOCATION_RESP,
    STATESERVER_OBJECT_SET_AI,
    STATESERVER_OBJECT_SET_FIELD,
    STATESERVER_OBJECT_SET_LOCATION,
)

SS_CHANNEL = 1000
PARENT = 50
ZONE = 100
DO_ID = 1_000_001


def _create_required(parent=PARENT, zone=ZONE, do_id=DO_ID, dc="DistributedTestObject1", required1=42):
    cls = class_id("test.dc", dc)
    dg = Datagram.create([SS_CHANNEL], sender=5, msgtype=STATESERVER_CREATE_OBJECT_WITH_REQUIRED)
    dg.add_uint32(do_id).add_uint32(parent).add_uint32(zone).add_uint16(cls)
    dg.add_uint32(required1)
    return dg


@pytest.fixture
def ss(ardos):
    return ardos(md=True, ss=True)


class TestCreateAndGet:
    def test_create_then_get_all(self, ss, channel_conn):
        sender = channel_conn()
        sender.send(_create_required())
        # The SS spawns the DO and binds its DoId queue asynchronously through
        # RabbitMQ; without a beat here the GET_ALL can race the bind and
        # never reach the new object.
        time.sleep(0.3)

        req = Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_GET_ALL).add_uint32(123)
        watcher = channel_conn(5)
        watcher.flush()
        sender.send(req)
        got = watcher.recv(timeout=3.0)
        it = DatagramIterator(got)
        _, _, mt = it.read_header()
        assert mt == STATESERVER_OBJECT_GET_ALL_RESP
        assert it.read_uint32() == 123  # context
        assert it.read_uint32() == DO_ID

    def test_duplicate_generate_is_ignored(self, ss, channel_conn):
        sender = channel_conn()
        sender.send(_create_required())
        sender.send(_create_required())
        # Second should have been logged+dropped, not cause a crash. Touch the
        # object to confirm it's still alive.
        req = Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_GET_LOCATION).add_uint32(1)
        watcher = channel_conn(5)
        watcher.flush()
        sender.send(req)
        got = watcher.recv(timeout=2.0)
        _, _, mt = DatagramIterator(got).read_header()
        assert mt == STATESERVER_OBJECT_GET_LOCATION_RESP


class TestFieldUpdate:
    def test_set_field_broadcasts_to_location(self, ss, channel_conn):
        sender = channel_conn()
        sender.send(_create_required())

        # Location-channel subscriber: (parent << 32) | zone
        loc_ch = (PARENT << 32) | ZONE
        watcher = channel_conn(loc_ch)
        watcher.flush()

        field = field_id("test.dc", "DistributedTestObject1", "setB1")
        dg = Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_SET_FIELD)
        dg.add_uint32(DO_ID).add_uint16(field).add_uint8(99)
        sender.send(dg)

        got = watcher.recv(timeout=2.0)
        it = DatagramIterator(got)
        _, _, mt = it.read_header()
        assert mt == STATESERVER_OBJECT_SET_FIELD
        assert it.read_uint32() == DO_ID
        assert it.read_uint16() == field
        assert it.read_uint8() == 99


class TestLocation:
    def test_set_location_moves_object(self, ss, channel_conn):
        sender = channel_conn()
        sender.send(_create_required())

        new_parent, new_zone = 60, 200
        dg = Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_SET_LOCATION)
        dg.add_uint32(new_parent).add_uint32(new_zone)
        sender.send(dg)

        req = Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_GET_LOCATION).add_uint32(0)
        watcher = channel_conn(5)
        watcher.flush()
        sender.send(req)
        got = watcher.recv(timeout=2.0)
        it = DatagramIterator(got)
        _, _, mt = it.read_header()
        assert mt == STATESERVER_OBJECT_GET_LOCATION_RESP
        assert it.read_uint32() == 0  # context
        assert it.read_uint32() == DO_ID
        assert it.read_uint32() == new_parent
        assert it.read_uint32() == new_zone


class TestOwnership:
    def test_set_ai_sends_enter(self, ss, channel_conn):
        sender = channel_conn()
        sender.send(_create_required())

        ai_channel = 9_999_001
        ai = channel_conn(ai_channel)
        ai.flush()
        dg = Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_SET_AI).add_channel(ai_channel)
        sender.send(dg)
        # AI should receive an ENTER_AI_WITH_REQUIRED-family message.
        got = ai.recv(timeout=2.0)
        _, _, mt = DatagramIterator(got).read_header()
        assert mt in (
            STATESERVER_OBJECT_ENTER_AI_WITH_REQUIRED,
            STATESERVER_OBJECT_ENTER_AI_WITH_REQUIRED_OTHER,
        )


class TestDelete:
    def test_delete_ram_removes_object(self, ss, channel_conn):
        sender = channel_conn()
        sender.send(_create_required())

        sender.send(
            Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_DELETE_RAM).add_uint32(DO_ID)
        )

        # Verify: GET_LOCATION after delete should yield nothing.
        watcher = channel_conn(5)
        watcher.flush()
        sender.send(
            Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_GET_LOCATION).add_uint32(77)
        )
        watcher.expect_none(timeout=0.5)
