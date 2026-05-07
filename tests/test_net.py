"""Datagram wire-format round-trip tests.

These are pure-Python, no daemon needed. They pin the harness's `Datagram` /
`DatagramIterator` encoding to the spec in src/net/datagram.cpp — if the C++
side ever changes endianness or header layout, these fail fast.
"""

import pytest

from tests.common.ardos import Datagram, DatagramIterator


class TestPrimitives:
    @pytest.mark.parametrize("val", [0, 1, 127, -128, -1])
    def test_int8_round_trip(self, val):
        dg = Datagram().add_int8(val)
        assert DatagramIterator(dg).read_int8() == val

    @pytest.mark.parametrize("val", [0, 42, 0xFF])
    def test_uint8_round_trip(self, val):
        assert DatagramIterator(Datagram().add_uint8(val)).read_uint8() == val

    @pytest.mark.parametrize("val", [0, 0xFFFF, 0x1234])
    def test_uint16_little_endian(self, val):
        dg = Datagram().add_uint16(val)
        # Confirm LE bytes, not native-host-dependent.
        assert dg.bytes() == val.to_bytes(2, "little")
        assert DatagramIterator(dg).read_uint16() == val

    @pytest.mark.parametrize("val", [0, 0xFFFFFFFF, 0xDEADBEEF])
    def test_uint32_round_trip(self, val):
        assert DatagramIterator(Datagram().add_uint32(val)).read_uint32() == val

    @pytest.mark.parametrize("val", [0, 2**64 - 1, 1 << 40])
    def test_uint64_round_trip(self, val):
        assert DatagramIterator(Datagram().add_uint64(val)).read_uint64() == val

    def test_string_encoding(self):
        dg = Datagram().add_string("hello")
        # uint16 length prefix (5, LE) then utf-8 bytes.
        assert dg.bytes() == b"\x05\x00hello"
        assert DatagramIterator(dg).read_string() == "hello"

    def test_blob_encoding(self):
        data = bytes(range(10))
        dg = Datagram().add_blob(data)
        assert dg.bytes()[:2] == len(data).to_bytes(2, "little")
        assert DatagramIterator(dg).read_blob() == data

    def test_location_encoding(self):
        dg = Datagram().add_location(100, 5)
        it = DatagramIterator(dg)
        assert it.read_uint32() == 100
        assert it.read_uint32() == 5


class TestHeaders:
    def test_single_recipient_header(self):
        dg = Datagram.create([0xDEAD_BEEF], sender=0xFEED, msgtype=2000)
        recipients, sender, mt = DatagramIterator(dg).read_header()
        assert recipients == [0xDEAD_BEEF]
        assert sender == 0xFEED
        assert mt == 2000

    def test_multi_recipient_header(self):
        recips = [1001, 1002, 1003]
        dg = Datagram.create(recips, sender=5, msgtype=2040)
        got_recips, got_sender, got_mt = DatagramIterator(dg).read_header()
        assert got_recips == recips
        assert got_sender == 5
        assert got_mt == 2040

    def test_control_header(self):
        dg = Datagram.create_control(9000).add_channel(4242)
        it = DatagramIterator(dg)
        recips, sender, _ = (it.read_uint8(), None, None)
        # control: count=1, channel=CONTROL_CHANNEL, msgtype, then payload
        assert recips == 1
        assert it.read_uint64() == 1  # CONTROL_CHANNEL
        assert it.read_uint16() == 9000
        assert it.read_uint64() == 4242

    def test_client_header(self):
        dg = Datagram.create_client(5).add_uint32(42)
        it = DatagramIterator(dg)
        assert it.read_client_msgtype() == 5
        assert it.read_uint32() == 42


class TestEquality:
    def test_equal_by_bytes(self):
        a = Datagram.create([5], 1, 100).add_uint32(42)
        b = Datagram.create([5], 1, 100).add_uint32(42)
        assert a == b

    def test_unequal(self):
        a = Datagram.create([5], 1, 100)
        b = Datagram.create([6], 1, 100)
        assert a != b


class TestIteratorBounds:
    def test_read_past_end_raises(self):
        it = DatagramIterator(Datagram().add_uint8(1))
        it.read_uint8()
        with pytest.raises(IndexError):
            it.read_uint8()

    def test_seek_and_tell(self):
        dg = Datagram().add_uint32(1).add_uint32(2).add_uint32(3)
        it = DatagramIterator(dg)
        it.read_uint32()
        assert it.tell() == 4
        it.seek(8)
        assert it.read_uint32() == 3
