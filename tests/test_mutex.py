"""Mutex keyword tests.

A field tagged `mutex` in the DC file locks per client at the CA when
called, a second call while locked ejects the client, and the lock is
released by CLIENTAGENT_MUTEX_RELEASE, expired by mutex-timeout, or
dropped with the connection. CLIENTAGENT_MUTEX_ACQUIRE lets the internal
side lock additional mutexed fields.
"""

import time

import pytest

from tests.common.ardos import Datagram, DatagramIterator
from tests.common.dc import dc_hash, field_id
from tests.common.msgtypes import (
    CLIENT_DISCONNECT_MUTEX_VIOLATION,
    CLIENT_HEARTBEAT,
    CLIENTAGENT_MUTEX_ACQUIRE,
    CLIENTAGENT_MUTEX_RELEASE,
    CLIENTAGENT_SEND_DATAGRAM,
    STATESERVER_OBJECT_SET_FIELD,
)

UD_DOID = 4665
CLIENT_CHANNEL = 1_600_000_001
SYNC_SENTINEL = 0xBEEF


def _boot(ardos, mutex_timeout=0):
    return ardos(
        md=True,
        ca=True,
        uberdogs=[{"id": UD_DOID, "class": "AuthManager", "anonymous": True}],
        overrides={
            "client-agent": {
                "channels": {"min": CLIENT_CHANNEL, "max": CLIENT_CHANNEL},
                "mutex-timeout": mutex_timeout,
            },
        },
    )


def _watch_uberdog(channel_conn):
    """Subscribe the uberdog's channel and self-probe until the
    subscription answers, so a forwarded call can't race it."""
    watcher = channel_conn(UD_DOID)
    watcher.send(Datagram.create([UD_DOID], sender=0, msgtype=SYNC_SENTINEL))
    watcher.wait_for(_is_msgtype(SYNC_SENTINEL))
    return watcher


def _is_msgtype(msgtype):
    def pred(dg):
        try:
            it = DatagramIterator(dg)
            _, _, mt = it.read_header()
            return mt == msgtype
        except Exception:
            return False

    return pred


def _connect(client_conn):
    c = client_conn()
    c.hello(dc_hash("test.dc"), "dev")
    c.expect_hello_resp()
    return c


def _sync_internal(ai, client):
    """Frames from the AI to the client's channel are FIFO, a heartbeat
    arriving at the client proves everything sent before it was handled."""
    ai.send(
        Datagram.create(
            [CLIENT_CHANNEL], sender=ai.ai_channel, msgtype=CLIENTAGENT_SEND_DATAGRAM
        ).add_raw(Datagram.create_client(CLIENT_HEARTBEAT).bytes())
    )
    got = client.recv(timeout=3.0)
    assert DatagramIterator(got).read_client_msgtype() == CLIENT_HEARTBEAT


def _string_payload(value="tok"):
    return Datagram().add_string(value).bytes()


def _uint8_payload(value=1):
    return Datagram().add_uint8(value).bytes()


def test_double_call_ejects(ardos, channel_conn, client_conn):
    """A second call to a locked mutex field ejects with the dedicated
    reason code, the first call still forwards."""
    _boot(ardos)
    watcher = _watch_uberdog(channel_conn)
    client = _connect(client_conn)
    fid = field_id("test.dc", "AuthManager", "lockedCall")

    client.send_field(UD_DOID, fid, _string_payload())
    client.send_field(UD_DOID, fid, _string_payload())

    watcher.wait_for(_is_msgtype(STATESERVER_OBJECT_SET_FIELD))
    client.expect_eject(reason=CLIENT_DISCONNECT_MUTEX_VIOLATION)


def test_release_unlocks(ardos, channel_conn, client_conn, ai_conn):
    """MUTEX_RELEASE from the internal side unlocks the field, a
    following call forwards instead of ejecting."""
    _boot(ardos)
    watcher = _watch_uberdog(channel_conn)
    client = _connect(client_conn)
    ai = ai_conn()
    fid = field_id("test.dc", "AuthManager", "lockedCall")

    client.send_field(UD_DOID, fid, _string_payload())
    watcher.wait_for(_is_msgtype(STATESERVER_OBJECT_SET_FIELD))

    ai.send(
        Datagram.create(
            [CLIENT_CHANNEL], sender=ai.ai_channel, msgtype=CLIENTAGENT_MUTEX_RELEASE
        )
        .add_uint32(UD_DOID)
        .add_uint16(fid)
    )
    _sync_internal(ai, client)

    client.send_field(UD_DOID, fid, _string_payload())
    watcher.wait_for(_is_msgtype(STATESERVER_OBJECT_SET_FIELD))
    assert client.recv_maybe(timeout=0.5) is None


def test_acquire_locks_other_field(ardos, channel_conn, client_conn, ai_conn):
    """MUTEX_ACQUIRE locks an additional mutexed field, the client's
    call to it ejects."""
    _boot(ardos)
    _watch_uberdog(channel_conn)
    client = _connect(client_conn)
    ai = ai_conn()
    fid = field_id("test.dc", "AuthManager", "otherLockedCall")

    ai.send(
        Datagram.create(
            [CLIENT_CHANNEL], sender=ai.ai_channel, msgtype=CLIENTAGENT_MUTEX_ACQUIRE
        )
        .add_uint32(UD_DOID)
        .add_uint16(fid)
    )
    _sync_internal(ai, client)

    client.send_field(UD_DOID, fid, _uint8_payload())
    client.expect_eject(reason=CLIENT_DISCONNECT_MUTEX_VIOLATION)


def test_acquire_untagged_field_ignored(ardos, channel_conn, client_conn, ai_conn):
    """MUTEX_ACQUIRE on a field without the mutex keyword warns and does
    nothing, the client's call forwards freely."""
    _boot(ardos)
    watcher = _watch_uberdog(channel_conn)
    client = _connect(client_conn)
    ai = ai_conn()
    fid = field_id("test.dc", "AuthManager", "plainCall")

    ai.send(
        Datagram.create(
            [CLIENT_CHANNEL], sender=ai.ai_channel, msgtype=CLIENTAGENT_MUTEX_ACQUIRE
        )
        .add_uint32(UD_DOID)
        .add_uint16(fid)
    )
    _sync_internal(ai, client)

    client.send_field(UD_DOID, fid, _uint8_payload())
    watcher.wait_for(_is_msgtype(STATESERVER_OBJECT_SET_FIELD))
    assert client.recv_maybe(timeout=0.5) is None


def test_timeout_expires_lock(ardos, channel_conn, client_conn):
    """With mutex-timeout set, an unreleased lock expires and a retry
    forwards instead of ejecting."""
    _boot(ardos, mutex_timeout=300)
    watcher = _watch_uberdog(channel_conn)
    client = _connect(client_conn)
    fid = field_id("test.dc", "AuthManager", "lockedCall")

    client.send_field(UD_DOID, fid, _string_payload())
    watcher.wait_for(_is_msgtype(STATESERVER_OBJECT_SET_FIELD))

    time.sleep(0.6)

    client.send_field(UD_DOID, fid, _string_payload())
    watcher.wait_for(_is_msgtype(STATESERVER_OBJECT_SET_FIELD))
    assert client.recv_maybe(timeout=0.5) is None
