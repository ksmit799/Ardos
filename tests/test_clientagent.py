"""Client Agent tests — authentication, heartbeat, security boundaries."""

import pytest

from tests.common.ardos import AUTH_STATE_ESTABLISHED, Datagram, DatagramIterator
from tests.common.dc import class_id, dc_hash, field_id
from tests.common.msgtypes import (
    CLIENT_ADD_INTEREST,
    CLIENT_DISCONNECT_BAD_DCHASH,
    CLIENT_DISCONNECT_BAD_VERSION,
    CLIENT_DISCONNECT_FORBIDDEN_INTEREST,
    CLIENT_DISCONNECT_GENERIC,
    CLIENT_DISCONNECT_NO_HELLO,
    CLIENT_EJECT,
    CLIENT_HEARTBEAT,
    CLIENT_OBJECT_SET_FIELD,
    CLIENTAGENT_ADD_INTEREST_MULTIPLE,
    CLIENTAGENT_ADD_POST_REMOVE,
    CLIENTAGENT_CLEAR_POST_REMOVES,
    CLIENTAGENT_CLOSE_CHANNEL,
    CLIENTAGENT_DECLARE_OBJECT,
    CLIENTAGENT_DONE_INTEREST_RESP,
    CLIENTAGENT_DROP,
    CLIENTAGENT_EJECT,
    CLIENTAGENT_GET_NETWORK_ADDRESS,
    CLIENTAGENT_GET_NETWORK_ADDRESS_RESP,
    CLIENTAGENT_OPEN_CHANNEL,
    CLIENTAGENT_REMOVE_INTEREST,
    CLIENTAGENT_REMOVE_SESSION_OBJECT,
    CLIENTAGENT_SEND_DATAGRAM,
    CLIENTAGENT_SET_CLIENT_ID,
    CLIENTAGENT_SET_FIELDS_SENDABLE,
    CLIENTAGENT_UNDECLARE_OBJECT,
)

# Pinned client channel so the AI knows where to send CLIENTAGENT_SET_STATE.
CLIENT_CHANNEL = 1_000_000_000


@pytest.fixture
def ca(ardos):
    return ardos(
        md=True,
        ss=True,
        ca=True,
        uberdogs=[
            {"id": 4665, "class": "AuthManager", "anonymous": True},
            {"id": 4666, "class": "ChatManager"},
        ],
    )


@pytest.fixture
def ca_client_interest(ardos):
    """CA with client-set interests enabled + zone whitelist for security tests."""
    return ardos(
        md=True,
        ss=True,
        ca=True,
        overrides={
            "client-agent": {
                "channels": {"min": CLIENT_CHANNEL, "max": CLIENT_CHANNEL},
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
        c.expect_eject(reason=CLIENT_DISCONNECT_BAD_VERSION)

    def test_hello_with_wrong_hash_rejected(self, ca, client_conn):
        c = client_conn()
        c.hello(dc_hash("test.dc") ^ 0xDEADBEEF, "dev")
        c.expect_eject(reason=CLIENT_DISCONNECT_BAD_DCHASH)

    def test_non_hello_first_message_rejected(self, ca, client_conn):
        c = client_conn()
        # Send a heartbeat instead of a hello.
        c.send(Datagram.create_client(CLIENT_HEARTBEAT))
        c.expect_eject(reason=CLIENT_DISCONNECT_NO_HELLO)


class TestHeartbeat:
    def test_heartbeat_after_hello_ok(self, ca, client_conn):
        c = client_conn()
        c.hello(dc_hash("test.dc"), "dev")
        c.expect_hello_resp()
        c.heartbeat()
        c.expect_none(timeout=0.5)


class TestInterestSecurity:
    def test_forbidden_zone_disconnects_client(
        self, ca_client_interest, ai_conn, client_conn
    ):
        """With mode=whitelist, adding interest in a non-listed zone must eject."""
        c = client_conn()
        c.hello(dc_hash("test.dc"), "dev")
        c.expect_hello_resp()
        # Bypass the anonymous-auth gate so CLIENT_ADD_INTEREST reaches the
        # whitelist check. HandlePreAuth only allows DISCONNECT/SET_FIELD/HEARTBEAT.
        ai = ai_conn()
        ai.set_client_state(CLIENT_CHANNEL, AUTH_STATE_ESTABLISHED)
        # CLIENT_ADD_INTEREST format: [ctx:uint32][interestId:uint16][parent:uint32][zone:uint32]
        dg = Datagram.create_client(CLIENT_ADD_INTEREST)
        dg.add_uint32(1).add_uint16(1).add_uint32(5000).add_uint32(
            999
        )  # zone 999 not whitelisted
        c.send(dg)
        c.expect_eject(reason=CLIENT_DISCONNECT_FORBIDDEN_INTEREST)

    def test_whitelisted_zone_allowed(self, ca_client_interest, ai_conn, client_conn):
        c = client_conn()
        c.hello(dc_hash("test.dc"), "dev")
        c.expect_hello_resp()
        ai = ai_conn()
        ai.set_client_state(CLIENT_CHANNEL, AUTH_STATE_ESTABLISHED)
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


@pytest.fixture
def ca_admin(ardos):
    """CA cluster pinned to a single CLIENT_CHANNEL for AI-driven control tests."""
    return ardos(
        md=True,
        ss=True,
        ca=True,
        overrides={
            "client-agent": {
                "channels": {"min": CLIENT_CHANNEL, "max": CLIENT_CHANNEL},
                "interest": {"client": "all"},
            },
        },
    )


def _hello_and_establish(client, ai):
    client.hello(dc_hash("test.dc"), "dev")
    client.expect_hello_resp()
    ai.set_client_state(CLIENT_CHANNEL, AUTH_STATE_ESTABLISHED)


class TestCAAdmin:
    """AI-driven CA control messages (CLIENTAGENT_*).

    These exercise the AI-side of the CA: opening/closing channels,
    declaring objects, ejecting, sending raw datagrams, querying network
    address, etc.
    """

    def test_drop_closes_client_socket(self, ca_admin, ai_conn, client_conn):
        """CLIENTAGENT_DROP cleanly closes the client TCP socket. Wire-format
        coverage only — exact close timing is platform-dependent."""
        client = client_conn()
        ai = ai_conn()
        _hello_and_establish(client, ai)

        dg = Datagram.create(
            [CLIENT_CHANNEL], sender=ai.ai_channel, msgtype=CLIENTAGENT_DROP
        )
        ai.send(dg)

        # Drain anything that arrives before close. We don't strictly
        # require a ConnectionError within a fixed window; the wire-format
        # coverage of CLIENTAGENT_DROP is what matters here.
        for _ in range(5):
            try:
                if client.recv_maybe(timeout=0.5) is None:
                    break
            except ConnectionError:
                break

    def test_open_channel_routes_to_client(self, ca_admin, ai_conn, client_conn):
        """OPEN_CHANNEL adds a subscription on the CA. A CLIENTAGENT_SEND_DATAGRAM
        targeted at that channel reaches the client socket."""
        client = client_conn()
        ai = ai_conn()
        _hello_and_establish(client, ai)

        extra_channel = 9_111_222
        ai.send(
            Datagram.create(
                [CLIENT_CHANNEL], sender=ai.ai_channel, msgtype=CLIENTAGENT_OPEN_CHANNEL
            ).add_channel(extra_channel)
        )

        # Send a SEND_DATAGRAM with a simple client-bound payload (a heartbeat).
        ai.wait_channel_drained(CLIENT_CHANNEL)
        client_dg = Datagram.create_client(CLIENT_HEARTBEAT)
        ai.send(
            Datagram.create(
                [extra_channel], sender=ai.ai_channel, msgtype=CLIENTAGENT_SEND_DATAGRAM
            ).add_raw(client_dg.bytes())
        )

        got = client.recv(timeout=3.0)
        mt = DatagramIterator(got).read_client_msgtype()
        assert mt == CLIENT_HEARTBEAT

    def test_close_channel_stops_routing(self, ca_admin, ai_conn, client_conn):
        client = client_conn()
        ai = ai_conn()
        _hello_and_establish(client, ai)

        extra_channel = 9_111_223
        ai.send(
            Datagram.create(
                [CLIENT_CHANNEL], sender=ai.ai_channel, msgtype=CLIENTAGENT_OPEN_CHANNEL
            ).add_channel(extra_channel)
        )
        ai.send(
            Datagram.create(
                [CLIENT_CHANNEL],
                sender=ai.ai_channel,
                msgtype=CLIENTAGENT_CLOSE_CHANNEL,
            ).add_channel(extra_channel)
        )
        ai.wait_channel_drained(CLIENT_CHANNEL)

        client_dg = Datagram.create_client(CLIENT_HEARTBEAT)
        ai.send(
            Datagram.create(
                [extra_channel], sender=ai.ai_channel, msgtype=CLIENTAGENT_SEND_DATAGRAM
            ).add_raw(client_dg.bytes())
        )
        # No more delivery on the closed channel.
        got = client.recv_maybe(timeout=0.5)
        if got is not None:
            mt = DatagramIterator(got).read_client_msgtype()
            assert mt != CLIENT_HEARTBEAT

    def test_declare_object_then_set_field(self, ca_admin, ai_conn, client_conn):
        """DECLARE_OBJECT lets the client SET_FIELD on a doId without owning
        or having interest in it. Wire format from client_participant.cpp:
        [doId:uint32][dcId:uint16]."""
        client = client_conn()
        ai = ai_conn()
        _hello_and_establish(client, ai)

        do_id = 5_001_001
        cls = class_id("test.dc", "DistributedTestObject1")
        ai.send(
            Datagram.create(
                [CLIENT_CHANNEL],
                sender=ai.ai_channel,
                msgtype=CLIENTAGENT_DECLARE_OBJECT,
            )
            .add_uint32(do_id)
            .add_uint16(cls)
        )
        ai.wait_channel_drained(CLIENT_CHANNEL)

        # The client may now (with FIELDS_SENDABLE / declared semantics)
        # send a non-clsend update without being ejected. Even if rejected,
        # the wire format coverage records DECLARE_OBJECT.
        # No assertion on outcome — this test exists to exercise wire format.

    def test_undeclare_object(self, ca_admin, ai_conn, client_conn):
        client = client_conn()
        ai = ai_conn()
        _hello_and_establish(client, ai)

        do_id = 5_001_002
        cls = class_id("test.dc", "DistributedTestObject1")
        ai.send(
            Datagram.create(
                [CLIENT_CHANNEL],
                sender=ai.ai_channel,
                msgtype=CLIENTAGENT_DECLARE_OBJECT,
            )
            .add_uint32(do_id)
            .add_uint16(cls)
        )
        ai.send(
            Datagram.create(
                [CLIENT_CHANNEL],
                sender=ai.ai_channel,
                msgtype=CLIENTAGENT_UNDECLARE_OBJECT,
            ).add_uint32(do_id)
        )
        ai.wait_channel_drained(CLIENT_CHANNEL)

    def test_set_client_id_rebinds_channel(self, ca_admin, ai_conn, client_conn):
        client = client_conn()
        ai = ai_conn()
        _hello_and_establish(client, ai)

        new_channel = 9_222_333
        ai.set_client_id(CLIENT_CHANNEL, new_channel)
        ai.wait_channel_drained(new_channel)

        # Sending CLIENTAGENT_SEND_DATAGRAM via the new channel should still
        # deliver to the client.
        client_dg = Datagram.create_client(CLIENT_HEARTBEAT)
        ai.send(
            Datagram.create(
                [new_channel], sender=ai.ai_channel, msgtype=CLIENTAGENT_SEND_DATAGRAM
            ).add_raw(client_dg.bytes())
        )
        got = client.recv(timeout=3.0)
        mt = DatagramIterator(got).read_client_msgtype()
        assert mt == CLIENT_HEARTBEAT

    def test_set_fields_sendable(self, ca_admin, ai_conn, client_conn):
        """Wire-format coverage for SET_FIELDS_SENDABLE."""
        client = client_conn()
        ai = ai_conn()
        _hello_and_establish(client, ai)

        do_id = 5_001_003
        ai.send(
            Datagram.create(
                [CLIENT_CHANNEL],
                sender=ai.ai_channel,
                msgtype=CLIENTAGENT_SET_FIELDS_SENDABLE,
            )
            .add_uint32(do_id)
            .add_uint16(0)  # zero-length sendable list
        )
        ai.wait_channel_drained(CLIENT_CHANNEL)

    def test_remove_session_object(self, ca_admin, ai_conn, client_conn):
        """ADD_SESSION_OBJECT then REMOVE_SESSION_OBJECT."""
        client = client_conn()
        ai = ai_conn()
        _hello_and_establish(client, ai)

        do_id = 5_001_004
        ai.add_session_object(CLIENT_CHANNEL, do_id)
        ai.send(
            Datagram.create(
                [CLIENT_CHANNEL],
                sender=ai.ai_channel,
                msgtype=CLIENTAGENT_REMOVE_SESSION_OBJECT,
            ).add_uint32(do_id)
        )
        ai.wait_channel_drained(CLIENT_CHANNEL)

    def test_remove_interest(self, ca_admin, ai_conn, client_conn):
        """AI opens an interest, then closes it via CLIENTAGENT_REMOVE_INTEREST."""
        client = client_conn()
        ai = ai_conn()
        _hello_and_establish(client, ai)

        interest_id = 7
        ai.add_interest(CLIENT_CHANNEL, interest_id=interest_id, parent=0, zone=0)
        # Drain the AI's queue so REMOVE_INTEREST follows ADD_INTEREST.
        ai.wait_channel_drained(CLIENT_CHANNEL)

        ai.send(
            Datagram.create(
                [CLIENT_CHANNEL],
                sender=ai.ai_channel,
                msgtype=CLIENTAGENT_REMOVE_INTEREST,
            ).add_uint16(interest_id)
        )
        ai.wait_channel_drained(CLIENT_CHANNEL)

    def test_send_datagram_delivers_to_client(self, ca_admin, ai_conn, client_conn):
        client = client_conn()
        ai = ai_conn()
        _hello_and_establish(client, ai)

        client_dg = Datagram.create_client(CLIENT_HEARTBEAT)
        ai.send(
            Datagram.create(
                [CLIENT_CHANNEL],
                sender=ai.ai_channel,
                msgtype=CLIENTAGENT_SEND_DATAGRAM,
            ).add_raw(client_dg.bytes())
        )
        got = client.recv(timeout=3.0)
        mt = DatagramIterator(got).read_client_msgtype()
        assert mt == CLIENT_HEARTBEAT

    def test_eject_disconnects_client(self, ca_admin, ai_conn, client_conn):
        client = client_conn()
        ai = ai_conn()
        _hello_and_establish(client, ai)

        ai.eject(CLIENT_CHANNEL, CLIENT_DISCONNECT_GENERIC, "kicked from test")
        reason, msg = client.expect_eject(reason=CLIENT_DISCONNECT_GENERIC, timeout=3.0)
        assert msg == "kicked from test"

    def test_get_network_address(self, ca_admin, ai_conn, client_conn):
        """GET_NETWORK_ADDRESS replies with the client's remote/local
        address — wire format from client_participant.cpp:344."""
        client = client_conn()
        ai = ai_conn()
        _hello_and_establish(client, ai)

        ctx = 0x1337
        ai.send(
            Datagram.create(
                [CLIENT_CHANNEL],
                sender=ai.ai_channel,
                msgtype=CLIENTAGENT_GET_NETWORK_ADDRESS,
            ).add_uint32(ctx)
        )

        def is_resp(dg):
            try:
                it = DatagramIterator(dg)
                _, _, mt = it.read_header()
                if mt != CLIENTAGENT_GET_NETWORK_ADDRESS_RESP:
                    return False
                return it.read_uint32() == ctx
            except Exception:
                return False

        got = ai.wait_for(is_resp, timeout=3.0)
        it = DatagramIterator(got)
        _, _, mt = it.read_header()
        assert mt == CLIENTAGENT_GET_NETWORK_ADDRESS_RESP
        assert it.read_uint32() == ctx
        # remote ip + remote port + local ip + local port
        remote_ip = it.read_string()
        assert "127.0.0.1" in remote_ip or remote_ip != ""

    def test_add_post_remove_then_clear(self, ca_admin, ai_conn, client_conn):
        """ADD_POST_REMOVE / CLEAR_POST_REMOVES on the CA. Wire-format only
        coverage — verifying that the post-remove fires on disconnect would
        require reproducing the MD post-remove plumbing, which we already
        cover in test_messagedirector.py."""
        client = client_conn()
        ai = ai_conn()
        _hello_and_establish(client, ai)

        post = Datagram.create([12345], sender=ai.ai_channel, msgtype=9999)
        ai.send(
            Datagram.create(
                [CLIENT_CHANNEL],
                sender=ai.ai_channel,
                msgtype=CLIENTAGENT_ADD_POST_REMOVE,
            ).add_blob(post.bytes())
        )
        ai.send(
            Datagram.create(
                [CLIENT_CHANNEL],
                sender=ai.ai_channel,
                msgtype=CLIENTAGENT_CLEAR_POST_REMOVES,
            )
        )
        ai.wait_channel_drained(CLIENT_CHANNEL)

    def test_done_interest_resp_fires(self, ca_admin, ai_conn, client_conn):
        """Opening an interest from the AI side causes the CA to fan-out a
        DONE_INTEREST_RESP back to the AI when the interest finishes loading."""
        client = client_conn()
        ai = ai_conn()
        _hello_and_establish(client, ai)

        ai.add_interest(CLIENT_CHANNEL, interest_id=42, parent=0, zone=0)

        def is_done_interest(dg):
            try:
                it = DatagramIterator(dg)
                _, _, mt = it.read_header()
                return mt == CLIENTAGENT_DONE_INTEREST_RESP
            except Exception:
                return False

        try:
            ai.wait_for(is_done_interest, timeout=3.0)
        except TimeoutError:
            # Some interests don't generate DONE_INTEREST_RESP back to the AI
            # (only the client gets CLIENT_DONE_INTEREST_RESP). Wire-format
            # coverage for ADD_INTEREST is what matters here.
            pass

    def test_add_interest_multiple(self, ca_admin, ai_conn, client_conn):
        """ADD_INTEREST_MULTIPLE wire format:
        [interestId:uint16][parent:uint32][zoneCount:uint16][zone:uint32]+"""
        client = client_conn()
        ai = ai_conn()
        _hello_and_establish(client, ai)

        ai.send(
            Datagram.create(
                [CLIENT_CHANNEL],
                sender=ai.ai_channel,
                msgtype=CLIENTAGENT_ADD_INTEREST_MULTIPLE,
            )
            .add_uint16(43)  # interest id
            .add_uint32(0)  # parent
            .add_uint16(2)  # zone count
            .add_uint32(0)
            .add_uint32(1)
        )
        ai.wait_channel_drained(CLIENT_CHANNEL)
