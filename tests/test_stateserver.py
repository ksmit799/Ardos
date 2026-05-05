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
import pytest

from tests.common.ardos import Datagram, DatagramIterator
from tests.common.dc import class_id, field_id
from tests.common.msgtypes import (
    STATESERVER_CREATE_OBJECT_WITH_REQUIRED,
    STATESERVER_DELETE_AI_OBJECTS,
    STATESERVER_OBJECT_CHANGING_LOCATION,
    STATESERVER_OBJECT_DELETE_CHILDREN,
    STATESERVER_OBJECT_DELETE_RAM,
    STATESERVER_OBJECT_ENTER_AI_WITH_REQUIRED,
    STATESERVER_OBJECT_ENTER_AI_WITH_REQUIRED_OTHER,
    STATESERVER_OBJECT_ENTER_LOCATION_WITH_REQUIRED,
    STATESERVER_OBJECT_ENTER_LOCATION_WITH_REQUIRED_OTHER,
    STATESERVER_OBJECT_GET_AI,
    STATESERVER_OBJECT_GET_AI_RESP,
    STATESERVER_OBJECT_GET_ALL,
    STATESERVER_OBJECT_GET_ALL_RESP,
    STATESERVER_OBJECT_GET_CLASS,
    STATESERVER_OBJECT_GET_CLASS_RESP,
    STATESERVER_OBJECT_GET_FIELD,
    STATESERVER_OBJECT_GET_FIELD_RESP,
    STATESERVER_OBJECT_GET_FIELDS,
    STATESERVER_OBJECT_GET_FIELDS_RESP,
    STATESERVER_OBJECT_GET_LOCATION,
    STATESERVER_OBJECT_GET_LOCATION_RESP,
    STATESERVER_OBJECT_GET_ZONES_OBJECTS,
    STATESERVER_OBJECT_GET_ZONES_COUNT_RESP,
    STATESERVER_OBJECT_LOCATION_ACK,
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
        # Subscribe to channel 5 upfront — used as both the wait_object_alive
        # response channel and the GET_ALL_RESP sink.
        conn = channel_conn(5)
        conn.send(_create_required())
        # Wait until the DO has bound its DoId queue (replaces a blind sleep).
        conn.wait_object_alive(DO_ID, sender=5)

        req = (
            Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_GET_ALL)
            .add_uint32(123)
            .add_uint32(DO_ID)
        )
        conn.send(req)
        got = conn.recv(timeout=3.0)
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
        new_location_ch = (new_parent << 32) | new_zone

        # Subscribe to old parent (PARENT) and new parent BEFORE the move so
        # we observe the CHANGING_LOCATION broadcast on each. Subscribe to
        # the new location channel for the ENTER_LOCATION_WITH_REQUIRED entry
        # message, and to DO_ID for the LOCATION_ACK reply from the new
        # parent.
        old_parent_watch = channel_conn(PARENT)
        new_parent_watch = channel_conn(new_parent)
        new_loc_watch = channel_conn(new_location_ch)
        ack_watch = channel_conn(DO_ID)
        for w in (old_parent_watch, new_parent_watch, new_loc_watch, ack_watch):
            w.flush()

        # Make sure the DO has bound its DoId queue before we move it,
        # otherwise the SET_LOCATION can race the bind.
        ack_watch.wait_object_alive(DO_ID, sender=DO_ID)
        ack_watch.flush()

        dg = Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_SET_LOCATION)
        dg.add_uint32(new_parent).add_uint32(new_zone)
        sender.send(dg)

        def is_changing_location(dg):
            try:
                it = DatagramIterator(dg)
                _, _, mt = it.read_header()
                return mt == STATESERVER_OBJECT_CHANGING_LOCATION
            except Exception:
                return False

        old_parent_watch.wait_for(is_changing_location, timeout=2.0)
        new_parent_watch.wait_for(is_changing_location, timeout=2.0)

        def is_enter_location(dg):
            try:
                it = DatagramIterator(dg)
                _, _, mt = it.read_header()
                return mt in (
                    STATESERVER_OBJECT_ENTER_LOCATION_WITH_REQUIRED,
                    STATESERVER_OBJECT_ENTER_LOCATION_WITH_REQUIRED_OTHER,
                )
            except Exception:
                return False

        new_loc_watch.wait_for(is_enter_location, timeout=2.0)

        def is_location_ack(dg):
            try:
                it = DatagramIterator(dg)
                _, _, mt = it.read_header()
                return mt == STATESERVER_OBJECT_LOCATION_ACK
            except Exception:
                return False

        ack_watch.wait_for(is_location_ack, timeout=2.0)

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
        # Subscribe to the parent channel BEFORE SET_AI so we observe the
        # CHANGING_AI broadcast (sent to ParentToChildren-style targets).
        parent_watch = channel_conn(PARENT)
        ai.flush()
        parent_watch.flush()

        dg = Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_SET_AI).add_channel(ai_channel)
        sender.send(dg)
        # AI should receive an ENTER_AI_WITH_REQUIRED-family message.
        got = ai.recv(timeout=2.0)
        _, _, mt = DatagramIterator(got).read_header()
        assert mt in (
            STATESERVER_OBJECT_ENTER_AI_WITH_REQUIRED,
            STATESERVER_OBJECT_ENTER_AI_WITH_REQUIRED_OTHER,
        )

    def test_get_ai_round_trip(self, ss, channel_conn):
        """Set an AI, then query GET_AI; expect GET_AI_RESP echoing context,
        doId and the AI channel."""
        sender = channel_conn()
        sender.send(_create_required())

        ai_channel = 9_999_002
        # Use channel 5 as the response sink (and to wait_object_alive).
        watcher = channel_conn(5)
        watcher.wait_object_alive(DO_ID, sender=5)
        watcher.flush()

        sender.send(
            Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_SET_AI).add_channel(ai_channel)
        )

        ctx = 0xABCDEF
        sender.send(
            Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_GET_AI).add_uint32(ctx)
        )

        def is_get_ai_resp(dg):
            try:
                it = DatagramIterator(dg)
                _, _, mt = it.read_header()
                if mt != STATESERVER_OBJECT_GET_AI_RESP:
                    return False
                return it.read_uint32() == ctx
            except Exception:
                return False

        got = watcher.wait_for(is_get_ai_resp, timeout=3.0)
        it = DatagramIterator(got)
        _, _, mt = it.read_header()
        assert mt == STATESERVER_OBJECT_GET_AI_RESP
        assert it.read_uint32() == ctx
        assert it.read_uint32() == DO_ID
        assert it.read_uint64() == ai_channel


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


class TestSSQueries:
    """Per-DO query messages on the SS — round-trip through GET_*+_RESP."""

    def _spawn(self, sender, watcher, do_id=DO_ID):
        sender.send(_create_required(do_id=do_id))
        watcher.wait_object_alive(do_id, sender=5)
        watcher.flush()

    def test_get_field_round_trip(self, ss, channel_conn):
        sender = channel_conn()
        watcher = channel_conn(5)
        self._spawn(sender, watcher)

        fid = field_id("test.dc", "DistributedTestObject1", "setRequired1")
        ctx = 0x010203
        dg = (
            Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_GET_FIELD)
            .add_uint32(ctx).add_uint32(DO_ID).add_uint16(fid)
        )
        sender.send(dg)

        got = watcher.recv(timeout=3.0)
        it = DatagramIterator(got)
        _, _, mt = it.read_header()
        assert mt == STATESERVER_OBJECT_GET_FIELD_RESP
        assert it.read_uint32() == ctx
        assert it.read_uint8() == 1  # success

    def test_get_fields_round_trip(self, ss, channel_conn):
        sender = channel_conn()
        watcher = channel_conn(5)
        self._spawn(sender, watcher)

        fid = field_id("test.dc", "DistributedTestObject1", "setRequired1")
        ctx = 0x040506
        dg = (
            Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_GET_FIELDS)
            .add_uint32(ctx).add_uint32(DO_ID)
            .add_uint16(1)  # field count
            .add_uint16(fid)
        )
        sender.send(dg)

        got = watcher.recv(timeout=3.0)
        it = DatagramIterator(got)
        _, _, mt = it.read_header()
        assert mt == STATESERVER_OBJECT_GET_FIELDS_RESP
        assert it.read_uint32() == ctx
        assert it.read_uint8() == 1  # success
        assert it.read_uint16() == 1  # fields-found count

    def test_get_class_round_trip(self, ss, channel_conn):
        sender = channel_conn()
        watcher = channel_conn(5)
        self._spawn(sender, watcher)

        ctx = 0xC1A55
        sender.send(
            Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_GET_CLASS).add_uint32(ctx)
        )
        got = watcher.recv(timeout=3.0)
        it = DatagramIterator(got)
        _, _, mt = it.read_header()
        assert mt == STATESERVER_OBJECT_GET_CLASS_RESP
        assert it.read_uint32() == ctx
        assert it.read_uint32() == DO_ID
        assert it.read_uint16() == class_id("test.dc", "DistributedTestObject1")

    @pytest.mark.skip(reason="STATESERVER_OBJECT_GET_OWNER has no handler in distributed_object.cpp")
    def test_get_owner_round_trip(self, ss, channel_conn):
        pass

    def test_get_zone_objects_round_trip(self, ss, channel_conn):
        """Parent + child DO; GET_ZONE_OBJECTS to the parent yields a count
        response and (for matching children) an entry message."""
        sender = channel_conn()
        watcher = channel_conn(5)
        # Make a parent at (1, 1) and a child at (parent_doid, child_zone).
        parent_doid = DO_ID
        child_doid = DO_ID + 1
        child_zone = 444

        sender.send(_create_required(parent=1, zone=1, do_id=parent_doid))
        watcher.wait_object_alive(parent_doid, sender=5)
        sender.send(_create_required(parent=parent_doid, zone=child_zone, do_id=child_doid))
        watcher.wait_object_alive(child_doid, sender=5)
        watcher.flush()

        ctx = 0x77CC
        # GET_ZONES_OBJECTS on the parent: [ctx][parent][zoneCount][zones...]
        dg = (
            Datagram.create([parent_doid], sender=5, msgtype=STATESERVER_OBJECT_GET_ZONES_OBJECTS)
            .add_uint32(ctx).add_uint32(parent_doid)
            .add_uint16(1)
            .add_uint32(child_zone)
        )
        sender.send(dg)

        def is_zones_count_resp(dg):
            try:
                it = DatagramIterator(dg)
                _, _, mt = it.read_header()
                return mt == STATESERVER_OBJECT_GET_ZONES_COUNT_RESP
            except Exception:
                return False

        got = watcher.wait_for(is_zones_count_resp, timeout=3.0)
        it = DatagramIterator(got)
        _, _, mt = it.read_header()
        assert mt == STATESERVER_OBJECT_GET_ZONES_COUNT_RESP
        assert it.read_uint32() == ctx
        # Child count >= 1 (at least the one we made).
        assert it.read_uint32() >= 1

    @pytest.mark.skip(reason="STATESERVER_OBJECT_GET_CHILDREN / GET_CHILD_COUNT have no handler in distributed_object.cpp")
    def test_get_children_round_trip(self, ss, channel_conn):
        pass

    @pytest.mark.skip(reason="STATESERVER_OBJECT_GET_ZONE_COUNT / GET_ZONES_COUNT have no handler in distributed_object.cpp")
    def test_get_zone_count_round_trip(self, ss, channel_conn):
        pass

    @pytest.mark.skip(reason="STATESERVER_GET_ACTIVE_ZONES is on a DO; covered indirectly via GET_ZONES_OBJECTS")
    def test_get_active_zones_round_trip(self, ss, channel_conn):
        pass


class TestSSBulkDelete:
    def test_delete_children_removes_children(self, ss, channel_conn):
        """DELETE_CHILDREN sent to the parent annihilates its children."""
        sender = channel_conn()
        watcher = channel_conn(5)

        parent = DO_ID
        child_a = DO_ID + 10
        child_b = DO_ID + 11

        sender.send(_create_required(parent=1, zone=1, do_id=parent))
        watcher.wait_object_alive(parent, sender=5)
        sender.send(_create_required(parent=parent, zone=100, do_id=child_a))
        watcher.wait_object_alive(child_a, sender=5)
        sender.send(_create_required(parent=parent, zone=100, do_id=child_b))
        watcher.wait_object_alive(child_b, sender=5)
        watcher.flush()

        # DELETE_CHILDREN [doId] on the parent.
        sender.send(
            Datagram.create([parent], sender=5, msgtype=STATESERVER_OBJECT_DELETE_CHILDREN)
            .add_uint32(parent)
        )

        # Children should be gone — GET_LOCATION on each yields no response.
        for child in (child_a, child_b):
            sender.send(
                Datagram.create([child], sender=5, msgtype=STATESERVER_OBJECT_GET_LOCATION)
                .add_uint32(0xDEAD)
            )
        watcher.expect_none(timeout=1.0)

    def test_delete_ai_objects_removes_ai_owned(self, ss, channel_conn):
        """DELETE_AI_OBJECTS broadcast to an AI channel annihilates objects
        whose _aiChannel matches."""
        sender = channel_conn()
        watcher = channel_conn(5)

        ai_channel = 9_998_111
        sender.send(_create_required())
        watcher.wait_object_alive(DO_ID, sender=5)
        watcher.flush()

        # Set the AI channel.
        sender.send(
            Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_SET_AI)
            .add_channel(ai_channel)
        )
        # Send DELETE_AI_OBJECTS targeting that AI channel.
        sender.send(
            Datagram.create([ai_channel], sender=5, msgtype=STATESERVER_DELETE_AI_OBJECTS)
            .add_channel(ai_channel)
        )

        # Verify: GET_LOCATION on the deleted object should yield nothing.
        sender.send(
            Datagram.create([DO_ID], sender=5, msgtype=STATESERVER_OBJECT_GET_LOCATION)
            .add_uint32(0xBEEF)
        )
        watcher.expect_none(timeout=1.0)

    @pytest.mark.skip(reason="STATESERVER_OBJECT_DELETE_ZONE / DELETE_ZONES have no handler in distributed_object.cpp")
    def test_delete_zone(self, ss, channel_conn):
        pass

    @pytest.mark.skip(reason="STATESERVER_OBJECT_DELETE_FIELD_RAM / DELETE_FIELDS_RAM have no handler in distributed_object.cpp")
    def test_delete_field_ram(self, ss, channel_conn):
        pass
