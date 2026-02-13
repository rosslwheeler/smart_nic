#pragma once

#include <cstddef>
#include <cstdint>

#include "nic/host_memory.h"
#include "nic/offload.h"

namespace nic {

enum class ChecksumMode : std::uint8_t { None, Layer3, Layer4 };

enum class CompletionCode : std::uint16_t {
  Success = 0,
  BufferTooSmall = 1,
  ChecksumError = 2,
  NoDescriptor = 3,
  Fault = 4,
  MtuExceeded = 5,
  InvalidMss = 6,
  TooManySegments = 7,
};

struct TxDescriptor {
  HostAddress buffer_address{0};
  std::uint32_t length{0};
  ChecksumMode checksum{ChecksumMode::None};
  std::uint16_t descriptor_index{0};
  std::uint16_t checksum_value{0};
  bool checksum_offload{false};
  bool tso_enabled{false};
  bool gso_enabled{false};
  std::uint16_t mss{0};
  std::uint16_t header_length{0};  ///< Bytes of headers to keep intact for segmentation
  bool vlan_insert{false};
  std::uint16_t vlan_tag{0};
};

struct RxDescriptor {
  HostAddress buffer_address{0};
  std::uint32_t buffer_length{0};
  ChecksumMode checksum{ChecksumMode::None};
  std::uint16_t descriptor_index{0};
  bool checksum_offload{false};
  bool vlan_strip{false};
  bool vlan_present{false};
  std::uint16_t vlan_tag{0};
  bool gro_enabled{false};  ///< Placeholder for GRO/LRO behavior
};

struct TxCompletion {
  std::uint16_t queue_id{0};
  std::uint16_t descriptor_index{0};
  CompletionCode status{CompletionCode::Success};
  bool checksum_offloaded{false};
  bool tso_performed{false};
  bool gso_performed{false};
  bool vlan_inserted{false};
  std::uint16_t segments_produced{1};
  std::uint16_t vlan_tag{0};
};

struct RxCompletion {
  std::uint16_t queue_id{0};
  std::uint16_t descriptor_index{0};
  CompletionCode status{CompletionCode::Success};
  bool checksum_verified{false};
  bool vlan_stripped{false};
  bool gro_aggregated{false};
  std::uint16_t vlan_tag{0};
};

}  // namespace nic
