#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "nic/doorbell.h"
#include "nic/trace.h"

namespace nic {

struct CompletionEntry {
  std::uint16_t queue_id{0};
  std::uint16_t descriptor_index{0};
  std::uint32_t status{0};
  bool checksum_offloaded{false};
  bool checksum_verified{false};
  bool tso_performed{false};
  bool gso_performed{false};
  bool vlan_inserted{false};
  bool vlan_stripped{false};
  bool gro_aggregated{false};
  std::uint16_t segments_produced{1};
  std::uint16_t vlan_tag{0};
};

struct CompletionQueueConfig {
  std::size_t ring_size{0};
  std::uint16_t queue_id{0};
};

/// In-model completion queue with doorbell notification.
class CompletionQueue {
public:
  CompletionQueue(CompletionQueueConfig config, Doorbell* doorbell = nullptr);

  [[nodiscard]] bool is_full() const noexcept;
  [[nodiscard]] bool is_empty() const noexcept;
  [[nodiscard]] std::size_t available() const noexcept;
  [[nodiscard]] std::size_t space() const noexcept;

  bool post_completion(const CompletionEntry& entry);
  std::optional<CompletionEntry> poll_completion();

  void reset() noexcept;

private:
  CompletionQueueConfig config_{};
  Doorbell* doorbell_{nullptr};
  std::vector<CompletionEntry> entries_;
  std::uint32_t producer_index_{0};
  std::uint32_t consumer_index_{0};
  std::uint32_t count_{0};
};

}  // namespace nic
