"""Mesh backbone tests.

Multiple daemons form a full mesh, subscriptions propagate as deltas and
snapshots, datagrams cross exactly one link, and a crashed instance has
its replicated post removes fired by a survivor.

Cross instance propagation is asynchronous, so these tests re-send probe
datagrams until one lands rather than sleeping. At most once delivery
makes an early probe vanish harmlessly.
"""

import time

import pytest

from tests.common.ardos import Datagram, DatagramIterator
from tests.common.msgtypes import CONTROL_ADD_POST_REMOVE

# Arbitrary sentinel msgtypes, raw participant connections receive
# whatever is routed to their channels, the value only needs to be echoed.
MT_PROBE = 0xBEE1
MT_MULTI = 0xBEE2
MT_SYNC = 0xBEE3
MT_POST_REMOVE = 0xBEE4

MD_A, MD_B, MD_C = 7100, 7110, 7120
MESH_A, MESH_B, MESH_C = 7301, 7302, 7303


def _is_msgtype(msgtype):
    def pred(dg):
        try:
            it = DatagramIterator(dg)
            _, _, mt = it.read_header()
            return mt == msgtype
        except Exception:
            return False

    return pred


def _wait_cross_route(pub, sub, channel, msgtype, timeout=10.0):
    """Publish to ``channel`` on one daemon until it arrives at ``sub`` on
    another. The first sends can race the subscription delta crossing the
    mesh, re-sending until one round-trips is the deterministic wait."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        pub.send(Datagram.create([channel], sender=0, msgtype=msgtype))
        try:
            return sub.wait_for(_is_msgtype(msgtype), timeout=0.25)
        except TimeoutError:
            continue
    raise TimeoutError(f"nothing routed to channel {channel} within {timeout}s")


def _count_arrivals(sub, msgtype, window=0.75):
    count = 0
    deadline = time.monotonic() + window
    while time.monotonic() < deadline:
        dg = sub.recv_maybe(timeout=0.25)
        if dg is not None and _is_msgtype(msgtype)(dg):
            count += 1
    return count


@pytest.fixture
def pair(ardos):
    """Two daemons meshed together, B seeds off A."""
    a = ardos(md=True, md_port=MD_A, mesh_node_id=1, mesh_port=MESH_A)
    b = ardos(
        md=True,
        md_port=MD_B,
        mesh_node_id=2,
        mesh_port=MESH_B,
        mesh_seeds=[f"127.0.0.1:{MESH_A}"],
    )
    return a, b


def test_mesh_routes_point_subscriptions(pair, channel_conn, md_conn):
    """A channel subscribed on one instance receives publishes from the
    other, in both directions."""
    watcher_a = channel_conn(0x4A11CE, port=MD_A)
    publisher_b = md_conn(port=MD_B)
    _wait_cross_route(publisher_b, watcher_a, 0x4A11CE, MT_PROBE)

    watcher_b = channel_conn(0x4B0B00, port=MD_B)
    publisher_a = md_conn(port=MD_A)
    _wait_cross_route(publisher_a, watcher_b, 0x4B0B00, MT_PROBE)


def test_mesh_routes_range_subscriptions(pair, channel_conn, md_conn):
    """A range subscribed on one instance is a single entry mesh-wide, a
    publish inside it from the other instance arrives."""
    watcher_a = channel_conn(port=MD_A)
    watcher_a.add_range(0x5000_0000, 0x5FFF_FFFF)
    publisher_b = md_conn(port=MD_B)
    _wait_cross_route(publisher_b, watcher_a, 0x5ABC_1234, MT_PROBE)


def test_mesh_multichannel_delivers_once(pair, channel_conn, md_conn):
    """One datagram addressed to two channels held by the same remote
    subscriber arrives exactly once, the at most once invariant."""
    x, y, sync = 0x6000_0001, 0x6000_0002, 0x6000_0003
    watcher = channel_conn(x, y, sync, port=MD_A)
    publisher = md_conn(port=MD_B)

    # Deltas travel the link in subscribe order, once sync routes, x and
    # y are in the publishers table too.
    _wait_cross_route(publisher, watcher, sync, MT_SYNC)

    publisher.send(Datagram.create([x, y], sender=0, msgtype=MT_MULTI))
    assert _count_arrivals(watcher, MT_MULTI) == 1


def test_local_multichannel_delivers_once(ardos, channel_conn, md_conn):
    """Same invariant inside a single standalone instance, this was a
    real duplicate under the old per channel publish loop."""
    ardos(md=True)
    x, y = 0x7000_0001, 0x7000_0002
    watcher = channel_conn(x, y)
    publisher = md_conn()

    # The watcher and publisher are separate sockets, sync on the last
    # subscribed channel so the publish can't race the subscribes.
    _wait_cross_route(publisher, watcher, y, MT_SYNC)

    publisher.send(Datagram.create([x, y], sender=0, msgtype=MT_MULTI))
    assert _count_arrivals(watcher, MT_MULTI) == 1


def test_mesh_gossip_joins_via_single_seed(ardos, channel_conn, md_conn):
    """A third instance seeding off one peer learns the other through
    gossip and can route to it directly."""
    ardos(md=True, md_port=MD_A, mesh_node_id=1, mesh_port=MESH_A)
    ardos(
        md=True,
        md_port=MD_B,
        mesh_node_id=2,
        mesh_port=MESH_B,
        mesh_seeds=[f"127.0.0.1:{MESH_A}"],
    )
    ardos(
        md=True,
        md_port=MD_C,
        mesh_node_id=3,
        mesh_port=MESH_C,
        mesh_seeds=[f"127.0.0.1:{MESH_A}"],  # only A, B is learned via gossip
    )

    watcher_b = channel_conn(0x60551B, port=MD_B)
    publisher_c = md_conn(port=MD_C)
    _wait_cross_route(publisher_c, watcher_b, 0x60551B, MT_PROBE)


def test_post_removes_fire_when_instance_crashes(pair, channel_conn, md_conn):
    """A participants post removes are replicated across the mesh, when
    the whole instance dies uncleanly a survivor fires them."""
    a, b = pair
    target = 0x8000_0001

    watcher_b = channel_conn(target, port=MD_B)
    participant_a = md_conn(port=MD_A)

    # Lodge a post remove on A that deletes something watched via B.
    inner = Datagram.create([target], sender=0, msgtype=MT_POST_REMOVE)
    participant_a.send(
        Datagram.create_control(CONTROL_ADD_POST_REMOVE)
        .add_channel(0xCAFE)
        .add_blob(inner.bytes())
    )

    # Frames are FIFO per link, once a publish round-trips A to B the
    # earlier post remove replication has landed on B too.
    _wait_cross_route(participant_a, watcher_b, target, MT_SYNC)

    # Crash A, no graceful shutdown, no clean disconnect for anyone.
    a.kill()

    # B detects the death, fires the bundle it holds, and the post remove
    # routes to our watcher.
    watcher_b.wait_for(_is_msgtype(MT_POST_REMOVE), timeout=10.0)
