"""Shared (load balanced) channel tests.

An uberdog declared load-balanced turns its channel into a shared group,
the router delivers each datagram to exactly one member, keyed on the
sender so one client always lands on the same member while the group is
stable. Members can live on any instance in the mesh.

Cross instance propagation is asynchronous, so these tests re-send probe
datagrams until one lands rather than sleeping.
"""

import time

import pytest

from tests.common.ardos import Datagram, DatagramIterator

MT_PROBE = 0xBEA1
MT_SYNC = 0xBEA2

SHARED = 4665
UBERDOGS = [
    {"id": SHARED, "class": "AuthManager", "anonymous": True, "load-balanced": True}
]

MD_A, MD_B = 7100, 7110
MESH_A, MESH_B = 7311, 7312


def _is_msgtype(msgtype):
    def pred(dg):
        try:
            it = DatagramIterator(dg)
            _, _, mt = it.read_header()
            return mt == msgtype
        except Exception:
            return False

    return pred


def _wait_route(pub, sub, channel, msgtype, sender=0, timeout=10.0):
    """Publish to ``channel`` until it arrives at ``sub``. The first sends
    can race subscription processing, re-sending until one round-trips is
    the deterministic wait."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        pub.send(Datagram.create([channel], sender=sender, msgtype=msgtype))
        try:
            return sub.wait_for(_is_msgtype(msgtype), timeout=0.25)
        except TimeoutError:
            continue
    raise TimeoutError(f"nothing routed to channel {channel} within {timeout}s")


def _drain_senders(sub, msgtype, window=1.0):
    """Collect the sender of every ``msgtype`` arrival within the window."""
    senders = []
    deadline = time.monotonic() + window
    while time.monotonic() < deadline:
        dg = sub.recv_maybe(timeout=0.25)
        if dg is None:
            continue
        try:
            it = DatagramIterator(dg)
            _, sender, mt = it.read_header()
        except Exception:
            continue
        if mt == msgtype:
            senders.append(sender)
    return senders


def _wait_log(daemon, text, timeout=10.0):
    """Poll the daemon log until ``text`` shows up, log writes are async."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if text in daemon.log_path.read_text(errors="replace"):
                return
        except OSError:
            pass
        time.sleep(0.1)
    raise TimeoutError(f"{text!r} never appeared in {daemon.log_path}")


def _sync_member(publisher, member, sync_channel):
    """Prove a members subscribes are processed. Control frames are FIFO
    per socket, once the later sync subscription routes, the earlier
    shared subscription is filed too."""
    _wait_route(publisher, member, sync_channel, MT_SYNC)


@pytest.fixture
def shared_daemon(ardos):
    return ardos(md=True, uberdogs=UBERDOGS)


def test_shared_exactly_one_member_and_stickiness(shared_daemon, channel_conn, md_conn):
    """Every datagram lands on exactly one of the two members, all of a
    senders datagrams land on the same member, and enough senders spread
    across both members."""
    publisher = md_conn()

    # No members yet, sends to the shared channel drop with a warning.
    publisher.send(Datagram.create([SHARED], sender=1, msgtype=MT_PROBE))
    _wait_log(shared_daemon, f"Shared channel {SHARED} has no members")

    sync_a, sync_b = 0x9A00_0001, 0x9A00_0002
    member_a = channel_conn(SHARED, sync_a)
    member_b = channel_conn(SHARED, sync_b)
    _sync_member(publisher, member_a, sync_a)
    _sync_member(publisher, member_b, sync_b)

    senders = list(range(2000, 2040))
    for sender in senders:
        for _ in range(3):
            publisher.send(Datagram.create([SHARED], sender=sender, msgtype=MT_PROBE))

    got_a = _drain_senders(member_a, MT_PROBE)
    got_b = _drain_senders(member_b, MT_PROBE)

    assert len(got_a) + len(got_b) == len(senders) * 3
    overlap = set(got_a) & set(got_b)
    assert not overlap, f"senders split across members: {sorted(overlap)}"
    for sender in senders:
        assert got_a.count(sender) + got_b.count(sender) == 3
    assert got_a and got_b, "rendezvous never spread across both members"


def test_shared_failover_on_member_disconnect(shared_daemon, channel_conn, md_conn):
    """When a member leaves, its senders remap to a survivor."""
    publisher = md_conn()

    sync_a, sync_b = 0x9B00_0001, 0x9B00_0002
    member_a = channel_conn(SHARED, sync_a)
    member_b = channel_conn(SHARED, sync_b)
    _sync_member(publisher, member_a, sync_a)
    _sync_member(publisher, member_b, sync_b)

    # Find which member owns this sender.
    sender = 3001
    publisher.send(Datagram.create([SHARED], sender=sender, msgtype=MT_PROBE))
    if _drain_senders(member_a, MT_PROBE, window=0.75):
        owner, survivor = member_a, member_b
    else:
        assert _drain_senders(member_b, MT_PROBE, window=0.75)
        owner, survivor = member_b, member_a

    # Kill the owner, re-sends land on the survivor once the daemon has
    # processed the disconnect. Drain stragglers from the re-send loop
    # before counting.
    owner.close()
    _wait_route(publisher, survivor, SHARED, MT_PROBE, sender=sender)
    _drain_senders(survivor, MT_PROBE, window=0.5)

    for _ in range(3):
        publisher.send(Datagram.create([SHARED], sender=sender, msgtype=MT_PROBE))
    assert len(_drain_senders(survivor, MT_PROBE)) == 3


def test_shared_routes_across_mesh(ardos, channel_conn, md_conn):
    """Members on different instances form one group, the publisher picks
    across the whole mesh and a dead instances members drop out."""
    ardos(
        md=True,
        md_port=MD_A,
        mesh_node_id=1,
        mesh_port=MESH_A,
        uberdogs=UBERDOGS,
    )
    ardos(
        md=True,
        md_port=MD_B,
        mesh_node_id=2,
        mesh_port=MESH_B,
        mesh_seeds=[f"127.0.0.1:{MESH_A}"],
        uberdogs=UBERDOGS,
    )

    publisher = md_conn(port=MD_A)

    # Only B holds a member, once its advert crosses the mesh every send
    # from A routes to it, exactly once each.
    member_b = channel_conn(SHARED, port=MD_B)
    _wait_route(publisher, member_b, SHARED, MT_PROBE)
    _drain_senders(member_b, MT_PROBE, window=0.5)

    senders = list(range(4000, 4020))
    for sender in senders:
        publisher.send(Datagram.create([SHARED], sender=sender, msgtype=MT_PROBE))
    assert sorted(_drain_senders(member_b, MT_PROBE)) == senders

    # A local member joins the group, the pick spreads across instances.
    sync_a = 0x9C00_0001
    member_a = channel_conn(SHARED, sync_a, port=MD_A)
    _sync_member(publisher, member_a, sync_a)

    spread = list(range(5000, 5060))
    for sender in spread:
        publisher.send(Datagram.create([SHARED], sender=sender, msgtype=MT_PROBE))
    got_a = _drain_senders(member_a, MT_PROBE)
    got_b = _drain_senders(member_b, MT_PROBE)
    assert sorted(got_a + got_b) == spread
    assert got_a and got_b, "pick never spread across instances"

    # B's member leaves, everything lands on A's member. Drain stragglers
    # from the re-send loop before counting.
    member_b.close()
    _wait_route(publisher, member_a, SHARED, MT_PROBE, sender=got_b[0])
    _drain_senders(member_a, MT_PROBE, window=0.5)
    for sender in got_b[:3]:
        publisher.send(Datagram.create([SHARED], sender=sender, msgtype=MT_PROBE))
    assert len(_drain_senders(member_a, MT_PROBE)) == 3


def test_shared_config_mismatch_logs_and_still_routes(ardos, channel_conn, md_conn):
    """A peer subscribing a load balanced channel as a normal one is out
    of sync, shared wins and the mismatch is logged."""
    a = ardos(
        md=True, md_port=MD_A, mesh_node_id=1, mesh_port=MESH_A, uberdogs=UBERDOGS
    )
    ardos(
        md=True,
        md_port=MD_B,
        mesh_node_id=2,
        mesh_port=MESH_B,
        mesh_seeds=[f"127.0.0.1:{MESH_A}"],
    )

    # B doesn't declare the channel load balanced, its member subscribes
    # normally and A converts the advert to a one member shared group.
    member_b = channel_conn(SHARED, port=MD_B)
    publisher = md_conn(port=MD_A)
    _wait_route(publisher, member_b, SHARED, MT_PROBE)

    _wait_log(a, f"subscribes load balanced channel {SHARED}")
