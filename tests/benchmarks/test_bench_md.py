"""MD fanout benchmarks.

Measures the cost of broadcasting a single datagram to N subscribers. Covers
the RabbitMQ routing rework (commit 2948152).
"""
import pytest

from tests.common.ardos import Datagram, DatagramIterator

pytestmark = pytest.mark.benchmark(group="md")

N_SUBSCRIBERS = [1, 8, 64]


@pytest.fixture
def md(ardos):
    return ardos(md=True)


@pytest.mark.parametrize("n", N_SUBSCRIBERS)
def test_fanout_latency(md, channel_conn, benchmark, n):
    channel = 2_000_000
    subs = [channel_conn(channel + i) for i in range(n)]
    for s in subs:
        s.flush()
    sender = channel_conn()

    def step():
        recipients = [channel + i for i in range(n)]
        sender.send(Datagram.create(recipients, sender=0, msgtype=4242))
        for s in subs:
            dg = s.recv(timeout=5.0)
            _, _, mt = DatagramIterator(dg).read_header()
            assert mt == 4242

    benchmark(step)
