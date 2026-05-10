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
    DBSERVER_OBJECT_DELETE_FIELD,
    DBSERVER_OBJECT_DELETE_FIELDS,
    DBSERVER_OBJECT_GET_ALL,
    DBSERVER_OBJECT_GET_ALL_RESP,
    DBSERVER_OBJECT_GET_FIELD,
    DBSERVER_OBJECT_GET_FIELD_RESP,
    DBSERVER_OBJECT_GET_FIELDS,
    DBSERVER_OBJECT_GET_FIELDS_RESP,
    DBSERVER_OBJECT_SET_FIELD,
    DBSERVER_OBJECT_SET_FIELDS,
    DBSERVER_OBJECT_SET_FIELD_IF_EMPTY,
    DBSERVER_OBJECT_SET_FIELD_IF_EMPTY_RESP,
    DBSERVER_OBJECT_SET_FIELD_IF_EQUALS,
    DBSERVER_OBJECT_SET_FIELD_IF_EQUALS_RESP,
    DBSERVER_OBJECT_SET_FIELDS_IF_EQUALS,
    DBSERVER_OBJECT_SET_FIELDS_IF_EQUALS_RESP,
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
    dg.add_uint32(1)  # context
    dg.add_uint16(cls)  # dcId
    dg.add_uint16(1)  # fieldCount
    dg.add_uint16(setname)  # field id
    dg.add_string(name)  # value (matches DCPacker for a string field)
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


class TestGetSet:
    def _make_and_get_id(self, sender, name="bob") -> int:
        sender.send(_create_player(name))
        it = DatagramIterator(sender.recv(timeout=5.0))
        it.read_header()
        it.read_uint32()
        return it.read_uint32()

    def test_get_field_round_trip(self, db, channel_conn):
        sender = channel_conn(SENDER)
        sender.flush()
        do_id = self._make_and_get_id(sender)

        setname = field_id("test.dc", "DistributedPlayer", "setName")
        dg = Datagram.create(
            [DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_GET_FIELD
        )
        dg.add_uint32(42).add_uint32(do_id).add_uint16(setname)
        sender.send(dg)

        it = DatagramIterator(sender.recv(timeout=5.0))
        _, _, mt = it.read_header()
        assert mt == DBSERVER_OBJECT_GET_FIELD_RESP
        assert it.read_uint32() == 42  # context
        assert it.read_uint8() == 1  # success
        assert it.read_uint16() == setname
        assert it.read_string() == "bob"

    def test_set_then_get(self, db, channel_conn):
        sender = channel_conn(SENDER)
        sender.flush()
        do_id = self._make_and_get_id(sender, "alice")

        setname = field_id("test.dc", "DistributedPlayer", "setName")
        dg = Datagram.create(
            [DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_SET_FIELD
        )
        dg.add_uint32(do_id).add_uint16(setname).add_string("alice2")
        sender.send(dg)

        dg = Datagram.create(
            [DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_GET_FIELD
        )
        dg.add_uint32(1).add_uint32(do_id).add_uint16(setname)
        sender.send(dg)
        it = DatagramIterator(sender.recv(timeout=5.0))
        it.read_header()
        it.read_uint32()
        assert it.read_uint8() == 1
        it.read_uint16()
        assert it.read_string() == "alice2"


class TestDelete:
    def test_delete_removes_object(self, db, channel_conn):
        sender = channel_conn(SENDER)
        sender.flush()
        sender.send(_create_player("victim"))
        it = DatagramIterator(sender.recv(timeout=5.0))
        it.read_header()
        it.read_uint32()
        do_id = it.read_uint32()

        dg = Datagram.create(
            [DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_DELETE
        )
        dg.add_uint32(do_id)
        sender.send(dg)

        # GET_ALL on a deleted object returns failure (success byte = 0).
        dg = Datagram.create(
            [DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_GET_ALL
        )
        dg.add_uint32(9).add_uint32(do_id)
        sender.send(dg)
        it = DatagramIterator(sender.recv(timeout=5.0))
        _, _, mt = it.read_header()
        assert mt == DBSERVER_OBJECT_GET_ALL_RESP
        assert it.read_uint32() == 9
        assert it.read_uint8() == 0


class TestDBMultiField:
    """Multi-field and conditional set/get/delete on the DB.

    Wire formats from src/database/database_server.cpp:
      - GET_FIELDS:     [ctx][doId][count][fieldId]+
      - SET_FIELDS:     [doId][count][fieldId, value]+
      - SET_FIELD_IF_EMPTY:    [ctx][doId][fieldId][value]
      - SET_FIELD_IF_EQUALS:   [ctx][doId][fieldId][expected][newValue]
      - SET_FIELDS_IF_EQUALS:  [ctx][doId][count][fieldId, expected, newValue]+
      - DELETE_FIELD:   [doId][fieldId]
      - DELETE_FIELDS:  [doId][count][fieldId]+
    """

    def _create_and_get_id(self, sender, name="multi") -> int:
        sender.send(_create_player(name))
        it = DatagramIterator(sender.recv(timeout=5.0))
        it.read_header()
        it.read_uint32()
        return it.read_uint32()

    def _setname_field(self) -> int:
        return field_id("test.dc", "DistributedPlayer", "setName")

    def test_get_fields_round_trip(self, db, channel_conn):
        sender = channel_conn(SENDER)
        sender.flush()
        do_id = self._create_and_get_id(sender, "alpha")

        setname = self._setname_field()
        dg = (
            Datagram.create(
                [DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_GET_FIELDS
            )
            .add_uint32(50)
            .add_uint32(do_id)
            .add_uint16(1)
            .add_uint16(setname)
        )
        sender.send(dg)
        it = DatagramIterator(sender.recv(timeout=5.0))
        _, _, mt = it.read_header()
        assert mt == DBSERVER_OBJECT_GET_FIELDS_RESP
        assert it.read_uint32() == 50
        assert it.read_uint8() == 1  # success
        assert it.read_uint16() == 1  # count

    def test_set_fields_round_trip(self, db, channel_conn):
        sender = channel_conn(SENDER)
        sender.flush()
        do_id = self._create_and_get_id(sender, "beta")

        setname = self._setname_field()
        dg = (
            Datagram.create(
                [DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_SET_FIELDS
            )
            .add_uint32(do_id)
            .add_uint16(1)
            .add_uint16(setname)
            .add_string("beta-set")
        )
        sender.send(dg)

        # Verify with a follow-up GET_FIELD.
        dg = (
            Datagram.create(
                [DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_GET_FIELD
            )
            .add_uint32(60)
            .add_uint32(do_id)
            .add_uint16(setname)
        )
        sender.send(dg)
        it = DatagramIterator(sender.recv(timeout=5.0))
        it.read_header()
        it.read_uint32()
        assert it.read_uint8() == 1
        it.read_uint16()
        assert it.read_string() == "beta-set"

    def test_set_field_if_empty_when_present(self, db, channel_conn):
        """When the field already exists, SET_FIELD_IF_EMPTY responds with
        failure and includes the existing field."""
        sender = channel_conn(SENDER)
        sender.flush()
        do_id = self._create_and_get_id(sender, "gamma")

        setname = self._setname_field()
        dg = (
            Datagram.create(
                [DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_SET_FIELD_IF_EMPTY
            )
            .add_uint32(70)
            .add_uint32(do_id)
            .add_uint16(setname)
            .add_string("not-applied")
        )
        sender.send(dg)

        # No reply is sent on success/failure for SET_FIELD_IF_EMPTY in the
        # current implementation — verify via GET_FIELD that name is unchanged.
        dg = (
            Datagram.create(
                [DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_GET_FIELD
            )
            .add_uint32(71)
            .add_uint32(do_id)
            .add_uint16(setname)
        )
        sender.send(dg)
        it = DatagramIterator(sender.recv(timeout=5.0))
        it.read_header()
        it.read_uint32()
        assert it.read_uint8() == 1
        it.read_uint16()
        assert it.read_string() == "gamma"

    def test_set_field_if_equals_match(self, db, channel_conn):
        """When the expected value matches, SET_FIELD_IF_EQUALS replies with
        success=true and updates the field."""
        sender = channel_conn(SENDER)
        sender.flush()
        do_id = self._create_and_get_id(sender, "delta")

        setname = self._setname_field()
        dg = (
            Datagram.create(
                [DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_SET_FIELD_IF_EQUALS
            )
            .add_uint32(80)
            .add_uint32(do_id)
            .add_uint16(setname)
            .add_string("delta")  # expected
            .add_string("delta-2")  # new value
        )
        sender.send(dg)
        it = DatagramIterator(sender.recv(timeout=5.0))
        _, _, mt = it.read_header()
        assert mt == DBSERVER_OBJECT_SET_FIELD_IF_EQUALS_RESP
        assert it.read_uint32() == 80
        assert it.read_uint8() == 1  # success

    def test_set_field_if_equals_mismatch(self, db, channel_conn):
        """When expected mismatches, RESP carries success=false and the
        actual stored field."""
        sender = channel_conn(SENDER)
        sender.flush()
        do_id = self._create_and_get_id(sender, "epsilon")

        setname = self._setname_field()
        dg = (
            Datagram.create(
                [DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_SET_FIELD_IF_EQUALS
            )
            .add_uint32(90)
            .add_uint32(do_id)
            .add_uint16(setname)
            .add_string("WRONG")
            .add_string("never-applied")
        )
        sender.send(dg)
        it = DatagramIterator(sender.recv(timeout=5.0))
        _, _, mt = it.read_header()
        assert mt == DBSERVER_OBJECT_SET_FIELD_IF_EQUALS_RESP
        assert it.read_uint32() == 90
        assert it.read_uint8() == 0  # failure

    def test_set_fields_if_equals_match(self, db, channel_conn):
        sender = channel_conn(SENDER)
        sender.flush()
        do_id = self._create_and_get_id(sender, "zeta")

        setname = self._setname_field()
        dg = (
            Datagram.create(
                [DB_CHANNEL],
                sender=SENDER,
                msgtype=DBSERVER_OBJECT_SET_FIELDS_IF_EQUALS,
            )
            .add_uint32(100)
            .add_uint32(do_id)
            .add_uint16(1)
            .add_uint16(setname)
            .add_string("zeta")
            .add_string("zeta-2")
        )
        sender.send(dg)
        it = DatagramIterator(sender.recv(timeout=5.0))
        _, _, mt = it.read_header()
        assert mt == DBSERVER_OBJECT_SET_FIELDS_IF_EQUALS_RESP
        assert it.read_uint32() == 100
        assert it.read_uint8() == 1

    def test_delete_field(self, db, channel_conn):
        sender = channel_conn(SENDER)
        sender.flush()
        do_id = self._create_and_get_id(sender, "eta")

        setname = self._setname_field()
        dg = (
            Datagram.create(
                [DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_DELETE_FIELD
            )
            .add_uint32(do_id)
            .add_uint16(setname)
        )
        sender.send(dg)

        # Verify: GET_FIELD on the deleted field — the DB falls back to the
        # default value. setName has no default so the GET should still
        # respond (success may be true with default, or false if no fallback).
        dg = (
            Datagram.create(
                [DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_GET_FIELD
            )
            .add_uint32(110)
            .add_uint32(do_id)
            .add_uint16(setname)
        )
        sender.send(dg)
        it = DatagramIterator(sender.recv(timeout=5.0))
        _, _, mt = it.read_header()
        assert mt == DBSERVER_OBJECT_GET_FIELD_RESP
        assert it.read_uint32() == 110

    def test_delete_fields(self, db, channel_conn):
        sender = channel_conn(SENDER)
        sender.flush()
        do_id = self._create_and_get_id(sender, "theta")

        setname = self._setname_field()
        dg = (
            Datagram.create(
                [DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_DELETE_FIELDS
            )
            .add_uint32(do_id)
            .add_uint16(1)
            .add_uint16(setname)
        )
        sender.send(dg)
        # No response message; verify via a follow-up GET_FIELD round trip.
        dg = (
            Datagram.create(
                [DB_CHANNEL], sender=SENDER, msgtype=DBSERVER_OBJECT_GET_FIELD
            )
            .add_uint32(120)
            .add_uint32(do_id)
            .add_uint16(setname)
        )
        sender.send(dg)
        it = DatagramIterator(sender.recv(timeout=5.0))
        _, _, mt = it.read_header()
        assert mt == DBSERVER_OBJECT_GET_FIELD_RESP
        assert it.read_uint32() == 120
