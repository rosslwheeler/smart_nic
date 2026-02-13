#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "nic/host_memory.h"

namespace nic {

struct SglEntry {
  HostAddress address{0};
  std::size_t length{0};
};

/// Lightweight view over scatter-gather entries.
class SglView {
public:
  SglView() = default;
  explicit SglView(std::span<const SglEntry> entries) : entries_(entries) {}
  explicit SglView(const std::vector<SglEntry>& entries) : entries_(entries) {}

  [[nodiscard]] std::span<const SglEntry> entries() const noexcept { return entries_; }

  [[nodiscard]] std::size_t total_length() const noexcept {
    std::size_t total = 0;
    for (const auto& entry : entries_) {
      total += entry.length;
    }
    return total;
  }

  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

private:
  std::span<const SglEntry> entries_{};
};

}  // namespace nic
