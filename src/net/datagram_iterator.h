#ifndef ARDOS_DATAGRAM_ITERATOR_H
#define ARDOS_DATAGRAM_ITERATOR_H

#include <memory>
#include <stdexcept>

#include <dcPackerInterface.h>

#include "datagram.h"

namespace Ardos {

/**
 * A DatagramIteratorEOF is an exception that is thrown when attempting to read
 * past the end of a datagram.
 */
class DatagramIteratorEOF : public std::runtime_error {
public:
  explicit DatagramIteratorEOF(const std::string &what)
      : std::runtime_error(what) {}
};

/**
 * A class to retrieve the individual data elements previously stored in a
 * Datagram.  Elements may be retrieved one at a time; it is up to the caller
 * to know the correct type and order of each element.
 *
 * Note that it is the responsibility of the caller to ensure that the datagram
 * object is not destructed while this DatagramIterator is in use.
 */
class DatagramIterator {
public:
  explicit DatagramIterator(const std::shared_ptr<Datagram> &dg,
                            size_t offset = 0);

  bool GetBool();
  int8_t GetInt8();
  uint8_t GetUint8();

  int16_t GetInt16();
  uint16_t GetUint16();

  int32_t GetInt32();
  uint32_t GetUint32();

  int64_t GetInt64();
  uint64_t GetUint64();

  float GetFloat32();
  double GetFloat64();

  std::string GetString();
  std::vector<uint8_t> GetBlob();
  std::vector<uint8_t> GetData(const size_t &size);

  void UnpackField(DCPackerInterface *field, std::vector<uint8_t> &buffer);

  void Seek(const size_t &offset);
  void SeekPayload();

  size_t GetRemainingSize();
  std::vector<uint8_t> GetRemainingBytes();

private:
  void EnsureLength(const size_t &length);

  std::shared_ptr<Datagram> _dg;
  size_t _offset;
};

} // namespace Ardos

#endif // ARDOS_DATAGRAM_ITERATOR_H
