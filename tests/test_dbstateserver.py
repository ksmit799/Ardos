"""DBSS tests — DB-backed state-server objects.

Exercises ACTIVATE_WITH_DEFAULTS (loading a DB-backed object into RAM),
GET_ACTIVATED queries, and delete-from-disk lifecycle.
"""

import pytest

from tests.common.ardos import Datagram, DatagramIterator
from tests.common.dc import class_id, field_id
from tests.common.msgtypes import (
    DBSERVER_CREATE_OBJECT,
    DBSERVER_OBJECT_GET_ALL,
    DBSERVER_OBJECT_GET_ALL_RESP,
    DBSS_OBJECT_ACTIVATE_WITH_DEFAULTS,
    DBSS_OBJECT_ACTIVATE_WITH_DEFAULTS_OTHER,
    DBSS_OBJECT_DELETE_DISK,
    DBSS_OBJECT_GET_ACTIVATED,
    DBSS_OBJECT_GET_ACTIVATED_RESP,
    STATESERVER_OBJECT_GET_ALL,
    STATESERVER_OBJECT_GET_ALL_RESP,
)

DB_CHANNEL = 4003
SENDER = 54_321


def _create_player(name: str) -> Datagram:
    cls = class_id("test.dc", "DistributedPlayer")
    setname = field_id("test.dc", "DistributedPlayer", "setName")
    dg = Datagram.create([DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_CREATE_OBJECT)
    dg.add_uint32(1).add_uint16(cls).add_uint16(1).add_uint16(setname).add_string(name)
    return dg


@pytest.fixture
def dbss(ardos):
    return ardos(md=True, ss=True, db=True, dbss=True)


def _seed_player(sender_conn, name="activator") -> int:
    sender_conn.send(_create_player(name))
    it = DatagramIterator(sender_conn.recv(timeout=5.0))
    it.read_header()
    it.read_uint32()
    return it.read_uint32()


class TestActivate:
    def test_activate_pulls_object_into_ss(self, dbss, channel_conn):
        sender = channel_conn(SENDER)
        sender.flush()
        do_id = _seed_player(sender)

        dg = Datagram.create(
            [do_id], sender=SENDER, msgtype=DBSS_OBJECT_ACTIVATE_WITH_DEFAULTS
        )
        dg.add_uint32(do_id).add_uint32(0).add_uint32(0)
        sender.send(dg)

        # Wait for the DBSS to finish loading + activating by probing the
        # newly-active SS object until GET_LOCATION_RESP comes back.
        sender.wait_object_alive(do_id, sender=SENDER, timeout=5.0)

        dg = (
            Datagram.create([do_id], sender=SENDER, msgtype=STATESERVER_OBJECT_GET_ALL)
            .add_uint32(77)
            .add_uint32(do_id)
        )
        sender.send(dg)
        it = DatagramIterator(sender.recv(timeout=5.0))
        _, _, mt = it.read_header()
        assert mt == STATESERVER_OBJECT_GET_ALL_RESP
        assert it.read_uint32() == 77

    def test_get_activated_before_activate(self, dbss, channel_conn):
        sender = channel_conn(SENDER)
        sender.flush()
        do_id = _seed_player(sender, "passive")
        dg = Datagram.create([do_id], sender=SENDER, msgtype=DBSS_OBJECT_GET_ACTIVATED)
        dg.add_uint32(11).add_uint32(do_id)
        sender.send(dg)
        it = DatagramIterator(sender.recv(timeout=5.0))
        _, _, mt = it.read_header()
        assert mt == DBSS_OBJECT_GET_ACTIVATED_RESP
        assert it.read_uint32() == 11
        assert it.read_uint32() == do_id
        assert it.read_uint8() == 0  # not activated


class TestDBSSDelete:
    """Disk-deletion + activate-with-other lifecycle on the DBSS."""

    def test_delete_disk_removes_from_db(self, dbss, channel_conn):
        """DBSS_OBJECT_DELETE_DISK on an inactive (DB-only) object purges it
        from MongoDB. A follow-up DBSERVER_OBJECT_GET_ALL returns failure."""
        sender = channel_conn(SENDER)
        sender.flush()
        do_id = _seed_player(sender, "victim")

        # Send DELETE_DISK at the DBSS (the object's channel forwards into
        # the DBSS handler when not active in RAM).
        dg = Datagram.create(
            [do_id], sender=SENDER, msgtype=DBSS_OBJECT_DELETE_DISK
        ).add_uint32(do_id)
        sender.send(dg)

        # Probe the DB directly for the object — should now fail.
        dg = (
            Datagram.create(
                [DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_GET_ALL
            )
            .add_uint32(0xDD)
            .add_uint32(do_id)
        )
        sender.send(dg)
        it = DatagramIterator(sender.recv(timeout=5.0))
        _, _, mt = it.read_header()
        assert mt == DBSERVER_OBJECT_GET_ALL_RESP
        assert it.read_uint32() == 0xDD
        assert it.read_uint8() == 0  # success=false on deleted object

    def test_activate_with_defaults_other(self, dbss, channel_conn):
        """ACTIVATE_WITH_DEFAULTS_OTHER carries an extra
        [dcId][fieldCount][fieldId, value]+ payload that becomes the object's
        ram fields on activation."""
        sender = channel_conn(SENDER)
        sender.flush()
        do_id = _seed_player(sender, "activator-other")

        cls = class_id("test.dc", "DistributedPlayer")
        # setLocation is `broadcast ownsend` — not ram, so use a different
        # field. DistributedPlayer doesn't have a ram-only field aside from
        # setName (which is required). Pass setName via the `other` payload —
        # the DBSS only stores ram fields, but the wire format coverage still
        # exercises the parser even when the field is required.
        setname = field_id("test.dc", "DistributedPlayer", "setName")
        dg = (
            Datagram.create(
                [do_id], sender=SENDER, msgtype=DBSS_OBJECT_ACTIVATE_WITH_DEFAULTS_OTHER
            )
            .add_uint32(do_id)
            .add_uint32(0)
            .add_uint32(0)
            .add_uint16(cls)
            .add_uint16(1)  # field count
            .add_uint16(setname)
            .add_string("override")
        )
        sender.send(dg)

        # Confirm the object is now alive in the SS.
        sender.wait_object_alive(do_id, sender=SENDER, timeout=5.0)

        dg = (
            Datagram.create([do_id], sender=SENDER, msgtype=STATESERVER_OBJECT_GET_ALL)
            .add_uint32(0xAA)
            .add_uint32(do_id)
        )
        sender.send(dg)
        it = DatagramIterator(sender.recv(timeout=5.0))
        _, _, mt = it.read_header()
        assert mt == STATESERVER_OBJECT_GET_ALL_RESP

    @pytest.mark.skip(
        reason="DBSS_OBJECT_DELETE_FIELD_RAM / DELETE_FIELDS_RAM have no handler in database_state_server.cpp"
    )
    def test_delete_field_ram(self, dbss, channel_conn):
        pass
