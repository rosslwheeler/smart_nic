#pragma once

/// @file engine.h
/// @brief Top-level RoCEv2 RDMA engine coordinator.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "nic/dma_engine.h"
#include "nic/host_memory.h"
#include "nic/rocev2/completion_queue.h"
#include "nic/rocev2/congestion.h"
#include "nic/rocev2/memory_region.h"
#include "nic/rocev2/packet.h"
#include "nic/rocev2/protection_domain.h"
#include "nic/rocev2/queue_pair.h"
#include "nic/rocev2/rdma_read.h"
#include "nic/rocev2/rdma_write.h"
#include "nic/rocev2/send_recv.h"
#include "nic/rocev2/types.h"
#include "nic/trace.h"

namespace nic::rocev2 {

/// Configuration for the RDMA engine.
struct RdmaEngineConfig {
  bool enabled{true};                      ///< Whether RDMA is enabled
  std::size_t max_pds{256};                ///< Maximum protection domains
  std::size_t max_mrs{4096};               ///< Maximum memory regions
  std::size_t max_qps{1024};               ///< Maximum queue pairs
  std::size_t max_cqs{512};                ///< Maximum completion queues
  std::size_t default_cq_depth{256};       ///< Default CQ depth
  std::uint32_t mtu{4096};                 ///< RDMA MTU
  DcqcnConfig dcqcn_config{};              ///< Congestion control config
  ReliabilityConfig reliability_config{};  ///< Reliability config
};

/// Statistics for the RDMA engine.
struct RdmaEngineStats {
  std::uint64_t packets_received{0};
  std::uint64_t packets_sent{0};
  std::uint64_t bytes_received{0};
  std::uint64_t bytes_sent{0};
  std::uint64_t send_wqes_posted{0};
  std::uint64_t recv_wqes_posted{0};
  std::uint64_t cqes_generated{0};
  std::uint64_t errors{0};
  std::uint64_t pds_created{0};
  std::uint64_t mrs_registered{0};
  std::uint64_t qps_created{0};
  std::uint64_t cqs_created{0};
};

/// Outgoing packet with metadata.
struct OutgoingPacket {
  std::vector<std::byte> data;
  std::array<std::uint8_t, 4> dest_ip{};
  std::uint16_t dest_port{kRoceUdpPort};
  std::uint16_t src_port{0};  // 0 = use flow-based hash
};

/// RoCEv2 RDMA Engine - top-level coordinator for RDMA operations.
class RdmaEngine {
public:
  RdmaEngine(RdmaEngineConfig config, DMAEngine& dma_engine, HostMemory& host_memory);

  // ============================================
  // Protection Domain Management
  // ============================================

  /// Allocate a new protection domain.
  /// @return PD handle, or nullopt if allocation failed.
  [[nodiscard]] std::optional<std::uint32_t> create_pd();

  /// Destroy a protection domain.
  /// @param pd_handle The PD handle to destroy.
  /// @return True if destroyed, false if PD not found or in use.
  bool destroy_pd(std::uint32_t pd_handle);

  // ============================================
  // Memory Region Management
  // ============================================

  /// Register a memory region.
  /// @param pd_handle Protection domain handle.
  /// @param virtual_address Virtual address of the region.
  /// @param length Length of the region in bytes.
  /// @param access Access flags for the region.
  /// @return lkey for the registered MR, or nullopt on failure.
  [[nodiscard]] std::optional<std::uint32_t> register_mr(std::uint32_t pd_handle,
                                                         std::uint64_t virtual_address,
                                                         std::size_t length,
                                                         AccessFlags access);

  /// Deregister a memory region.
  /// @param lkey The local key of the MR to deregister.
  /// @return True if deregistered, false if MR not found.
  bool deregister_mr(std::uint32_t lkey);

  // ============================================
  // Completion Queue Management
  // ============================================

  /// Create a completion queue.
  /// @param depth Number of entries in the CQ.
  /// @return CQ number, or nullopt on failure.
  [[nodiscard]] std::optional<std::uint32_t> create_cq(std::size_t depth);

  /// Destroy a completion queue.
  /// @param cq_number The CQ to destroy.
  /// @return True if destroyed, false if CQ not found or in use.
  bool destroy_cq(std::uint32_t cq_number);

  /// Poll a completion queue for completions.
  /// @param cq_number The CQ to poll.
  /// @param max_cqes Maximum number of CQEs to return.
  /// @return Vector of CQEs (may be empty).
  [[nodiscard]] std::vector<RdmaCqe> poll_cq(std::uint32_t cq_number, std::size_t max_cqes);

  // ============================================
  // Queue Pair Management
  // ============================================

  /// Create a queue pair.
  /// @param config QP configuration.
  /// @return QP number, or nullopt on failure.
  [[nodiscard]] std::optional<std::uint32_t> create_qp(const RdmaQpConfig& config);

  /// Destroy a queue pair.
  /// @param qp_number The QP to destroy.
  /// @return True if destroyed, false if QP not found.
  bool destroy_qp(std::uint32_t qp_number);

  /// Modify a queue pair state and parameters.
  /// @param qp_number The QP to modify.
  /// @param params Modification parameters.
  /// @return True if modified successfully.
  bool modify_qp(std::uint32_t qp_number, const RdmaQpModifyParams& params);

  /// Query a queue pair.
  /// @param qp_number The QP to query.
  /// @return QP pointer, or nullptr if not found.
  [[nodiscard]] RdmaQueuePair* query_qp(std::uint32_t qp_number);

  // ============================================
  // Work Request Posting
  // ============================================

  /// Post a send work request to a QP.
  /// @param qp_number Target QP.
  /// @param wqe The send WQE.
  /// @return True if posted successfully.
  bool post_send(std::uint32_t qp_number, const SendWqe& wqe);

  /// Post a receive work request to a QP.
  /// @param qp_number Target QP.
  /// @param wqe The receive WQE.
  /// @return True if posted successfully.
  bool post_recv(std::uint32_t qp_number, const RecvWqe& wqe);

  // ============================================
  // Packet Processing
  // ============================================

  /// Process an incoming RoCEv2 packet (UDP payload).
  /// @param udp_payload The UDP payload containing BTH and data.
  /// @param src_ip Source IP address.
  /// @param dst_ip Destination IP address.
  /// @param src_port Source UDP port.
  /// @return True if processed successfully.
  bool process_incoming_packet(std::span<const std::byte> udp_payload,
                               std::array<std::uint8_t, 4> src_ip,
                               std::array<std::uint8_t, 4> dst_ip,
                               std::uint16_t src_port);

  /// Generate outgoing packets from all QPs.
  /// @return Vector of packets ready to send.
  [[nodiscard]] std::vector<OutgoingPacket> generate_outgoing_packets();

  // ============================================
  // Time and Housekeeping
  // ============================================

  /// Advance simulation time for timeouts and recovery.
  /// @param elapsed_us Microseconds to advance.
  void advance_time(std::uint64_t elapsed_us);

  /// Reset the engine to initial state.
  void reset();

  // ============================================
  // Accessors
  // ============================================

  [[nodiscard]] const RdmaEngineConfig& config() const noexcept { return config_; }
  [[nodiscard]] const RdmaEngineStats& stats() const noexcept { return stats_; }
  [[nodiscard]] bool is_enabled() const noexcept { return config_.enabled; }

  // Component access for testing
  [[nodiscard]] const MemoryRegionTable& mr_table() const noexcept { return mr_table_; }
  [[nodiscard]] const CongestionControlManager& congestion_manager() const noexcept {
    return congestion_manager_;
  }
  [[nodiscard]] const ReliabilityManager& reliability_manager() const noexcept {
    return reliability_manager_;
  }

private:
  RdmaEngineConfig config_;
  RdmaEngineStats stats_;
  [[maybe_unused]] DMAEngine& dma_engine_;
  [[maybe_unused]] HostMemory& host_memory_;

  // Resource tables
  PdTable pd_table_;
  MemoryRegionTable mr_table_;
  std::unordered_map<std::uint32_t, std::unique_ptr<RdmaCompletionQueue>> cqs_;
  std::unordered_map<std::uint32_t, std::unique_ptr<RdmaQueuePair>> qps_;
  std::uint32_t next_cq_number_{1};
  std::uint32_t next_qp_number_{1};

  // Processors
  SendRecvProcessor send_recv_processor_;
  WriteProcessor write_processor_;
  ReadProcessor read_processor_;
  CongestionControlManager congestion_manager_;
  ReliabilityManager reliability_manager_;

  // Pending outgoing packets
  std::vector<OutgoingPacket> outgoing_packets_;

  // Internal helpers
  void process_send_packet(RdmaQueuePair& qp,
                           const RdmaPacketParser& parser,
                           std::array<std::uint8_t, 4> src_ip);
  void process_write_packet(RdmaQueuePair& qp, const RdmaPacketParser& parser);
  void process_read_request_packet(RdmaQueuePair& qp, const RdmaPacketParser& parser);
  void process_read_response_packet(RdmaQueuePair& qp, const RdmaPacketParser& parser);
  void process_ack_packet(RdmaQueuePair& qp, const RdmaPacketParser& parser);
  void process_cnp_packet(RdmaQueuePair& qp, const RdmaPacketParser& parser);
  void generate_ack(RdmaQueuePair& qp, std::uint32_t psn, AethSyndrome syndrome);
  void generate_nak(RdmaQueuePair& qp, std::uint32_t psn, AethSyndrome syndrome);
  void queue_outgoing_packet(std::vector<std::byte> packet, RdmaQueuePair& qp);
  void deliver_cqe(std::uint32_t cq_number, const RdmaCqe& cqe);
};

}  // namespace nic::rocev2
