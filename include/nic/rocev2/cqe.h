#pragma once

/// @file cqe.h
/// @brief Completion Queue Entry for RoCEv2.

#include <cstdint>

#include "nic/rocev2/types.h"

namespace nic::rocev2 {

/// Completion Queue Entry - result of a completed WQE.
struct RdmaCqe {
  std::uint64_t wr_id{0};            // Work request ID from original WQE
  WqeStatus status{};                // Completion status
  WqeOpcode opcode{};                // Operation type
  std::uint32_t qp_number{0};        // Queue pair that generated this CQE
  std::uint32_t bytes_completed{0};  // Number of bytes transferred
  std::uint32_t immediate_data{0};   // Immediate data (if present)
  bool has_immediate{false};         // True if immediate data is valid
  bool is_send{true};                // True if send CQE, false if recv CQE
};

}  // namespace nic::rocev2
