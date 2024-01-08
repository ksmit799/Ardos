#ifndef ARDOS_DISTRIBUTED_OBJECT_H
#define ARDOS_DISTRIBUTED_OBJECT_H

#include <dcClass.h>

#include "../net/message_types.h"
#include "../util/globals.h"
#include "state_server.h"

namespace Ardos {

class DistributedObject final : public ChannelSubscriber {
public:
  friend class LoadingObject;

  DistributedObject(StateServerImplementation *stateServer,
                    const uint32_t &doId, const uint32_t &parentId,
                    const uint32_t &zoneId, DCClass *dclass,
                    DatagramIterator &dgi, const bool &other);
  DistributedObject(StateServerImplementation *stateServer,
                    const uint64_t &sender, const uint32_t &doId,
                    const uint32_t &parentId, const uint32_t &zoneId,
                    DCClass *dclass, FieldMap &reqFields, FieldMap &ramFields);

  [[nodiscard]] size_t Size() const;

  [[nodiscard]] uint64_t GetAI() const;
  [[nodiscard]] bool IsAIExplicitlySet() const;

  [[nodiscard]] uint32_t GetDoId() const;

  [[nodiscard]] uint64_t GetLocation() const;
  [[nodiscard]] uint64_t GetOwner() const;

private:
  void Annihilate(const uint64_t &sender, const bool &notifyParent = true);
  void DeleteChildren(const uint64_t &sender);

  void HandleDatagram(const std::shared_ptr<Datagram> &dg) override;

  void HandleLocationChange(const uint32_t &newParent, const uint32_t &newZone,
                            const uint64_t &sender);
  void HandleAIChange(const uint64_t &newAI, const uint64_t &sender,
                      const bool &channelIsExplicit);

  void WakeChildren();

  void SendLocationEntry(const uint64_t &location);
  void SendAIEntry(const uint64_t &location);
  void SendOwnerEntry(const uint64_t &location);
  void SendInterestEntry(const uint64_t &location, const uint32_t &context);

  void AppendRequiredData(const std::shared_ptr<Datagram> &dg,
                          const bool &clientOnly = false,
                          const bool &alsoOwner = false);
  void AppendOtherData(const std::shared_ptr<Datagram> &dg,
                       const bool &clientOnly = false,
                       const bool &alsoOwner = false);

  void SaveField(DCField *field, const std::vector<uint8_t> &data);
  bool HandleOneUpdate(DatagramIterator &dgi, const uint64_t &sender);
  bool HandleOneGet(const std::shared_ptr<Datagram> &dg, uint16_t fieldId,
                    const bool &succeedIfUnset = false,
                    const bool &isSubfield = false);

  StateServerImplementation *_stateServer;
  uint32_t _doId;
  uint32_t _parentId;
  uint32_t _zoneId;
  DCClass *_dclass;

  FieldMap _requiredFields;
  FieldMap _ramFields;

  std::unordered_map<uint32_t, std::unordered_set<uint32_t>> _zoneObjects;

  uint64_t _aiChannel = INVALID_CHANNEL;
  uint64_t _ownerChannel = INVALID_CHANNEL;
  uint32_t _nextContext = 0;
  bool _aiExplicitlySet = false;
  bool _parentSynchronized = false;
};

} // namespace Ardos

#endif // ARDOS_DISTRIBUTED_OBJECT_H
