"""WebSocket transport smoke test.

The Client Agent's transport backend is configurable via the
``client-agent.transport`` config option. This file proves the WS
transport delivers protocol datagrams end-to-end: a Python WS client
connects, sends CLIENT_HELLO, and receives CLIENT_HELLO_RESP.

Wire format note: over WS each protocol datagram is a single binary
frame. There is no ``[uint16 length]`` prefix on the wire -- the WS
frame itself is the boundary. (Over TCP we still prepend the length
prefix because TCP is a byte stream; the framing decision lives inside
each transport implementation, not in the protocol layer.)
"""

from __future__ import annotations

import struct

import pytest
import websocket

from tests.common.ardos import Datagram, DatagramIterator
from tests.common.dc import dc_hash
from tests.common.msgtypes import CLIENT_HELLO, CLIENT_HELLO_RESP


@pytest.fixture
def ca_ws(ardos):
    """CA configured to listen over WebSockets instead of plain TCP."""
    return ardos(
        md=True,
        ss=True,
        ca=True,
        overrides={"client-agent": {"transport": "ws"}},
    )


def test_ws_hello_handshake(ca_ws):
    """The WS transport accepts a CLIENT_HELLO sent as a binary frame and
    responds with CLIENT_HELLO_RESP on the same connection. This is the
    end-to-end proof that ITransportListener+ITransportConnection routing
    works under ws28, without duplicating the full TCP test suite -- the
    rest of the protocol is transport-agnostic by design.
    """
    ws = websocket.create_connection(
        "ws://127.0.0.1:6667/", subprotocols=[], timeout=5.0
    )
    try:
        # CLIENT_HELLO payload: [uint16 msgtype][uint32 dc_hash][string version]
        payload = (
            Datagram()
            .add_uint16(CLIENT_HELLO)
            .add_uint32(dc_hash("test.dc"))
            .add_string("dev")
            .bytes()
        )
        # WS framing: send the payload as a single binary frame. No
        # length prefix -- the WS frame IS the boundary.
        ws.send_binary(payload)

        # Receive the response. websocket-client gives us the complete
        # frame as bytes.
        resp = ws.recv()
        assert isinstance(
            resp, (bytes, bytearray)
        ), f"expected binary frame; got {type(resp).__name__}"

        it = DatagramIterator(bytes(resp))
        mt = it.read_uint16()
        assert (
            mt == CLIENT_HELLO_RESP
        ), f"expected CLIENT_HELLO_RESP ({CLIENT_HELLO_RESP}); got {mt}"
    finally:
        ws.close()
