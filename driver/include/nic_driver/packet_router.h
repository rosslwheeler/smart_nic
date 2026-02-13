#pragma once

/// @file packet_router.h
/// @brief Routes RoCEv2 packets between NicDriver instances for testing.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "nic/rocev2/engine.h"

namespace nic_driver {

class NicDriver;

/// Routes RoCEv2 packets between NicDriver instances by IP address.
/// Used for testing two-driver loopback scenarios.
class PacketRouter {
public:
  using IpAddress = std::array<std::uint8_t, 4>;

  PacketRouter() = default;
  ~PacketRouter() = default;

  // Disable copy/move
  PacketRouter(const PacketRouter&) = delete;
  PacketRouter& operator=(const PacketRouter&) = delete;
  PacketRouter(PacketRouter&&) = delete;
  PacketRouter& operator=(PacketRouter&&) = delete;

  /// Register a driver at a given IP address.
  /// @param ip The IP address to associate with the driver.
  /// @param driver Pointer to the NicDriver instance.
  void register_driver(IpAddress ip, NicDriver* driver);

  /// Unregister a driver by IP address.
  /// @param ip The IP address to unregister.
  void unregister_driver(IpAddress ip);

  /// Route a single outgoing packet to its destination driver.
  /// @param packet The packet to route.
  /// @param src_ip Source IP address of the sender.
  /// @return True if the packet was delivered, false if destination not found.
  bool route_packet(const nic::rocev2::OutgoingPacket& packet, IpAddress src_ip);

  /// Process all pending outgoing packets from all registered drivers.
  /// Generates packets from each driver and routes them to destinations.
  /// @return Number of packets routed.
  std::size_t process_all();

  /// Get the number of registered drivers.
  [[nodiscard]] std::size_t driver_count() const noexcept { return drivers_.size(); }

private:
  struct DriverEntry {
    IpAddress ip{};
    NicDriver* driver{nullptr};
  };

  std::vector<DriverEntry> drivers_;

  /// Find a driver by IP address.
  /// @param ip The IP address to search for.
  /// @return Pointer to the driver, or nullptr if not found.
  NicDriver* find_driver(IpAddress ip);
};

}  // namespace nic_driver
