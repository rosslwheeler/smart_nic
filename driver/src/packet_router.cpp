#include "nic_driver/packet_router.h"

#include <algorithm>

#include "nic/log.h"
#include "nic/trace.h"
#include "nic_driver/driver.h"

namespace nic_driver {

void PacketRouter::register_driver(IpAddress ip, NicDriver* driver) {
  NIC_TRACE_SCOPED(__func__);

  if (driver == nullptr) {
    return;
  }

  // Check if already registered
  for (auto& entry : drivers_) {
    if (entry.ip == ip) {
      entry.driver = driver;
      return;
    }
  }

  drivers_.push_back({ip, driver});
}

void PacketRouter::unregister_driver(IpAddress ip) {
  NIC_TRACE_SCOPED(__func__);

  auto iter = std::find_if(
      drivers_.begin(), drivers_.end(), [&ip](const DriverEntry& entry) { return entry.ip == ip; });

  if (iter != drivers_.end()) {
    drivers_.erase(iter);
  }
}

bool PacketRouter::route_packet(const nic::rocev2::OutgoingPacket& packet, IpAddress src_ip) {
  NIC_TRACE_SCOPED(__func__);

  NicDriver* dest_driver = find_driver(packet.dest_ip);
  if (dest_driver == nullptr) {
    NIC_LOGF_WARNING("route failed: no driver for IP {}.{}.{}.{}",
                     packet.dest_ip[0],
                     packet.dest_ip[1],
                     packet.dest_ip[2],
                     packet.dest_ip[3]);
    return false;
  }

  // Deliver the packet to the destination driver
  return dest_driver->rdma_process_packet(
      std::span<const std::byte>(packet.data.data(), packet.data.size()),
      src_ip,
      packet.dest_ip,
      packet.src_port);
}

std::size_t PacketRouter::process_all() {
  NIC_TRACE_SCOPED(__func__);

  std::size_t total_routed = 0;

  // Collect all outgoing packets from all drivers first
  // to avoid modifying the collection while iterating
  std::vector<std::pair<IpAddress, std::vector<nic::rocev2::OutgoingPacket>>> pending_packets;
  pending_packets.reserve(drivers_.size());

  for (const auto& entry : drivers_) {
    if ((entry.driver != nullptr) && entry.driver->rdma_enabled()) {
      auto packets = entry.driver->rdma_generate_packets();
      if (!packets.empty()) {
        pending_packets.emplace_back(entry.ip, std::move(packets));
      }
    }
  }

  // Route all collected packets
  for (const auto& [src_ip, packets] : pending_packets) {
    for (const auto& packet : packets) {
      if (route_packet(packet, src_ip)) {
        total_routed++;
      }
    }
  }

  return total_routed;
}

NicDriver* PacketRouter::find_driver(IpAddress ip) {
  NIC_TRACE_SCOPED(__func__);

  for (auto& entry : drivers_) {
    if (entry.ip == ip) {
      return entry.driver;
    }
  }
  return nullptr;
}

}  // namespace nic_driver
