"""Message Director routing tests.

Verifies channel subscribe/unsubscribe, range subscriptions, fan-out across
multiple subscribers, and post-remove hooks. This is the core of the cluster
so everything else depends on it being solid.
"""
import time

import pytest

from tests.common.ardos import Datagram, DatagramIterator
from tests.common.msgtypes import (
    CONTROL_ADD_POST_REMOVE,
    CONTROL_CLEAR_POST_REMOVES,
    CONTROL_REMOVE_RANGE,
)

CH_A = 1_000_100
CH_B = 1_000_200
CH_C = 1_000_300


@pytest.fixture
def md(ardos):
    return ardos(md=True)


class TestRouting:
    def test_subscriber_receives(self, md, channel_conn):
        """A subscriber to channel X receives messages sent to X."""
        sub = channel_conn(CH_A)
        sub.flush()
        sender = channel_conn()
        payload = Datagram.create([CH_A], sender=0, msgtype=1234).add_uint32(42)
        sender.send(payload)
        got = sub.recv(timeout=2.0)
        _, _, mt = DatagramIterator(got).read_header()
        assert mt == 1234

    def test_non_subscriber_does_not_receive(self, md, channel_conn):
        sub = channel_conn(CH_A)
        sub.flush()
        sender = channel_conn()
        sender.send(Datagram.create([CH_B], sender=0, msgtype=1234))
        sub.expect_none(timeout=0.5)

    def test_fanout_to_multiple_subscribers(self, md, channel_conn):
        a = channel_conn(CH_A)
        b = channel_conn(CH_A)
        a.flush(); b.flush()
        sender = channel_conn()
        dg = Datagram.create([CH_A], sender=0, msgtype=2000).add_string("hi")
        sender.send(dg)
        for sub in (a, b):
            got = sub.recv(timeout=2.0)
            it = DatagramIterator(got)
            _, _, mt = it.read_header()
            assert mt == 2000
            assert it.read_string() == "hi"

    def test_unsubscribe_stops_delivery(self, md, channel_conn):
        sub = channel_conn(CH_A)
        sub.flush()
        sub.unsubscribe(CH_A)
        sender = channel_conn()
        sender.send(Datagram.create([CH_A], sender=0, msgtype=1234))
        sub.expect_none(timeout=0.5)


class TestRanges:
    def test_range_subscription(self, md, channel_conn):
        sub = channel_conn()
        sub.add_range(CH_A, CH_A + 100)
        # Range subscriptions go through bucket-based RabbitMQ bindings
        # (src/messagedirector/channel_subscriber.cpp:100); flush()'s 50ms
        # poll isn't long enough for the bindings to propagate. Sleep here
        # rather than slowing every test.
        time.sleep(0.2)
        sender = channel_conn()
        sender.send(Datagram.create([CH_A + 50], sender=0, msgtype=1234))
        got = sub.recv(timeout=2.0)
        _, _, mt = DatagramIterator(got).read_header()
        assert mt == 1234

    def test_range_removal(self, md, channel_conn):
        sub = channel_conn()
        sub.add_range(CH_A, CH_A + 100)
        time.sleep(0.2)
        sub.send(
            Datagram.create_control(CONTROL_REMOVE_RANGE)
            .add_channel(CH_A).add_channel(CH_A + 100)
        )
        sender = channel_conn()
        sender.send(Datagram.create([CH_A + 50], sender=0, msgtype=1234))
        sub.expect_none(timeout=0.5)


class TestPostRemove:
    def test_post_remove_fires_on_disconnect(self, md, channel_conn):
        """When a subscriber disconnects, its registered post-remove payloads
        get published. Used by roles to clean up state on failure."""
        watcher = channel_conn(CH_B)
        watcher.flush()

        victim = channel_conn()
        post = Datagram.create([CH_B], sender=0, msgtype=9999).add_string("bye")
        victim.send(
            Datagram.create_control(CONTROL_ADD_POST_REMOVE)
            .add_channel(0)
            .add_blob(post.bytes())
        )
        victim.close()

        got = watcher.recv(timeout=2.0)
        it = DatagramIterator(got)
        _, _, mt = it.read_header()
        assert mt == 9999
        assert it.read_string() == "bye"

    def test_clear_post_removes(self, md, channel_conn):
        watcher = channel_conn(CH_B)
        watcher.flush()

        victim = channel_conn()
        post = Datagram.create([CH_B], sender=0, msgtype=9999)
        victim.send(
            Datagram.create_control(CONTROL_ADD_POST_REMOVE)
            .add_channel(0)
            .add_blob(post.bytes())
        )
        victim.send(Datagram.create_control(CONTROL_CLEAR_POST_REMOVES).add_channel(0))
        victim.close()

        watcher.expect_none(timeout=1.0)
