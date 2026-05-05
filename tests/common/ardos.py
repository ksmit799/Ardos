"""Ardos test harness — daemon lifecycle + wire-protocol connections.

Wire format reference:
  - TCP framing:      [uint16 LE length][payload]
                      (src/net/network_client.cpp:98)
  - Internal header:  [uint8 n][uint64 ch1]...[uint64 chN][uint64 sender][uint16 msgtype]
                      (src/net/datagram.cpp Datagram ctors)
  - Client header:    [uint16 msgtype][payload]
                      (src/clientagent/client_participant_remote.cpp:HandlePreHello)
  - Strings/blobs:    [uint16 LE length][bytes]
                      (src/net/datagram.cpp AddString/AddBlob)
  - Byte order:       native LE (no htonl/htons anywhere).
"""

from __future__ import annotations

import os
import select
import signal
import socket
import struct
import subprocess
import time
from pathlib import Path
from typing import Callable, Iterable, List, Optional, Sequence

from .msg_coverage import symbol_for, tracker
from .msgtypes import (
    CLIENTAGENT_ADD_INTEREST,
    CLIENTAGENT_ADD_SESSION_OBJECT,
    CLIENTAGENT_EJECT,
    CLIENTAGENT_GET_NETWORK_ADDRESS,
    CLIENTAGENT_GET_NETWORK_ADDRESS_RESP,
    CLIENTAGENT_SET_CLIENT_ID,
    CLIENTAGENT_SET_STATE,
    CLIENT_ADD_INTEREST,
    CLIENT_ADD_INTEREST_MULTIPLE,
    CLIENT_DONE_INTEREST_RESP,
    CLIENT_EJECT,
    CLIENT_ENTER_OBJECT_REQUIRED,
    CLIENT_ENTER_OBJECT_REQUIRED_OTHER,
    CLIENT_ENTER_OBJECT_REQUIRED_OTHER_OWNER,
    CLIENT_ENTER_OBJECT_REQUIRED_OWNER,
    CLIENT_HELLO,
    CLIENT_HELLO_RESP,
    CLIENT_HEARTBEAT,
    CLIENT_OBJECT_SET_FIELD,
    CONTROL_ADD_CHANNEL,
    CONTROL_ADD_RANGE,
    CONTROL_CHANNEL,
    CONTROL_REMOVE_CHANNEL,
    STATESERVER_CREATE_OBJECT_WITH_REQUIRED,
    STATESERVER_CREATE_OBJECT_WITH_REQUIRED_OTHER,
    STATESERVER_OBJECT_DELETE_RAM,
    STATESERVER_OBJECT_GET_LOCATION,
    STATESERVER_OBJECT_GET_LOCATION_RESP,
    STATESERVER_OBJECT_SET_FIELD,
    STATESERVER_OBJECT_SET_OWNER,
)

# Interest-bookkeeping messages the CA emits around object entries; the
# `expect_object_entry` helper transparently skips these.
_INTEREST_BOOKKEEPING_MSGS = {
    CLIENT_ADD_INTEREST,
    CLIENT_ADD_INTEREST_MULTIPLE,
    CLIENT_DONE_INTEREST_RESP,
}

# ClientParticipant::AuthState values. Sending CLIENTAGENT_SET_STATE with
# value=ESTABLISHED bypasses the normal hello+anonymous-auth handshake so the
# test can jump directly to driving the ownership flow.
AUTH_STATE_NEW = 0
AUTH_STATE_ANONYMOUS = 1
AUTH_STATE_ESTABLISHED = 2

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUILD_DIRS = [
    REPO_ROOT / "build",
    REPO_ROOT / "cmake-build-debug",
    REPO_ROOT / "cmake-build-release",
]


def locate_binary() -> Path:
    """Find the ardos binary. Honours $ARDOS_BINARY then falls back to build/."""
    env = os.environ.get("ARDOS_BINARY")
    if env:
        p = Path(env)
        if p.is_file():
            return p
        raise FileNotFoundError(f"$ARDOS_BINARY points at missing file: {env}")
    for root in DEFAULT_BUILD_DIRS:
        cand = root / "bin" / "ardos"
        if cand.is_file():
            return cand
    raise FileNotFoundError("ardos binary not found. Build it or set $ARDOS_BINARY.")


class Datagram:
    """Builder for Ardos datagrams. Little-endian throughout."""

    def __init__(self, data: bytes = b"") -> None:
        self._buf = bytearray(data)

    # --- primitive writers ---
    def add_bool(self, v: bool) -> "Datagram":
        return self.add_uint8(1 if v else 0)

    def add_int8(self, v: int) -> "Datagram":
        self._buf += struct.pack("<b", v)
        return self

    def add_uint8(self, v: int) -> "Datagram":
        self._buf += struct.pack("<B", v)
        return self

    def add_int16(self, v: int) -> "Datagram":
        self._buf += struct.pack("<h", v)
        return self

    def add_uint16(self, v: int) -> "Datagram":
        self._buf += struct.pack("<H", v)
        return self

    def add_int32(self, v: int) -> "Datagram":
        self._buf += struct.pack("<i", v)
        return self

    def add_uint32(self, v: int) -> "Datagram":
        self._buf += struct.pack("<I", v)
        return self

    def add_int64(self, v: int) -> "Datagram":
        self._buf += struct.pack("<q", v)
        return self

    def add_uint64(self, v: int) -> "Datagram":
        self._buf += struct.pack("<Q", v)
        return self

    def add_float32(self, v: float) -> "Datagram":
        self._buf += struct.pack("<f", v)
        return self

    def add_float64(self, v: float) -> "Datagram":
        self._buf += struct.pack("<d", v)
        return self

    def add_string(self, s: str) -> "Datagram":
        data = s.encode("utf-8")
        return self.add_uint16(len(data)).add_raw(data)

    def add_blob(self, b: bytes) -> "Datagram":
        return self.add_uint16(len(b)).add_raw(b)

    def add_channel(self, ch: int) -> "Datagram":
        return self.add_uint64(ch)

    def add_doid(self, doid: int) -> "Datagram":
        return self.add_uint32(doid)

    def add_location(self, parent: int, zone: int) -> "Datagram":
        return self.add_uint32(parent).add_uint32(zone)

    def add_raw(self, data: bytes) -> "Datagram":
        self._buf += data
        return self

    def bytes(self) -> bytes:
        return bytes(self._buf)

    def __len__(self) -> int:
        return len(self._buf)

    def __eq__(self, other: object) -> bool:
        return isinstance(other, Datagram) and bytes(self._buf) == other.bytes()

    def __repr__(self) -> str:
        return f"Datagram({len(self._buf)}B: {self._buf.hex()})"

    # --- factories ---
    @classmethod
    def create(cls, recipients: Iterable[int], sender: int, msgtype: int) -> "Datagram":
        """Build an internal datagram with recipient/sender/msgtype header."""
        rec = list(recipients)
        dg = cls()
        dg.add_uint8(len(rec))
        for r in rec:
            dg.add_channel(r)
        dg.add_channel(sender)
        dg.add_uint16(msgtype)
        tracker.record_sent(msgtype)
        return dg

    @classmethod
    def create_control(cls, msgtype: int) -> "Datagram":
        """Control messages use recipient=CONTROL_CHANNEL and no sender field
        (the MD strips the recipient and consumes the msgtype directly)."""
        dg = cls()
        dg.add_uint8(1)
        dg.add_channel(CONTROL_CHANNEL)
        dg.add_uint16(msgtype)
        tracker.record_sent(msgtype)
        return dg

    @classmethod
    def create_client(cls, msgtype: int) -> "Datagram":
        """Build a client->CA datagram (no routing header)."""
        dg = cls()
        dg.add_uint16(msgtype)
        tracker.record_sent(msgtype)
        return dg


class DatagramIterator:
    """Reader for datagrams built above."""

    def __init__(self, dg_or_bytes) -> None:
        if isinstance(dg_or_bytes, Datagram):
            self._buf = memoryview(dg_or_bytes.bytes())
        else:
            self._buf = memoryview(bytes(dg_or_bytes))
        self._off = 0

    # --- primitive readers ---
    def _read(self, fmt: str, size: int):
        if self._off + size > len(self._buf):
            raise IndexError(f"DatagramIterator read past end at offset {self._off}")
        v = struct.unpack_from(fmt, self._buf, self._off)[0]
        self._off += size
        return v

    def read_bool(self) -> bool:
        return bool(self._read("<B", 1))

    def read_int8(self) -> int:
        return self._read("<b", 1)

    def read_uint8(self) -> int:
        return self._read("<B", 1)

    def read_int16(self) -> int:
        return self._read("<h", 2)

    def read_uint16(self) -> int:
        return self._read("<H", 2)

    def read_int32(self) -> int:
        return self._read("<i", 4)

    def read_uint32(self) -> int:
        return self._read("<I", 4)

    def read_int64(self) -> int:
        return self._read("<q", 8)

    def read_uint64(self) -> int:
        return self._read("<Q", 8)

    def read_float32(self) -> float:
        return self._read("<f", 4)

    def read_float64(self) -> float:
        return self._read("<d", 8)

    def read_string(self) -> str:
        n = self.read_uint16()
        s = bytes(self._buf[self._off : self._off + n])
        self._off += n
        return s.decode("utf-8")

    def read_blob(self) -> bytes:
        n = self.read_uint16()
        b = bytes(self._buf[self._off : self._off + n])
        self._off += n
        return b

    def read_channel(self) -> int:
        return self.read_uint64()

    def read_doid(self) -> int:
        return self.read_uint32()

    def read_header(self):
        """Consume the [count][recipients...][sender][msgtype] header.

        Returns (recipients, sender, msgtype). Also records the msgtype in the
        coverage tracker.
        """
        count = self.read_uint8()
        recipients = [self.read_channel() for _ in range(count)]
        sender = self.read_channel()
        msgtype = self.read_uint16()
        tracker.record_recv(msgtype)
        return recipients, sender, msgtype

    def read_client_msgtype(self) -> int:
        """For datagrams going to/from a client (no routing header)."""
        mt = self.read_uint16()
        tracker.record_recv(mt)
        return mt

    def seek(self, offset: int) -> None:
        self._off = offset

    def tell(self) -> int:
        return self._off

    def remaining(self) -> int:
        return len(self._buf) - self._off

    def peek(self, n: int) -> bytes:
        return bytes(self._buf[self._off : self._off + n])


class MDConnection:
    """Raw MD-protocol TCP connection."""

    TIMEOUT = 5.0

    def __init__(self, host: str, port: int) -> None:
        self.sock = socket.create_connection((host, port), timeout=self.TIMEOUT)
        self.sock.setblocking(True)
        self._rx = bytearray()

    def close(self) -> None:
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()

    def send(self, dg: Datagram) -> None:
        data = dg.bytes()
        if len(data) > 0xFFFF:
            raise ValueError(f"datagram too large: {len(data)}B")
        self.sock.sendall(struct.pack("<H", len(data)) + data)

    def _recv_n(self, n: int, timeout: float) -> bytes:
        """Read exactly n bytes from the socket with a timeout."""
        deadline = time.monotonic() + timeout
        while len(self._rx) < n:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    f"timed out waiting for {n}B (have {len(self._rx)}B)"
                )
            ready, _, _ = select.select([self.sock], [], [], remaining)
            if not ready:
                raise TimeoutError(
                    f"timed out waiting for {n}B (have {len(self._rx)}B)"
                )
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("peer closed connection")
            self._rx.extend(chunk)
        out = bytes(self._rx[:n])
        del self._rx[:n]
        return out

    def recv(self, timeout: Optional[float] = None) -> Datagram:
        t = self.TIMEOUT if timeout is None else timeout
        length = struct.unpack("<H", self._recv_n(2, t))[0]
        payload = self._recv_n(length, t) if length else b""
        return Datagram(payload)

    def recv_maybe(self, timeout: float = 0.5) -> Optional[Datagram]:
        """Return a datagram if one arrives within timeout, else None."""
        try:
            return self.recv(timeout=timeout)
        except TimeoutError:
            return None

    def wait_for(
        self,
        predicate: Callable[["Datagram"], bool],
        *,
        timeout: float = 2.0,
    ) -> Datagram:
        """Receive datagrams until ``predicate(dg)`` returns True or timeout.

        Non-matching datagrams are dropped. Use this instead of a blind sleep
        whenever the test can identify a specific message that proves the
        daemon has reached a desired state — round-trip a probe through the
        SS, watch a location channel for an entry, etc.

        Raises TimeoutError if no datagram satisfies the predicate within the
        deadline. Forward-compatible with ``ConnectionError`` from
        ``recv()`` (peer closed): the underlying error propagates.
        """
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    f"wait_for: predicate not satisfied within {timeout}s"
                )
            try:
                dg = self.recv(timeout=remaining)
            except TimeoutError:
                raise TimeoutError(
                    f"wait_for: predicate not satisfied within {timeout}s"
                )
            if predicate(dg):
                return dg

    # --- assertion helpers (Astron-style) ---
    def expect(self, expected: Datagram, timeout: float = 2.0) -> Datagram:
        got = self.recv(timeout=timeout)
        if got != expected:
            raise AssertionError(
                f"\nexpected: {expected.bytes().hex()}\n"
                f"     got: {got.bytes().hex()}\n"
            )
        return got

    def expect_none(self, timeout: float = 0.25) -> None:
        got = self.recv_maybe(timeout=timeout)
        if got is not None:
            it = DatagramIterator(got)
            try:
                _, _, mt = it.read_header()
                name = symbol_for(mt)
            except Exception:
                name = "?"
            raise AssertionError(
                f"expected no datagram; got {name}: {got.bytes().hex()}"
            )

    def expect_multi(self, expected: Sequence[Datagram], timeout: float = 2.0) -> None:
        """Expect a set of datagrams (order-independent) within timeout."""
        remaining: List[Datagram] = list(expected)
        deadline = time.monotonic() + timeout
        while remaining:
            t = max(0.01, deadline - time.monotonic())
            got = self.recv(timeout=t)
            if got in remaining:
                remaining.remove(got)
            else:
                raise AssertionError(
                    f"unexpected datagram: {got.bytes().hex()}\n"
                    f"still expecting ({len(remaining)}): "
                    + ", ".join(dg.bytes().hex() for dg in remaining)
                )


class ChannelConnection(MDConnection):
    """MD connection bound to a set of subscribed channels."""

    def __init__(self, host: str, port: int, *channels: int) -> None:
        super().__init__(host, port)
        self._subs: List[int] = []
        for ch in channels:
            self.subscribe(ch)

    def subscribe(self, channel: int) -> None:
        self._subs.append(channel)
        dg = Datagram.create_control(CONTROL_ADD_CHANNEL).add_channel(channel)
        self.send(dg)

    def unsubscribe(self, channel: int) -> None:
        if channel in self._subs:
            self._subs.remove(channel)
        dg = Datagram.create_control(CONTROL_REMOVE_CHANNEL).add_channel(channel)
        self.send(dg)

    def add_range(self, lo: int, hi: int) -> None:
        dg = Datagram.create_control(CONTROL_ADD_RANGE).add_channel(lo).add_channel(hi)
        self.send(dg)

    def flush(self) -> None:
        """Drop any queued datagrams (e.g. after setup churn)."""
        while self.recv_maybe(timeout=0.05) is not None:
            pass

    def wait_object_alive(
        self,
        do_id: int,
        *,
        sender: Optional[int] = None,
        timeout: float = 2.0,
        probe_interval: float = 0.1,
    ) -> None:
        """Round-trip GET_LOCATION through ``do_id`` to confirm the DO has
        finished spawning and bound its DoId queue on RabbitMQ. Use this
        after a create_object instead of a blind sleep before sending
        anything to that DoId.

        Re-sends every ``probe_interval`` seconds because the first probe
        may itself race the bind and be dropped — once the bind lands, a
        probe will reach the DO and we'll observe the response.

        ``sender`` is the channel the GET_LOCATION_RESP comes back on; it
        must be one this connection is subscribed to. Defaults to the
        first subscribed channel.
        """
        if sender is None:
            if not self._subs:
                raise RuntimeError(
                    "wait_object_alive: no subscribed channel to receive on; "
                    "pass `sender=` or subscribe first"
                )
            sender = self._subs[0]

        ctx = 0xC0FFEE  # arbitrary, we only check it's echoed back

        def send_probe() -> None:
            self.send(
                Datagram.create(
                    [do_id],
                    sender=sender,
                    msgtype=STATESERVER_OBJECT_GET_LOCATION,
                ).add_uint32(ctx)
            )

        def is_resp(dg: Datagram) -> bool:
            try:
                it = DatagramIterator(dg)
                _, _, mt = it.read_header()
                if mt != STATESERVER_OBJECT_GET_LOCATION_RESP:
                    return False
                return it.read_uint32() == ctx
            except Exception:
                return False

        deadline = time.monotonic() + timeout
        send_probe()
        last_send = time.monotonic()
        while time.monotonic() < deadline:
            try:
                dg = self.recv(timeout=probe_interval)
            except TimeoutError:
                if time.monotonic() - last_send >= probe_interval:
                    send_probe()
                    last_send = time.monotonic()
                continue
            if is_resp(dg):
                return
            # Other traffic on this channel — drop and keep waiting.
        raise TimeoutError(
            f"wait_object_alive: no GET_LOCATION_RESP for {do_id} in {timeout}s"
        )

    def wait_range_active(
        self, lo: int, hi: int, *, sender: int = 0, timeout: float = 2.0
    ) -> None:
        """Confirm a range subscription is live by self-probing.

        Sends a sentinel-msgtype datagram to a channel inside [lo, hi] and
        waits for it to come back through the bus. Replaces a blind sleep
        after ``add_range`` with a wait on an actual signal that the
        bucket-based RabbitMQ binding is in place.

        Picks ``lo`` as the probe channel. ``sender`` defaults to 0 since
        this connection's subscription is via range only (no explicit
        sender channel needed for the round-trip).
        """
        sentinel = 0xBEEF
        self.send(Datagram.create([lo], sender=sender, msgtype=sentinel))

        def is_probe(dg: Datagram) -> bool:
            try:
                it = DatagramIterator(dg)
                _, _, mt = it.read_header()
                return mt == sentinel
            except Exception:
                return False

        self.wait_for(is_probe, timeout=timeout)


class ClientConnection(MDConnection):
    """CA-facing TCP connection. Implements the CLIENT_HELLO handshake."""

    def hello(self, dc_hash: int, version: str = "dev") -> None:
        dg = (
            Datagram.create_client(CLIENT_HELLO).add_uint32(dc_hash).add_string(version)
        )
        self.send(dg)

    def expect_hello_resp(self, timeout: float = 2.0) -> None:
        got = self.recv(timeout=timeout)
        it = DatagramIterator(got)
        mt = it.read_client_msgtype()
        if mt != CLIENT_HELLO_RESP:
            raise AssertionError(
                f"expected CLIENT_HELLO_RESP ({CLIENT_HELLO_RESP}); got {mt} "
                f"({symbol_for(mt)})"
            )

    def heartbeat(self) -> None:
        self.send(Datagram.create_client(CLIENT_HEARTBEAT))

    # --- avatar-ownership / field-update helpers ------------------------------

    def send_field(self, do_id: int, field_id: int, payload: bytes = b"") -> None:
        """Send a clsend/ownsend field update from the client to the server.

        Wire format (client -> CA):
          [uint16 CLIENT_OBJECT_SET_FIELD][uint32 doId][uint16 fieldId][data]

        `payload` is the already-packed field value (use a Datagram builder
        for anything non-trivial, then `.bytes()` it).
        """
        dg = (
            Datagram.create_client(CLIENT_OBJECT_SET_FIELD)
            .add_uint32(do_id)
            .add_uint16(field_id)
            .add_raw(payload)
        )
        self.send(dg)

    def expect_object_entry(
        self,
        *,
        owner: Optional[bool] = True,
        timeout: float = 5.0,
    ) -> "ObjectEntry":
        """Wait for a CLIENT_ENTER_OBJECT_REQUIRED[_OWNER][_OTHER] and decode it.

        CLIENT_ADD_INTEREST, CLIENT_ADD_INTEREST_MULTIPLE, and
        CLIENT_DONE_INTEREST_RESP datagrams that arrive ahead of the entry
        are transparently consumed (each is its own framed datagram) so
        callers can ignore the bookkeeping messages the CA emits around
        AI-driven interest opens.

        If `owner` is None either owner or non-owner variants are accepted.

        Wire format (src/clientagent/client_participant.cpp HandleAddOwnership /
        HandleAddObject):
          [uint16 msgType][uint32 doId][uint32 parent][uint32 zone]
          [uint16 dcId][<required field data...>]

        Required-field payload is opaque to the harness (it's DC-shaped); the
        caller slices it if needed.
        """
        if owner is None:
            expected = {
                CLIENT_ENTER_OBJECT_REQUIRED,
                CLIENT_ENTER_OBJECT_REQUIRED_OTHER,
                CLIENT_ENTER_OBJECT_REQUIRED_OWNER,
                CLIENT_ENTER_OBJECT_REQUIRED_OTHER_OWNER,
            }
        elif owner:
            expected = {
                CLIENT_ENTER_OBJECT_REQUIRED_OWNER,
                CLIENT_ENTER_OBJECT_REQUIRED_OTHER_OWNER,
            }
        else:
            expected = {
                CLIENT_ENTER_OBJECT_REQUIRED,
                CLIENT_ENTER_OBJECT_REQUIRED_OTHER,
            }

        deadline = time.monotonic() + timeout
        while True:
            remaining = max(0.01, deadline - time.monotonic())
            got = self.recv(timeout=remaining)
            it = DatagramIterator(got)
            mt = it.read_client_msgtype()
            if mt in _INTEREST_BOOKKEEPING_MSGS:
                # Drain bookkeeping datagrams and keep waiting for the entry.
                continue
            break
        if mt not in expected:
            raise AssertionError(
                f"expected object-entry msg ({sorted(expected)}); got "
                f"{mt} ({symbol_for(mt)})"
            )
        do_id = it.read_uint32()
        parent = it.read_uint32()
        zone = it.read_uint32()
        dc_id = it.read_uint16()
        required = bytes(it._buf[it._off :])
        return ObjectEntry(
            msgtype=mt,
            do_id=do_id,
            parent=parent,
            zone=zone,
            dc_id=dc_id,
            required=required,
            owner=(
                mt
                in {
                    CLIENT_ENTER_OBJECT_REQUIRED_OWNER,
                    CLIENT_ENTER_OBJECT_REQUIRED_OTHER_OWNER,
                }
            ),
        )

    def expect_object_set_field(
        self, *, do_id: Optional[int] = None, timeout: float = 2.0
    ) -> "ClientFieldUpdate":
        """Consume a CLIENT_OBJECT_SET_FIELD delivered from server -> client.

        Drains interest-bookkeeping and (re)entry datagrams that may arrive
        ahead of the field update so callers don't have to drive the order
        explicitly.
        """
        skip = _INTEREST_BOOKKEEPING_MSGS | {
            CLIENT_ENTER_OBJECT_REQUIRED,
            CLIENT_ENTER_OBJECT_REQUIRED_OTHER,
            CLIENT_ENTER_OBJECT_REQUIRED_OWNER,
            CLIENT_ENTER_OBJECT_REQUIRED_OTHER_OWNER,
        }
        deadline = time.monotonic() + timeout
        while True:
            remaining = max(0.01, deadline - time.monotonic())
            got = self.recv(timeout=remaining)
            it = DatagramIterator(got)
            mt = it.read_client_msgtype()
            if mt in skip:
                continue
            break
        if mt != CLIENT_OBJECT_SET_FIELD:
            raise AssertionError(
                f"expected CLIENT_OBJECT_SET_FIELD; got {mt} ({symbol_for(mt)})"
            )
        received_id = it.read_uint32()
        if do_id is not None and received_id != do_id:
            raise AssertionError(
                f"expected field update for doId {do_id}; got {received_id}"
            )
        field_id = it.read_uint16()
        payload = bytes(it._buf[it._off :])
        return ClientFieldUpdate(do_id=received_id, field_id=field_id, payload=payload)

    def expect_eject(self, *, reason: Optional[int] = None, timeout: float = 2.0):
        """Wait for a CLIENT_EJECT and return (reason, message).

        Records the disconnect reason in the coverage tracker so the
        ClientDisconnects enum gets exercised — reasons are read as raw
        uint16s otherwise and the tracker would never see them.
        """
        got = self.recv(timeout=timeout)
        it = DatagramIterator(got)
        mt = it.read_client_msgtype()
        if mt != CLIENT_EJECT:
            raise AssertionError(f"expected CLIENT_EJECT; got {mt} ({symbol_for(mt)})")
        got_reason = it.read_uint16()
        tracker.record_recv(got_reason)
        msg = it.read_string()
        if reason is not None and got_reason != reason:
            raise AssertionError(
                f"expected eject reason {reason}; got {got_reason} ({msg!r})"
            )
        return got_reason, msg


class ObjectEntry:
    """Parsed CLIENT_ENTER_OBJECT_REQUIRED[_OWNER][_OTHER] payload."""

    __slots__ = ("msgtype", "do_id", "parent", "zone", "dc_id", "required", "owner")

    def __init__(self, *, msgtype, do_id, parent, zone, dc_id, required, owner):
        self.msgtype = msgtype
        self.do_id = do_id
        self.parent = parent
        self.zone = zone
        self.dc_id = dc_id
        self.required = required
        self.owner = owner

    def __repr__(self) -> str:
        return (
            f"ObjectEntry(do_id={self.do_id}, parent={self.parent}, "
            f"zone={self.zone}, dc_id={self.dc_id}, "
            f"required={len(self.required)}B, owner={self.owner})"
        )


class ClientFieldUpdate:
    """Parsed CLIENT_OBJECT_SET_FIELD payload."""

    __slots__ = ("do_id", "field_id", "payload")

    def __init__(self, *, do_id, field_id, payload):
        self.do_id = do_id
        self.field_id = field_id
        self.payload = payload


class AIConnection(ChannelConnection):
    """MD connection that speaks the internal protocol as an "AI server" would.

    A real AI subscribes to its own channel, creates distributed objects in
    the state server, hands ownership to clients, and shapes client interest.
    This helper wraps those primitives so tests can assert behaviour without
    hand-rolling headers each time.

    The `ai_channel` is the sender value baked into outgoing messages. Pick
    anything that doesn't collide with config-configured channels (CA/SS/DB)
    or a CA's client-channel pool.
    """

    DEFAULT_AI_CHANNEL = 5000
    DEFAULT_SS_CHANNEL = 1000

    def __init__(
        self,
        host: str,
        port: int,
        *,
        ai_channel: int = DEFAULT_AI_CHANNEL,
        ss_channel: int = DEFAULT_SS_CHANNEL,
    ) -> None:
        super().__init__(host, port, ai_channel)
        self.ai_channel = ai_channel
        self.ss_channel = ss_channel

    # --- CA control -----------------------------------------------------------

    def set_client_state(
        self,
        client_channel: int,
        state: int,
        *,
        wait: bool = True,
        timeout: float = 2.0,
    ) -> None:
        """Force CLIENTAGENT_SET_STATE — jump a client's auth gate.

        With ``wait=True`` (default), follows the SET_STATE with a
        round-trip through the same channel and blocks until the response
        comes back. Required when the *next* operation will come from a
        different TCP socket (e.g. the test's ``client_conn``), since the
        daemon reads from each socket independently — otherwise the client's
        next message can race the SET_STATE and be rejected as pre-auth.
        """
        dg = Datagram.create(
            [client_channel], sender=self.ai_channel, msgtype=CLIENTAGENT_SET_STATE
        ).add_uint16(state)
        self.send(dg)
        if wait:
            self.wait_channel_drained(client_channel, timeout=timeout)

    def wait_channel_drained(self, channel: int, *, timeout: float = 2.0) -> None:
        """Block until prior messages sent on this AI socket to ``channel``
        have been processed.

        Implementation: send a CLIENTAGENT_GET_NETWORK_ADDRESS probe to the
        channel and wait for the matching RESP back on this AI's
        subscription. The CP handles its inbound queue in FIFO order, so
        the RESP arriving means everything we sent earlier is done.
        """
        ctx = 0xFEEDFACE
        self.send(
            Datagram.create(
                [channel],
                sender=self.ai_channel,
                msgtype=CLIENTAGENT_GET_NETWORK_ADDRESS,
            ).add_uint32(ctx)
        )

        def is_resp(dg: Datagram) -> bool:
            try:
                it = DatagramIterator(dg)
                _, _, mt = it.read_header()
                if mt != CLIENTAGENT_GET_NETWORK_ADDRESS_RESP:
                    return False
                return it.read_uint32() == ctx
            except Exception:
                return False

        self.wait_for(is_resp, timeout=timeout)

    def set_client_id(self, client_channel: int, new_channel: int) -> None:
        """Rebind a CA's subscription channel (CLIENTAGENT_SET_CLIENT_ID).

        The CA unsubscribes its old channel and subscribes `new_channel`. The
        caller is responsible for issuing subsequent messages to
        `new_channel` from here on.
        """
        dg = Datagram.create(
            [client_channel], sender=self.ai_channel, msgtype=CLIENTAGENT_SET_CLIENT_ID
        ).add_channel(new_channel)
        self.send(dg)

    def add_interest(
        self, client_channel: int, interest_id: int, parent: int, zone: int
    ) -> None:
        """Push a single-zone interest onto a client (CLIENTAGENT_ADD_INTEREST)."""
        dg = (
            Datagram.create(
                [client_channel],
                sender=self.ai_channel,
                msgtype=CLIENTAGENT_ADD_INTEREST,
            )
            .add_uint16(interest_id)
            .add_uint32(parent)
            .add_uint32(zone)
        )
        self.send(dg)

    def add_session_object(self, client_channel: int, do_id: int) -> None:
        """Bind an object's lifetime to the client session. Deleting the object
        disconnects the client (CLIENT_DISCONNECT_SESSION_OBJECT_DELETED)."""
        dg = Datagram.create(
            [client_channel],
            sender=self.ai_channel,
            msgtype=CLIENTAGENT_ADD_SESSION_OBJECT,
        ).add_uint32(do_id)
        self.send(dg)

    def eject(self, client_channel: int, reason: int, message: str) -> None:
        """Force-eject the client (CLIENTAGENT_EJECT)."""
        dg = (
            Datagram.create(
                [client_channel], sender=self.ai_channel, msgtype=CLIENTAGENT_EJECT
            )
            .add_uint16(reason)
            .add_string(message)
        )
        self.send(dg)

    # --- StateServer control --------------------------------------------------

    def create_object(
        self,
        do_id: int,
        parent: int,
        zone: int,
        dclass_id: int,
        required: bytes = b"",
    ) -> None:
        """Create a DistributedObject in the state server.

        `required` is the concatenated packed values of every required field
        in class order (see tests/common/dc.py for IDs). Use an empty bytes
        object for classes whose required fields all have defaults.
        """
        dg = (
            Datagram.create(
                [self.ss_channel],
                sender=self.ai_channel,
                msgtype=STATESERVER_CREATE_OBJECT_WITH_REQUIRED,
            )
            .add_uint32(do_id)
            .add_uint32(parent)
            .add_uint32(zone)
            .add_uint16(dclass_id)
            .add_raw(required)
        )
        self.send(dg)

    def create_object_with_other(
        self,
        do_id: int,
        parent: int,
        zone: int,
        dclass_id: int,
        required: bytes = b"",
        other: Sequence = (),
    ) -> None:
        """Create a DistributedObject with both required and ram (`other`)
        fields. Wire format adds [uint16 otherCount][fieldId, value]+ after
        the required block — see StateServerImplementation::HandleGenerate.

        ``other`` is a sequence of ``(field_id, packed_value_bytes)`` tuples.
        Pack each value with a Datagram builder and call ``.bytes()``.
        """
        dg = (
            Datagram.create(
                [self.ss_channel],
                sender=self.ai_channel,
                msgtype=STATESERVER_CREATE_OBJECT_WITH_REQUIRED_OTHER,
            )
            .add_uint32(do_id)
            .add_uint32(parent)
            .add_uint32(zone)
            .add_uint16(dclass_id)
            .add_raw(required)
            .add_uint16(len(other))
        )
        for field_id_, payload in other:
            dg.add_uint16(field_id_).add_raw(payload)
        self.send(dg)

    def set_owner(self, do_id: int, owner_channel: int) -> None:
        """Assign ownership (STATESERVER_OBJECT_SET_OWNER). The SS will push an
        ENTER_OWNER_WITH_REQUIRED[_OTHER] to `owner_channel`, which the CA
        translates into CLIENT_ENTER_OBJECT_REQUIRED_OWNER[_OTHER] for the
        client socket bound to that channel."""
        dg = Datagram.create(
            [do_id],
            sender=self.ai_channel,
            msgtype=STATESERVER_OBJECT_SET_OWNER,
        ).add_channel(owner_channel)
        self.send(dg)

    def set_field(self, do_id: int, field_id: int, payload: bytes) -> None:
        """Broadcast a field update authored by the AI side."""
        dg = (
            Datagram.create(
                [do_id],
                sender=self.ai_channel,
                msgtype=STATESERVER_OBJECT_SET_FIELD,
            )
            .add_uint32(do_id)
            .add_uint16(field_id)
            .add_raw(payload)
        )
        self.send(dg)

    def delete_object(self, do_id: int) -> None:
        """Delete from RAM (STATESERVER_OBJECT_DELETE_RAM)."""
        dg = Datagram.create(
            [do_id],
            sender=self.ai_channel,
            msgtype=STATESERVER_OBJECT_DELETE_RAM,
        ).add_uint32(do_id)
        self.send(dg)


class Daemon:
    """Manages a single ardos process for the duration of a test."""

    # Seconds to wait for each listen socket before giving up.
    BOOT_TIMEOUT = 30.0

    def __init__(
        self,
        config_path: Path,
        log_path: Path,
        ports: Iterable[int],
        md_port: Optional[int] = None,
    ) -> None:
        self.config_path = Path(config_path)
        self.log_path = Path(log_path)
        self.ports = list(ports)
        # MD port — used for the readiness round-trip after listen sockets
        # come up. None means the daemon doesn't host an MD (probe is skipped).
        self.md_port = md_port
        self._proc: Optional[subprocess.Popen] = None

    def start(self) -> None:
        binary = locate_binary()
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        log = self.log_path.open("wb")
        self._proc = subprocess.Popen(
            [str(binary), "--config", str(self.config_path)],
            stdout=log,
            stderr=subprocess.STDOUT,
            cwd=self.config_path.parent,
        )
        self._wait_ready()

    def _wait_ready(self) -> None:
        deadline = time.monotonic() + self.BOOT_TIMEOUT
        pending = list(self.ports)
        while pending:
            if time.monotonic() > deadline:
                self._dump_tail()
                raise TimeoutError(
                    f"ardos did not open ports {pending} within {self.BOOT_TIMEOUT}s"
                )
            if self._proc and self._proc.poll() is not None:
                self._dump_tail()
                raise RuntimeError(f"ardos exited early (rc={self._proc.returncode})")
            still = []
            for p in pending:
                try:
                    with socket.create_connection(("127.0.0.1", p), timeout=0.25):
                        pass
                except OSError:
                    still.append(p)
            pending = still
            if pending:
                time.sleep(0.1)

        # Listen sockets are open, but the MD's RabbitMQ subscriber set may
        # still be coming online — accept-then-immediately-publish would
        # race the consumer. Round-trip a control-message probe through the
        # bus to confirm the MD is fully wired.
        if self.md_port is not None:
            self._md_round_trip(timeout=max(2.0, deadline - time.monotonic()))

    def _md_round_trip(self, *, timeout: float) -> None:
        """Probe the MD by subscribing to a private channel, publishing a
        sentinel-msgtype datagram to it, and waiting for it to come back.
        Re-sends every 250ms in case the first probe races the queue
        binding."""
        probe_channel = 0xC0FFEEC0FFEE  # private to this probe
        sentinel_mt = 0xBEEF
        try:
            conn = ChannelConnection("127.0.0.1", self.md_port, probe_channel)
        except OSError:
            return  # MD port wasn't actually open; let the caller's tests fail loudly
        try:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                conn.send(
                    Datagram.create([probe_channel], sender=0, msgtype=sentinel_mt)
                )

                def is_probe(dg: Datagram) -> bool:
                    try:
                        it = DatagramIterator(dg)
                        _, _, mt = it.read_header()
                        return mt == sentinel_mt
                    except Exception:
                        return False

                try:
                    conn.wait_for(is_probe, timeout=0.25)
                    return
                except TimeoutError:
                    continue
            self._dump_tail()
            raise TimeoutError(f"MD round-trip probe never came back within {timeout}s")
        finally:
            try:
                conn.close()
            except Exception:
                pass

    def _dump_tail(self) -> None:
        try:
            tail = self.log_path.read_bytes()[-4096:].decode("utf-8", "replace")
            print(f"\n--- ardos log tail ({self.log_path}) ---\n{tail}\n---")
        except OSError:
            pass

    def stop(self) -> None:
        if not self._proc:
            return
        if self._proc.poll() is None:
            self._proc.send_signal(signal.SIGTERM)
            try:
                self._proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait(timeout=2.0)
        self._proc = None

    def __enter__(self) -> "Daemon":
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.stop()
