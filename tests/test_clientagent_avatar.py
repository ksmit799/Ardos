"""End-to-end client-avatar tests — the flow that actually matters in prod.

A real deployment has an AI process that, after the client authenticates with
an UberDOG, creates an avatar object in the state server and hands ownership
to the client. The client then sees `CLIENT_ENTER_OBJECT_REQUIRED_OWNER`,
can emit `clsend`/`ownsend` field updates on it, and can observe broadcasts
from other objects in its interest. Before this file the CA tests only
covered handshake/heartbeat/anonymous cases — none of the real ownership
path.

These tests use a "dummy AI" (see `AIConnection` in tests/common/ardos.py)
to exercise:

  - CLIENTAGENT_SET_STATE     (jump a client straight to ESTABLISHED)
  - STATESERVER_CREATE_OBJECT_WITH_REQUIRED
  - STATESERVER_OBJECT_SET_OWNER  → CLIENT_ENTER_OBJECT_REQUIRED_OWNER
  - CLIENTAGENT_ADD_INTEREST   (AI-authored interest)
  - CLIENT_OBJECT_SET_FIELD    (round-trips: clsend out, broadcast in)
  - CLIENTAGENT_ADD_SESSION_OBJECT + STATESERVER_OBJECT_DELETE_RAM
    → CLIENT_EJECT with CLIENT_DISCONNECT_SESSION_OBJECT_DELETED
"""
from __future__ import annotations

import pytest

from tests.common.ardos import (
    AUTH_STATE_ESTABLISHED,
    Datagram,
    DatagramIterator,
)
from tests.common.dc import class_id, dc_hash, field_id
from tests.common.msgtypes import (
    CLIENT_DISCONNECT_SESSION_OBJECT_DELETED,
    STATESERVER_OBJECT_SET_FIELD,
)

# Pin the CA's client-channel pool to a single value so the AI knows exactly
# what channel to target without having to scrape a broadcast channel.
CLIENT_CHANNEL = 1_000_000_000

# Avatar placement.
AVATAR_PARENT = 0
AVATAR_ZONE = 10  # must appear in whitelist for interest tests
AVATAR_DOID = 2_000_001
PEER_DOID = 2_000_002  # second object in the same zone for broadcast tests


@pytest.fixture
def cluster(ardos):
    """MD + SS + CA pinned for deterministic avatar-ownership tests."""
    return ardos(
        md=True, ss=True, ca=True,
        overrides={
            "client-agent": {
                "avatar-class": "DistributedPlayer",
                "channels": {"min": CLIENT_CHANNEL, "max": CLIENT_CHANNEL},
                "interest": {
                    "client": "all",
                    "mode": "whitelist",
                    "zones": [0, 5, 10, "100-399"],
                },
            },
        },
    )


def _hello(client):
    client.hello(dc_hash("test.dc"), "dev")
    client.expect_hello_resp()


def _required_setname(name: str) -> bytes:
    """Packed required-field payload for DistributedPlayer, which has only
    setName required. DC atomic string field = [uint16 len][bytes]."""
    return Datagram().add_string(name).bytes()


def _establish_and_own(ai, client, *, name="Alice") -> None:
    """Common setup: take a hello'd client, give it ownership of a fresh
    DistributedPlayer avatar at (AVATAR_PARENT, AVATAR_ZONE)."""
    ai.set_client_state(CLIENT_CHANNEL, AUTH_STATE_ESTABLISHED)
    ai.create_object(
        do_id=AVATAR_DOID,
        parent=AVATAR_PARENT,
        zone=AVATAR_ZONE,
        dclass_id=class_id("test.dc", "DistributedPlayer"),
        required=_required_setname(name),
    )
    ai.set_owner(AVATAR_DOID, CLIENT_CHANNEL)


class TestOwnershipHandoff:
    def test_set_owner_delivers_owner_entry_to_client(
        self, cluster, ai_conn, client_conn
    ):
        client = client_conn()
        _hello(client)
        ai = ai_conn()
        _establish_and_own(ai, client, name="Alice")

        entry = client.expect_object_entry(owner=True, timeout=5.0)
        assert entry.do_id == AVATAR_DOID
        assert entry.parent == AVATAR_PARENT
        assert entry.zone == AVATAR_ZONE
        assert entry.dc_id == class_id("test.dc", "DistributedPlayer")
        # setName was the only required field we supplied; it must round-trip
        # back to the client verbatim.
        assert entry.required == _required_setname("Alice")

    def test_non_owner_entry_when_interest_opens_without_ownership(
        self, cluster, ai_conn, client_conn
    ):
        """Same create, but we open an interest instead of assigning owner —
        the client should see the non-owner variant."""
        client = client_conn()
        _hello(client)
        ai = ai_conn()
        ai.set_client_state(CLIENT_CHANNEL, AUTH_STATE_ESTABLISHED)
        ai.create_object(
            do_id=AVATAR_DOID,
            parent=AVATAR_PARENT,
            zone=AVATAR_ZONE,
            dclass_id=class_id("test.dc", "DistributedPlayer"),
            required=_required_setname("Bob"),
        )
        ai.add_interest(CLIENT_CHANNEL, interest_id=1, parent=AVATAR_PARENT, zone=AVATAR_ZONE)

        entry = client.expect_object_entry(owner=False, timeout=5.0)
        assert entry.do_id == AVATAR_DOID
        assert entry.dc_id == class_id("test.dc", "DistributedPlayer")


class TestFieldRouting:
    def test_clsend_forwards_to_stateserver(self, cluster, ai_conn, client_conn):
        """sendChat() is clsend broadcast — client emits it, SS receives a
        STATESERVER_OBJECT_SET_FIELD on the avatar's channel."""
        client = client_conn()
        _hello(client)
        # Subscribe to the avatar's channel BEFORE the client sends — we need
        # to observe the forwarded SET_FIELD.
        ai = ai_conn()
        _establish_and_own(ai, client)
        client.expect_object_entry(owner=True)
        ai.subscribe(AVATAR_DOID)
        ai.flush()

        fid = field_id("test.dc", "DistributedPlayer", "sendChat")
        msg = Datagram().add_string("hi everyone").bytes()
        client.send_field(AVATAR_DOID, fid, msg)

        # Receive the forwarded update. The CA sets sender=_channel
        # (the client's allocated channel).
        dg = ai.recv(timeout=5.0)
        it = DatagramIterator(dg)
        recipients, sender, mt = it.read_header()
        assert AVATAR_DOID in recipients
        assert mt == STATESERVER_OBJECT_SET_FIELD
        assert sender == CLIENT_CHANNEL
        assert it.read_uint32() == AVATAR_DOID
        assert it.read_uint16() == fid
        assert it.read_string() == "hi everyone"

    def test_server_broadcast_reaches_client(self, cluster, ai_conn, client_conn):
        """AI publishes SET_FIELD on the owned avatar; because sendChat is
        `broadcast`, the CA delivers it as CLIENT_OBJECT_SET_FIELD."""
        client = client_conn()
        _hello(client)
        ai = ai_conn()
        _establish_and_own(ai, client)
        client.expect_object_entry(owner=True)

        fid = field_id("test.dc", "DistributedPlayer", "sendChat")
        payload = Datagram().add_string("broadcast!").bytes()
        ai.set_field(AVATAR_DOID, fid, payload)

        update = client.expect_object_set_field(do_id=AVATAR_DOID, timeout=5.0)
        assert update.field_id == fid
        assert update.payload == payload


class TestSessionObjects:
    def test_session_object_delete_ejects_client(
        self, cluster, ai_conn, client_conn
    ):
        client = client_conn()
        _hello(client)
        ai = ai_conn()
        _establish_and_own(ai, client)
        client.expect_object_entry(owner=True)

        ai.add_session_object(CLIENT_CHANNEL, AVATAR_DOID)
        ai.delete_object(AVATAR_DOID)

        reason, msg = client.expect_eject(
            reason=CLIENT_DISCONNECT_SESSION_OBJECT_DELETED, timeout=5.0
        )
        assert str(AVATAR_DOID) in msg
