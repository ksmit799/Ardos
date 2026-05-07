"""setParentingRules coverage.

Ardos parses `setParentingRules(string type, string Rule)` on a class the
first time a client's avatar gets assigned to a parent of that class
(see src/clientagent/client_participant_interest.cpp:447). The parser
itself lives in src/clientagent/parenting_rule.cpp and recognises four
kinds:

  - Stated      — no extra zones opened
  - Follow      — mirror the avatar's current zone
  - Cartesian   — "startZone:gridSize:radius" grid window
  - Auto        — "originZone:z1|z2|..." on interest open

This file has two tiers:

* Smoke: DC parses and CA boots with every valid rule kind plus a
  malformed one (warn-and-continue semantics).
* Integration: Cartesian opens zone-window around the avatar's cell
  automatically. A peer object in an adjacent cell must become visible
  the moment the avatar gets ownership; an avatar whose parent has no
  rule (Stated) must NOT see that peer.
"""

import time

import pytest

from tests.common.ardos import (
    AUTH_STATE_ESTABLISHED,
    Datagram,
    DatagramIterator,
    ObjectEntry,
)
from tests.common.dc import class_id, dc_hash
from tests.common.msgtypes import (
    CLIENT_ENTER_OBJECT_REQUIRED,
    CLIENT_ENTER_OBJECT_REQUIRED_OTHER,
    CLIENT_ENTER_OBJECT_REQUIRED_OTHER_OWNER,
    CLIENT_ENTER_OBJECT_REQUIRED_OWNER,
)

CLIENT_CHANNEL = 1_000_000_000
PARENT_DOID = 9_001
PEER_DOID = 9_002
AVATAR_DOID = 9_003


@pytest.fixture
def ca(ardos):
    return ardos(md=True, ss=True, ca=True)


@pytest.fixture
def avatar_cluster(ardos):
    """MD+SS+CA with avatar-class configured + a pinned client channel so
    the AI-side driver knows where to send CLIENTAGENT_* messages."""
    return ardos(
        md=True,
        ss=True,
        ca=True,
        overrides={
            "client-agent": {
                "avatar-class": "DistributedPlayer",
                "channels": {"min": CLIENT_CHANNEL, "max": CLIENT_CHANNEL},
                "interest": {"client": "all"},
            },
        },
    )


def test_dc_exposes_all_parenting_rule_classes():
    # Sanity: every rule-bearing class is reachable from the test DC.
    for name in (
        "DistributedAvatarStated",
        "DistributedAvatarFollow",
        "DistributedAvatarCartesian",
        "DistributedAvatarAuto",
        "DistributedAvatarBadCartesian",
    ):
        assert class_id("test.dc", name) >= 0


def test_ca_boots_with_all_rule_kinds_and_accepts_hello(ca, client_conn):
    """DC load + hash match + hello round-trip across the full rule set."""
    c = client_conn()
    c.hello(dc_hash("test.dc"), "dev")
    c.expect_hello_resp()


# ---------------------------------------------------------------------------
# Integration: Cartesian rule auto-opens surrounding zones when an avatar's
# parent is an instance of a class with setParentingRules("Cartesian", ...).
# ---------------------------------------------------------------------------


def _required_setname_and_rule(name: str, type_: str, rule: str) -> bytes:
    """Required payload for a DistributedAvatar{Stated,Cartesian,...} object.
    These classes inherit from DistributedPlayer (setName required) and add
    setParentingRules (required, two strings)."""
    return Datagram().add_string(name).add_string(type_).add_string(rule).bytes()


def _required_uint32(x: int) -> bytes:
    """Required payload for DistributedTestObject1 (setRequired1: uint32)."""
    return Datagram().add_uint32(x).bytes()


def _required_setname(name: str) -> bytes:
    return Datagram().add_string(name).bytes()


_DC_DEFAULT_RULE = {
    "DistributedAvatarStated": ("Stated", ""),
    "DistributedAvatarFollow": ("Follow", ""),
    "DistributedAvatarCartesian": ("Cartesian", "1000:4:1"),
    "DistributedAvatarAuto": ("Auto", "2000:2001|2002|2003"),
}


def _setup_peer_and_avatar(
    ai, client_conn, parent_dclass_name: str, avatar_zone: int, peer_zone: int
):
    """Build the standard Cartesian-integration fixture state.

    Returns the client connection. The peer is created at `peer_zone`, the
    avatar at `avatar_zone`, both under PARENT_DOID of class
    `parent_dclass_name`. Runtime required-field values match the DC
    defaults even though `TryParseParentingRule` reads the defaults directly
    — supplying matching values keeps the wire state legible when logs are
    dumped on failure.
    """
    client = client_conn()
    client.hello(dc_hash("test.dc"), "dev")
    client.expect_hello_resp()
    ai.set_client_state(CLIENT_CHANNEL, AUTH_STATE_ESTABLISHED)

    parent_cls = class_id("test.dc", parent_dclass_name)
    rule_type, rule_str = _DC_DEFAULT_RULE[parent_dclass_name]
    ai.create_object(
        do_id=PARENT_DOID,
        parent=0,
        zone=0,
        dclass_id=parent_cls,
        required=_required_setname_and_rule("Room", rule_type, rule_str),
    )
    # The SS binds each DO's DoId queue asynchronously through RabbitMQ;
    # wait on a real signal (GET_LOCATION_RESP round-trip) before issuing
    # follow-up messages so they don't race the bind.
    ai.wait_object_alive(PARENT_DOID)

    # Peer object for the client to discover.
    peer_cls = class_id("test.dc", "DistributedTestObject1")
    ai.create_object(
        do_id=PEER_DOID,
        parent=PARENT_DOID,
        zone=peer_zone,
        dclass_id=peer_cls,
        required=_required_uint32(777),
    )
    ai.wait_object_alive(PEER_DOID)

    # Avatar — DistributedPlayer so it passes the IsClassOrDerivedFrom
    # check against avatar-class=DistributedPlayer.
    avatar_cls = class_id("test.dc", "DistributedPlayer")
    ai.create_object(
        do_id=AVATAR_DOID,
        parent=PARENT_DOID,
        zone=avatar_zone,
        dclass_id=avatar_cls,
        required=_required_setname("Alice"),
    )
    ai.wait_object_alive(AVATAR_DOID)
    ai.set_owner(AVATAR_DOID, CLIENT_CHANNEL)
    return client


_ENTRY_MSGTYPES = {
    CLIENT_ENTER_OBJECT_REQUIRED,
    CLIENT_ENTER_OBJECT_REQUIRED_OTHER,
    CLIENT_ENTER_OBJECT_REQUIRED_OWNER,
    CLIENT_ENTER_OBJECT_REQUIRED_OTHER_OWNER,
}
_OWNER_MSGTYPES = {
    CLIENT_ENTER_OBJECT_REQUIRED_OWNER,
    CLIENT_ENTER_OBJECT_REQUIRED_OTHER_OWNER,
}


def _collect_entries(client, expected_count: int, timeout: float = 5.0):
    """Drain object entries until `expected_count` have arrived or `timeout`
    elapses. Non-entry datagrams (CLIENT_OBJECT_LOCATION, bookkeeping, etc.)
    are silently dropped. Order between owner and non-owner is not
    guaranteed — the Cartesian rule fires after an async GET_CLASS
    round-trip — so callers should compare on the resulting set."""
    entries = []
    deadline = time.monotonic() + timeout
    while len(entries) < expected_count:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        dg = client.recv_maybe(timeout=remaining)
        if dg is None:
            break
        it = DatagramIterator(dg)
        mt = it.read_client_msgtype()
        if mt not in _ENTRY_MSGTYPES:
            continue  # bookkeeping / location updates / etc.
        do_id = it.read_uint32()
        parent = it.read_uint32()
        zone = it.read_uint32()
        dc_id = it.read_uint16()
        required = bytes(it._buf[it._off :])
        entries.append(
            ObjectEntry(
                msgtype=mt,
                do_id=do_id,
                parent=parent,
                zone=zone,
                dc_id=dc_id,
                required=required,
                owner=mt in _OWNER_MSGTYPES,
            )
        )
    return entries


class TestCartesianRule:
    def test_adjacent_cell_peer_becomes_visible_via_rule(
        self, avatar_cluster, ai_conn, client_conn
    ):
        """Cartesian rule "1000:4:1" on the parent's class: avatar in cell
        [0,0] (zone 1000) with radius=1 opens zones {1000, 1001, 1004, 1005}.
        Peer at zone 1005 must therefore generate to the client."""
        ai = ai_conn()
        client = _setup_peer_and_avatar(
            ai,
            client_conn,
            parent_dclass_name="DistributedAvatarCartesian",
            avatar_zone=1000,
            peer_zone=1005,
        )

        # The Cartesian rule fires after an async GET_CLASS round-trip, so
        # the avatar's owner entry and the peer's non-owner entry can race.
        # Assert on the set of (do_id, owner-ness) seen, not the order.
        entries = _collect_entries(client, expected_count=2, timeout=8.0)
        by_doid = {e.do_id: e for e in entries}
        assert AVATAR_DOID in by_doid, f"avatar entry missing; got {entries!r}"
        assert PEER_DOID in by_doid, f"peer entry missing; got {entries!r}"
        assert by_doid[PEER_DOID].zone == 1005
        # Peer must reach the client as a non-owner; the avatar may arrive
        # in either form depending on which side of the race lands first.
        assert not by_doid[PEER_DOID].owner

    def test_off_window_peer_is_not_visible(self, avatar_cluster, ai_conn, client_conn):
        """Peer at zone 1015 (cell [3,3]) is outside radius-1 of cell [0,0].
        Cartesian rule must NOT open it; the client should only receive the
        avatar's entry (owner or non-owner depending on race)."""
        ai = ai_conn()
        client = _setup_peer_and_avatar(
            ai,
            client_conn,
            parent_dclass_name="DistributedAvatarCartesian",
            avatar_zone=1000,
            peer_zone=1015,
        )
        # Wait for at least the avatar to arrive. Then drain for a generous
        # window — the GET_CLASS round-trip + rule apply is async and we want
        # to make sure no PEER entry sneaks in late. _collect_entries' own
        # timeout is the only "wait" we need: it returns as soon as
        # expected_count entries arrive OR the timeout elapses.
        entries = _collect_entries(client, expected_count=1, timeout=5.0)
        assert any(
            e.do_id == AVATAR_DOID for e in entries
        ), f"avatar entry missing; got {entries!r}"
        # Allow a wide drain window for any belated peer entry.
        more = _collect_entries(client, expected_count=10, timeout=1.25)
        seen = {e.do_id for e in entries + more}
        assert (
            PEER_DOID not in seen
        ), f"peer at zone 1015 must not be visible; got {entries + more!r}"


class TestStatedRule:
    def test_stated_parent_does_not_auto_open_zones(
        self, avatar_cluster, ai_conn, client_conn
    ):
        """Parent class with Stated rule: no zone-window interest is opened,
        so the peer never becomes visible even though it sits in the same
        parent."""
        ai = ai_conn()
        client = _setup_peer_and_avatar(
            ai,
            client_conn,
            parent_dclass_name="DistributedAvatarStated",
            avatar_zone=1000,
            peer_zone=1005,
        )
        first = client.expect_object_entry(owner=True, timeout=5.0)
        assert first.do_id == AVATAR_DOID

        # Drain for a wide window; if the rule were misapplied the peer
        # would arrive within this period.
        extra = client.recv_maybe(timeout=1.25)
        assert (
            extra is None
        ), f"Stated rule must not open peer visibility; got {extra!r}"
