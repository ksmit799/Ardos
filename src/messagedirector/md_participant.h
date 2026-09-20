#ifndef ARDOS_MD_PARTICIPANT_H
#define ARDOS_MD_PARTICIPANT_H

#include <map>
#include <memory>
#include <uvw.hpp>
#include <vector>

#include "../net/datagram.h"
#include "../net/network_client.h"
#include "channel_subscriber.h"

namespace Ardos {

class MDParticipant final : public NetworkClient, public ChannelSubscriber {
 public:
  explicit MDParticipant(const std::shared_ptr<uvw::tcp_handle>& socket);
  ~MDParticipant() override;

  [[nodiscard]] std::string GetName() const { return _connName; }
  [[nodiscard]] size_t GetPostRemovesCount() const {
    size_t count = 0;
    for (const auto& [sender, dgs] : _postRemoves) {
      count += dgs.size();
    }
    return count;
  }

 private:
  void Shutdown() override;
  void HandleDisconnect(uv_errno_t code) override;
  void HandleClientDatagram(const std::shared_ptr<Datagram>& dg) override;
  void HandleDatagram(const std::shared_ptr<Datagram>& dg) override;

  std::string _connName = "Unnamed Participant";
  // Keyed by sender channel, the same key the mesh replicates under, so
  // peers can drop exactly what we fire or clear here.
  std::map<uint64_t, std::vector<std::shared_ptr<Datagram>>> _postRemoves;
};

}  // namespace Ardos

#endif  // ARDOS_MD_PARTICIPANT_H
