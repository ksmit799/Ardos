"""Client Agent benchmarks — client handshake + heartbeat cycle.

Covers the CA hot path (hello + first few messages). Interest-opening
benchmarks require populated zones; those are tracked as a follow-up.
"""
import pytest

from tests.common.ardos import Datagram
from tests.common.dc import dc_hash

pytestmark = pytest.mark.benchmark(group="ca")


@pytest.fixture
def ca(ardos):
    return ardos(md=True, ss=True, ca=True)


def test_hello_cycle(ca, client_conn, benchmark):
    def step():
        c = client_conn()
        c.hello(dc_hash("test.dc"), "dev")
        c.expect_hello_resp()
        c.close()
    benchmark(step)
