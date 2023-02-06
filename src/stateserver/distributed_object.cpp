#include "distributed_object.h"

#include <format>

#include "../util/logger.h"

namespace Ardos {

DistributedObject::DistributedObject(StateServer *stateServer,
                                     const uint32_t &doId,
                                     const uint32_t &parentId,
                                     const uint32_t &zoneId, DCClass *dclass,
                                     DatagramIterator &dgi, const bool &other)
    : _stateServer(stateServer), _doId(doId), _parentId(parentId),
      _zoneId(zoneId), _dclass(dclass) {
  // Unpack required fields.
  for (int i = 0; i < _dclass->get_num_inherited_fields(); ++i) {
    auto field = _dclass->get_inherited_field(i);
    if (field->is_required() && !field->as_molecular_field()) {
      dgi.UnpackField(field, _required_fields[field]);
    }
  }

  // Unpack extra fields if supplied.
  if (other) {
    uint16_t count = dgi.GetUint16();
    for (int i = 0; i < count; ++i) {
      uint16_t fieldId = dgi.GetUint16();
      auto field = _dclass->get_field_by_index(fieldId);
      if (!field) {
        Logger::Error(std::format(
            "[SS] Received generated with unknown field id: {} for DoId: {}",
            fieldId, _doId));
        break;
      }

      // We only handle 'RAM' fields, if they're not to be stored on the SS then
      // that's an error.
      if (field->is_ram()) {
        dgi.UnpackField(field, _ram_fields[field]);
      } else {
        Logger::Error(std::format(
            "[SS] Received generated with non RAM field: {} for DoId: ",
            field->get_name(), _doId));
      }
    }
  }

  Logger::Verbose(std::format("[SS] Object: '{}' generated with DoId: {}",
                              _dclass->get_name(), _doId));
}

} // namespace Ardos
