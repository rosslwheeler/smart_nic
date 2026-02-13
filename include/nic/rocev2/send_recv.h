#pragma once

/// @file send_recv.h
/// @brief SEND/RECV operation processing for RoCEv2.

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "nic/host_memory.h"
#include "nic/rocev2/completion_queue.h"
#include "nic/rocev2/memory_region.h"
#include "nic/rocev2/packet.h"
#include "nic/rocev2/queue_pair.h"
#include "nic/rocev2/types.h"
#include "nic/rocev2/wqe.h"
#include "nic/trace.h"

namespace nic::rocev2 {

/// Result of processing an incoming packet.
struct RecvResult {
  bool success{false};
  bool needs_ack{false};
  bool is_message_complete{false};
  std::uint32_t ack_psn{0};
  AethSyndrome syndrome{AethSyndrome::Ack};
  std::optional<RdmaCqe> cqe;
};

/// Receiver state for multi-packet messages.
struct RecvMessageState {
  std::uint64_t wr_id{0};           // Work request ID from recv WQE
  std::uint32_t bytes_received{0};  // Bytes received so far
  std::uint32_t expected_psn{0};    // Next expected PSN
  std::vector<SglEntry> sgl;        // Scatter-gather list from recv WQE
  std::size_t current_sge_idx{0};   // Current SGE being filled
  std::size_t sge_offset{0};        // Offset within current SGE
  bool in_progress{false};          // True if message is being received
  std::uint32_t immediate_data{0};  // Immediate data (from last packet)
  bool has_immediate{false};        // True if immediate data present
};

/// Statistics for SEND/RECV operations.
struct SendRecvStats {
  std::uint64_t sends_started{0};
  std::uint64_t sends_completed{0};
  std::uint64_t recvs_completed{0};
  std::uint64_t send_packets_generated{0};
  std::uint64_t recv_packets_processed{0};
  std::uint64_t rnr_naks_sent{0};
  std::uint64_t sequence_errors{0};
  std::uint64_t bytes_sent{0};
  std::uint64_t bytes_received{0};
};

/// SEND/RECV processor - handles SEND and RECV operations.
class SendRecvProcessor {
public:
  SendRecvProcessor(HostMemory& host_memory, MemoryRegionTable& mr_table);

  /// Generate packets for a SEND operation.
  /// @param qp The queue pair for sending.
  /// @param wqe The send WQE (must be Send or SendImm opcode).
  /// @return Vector of packets to send.
  [[nodiscard]] std::vector<std::vector<std::byte>> generate_send_packets(RdmaQueuePair& qp,
                                                                          const SendWqe& wqe);

  /// Process an incoming SEND packet.
  /// @param qp The destination queue pair.
  /// @param parser Parsed packet (BTH and payload extracted).
  /// @return Result indicating success, ACK needs, and any CQE.
  [[nodiscard]] RecvResult process_recv_packet(RdmaQueuePair& qp, const RdmaPacketParser& parser);

  /// Generate an ACK packet.
  /// @param qp The queue pair generating the ACK.
  /// @param psn The PSN being acknowledged.
  /// @param syndrome The AETH syndrome (Ack or NAK type).
  /// @param msn Message sequence number.
  /// @return The ACK packet.
  [[nodiscard]] std::vector<std::byte> generate_ack(const RdmaQueuePair& qp,
                                                    std::uint32_t psn,
                                                    AethSyndrome syndrome,
                                                    std::uint32_t msn);

  /// Get statistics.
  [[nodiscard]] const SendRecvStats& stats() const noexcept { return stats_; }

  /// Reset processor state.
  void reset();

  /// Clear receiver state for a QP (e.g., on QP reset).
  void clear_recv_state(std::uint32_t qp_number);

private:
  HostMemory& host_memory_;
  MemoryRegionTable& mr_table_;
  SendRecvStats stats_;

  // Per-QP receiver state (for multi-packet messages)
  std::unordered_map<std::uint32_t, RecvMessageState> recv_states_;

  /// Read data from host memory using scatter-gather list.
  /// @param sgl The scatter-gather list.
  /// @param lkey The local key for validation.
  /// @param pd_handle The protection domain.
  /// @return Data read from memory, or empty vector on error.
  [[nodiscard]] std::vector<std::byte> read_from_sgl(const std::vector<SglEntry>& sgl,
                                                     std::uint32_t lkey,
                                                     std::uint32_t pd_handle);

  /// Write data to host memory using scatter-gather list.
  /// @param sgl The scatter-gather list.
  /// @param data The data to write.
  /// @param sge_idx Starting SGE index.
  /// @param sge_offset Starting offset within SGE.
  /// @return Number of bytes written, 0 on error.
  std::size_t write_to_sgl(const std::vector<SglEntry>& sgl,
                           std::span<const std::byte> data,
                           std::size_t& sge_idx,
                           std::size_t& sge_offset);

  /// Calculate number of packets needed for a message.
  [[nodiscard]] std::uint32_t calculate_packet_count(std::uint32_t total_length,
                                                     std::uint32_t mtu) const;

  /// Determine the appropriate SEND opcode for a packet.
  [[nodiscard]] RdmaOpcode get_send_opcode(bool is_first, bool is_last, bool has_immediate) const;
};

}  // namespace nic::rocev2
