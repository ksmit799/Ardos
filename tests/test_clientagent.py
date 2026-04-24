"""Client Agent tests — authentication, heartbeat, security boundaries."""
import pytest

from tests.common.ardos import Datagram, DatagramIterator
from tests.common.dc import dc_hash, field_id
from tests.common.msgtypes import (
    CLIENT_ADD_INTEREST,
    CLIENT_DISCONNECT_BAD_DCHASH,
    CLIENT_DISCONNECT_BAD_VERSION,
    CLIENT_DISCONNECT_NO_HELLO,
    CLIENT_EJECT,
    CLIENT_HEARTBEAT,
    CLIENT_OBJECT_SET_FIELD,
)


@pytest.fixture
def ca(ardos):
    return ardos(
        md=True, ss=True, ca=True,
        uberdogs=[
            {"id": 4665, "class": "AuthManager", "anonymous": True},
            {"id": 4666, "class": "ChatManager"},
        ],
    )


@pytest.fixture
def ca_client_interest(ardos):
    """CA with client-set interests enabled + zone whitelist for security tests."""
    return ardos(
        md=True, ss=True, ca=True,
        overrides={
            "client-agent": {
                "interest": {
                    "client": "all",
                    "mode": "whitelist",
                    "zones": [0, 5, 10, "100-399"],
                },
            },
        },
    )


class TestHandshake:
    def test_hello_with_correct_hash_succeeds(self, ca, client_conn):
        c = client_conn()
        c.hello(dc_hash("test.dc"), "dev")
        c.expect_hello_resp()

    def test_hello_with_wrong_version_rejected(self, ca, client_conn):
        c = client_conn()
        c.hello(dc_hash("test.dc"), "not-the-version")
        dg = c.recv(timeout=2.0)
        it = DatagramIterator(dg)
        assert it.read_client_msgtype() == CLIENT_EJECT
        assert it.read_uint16() == CLIENT_DISCONNECT_BAD_VERSION

    def test_hello_with_wrong_hash_rejected(self, ca, client_conn):
        c = client_conn()
        c.hello(dc_hash("test.dc") ^ 0xDEADBEEF, "dev")
        dg = c.recv(timeout=2.0)
        it = DatagramIterator(dg)
        assert it.read_client_msgtype() == CLIENT_EJECT
        assert it.read_uint16() == CLIENT_DISCONNECT_BAD_DCHASH

    def test_non_hello_first_message_rejected(self, ca, client_conn):
        c = client_conn()
        # Send a heartbeat instead of a hello.
        c.send(Datagram.create_client(CLIENT_HEARTBEAT))
        dg = c.recv(timeout=2.0)
        it = DatagramIterator(dg)
        assert it.read_client_msgtype() == CLIENT_EJECT
        assert it.read_uint16() == CLIENT_DISCONNECT_NO_HELLO


class TestHeartbeat:
    def test_heartbeat_after_hello_ok(self, ca, client_conn):
        c = client_conn()
        c.hello(dc_hash("test.dc"), "dev")
        c.expect_hello_resp()
        c.heartbeat()
        c.expect_none(timeout=0.5)


class TestInterestSecurity:
    def test_forbidden_zone_disconnects_client(self, ca_client_interest, client_conn):
        """With mode=whitelist, adding interest in a non-listed zone must eject."""
        c = client_conn()
        c.hello(dc_hash("test.dc"), "dev")
        c.expect_hello_resp()
        # CLIENT_ADD_INTEREST format: [ctx:uint32][interestId:uint16][parent:uint32][zone:uint32]
        dg = Datagram.create_client(CLIENT_ADD_INTEREST)
        dg.add_uint32(1).add_uint16(1).add_uint32(5000).add_uint32(999)  # zone 999 not whitelisted
        c.send(dg)
        got = c.recv(timeout=2.0)
        it = DatagramIterator(got)
        assert it.read_client_msgtype() == CLIENT_EJECT

    def test_whitelisted_zone_allowed(self, ca_client_interest, client_conn):
        c = client_conn()
        c.hello(dc_hash("test.dc"), "dev")
        c.expect_hello_resp()
        dg = Datagram.create_client(CLIENT_ADD_INTEREST)
        dg.add_uint32(1).add_uint16(1).add_uint32(5000).add_uint32(200)  # in 100-399
        c.send(dg)
        # Should not get an eject.
        ejected = c.recv_maybe(timeout=0.5)
        if ejected is not None:
            mt = DatagramIterator(ejected).read_client_msgtype()
            assert mt != CLIENT_EJECT


class TestAnonymousUberdog:
    def test_anonymous_can_send_to_uberdog(self, ca, client_conn):
        """`anonymous: true` on AuthManager lets a pre-auth client talk to it."""
        c = client_conn()
        c.hello(dc_hash("test.dc"), "dev")
        c.expect_hello_resp()
        fid = field_id("test.dc", "AuthManager", "login")
        dg = Datagram.create_client(CLIENT_OBJECT_SET_FIELD)
        dg.add_uint32(4665).add_uint16(fid).add_string("token-xyz")
        c.send(dg)
        # No disconnect should follow.
        dg = c.recv_maybe(timeout=0.5)
        if dg is not None:
            mt = DatagramIterator(dg).read_client_msgtype()
            assert mt != CLIENT_EJECT
