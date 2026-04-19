#include "datagram_iterator.h"

#include <spdlog/spdlog.h>

namespace Ardos {

DatagramIterator::DatagramIterator(const std::shared_ptr<Datagram>& dg,
                                   const size_t& offset)
    : _dg(dg), _offset(offset) {}

/**
 * Reads a boolean from the datagram.
 * @return
 */
bool DatagramIterator::GetBool() { return GetUint8(); }

/**
 * Reads a signed 8-bit integer from the datagram.
 * @return
 */
int8_t DatagramIterator::GetInt8() {
  EnsureLength(1);
  int8_t v;
  std::memcpy(&v, _dg->GetData() + _offset, sizeof(v));
  _offset += 1;
  return v;
}

/**
 * Reads an unsigned 8-bit integer from the datagram.
 * @return
 */
uint8_t DatagramIterator::GetUint8() {
  EnsureLength(1);
  uint8_t v;
  std::memcpy(&v, _dg->GetData() + _offset, sizeof(v));
  _offset += 1;
  return v;
}

/**
 * Reads a signed 16-bit integer from the datagram.
 * @return
 */
int16_t DatagramIterator::GetInt16() {
  EnsureLength(2);
  int16_t v;
  std::memcpy(&v, _dg->GetData() + _offset, sizeof(v));
  _offset += 2;
  return v;
}

/**
 * Reads an unsigned 16-bit integer from the datagram.
 * @return
 */
uint16_t DatagramIterator::GetUint16() {
  EnsureLength(2);
  uint16_t v;
  std::memcpy(&v, _dg->GetData() + _offset, sizeof(v));
  _offset += 2;
  return v;
}

/**
 * Reads a signed 32-bit integer from the datagram.
 * @return
 */
int32_t DatagramIterator::GetInt32() {
  EnsureLength(4);
  int32_t v;
  std::memcpy(&v, _dg->GetData() + _offset, sizeof(v));
  _offset += 4;
  return v;
}

/**
 * Reads an unsigned 32-bit integer from the datagram.
 * @return
 */
uint32_t DatagramIterator::GetUint32() {
  EnsureLength(4);
  uint32_t v;
  std::memcpy(&v, _dg->GetData() + _offset, sizeof(v));
  _offset += 4;
  return v;
}

/**
 * Reads a signed 64-bit integer from the datagram.
 * @return
 */
int64_t DatagramIterator::GetInt64() {
  EnsureLength(8);
  int64_t v;
  std::memcpy(&v, _dg->GetData() + _offset, sizeof(v));
  _offset += 8;
  return v;
}

/**
 * Reads an unsigned 64-bit integer from the datagram.
 * @return
 */
uint64_t DatagramIterator::GetUint64() {
  EnsureLength(8);
  uint64_t v;
  std::memcpy(&v, _dg->GetData() + _offset, sizeof(v));
  _offset += 8;
  return v;
}

/**
 * Reads a 32-bit floating point number from the datagram.
 * @return
 */
float DatagramIterator::GetFloat32() {
  EnsureLength(4);
  float v;
  std::memcpy(&v, _dg->GetData() + _offset, sizeof(v));
  _offset += 4;
  return v;
}

/**
 * Reads a 64-bit floating point number from the datagram.
 * @return
 */
double DatagramIterator::GetFloat64() {
  EnsureLength(8);
  double v;
  std::memcpy(&v, _dg->GetData() + _offset, sizeof(v));
  _offset += 8;
  return v;
}

/**
 * Reads a string from the datagram.
 * @return
 */
std::string DatagramIterator::GetString() {
  const uint16_t length = GetUint16();
  EnsureLength(length);
  std::string str;
  str.resize(length);
  if (length != 0) {
    std::memcpy(str.data(), _dg->GetData() + _offset, length);
  }
  _offset += length;
  return str;
}

/**
 * Reads a blob of data from the datagram.
 * @return
 */
std::vector<uint8_t> DatagramIterator::GetBlob() {
  const uint16_t length = GetUint16();
  EnsureLength(length);
  std::vector<uint8_t> blob(length);
  if (length != 0) {
    std::memcpy(blob.data(), _dg->GetData() + _offset, length);
  }
  _offset += length;
  return blob;
}

/**
 * Reads a size-specified blob of data from the datagram.
 * @param size
 * @return
 */
std::vector<uint8_t> DatagramIterator::GetData(const size_t& size) {
  EnsureLength(size);
  std::vector data(_dg->GetData() + _offset, _dg->GetData() + _offset + size);
  _offset += size;
  return data;
}

/**
 * Reads a blob of binary data from the datagram and returns a new datagram.
 * @return
 */
std::shared_ptr<Datagram> DatagramIterator::GetDatagram() {
  uint16_t length = GetUint16();
  auto dg = std::make_shared<Datagram>(_dg->GetData() + _offset, length);
  _offset += length;
  return dg;
}

/**
 * Returns the underlying datagram being iterated.
 * @return
 */
std::shared_ptr<Datagram> DatagramIterator::GetUnderlyingDatagram() {
  return _dg;
}

/**
 * Reads the packed field from this datagram into the supplied buffer.
 * @param field
 * @param buffer
 */
void DatagramIterator::UnpackField(const DCPackerInterface* field,
                                   std::vector<uint8_t>& buffer) {
  // If the field has a fixed size in bytes (int, uint, float, etc.)
  // we can unpack data directly using that size.
  if (field->has_fixed_byte_size()) {
    std::vector<uint8_t> data = GetData(field->get_fixed_byte_size());
    buffer.insert(buffer.end(), data.begin(), data.end());
    return;
  }

  // Otherwise, if the field has a variable size (string, blob, etc.)
  // read the length tag and unpack.
  size_t length = field->get_num_length_bytes();
  if (length) {
    switch (length) {
      case 2: {
        uint16_t lengthTag = GetUint16();
        buffer.insert(buffer.end(), reinterpret_cast<uint8_t*>(&lengthTag),
                      reinterpret_cast<uint8_t*>(&lengthTag) + 2);
        length = lengthTag;
        break;
      }
      case 4: {
        uint32_t lengthTag = GetUint32();
        buffer.insert(buffer.end(), reinterpret_cast<uint8_t*>(&lengthTag),
                      reinterpret_cast<uint8_t*>(&lengthTag) + 4);
        length = lengthTag;
        break;
      }
      default:
        spdlog::error("[DGI] Unhandled field unpack for variable length: {}",
                      length);
        throw DatagramIteratorEOF(std::format(
            "DatagramIterator tried to unpack field past Datagram length! "
            "Field: {}, Length: {}, Size: {}",
            field->get_name(), length, _dg->Size()));
    }

    // Unpack field data into the buffer.
    std::vector<uint8_t> data = GetData(length);
    buffer.insert(buffer.end(), data.begin(), data.end());
    return;
  }

  // Otherwise, if the field is non-atomic, process each nested field.
  const int numNested = field->get_num_nested_fields();
  for (int i = 0; i < numNested; ++i) {
    UnpackField(field->get_nested_field(i), buffer);
  }
}

/**
 * Returns the current read offset in bytes.
 * @return
 */
uint16_t DatagramIterator::Tell() const { return _offset; }

/**
 * Increases the read offset by the number of bytes.
 * @param bytes
 */
void DatagramIterator::Skip(const size_t& bytes) {
  EnsureLength(bytes);
  _offset += bytes;
}

/**
 * Sets the current read offset (in bytes).
 * @param offset
 */
void DatagramIterator::Seek(const size_t& offset) {
  if (offset > _dg->Size()) {
    throw DatagramIteratorEOF(
        std::format("DatagramIterator seek out of range! Offset: {}, Size: {}",
                    offset, _dg->Size()));
  }
  _offset = offset;
}

/**
 * Seeks to the beginning of this datagrams payload (sender).
 */
void DatagramIterator::SeekPayload() {
  _offset = 0;

  const uint8_t channels = GetUint8();
  for (int i = 0; i < channels; ++i) {
    GetUint64();
  }
}

/**
 * Skips reading past a packed field.
 * @param field
 */
void DatagramIterator::SkipField(const DCPackerInterface* field) {
  // If the field has a fixed size in bytes (int, uint, float, etc.)
  // we can use that as our offset.
  if (field->has_fixed_byte_size()) {
    const size_t length = field->get_fixed_byte_size();
    EnsureLength(length);
    _offset += length;
    return;
  }

  // Otherwise, if the field has a variable size (string, blob, etc.)
  // read the length tag and skip.
  size_t length = field->get_num_length_bytes();
  if (length) {
    switch (length) {
      case 2: {
        length = GetUint16();
        break;
      }
      case 4: {
        length = GetUint32();
        break;
      }
      default:
        spdlog::error("[DGI] Unhandled field unpack for variable length: {}",
                      length);
    }

    // Skip the field data.
    EnsureLength(length);
    _offset += length;
    return;
  }

  // Otherwise, if the field is non-atomic, skip each nested field.
  const int numNested = field->get_num_nested_fields();
  for (int i = 0; i < numNested; ++i) {
    SkipField(field->get_nested_field(i));
  }
}

/**
 * Returns the remaining read size in bytes.
 * @return
 */
size_t DatagramIterator::GetRemainingSize() const {
  return _dg->Size() - _offset;
}

/**
 * Returns the remaining bytes to be read.
 * @return
 */
std::vector<uint8_t> DatagramIterator::GetRemainingBytes() {
  const size_t length = GetRemainingSize();
  std::vector data(_dg->GetData() + _offset, _dg->GetData() + _offset + length);
  _offset += length;
  return data;
}

void DatagramIterator::EnsureLength(const size_t& length) const {
  // Make sure we don't overflow reading.
  if (_offset > _dg->Size() || length > (_dg->Size() - _offset)) {
    throw DatagramIteratorEOF(
        std::format("DatagramIterator tried to read past Datagram length! "
                    "Offset: {}, Size: {}",
                    _offset + length, _dg->Size()));
  }
}

}  // namespace Ardos