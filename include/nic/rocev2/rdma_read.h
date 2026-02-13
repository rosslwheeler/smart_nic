#pragma once

/// @file rdma_read.h
/// @brief RDMA READ operation processing for RoCEv2.

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

/// Result of processing an incoming READ request packet.
struct ReadRequestResult {
  bool success{false};
  bool needs_nak{false};
  std::uint32_t nak_psn{0};
  AethSyndrome syndrome{AethSyndrome::Ack};
  std::vector<std::vector<std::byte>> response_packets;  // READ_RESPONSE packets to send
};

/// Result of processing an incoming READ response packet.
struct ReadResponseResult {
  bool success{false};
  bool is_read_complete{false};  // True when all response packets received
  std::optional<RdmaCqe> cqe;    // CQE when read completes
};

/// Requester state for outstanding READ operations.
struct ReadRequestState {
  std::uint64_t wr_id{0};           // Work request ID for CQE
  std::uint64_t local_address{0};   // Local buffer address
  std::uint32_t local_lkey{0};      // Local key for write
  std::uint32_t total_length{0};    // Total expected response length
  std::uint32_t bytes_received{0};  // Bytes received so far
  std::uint32_t expected_psn{0};    // Expected next response PSN
  std::uint32_t start_psn{0};       // PSN of original request
  std::vector<SglEntry> sgl;        // Scatter-gather list for local buffer
  std::size_t current_sge_idx{0};   // Current SGE index
  std::size_t sge_offset{0};        // Offset within current SGE
  bool in_progress{false};          // True if waiting for responses
};

/// Responder state for incoming READ requests.
struct ReadResponderState {
  std::uint64_t remote_address{0};  // Remote address to read from
  std::uint32_t rkey{0};            // Remote key
  std::uint32_t total_length{0};    // Total bytes to send
  std::uint32_t bytes_sent{0};      // Bytes already sent
  std::uint32_t request_psn{0};     // PSN from original request
  std::uint32_t response_psn{0};    // Current response PSN
  bool in_progress{false};          // True if multi-packet response ongoing
};

/// Statistics for RDMA READ operations.
struct ReadStats {
  std::uint64_t reads_started{0};
  std::uint64_t reads_completed{0};
  std::uint64_t read_requests_generated{0};
  std::uint64_t read_responses_generated{0};
  std::uint64_t read_responses_processed{0};
  std::uint64_t bytes_read{0};
  std::uint64_t rkey_errors{0};
  std::uint64_t sequence_errors{0};
  std::uint64_t access_errors{0};
};

/// RDMA READ processor - handles READ request/response operations.
class ReadProcessor {
public:
  ReadProcessor(HostMemory& host_memory, MemoryRegionTable& mr_table);

  /// Generate READ_REQUEST packet for an RDMA READ operation.
  /// @param qp The queue pair for sending.
  /// @param wqe The send WQE (must be RdmaRead opcode).
  /// @return Vector containing single READ_REQUEST packet.
  [[nodiscard]] std::vector<std::vector<std::byte>> generate_read_request(RdmaQueuePair& qp,
                                                                          const SendWqe& wqe);

  /// Process an incoming READ_REQUEST packet (responder side).
  /// @param qp The destination queue pair.
  /// @param parser Parsed packet (BTH and RETH extracted).
  /// @return Result containing response packets or NAK.
  [[nodiscard]] ReadRequestResult process_read_request(RdmaQueuePair& qp,
                                                       const RdmaPacketParser& parser);

  /// Process an incoming READ_RESPONSE packet (requester side).
  /// @param qp The queue pair that originated the request.
  /// @param parser Parsed packet (BTH, AETH, and payload extracted).
  /// @return Result indicating success and CQE when complete.
  [[nodiscard]] ReadResponseResult process_read_response(RdmaQueuePair& qp,
                                                         const RdmaPacketParser& parser);

  /// Get statistics.
  [[nodiscard]] const ReadStats& stats() const noexcept { return stats_; }

  /// Reset processor state.
  void reset();

  /// Clear read state for a QP (e.g., on QP reset).
  void clear_read_state(std::uint32_t qp_number);

private:
  HostMemory& host_memory_;
  MemoryRegionTable& mr_table_;
  ReadStats stats_;

  // Per-QP requester state (waiting for responses)
  std::unordered_map<std::uint32_t, ReadRequestState> request_states_;

  // Per-QP responder state (generating responses)
  std::unordered_map<std::uint32_t, ReadResponderState> responder_states_;

  /// Read data from remote memory for response generation.
  [[nodiscard]] std::vector<std::byte> read_from_remote(std::uint64_t address,
                                                        std::uint32_t rkey,
                                                        std::uint32_t pd_handle,
                                                        std::size_t length);

  /// Write incoming response data to local SGL.
  std::size_t write_to_sgl(const std::vector<SglEntry>& sgl,
                           std::span<const std::byte> data,
                           std::size_t& sge_idx,
                           std::size_t& sge_offset,
                           std::uint32_t lkey);

  /// Generate READ_RESPONSE packets for a request.
  [[nodiscard]] std::vector<std::vector<std::byte>> generate_read_responses(
      RdmaQueuePair& qp,
      std::uint64_t address,
      std::uint32_t rkey,
      std::uint32_t length,
      std::uint32_t request_psn);

  /// Determine the appropriate READ_RESPONSE opcode for a packet.
  [[nodiscard]] RdmaOpcode get_read_response_opcode(bool is_first, bool is_last) const;
};

}  // namespace nic::rocev2
