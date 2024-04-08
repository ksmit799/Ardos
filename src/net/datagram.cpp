#include "datagram.h"

#include <cstring>

namespace Ardos {

Datagram::Datagram()
    : _buf(new uint8_t[kMinDgSize]), _bufOffset(0), _bufLength(kMinDgSize) {}

Datagram::Datagram(const uint8_t *data, const size_t &size)
    : _buf(new uint8_t[size]), _bufOffset(size), _bufLength(size) {
  memcpy(_buf, data, size);
}

Datagram::Datagram(const uint64_t &toChannel, const uint64_t &fromChannel,
                   const uint16_t &msgType)
    : _buf(new uint8_t[kMinDgSize]), _bufOffset(0), _bufLength(kMinDgSize) {
  AddUint8(1);
  AddUint64(toChannel);
  AddUint64(fromChannel);
  AddUint16(msgType);
}

Datagram::Datagram(const std::unordered_set<uint64_t> &toChannels,
                   const uint64_t &fromChannel, const uint16_t &msgType)
    : _buf(new uint8_t[kMinDgSize]), _bufOffset(0), _bufLength(kMinDgSize) {
  AddUint8(toChannels.size());
  for (const auto &channel : toChannels) {
    AddUint64(channel);
  }
  AddUint64(fromChannel);
  AddUint16(msgType);
}

Datagram::~Datagram() { delete[] _buf; }

/**
 * Clears this datagram of data ready for rewriting.
 * Good for re-using datagrams rather than re-alloc.
 */
void Datagram::Clear() {
  // Wipe out the buffer offset without deleting it.
  // This should prevent redundant re-sizing.
  _bufOffset = 0;
}

/**
 * Returns the number of bytes added to this datagram.
 * @return
 */
uint16_t Datagram::Size() const { return _bufOffset; }

/**
 * Returns the underlying data pointer for this datagram.
 * @return
 */
const uint8_t *Datagram::GetData() const { return _buf; }

/**
 * Returns the bytes packed into this datagram.
 * @return
 */
std::vector<uint8_t> Datagram::GetBytes() const {
  std::vector<uint8_t> data(GetData(), GetData() + Size());
  return data;
}

/**
 * Adds a boolean to this datagram.
 * @param v
 */
void Datagram::AddBool(const bool &v) { AddUint8(v ? 1 : 0); }

/**
 * Adds a signed 8-bit integer to this datagram.
 * @param v
 */
void Datagram::AddInt8(const int8_t &v) {
  EnsureLength(1);
  *(int8_t *)(_buf + _bufOffset) = v;
  _bufOffset += 1;
}

/**
 * Adds an unsigned 8-bit integer to this datagram.
 * @param v
 */
void Datagram::AddUint8(const uint8_t &v) {
  EnsureLength(1);
  *(uint8_t *)(_buf + _bufOffset) = v;
  _bufOffset += 1;
}

/**
 * Adds a signed 16-bit integer to this datagram.
 * @param v
 */
void Datagram::AddInt16(const int16_t &v) {
  EnsureLength(2);
  *(int16_t *)(_buf + _bufOffset) = v;
  _bufOffset += 2;
}

/**
 * Adds an unsigned 16-bit integer to this datagram.
 * @param v
 */
void Datagram::AddUint16(const uint16_t &v) {
  EnsureLength(2);
  *(uint16_t *)(_buf + _bufOffset) = v;
  _bufOffset += 2;
}

/**
 * Adds a signed 32-bit integer to this datagram.
 * @param v
 */
void Datagram::AddInt32(const int32_t &v) {
  EnsureLength(4);
  *(int32_t *)(_buf + _bufOffset) = v;
  _bufOffset += 4;
}

/**
 * Adds an unsigned 32-bit integer to this datagram.
 * @param v
 */
void Datagram::AddUint32(const uint32_t &v) {
  EnsureLength(4);
  *(uint32_t *)(_buf + _bufOffset) = v;
  _bufOffset += 4;
}

/**
 * Adds a signed 64-bit integer to this datagram.
 * @param v
 */
void Datagram::AddInt64(const int64_t &v) {
  EnsureLength(8);
  *(int64_t *)(_buf + _bufOffset) = v;
  _bufOffset += 8;
}

/**
 * Adds an unsigned 64-bit integer to this datagram.
 * @param v
 */
void Datagram::AddUint64(const uint64_t &v) {
  EnsureLength(8);
  *(uint64_t *)(_buf + _bufOffset) = v;
  _bufOffset += 8;
}

/**
 * Adds a 32-bit floating point number to this datagram.
 * @param v
 */
void Datagram::AddFloat32(const float &v) {
  EnsureLength(4);
  *(float *)(_buf + _bufOffset) = v;
  _bufOffset += 4;
}

/**
 * Adds a 64-bit floating point number to this datagram.
 * @param v
 */
void Datagram::AddFloat64(const double &v) {
  EnsureLength(8);
  *(double *)(_buf + _bufOffset) = v;
  _bufOffset += 8;
}

/**
 * Adds a string to this datagram.
 * NOTE: Strings are limited to a max-length of a uint16_t (65k)
 * @param v
 */
void Datagram::AddString(const std::string &v) {
  EnsureLength(v.length() + 2); // +2 for length tag.
  AddUint16(v.length());
  memcpy(_buf + _bufOffset, v.c_str(), v.length());
  _bufOffset += v.length();
}

/**
 * Adds a blob of arbitrary data to this datagram.
 * NOTE: Blobs are limited to a max-length of a uint16_t (65k)
 * @param v
 */
void Datagram::AddBlob(const std::vector<uint8_t> &v) {
  EnsureLength(v.size() + 2); // +2 for length tag.
  AddUint16(v.size());
  memcpy(_buf + _bufOffset, &v[0], v.size());
  _bufOffset += v.size();
}

void Datagram::AddBlob(const uint8_t *data, const size_t &length) {
  AddUint16(length);
  EnsureLength(length);
  memcpy(_buf + _bufOffset, data, length);
  _bufOffset += length;
}

/**
 * Adds bytes directly to the end of this datagram.
 * @param v
 */
void Datagram::AddData(const std::vector<uint8_t> &v) {
  if (!v.empty()) {
    EnsureLength(v.size());
    memcpy(_buf + _bufOffset, &v[0], v.size());
    _bufOffset += v.size();
  }
}

/**
 * Adds another datagrams data directly to the end of this datagram.
 * @param v
 */
void Datagram::AddData(const std::shared_ptr<Datagram> &v) {
  if (v->Size()) {
    EnsureLength(v->Size());
    memcpy(_buf + _bufOffset, v->GetData(), v->Size());
    _bufOffset += v->Size();
  }
}

/**
 * Adds raw binary data directly to the end of this datagram.
 * @param data
 * @param length
 */
void Datagram::AddData(const uint8_t *data, const uint32_t &length) {
  EnsureLength(length);
  memcpy(_buf + _bufOffset, data, length);
  _bufOffset += length;
}

/**
 * Adds a location pair to this datagram.
 * @param parentId
 * @param zoneId
 */
void Datagram::AddLocation(const uint32_t &parentId, const uint32_t &zoneId) {
  AddUint32(parentId);
  AddUint32(zoneId);
}

void Datagram::EnsureLength(const size_t &length) {
  // Make sure we don't overflow.
  size_t newOffset = _bufOffset + length;
  if (newOffset > kMaxDgSize) {
    throw DatagramOverflow(std::format("Datagram exceeded max size! {} => {}",
                                       _bufOffset, newOffset));
  }

  // Do we need to resize the buffer?
  if (newOffset > _bufLength) {
    const size_t newLength = _bufLength + kMinDgSize + length;

    // Copy our old buffer into a new one.
    auto *tempBuf = new uint8_t[newLength];
    memcpy(tempBuf, _buf, _bufLength);

    // Clear out the old buffer and assign the new one.
    delete[] _buf;
    _buf = tempBuf;
    _bufLength = newLength;
  }
}

} // namespace Ardos