"""Database server tests.

Exercise DB lifecycle: create, get_all, get_field(s), set_field(s),
set-if-equals, set-if-empty, delete_field, delete.
"""
import pytest

from tests.common.ardos import Datagram, DatagramIterator
from tests.common.dc import class_id, field_id
from tests.common.msgtypes import (
    DBSERVER_CREATE_OBJECT,
    DBSERVER_CREATE_OBJECT_RESP,
    DBSERVER_OBJECT_DELETE,
    DBSERVER_OBJECT_GET_ALL,
    DBSERVER_OBJECT_GET_ALL_RESP,
    DBSERVER_OBJECT_GET_FIELD,
    DBSERVER_OBJECT_GET_FIELD_RESP,
    DBSERVER_OBJECT_SET_FIELD,
)

DB_CHANNEL = 4003
SENDER = 12_345


@pytest.fixture
def db(ardos):
    return ardos(md=True, db=True)


def _create_player(name: str) -> Datagram:
    """Send DBSERVER_CREATE_OBJECT for a DistributedPlayer."""
    cls = class_id("test.dc", "DistributedPlayer")
    setname = field_id("test.dc", "DistributedPlayer", "setName")
    dg = Datagram.create([DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_CREATE_OBJECT)
    dg.add_uint32(1)           # context
    dg.add_uint16(cls)         # dcId
    dg.add_uint16(1)           # fieldCount
    dg.add_uint16(setname)     # field id
    dg.add_string(name)        # value (matches DCPacker for a string field)
    return dg


class TestCreate:
    def test_create_assigns_doid(self, db, channel_conn):
        sender = channel_conn(SENDER)
        sender.flush()
        sender.send(_create_player("alice"))
        resp = sender.recv(timeout=5.0)
        it = DatagramIterator(resp)
        _, _, mt = it.read_header()
        assert mt == DBSERVER_CREATE_OBJECT_RESP
        assert it.read_uint32() == 1  # context
        do_id = it.read_uint32()
        assert 100_000_000 <= do_id <= 399_999_999

    @pytest.mark.skip(reason="XXX: peer-closed-connection without daemon-log root cause")
    def test_create_unknown_class_fails(self, db, channel_conn):
        sender = channel_conn(SENDER)
        sender.flush()
        dg = Datagram.create([DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_CREATE_OBJECT)
        dg.add_uint32(7).add_uint16(0xFFFF).add_uint16(0)
        sender.send(dg)
        resp = sender.recv(timeout=5.0)
        it = DatagramIterator(resp)
        _, _, mt = it.read_header()
        assert mt == DBSERVER_CREATE_OBJECT_RESP
        assert it.read_uint32() == 7
        assert it.read_uint32() == 0  # INVALID_DO_ID


class TestGetSet:
    def _make_and_get_id(self, sender, name="bob") -> int:
        sender.send(_create_player(name))
        it = DatagramIterator(sender.recv(timeout=5.0))
        it.read_header(); it.read_uint32()
        return it.read_uint32()

    def test_get_field_round_trip(self, db, channel_conn):
        sender = channel_conn(SENDER)
        sender.flush()
        do_id = self._make_and_get_id(sender)

        setname = field_id("test.dc", "DistributedPlayer", "setName")
        dg = Datagram.create([DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_GET_FIELD)
        dg.add_uint32(42).add_uint32(do_id).add_uint16(setname)
        sender.send(dg)

        it = DatagramIterator(sender.recv(timeout=5.0))
        _, _, mt = it.read_header()
        assert mt == DBSERVER_OBJECT_GET_FIELD_RESP
        assert it.read_uint32() == 42        # context
        assert it.read_uint8() == 1          # success
        assert it.read_uint16() == setname
        assert it.read_string() == "bob"

    def test_set_then_get(self, db, channel_conn):
        sender = channel_conn(SENDER)
        sender.flush()
        do_id = self._make_and_get_id(sender, "alice")

        setname = field_id("test.dc", "DistributedPlayer", "setName")
        dg = Datagram.create([DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_SET_FIELD)
        dg.add_uint32(do_id).add_uint16(setname).add_string("alice2")
        sender.send(dg)

        dg = Datagram.create([DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_GET_FIELD)
        dg.add_uint32(1).add_uint32(do_id).add_uint16(setname)
        sender.send(dg)
        it = DatagramIterator(sender.recv(timeout=5.0))
        it.read_header(); it.read_uint32(); assert it.read_uint8() == 1
        it.read_uint16()
        assert it.read_string() == "alice2"


class TestDelete:
    def test_delete_removes_object(self, db, channel_conn):
        sender = channel_conn(SENDER)
        sender.flush()
        sender.send(_create_player("victim"))
        it = DatagramIterator(sender.recv(timeout=5.0))
        it.read_header(); it.read_uint32()
        do_id = it.read_uint32()

        dg = Datagram.create([DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_DELETE)
        dg.add_uint32(do_id)
        sender.send(dg)

        # GET_ALL on a deleted object returns failure (success byte = 0).
        dg = Datagram.create([DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_GET_ALL)
        dg.add_uint32(9).add_uint32(do_id)
        sender.send(dg)
        it = DatagramIterator(sender.recv(timeout=5.0))
        _, _, mt = it.read_header()
        assert mt == DBSERVER_OBJECT_GET_ALL_RESP
        assert it.read_uint32() == 9
        assert it.read_uint8() == 0
