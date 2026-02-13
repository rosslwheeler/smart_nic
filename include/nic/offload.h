#pragma once

#include <cstddef>
#include <cstdint>

namespace nic {

/// MTU and frame size constants
constexpr std::size_t kMinEthernetFrame = 64;
constexpr std::size_t kStandardMtu = 1500;
constexpr std::size_t kJumboMtu = 9000;
constexpr std::size_t kMaxJumboFrame = 9216;  ///< Including headers

/// Offload limits
constexpr std::size_t kMaxTsoSegments = 64;
constexpr std::size_t kMinMss = 1;  ///< Minimal value for testing; real-world minimum is 536
constexpr std::size_t kMaxMss = 9000;

/// VLAN constants
constexpr std::size_t kVlanHeaderSize = 4;
constexpr std::uint16_t kVlanEthertype = 0x8100;
constexpr std::uint16_t kQinQEthertype = 0x88A8;  ///< 802.1ad (QinQ)

/// Offload error codes
enum class OffloadError : std::uint8_t {
  None = 0,
  MtuExceeded,
  InvalidMss,
  InvalidHeaderLength,
  TooManySegments,
  VlanError,
  GroTimeout,
  GroFlowMismatch,
};

/// GRO/LRO state tracking
enum class GroState : std::uint8_t {
  Idle = 0,
  Aggregating,
  TimedOut,
  FlowChanged,
};

}  // namespace nic