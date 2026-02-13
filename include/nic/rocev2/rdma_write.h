#pragma once

/// @file rdma_write.h
/// @brief RDMA WRITE operation processing for RoCEv2.

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "nic/host_memory.h"
#include "nic/rocev2/cqe.h"
#include "nic/rocev2/memory_region.h"
#include "nic/rocev2/packet.h"
#include "nic/rocev2/queue_pair.h"
#include "nic/rocev2/types.h"
#include "nic/rocev2/wqe.h"
#include "nic/trace.h"

namespace nic::rocev2 {

/// Result of processing an incoming WRITE packet.
struct WriteResult {
  bool success{false};
  bool needs_ack{false};
  bool is_message_complete{false};
  std::uint32_t ack_psn{0};
  AethSyndrome syndrome{AethSyndrome::Ack};
  std::optional<RdmaCqe> recv_cqe;  // Only for WRITE with immediate
};

/// Receiver state for multi-packet WRITE operations.
struct WriteMessageState {
  std::uint64_t remote_address{0};  // Remote virtual address being written
  std::uint32_t rkey{0};            // Remote key for validation
  std::uint32_t total_length{0};    // Total DMA length from RETH
  std::uint32_t bytes_written{0};   // Bytes written so far
  std::uint32_t expected_psn{0};    // Next expected PSN
  bool in_progress{false};          // True if multi-packet write in progress
  std::uint32_t immediate_data{0};  // Immediate data (from last packet)
  bool has_immediate{false};        // True if WRITE with immediate
};

/// Statistics for RDMA WRITE operations.
struct WriteStats {
  std::uint64_t writes_started{0};
  std::uint64_t writes_completed{0};
  std::uint64_t write_packets_generated{0};
  std::uint64_t write_packets_processed{0};
  std::uint64_t bytes_written{0};
  std::uint64_t rkey_errors{0};
  std::uint64_t sequence_errors{0};
  std::uint64_t access_errors{0};
};

/// RDMA WRITE processor - handles one-sided WRITE operations.
class WriteProcessor {
public:
  WriteProcessor(HostMemory& host_memory, MemoryRegionTable& mr_table);

  /// Generate packets for an RDMA WRITE operation.
  /// @param qp The queue pair for sending.
  /// @param wqe The send WQE (must be RdmaWrite or RdmaWriteImm opcode).
  /// @return Vector of packets to send.
  [[nodiscard]] std::vector<std::vector<std::byte>> generate_write_packets(RdmaQueuePair& qp,
                                                                           const SendWqe& wqe);

  /// Process an incoming WRITE packet.
  /// @param qp The destination queue pair.
  /// @param parser Parsed packet (BTH, RETH, and payload extracted).
  /// @return Result indicating success, ACK needs, and any CQE (for immediate).
  [[nodiscard]] WriteResult process_write_packet(RdmaQueuePair& qp, const RdmaPacketParser& parser);

  /// Get statistics.
  [[nodiscard]] const WriteStats& stats() const noexcept { return stats_; }

  /// Reset processor state.
  void reset();

  /// Clear write state for a QP (e.g., on QP reset).
  void clear_write_state(std::uint32_t qp_number);

private:
  HostMemory& host_memory_;
  MemoryRegionTable& mr_table_;
  WriteStats stats_;

  // Per-QP write state (for multi-packet writes)
  std::unordered_map<std::uint32_t, WriteMessageState> write_states_;

  /// Read data from host memory using scatter-gather list.
  [[nodiscard]] std::vector<std::byte> read_from_sgl(const std::vector<SglEntry>& sgl,
                                                     std::uint32_t lkey);

  /// Write data directly to remote memory address.
  /// @param address Remote virtual address.
  /// @param rkey Remote key for validation.
  /// @param pd_handle Protection domain.
  /// @param data Data to write.
  /// @return true if write succeeded.
  bool write_to_remote(std::uint64_t address,
                       std::uint32_t rkey,
                       std::uint32_t pd_handle,
                       std::span<const std::byte> data);

  /// Calculate number of packets needed for a message.
  [[nodiscard]] std::uint32_t calculate_packet_count(std::uint32_t total_length,
                                                     std::uint32_t mtu) const;

  /// Determine the appropriate WRITE opcode for a packet.
  [[nodiscard]] RdmaOpcode get_write_opcode(bool is_first, bool is_last, bool has_immediate) const;
};

}  // namespace nic::rocev2
