#ifndef ARDOS_DATAGRAM_H
#define ARDOS_DATAGRAM_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace Ardos {

// Max amount of data we can have is an uint16 (65k bytes)
// -2 for the required prepended length tag.
const size_t kMaxDgSize = 0xffff - 2;
// 128 bytes seems like a good minimum datagram size.
const size_t kMinDgSize = 0x80;

/**
 * A DatagramOverflow is an exception which occurs when an Add<value> method is
 * called which would increase the size of the datagram past kMaxDgSize
 * (preventing integer and buffer overflow).
 */
class DatagramOverflow : public std::runtime_error {
public:
  explicit DatagramOverflow(const std::string &what)
      : std::runtime_error(what) {}
};

/**
 * An ordered list of data elements, formatted in memory for transmission over
 * a socket or writing to a data file.
 *
 * Data elements should be added one at a time, in order, to the Datagram.
 * The nature and contents of the data elements are totally up to the user.
 * When a Datagram has been transmitted and received, its data elements may be
 * extracted using a DatagramIterator; it is up to the caller to know the
 * correct type of each data element in order.
 *
 * A Datagram is itself headerless; it is simply a collection of data
 * elements.
 */
class Datagram {
public:
  Datagram();
  Datagram(const uint8_t *data, size_t size);
  Datagram(const uint64_t &toChannel, const uint64_t &fromChannel,
           const uint16_t &msgType);
  Datagram(const std::unordered_set<uint64_t> &toChannels,
           const uint64_t &fromChannel, const uint16_t &msgType);
  ~Datagram();

  [[nodiscard]] uint16_t Size() const;
  const uint8_t *GetData();

  void AddBool(const bool &v);
  void AddInt8(const int8_t &v);
  void AddUint8(const uint8_t &v);

  void AddInt16(const int16_t &v);
  void AddUint16(const uint16_t &v);

  void AddInt32(const int32_t &v);
  void AddUint32(const uint32_t &v);

  void AddInt64(const int64_t &v);
  void AddUint64(const uint64_t &v);

  void AddFloat32(const float &v);
  void AddFloat64(const double &v);

  void AddString(const std::string &v);
  void AddBlob(const std::vector<uint8_t> &v);

  void AddLocation(const uint32_t &parentId, const uint32_t &zoneId);

private:
  void EnsureLength(const size_t &length);

  uint8_t *_buf;
  size_t _bufOffset;
  size_t _bufLength;
};

} // namespace Ardos

#endif // ARDOS_DATAGRAM_H
