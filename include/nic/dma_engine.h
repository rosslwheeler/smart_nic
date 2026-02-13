#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "nic/dma_types.h"
#include "nic/host_memory.h"
#include "nic/sgl.h"

namespace nic {

struct DmaCounters {
  std::size_t read_ops{0};
  std::size_t write_ops{0};
  std::size_t burst_read_ops{0};
  std::size_t burst_write_ops{0};
  std::size_t bytes_read{0};
  std::size_t bytes_written{0};
  std::size_t errors{0};
};

/// DMA engine backed by a HostMemory implementation.
class DMAEngine {
public:
  explicit DMAEngine(HostMemory& memory);

  [[nodiscard]] HostMemory& host_memory() noexcept { return memory_; }
  [[nodiscard]] const HostMemory& host_memory() const noexcept { return memory_; }

  [[nodiscard]] DmaResult read(HostAddress address, std::span<std::byte> buffer);
  [[nodiscard]] DmaResult write(HostAddress address, std::span<const std::byte> data);

  [[nodiscard]] DmaResult read_burst(HostAddress address,
                                     std::span<std::byte> buffer,
                                     std::size_t beat_bytes,
                                     std::size_t stride_bytes);

  [[nodiscard]] DmaResult write_burst(HostAddress address,
                                      std::span<const std::byte> data,
                                      std::size_t beat_bytes,
                                      std::size_t stride_bytes);

  [[nodiscard]] DmaResult transfer_sgl(const SglView& sgl,
                                       DmaDirection direction,
                                       std::span<std::byte> buffer);

  [[nodiscard]] const DmaCounters& counters() const noexcept { return counters_; }
  void reset_counters() noexcept { counters_ = DmaCounters{}; }

private:
  HostMemory& memory_;
  DmaCounters counters_;

  DmaResult map_result(const HostMemoryResult& host_result,
                       DmaDirection direction,
                       std::size_t requested_bytes,
                       const char* context);
};

}  // namespace nic
