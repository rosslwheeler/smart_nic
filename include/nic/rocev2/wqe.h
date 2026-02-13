#pragma once

/// @file wqe.h
/// @brief Work Queue Element definitions for RoCEv2.

#include <cstdint>
#include <vector>

#include "nic/host_memory.h"
#include "nic/rocev2/types.h"
#include "nic/sgl.h"

namespace nic::rocev2 {

/// Send Work Queue Element.
struct SendWqe {
  std::uint64_t wr_id{0};             // Work request ID (user tag)
  WqeOpcode opcode{WqeOpcode::Send};  // SEND, WRITE, READ
  std::vector<SglEntry> sgl{};        // Scatter-gather list
  std::uint32_t total_length{0};      // Computed from SGL
  bool signaled{true};                // Generate CQE on completion
  bool solicited{false};              // Request solicited event
  bool fence{false};                  // Wait for prior ops
  bool inline_data{false};            // Data in WQE, not via SGL
  std::uint32_t immediate_data{0};    // For SendImm/WriteImm

  // RDMA WRITE/READ specific
  HostAddress remote_address{0};  // Remote virtual address
  std::uint32_t rkey{0};          // Remote key
  std::uint32_t local_lkey{0};    // Local key for SGL validation
};

/// Receive Work Queue Element.
struct RecvWqe {
  std::uint64_t wr_id{0};         // Work request ID
  std::vector<SglEntry> sgl{};    // Scatter-gather for receive buffers
  std::uint32_t total_length{0};  // Available buffer space
};

/// Compute total length from scatter-gather list.
[[nodiscard]] inline std::uint32_t compute_sgl_length(const std::vector<SglEntry>& sgl) noexcept {
  std::uint32_t total = 0;
  for (const auto& entry : sgl) {
    total += entry.length;
  }
  return total;
}

}  // namespace nic::rocev2
