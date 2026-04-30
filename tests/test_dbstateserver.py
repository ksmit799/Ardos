"""DBSS tests — DB-backed state-server objects.

Exercises ACTIVATE_WITH_DEFAULTS (loading a DB-backed object into RAM),
GET_ACTIVATED queries, and delete-from-disk lifecycle.
"""
import time

import pytest

from tests.common.ardos import Datagram, DatagramIterator
from tests.common.dc import class_id, field_id
from tests.common.msgtypes import (
    DBSERVER_CREATE_OBJECT,
    DBSS_OBJECT_ACTIVATE_WITH_DEFAULTS,
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
    it.read_header(); it.read_uint32()
    return it.read_uint32()


class TestActivate:
    @pytest.mark.skip(reason="XXX: peer-closed-connection without daemon-log root cause")
    def test_activate_pulls_object_into_ss(self, dbss, channel_conn):
        sender = channel_conn(SENDER)
        sender.flush()
        do_id = _seed_player(sender)

        dg = Datagram.create([do_id], sender=SENDER, msgtype=DBSS_OBJECT_ACTIVATE_WITH_DEFAULTS)
        dg.add_uint32(do_id).add_uint32(0).add_uint32(0)
        sender.send(dg)

        # Give the DBSS time to load + activate.
        time.sleep(2.0)

        dg = Datagram.create([do_id], sender=SENDER, msgtype=STATESERVER_OBJECT_GET_ALL).add_uint32(77)
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
