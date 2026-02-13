#pragma once

/// @file queue_pair.h
/// @brief RDMA Queue Pair for RoCEv2.

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "nic/rocev2/completion_queue.h"
#include "nic/rocev2/types.h"
#include "nic/rocev2/wqe.h"
#include "nic/trace.h"

namespace nic::rocev2 {

/// Queue Pair configuration.
struct RdmaQpConfig {
  QpType type{QpType::Rc};
  std::size_t send_queue_depth{256};
  std::size_t recv_queue_depth{256};
  std::uint32_t max_send_sge{8};
  std::uint32_t max_recv_sge{8};
  std::uint32_t pd_handle{0};          // Protection domain
  std::uint32_t send_cq_number{0};     // Send completion queue
  std::uint32_t recv_cq_number{0};     // Receive completion queue
  std::uint32_t max_inline_data{256};  // Maximum inline data size
  std::uint32_t retry_count{7};        // Number of retries before error
  std::uint32_t rnr_retry_count{7};    // RNR retry count
  std::uint32_t timeout{14};           // Timeout exponent (4.096us * 2^timeout)
  std::uint32_t min_rnr_timer{12};     // Min RNR NAK timer exponent
};

/// Queue Pair state transition parameters.
struct RdmaQpModifyParams {
  std::optional<QpState> target_state;
  std::optional<std::uint32_t> dest_qp_number;
  std::optional<std::uint32_t> initial_psn;
  std::optional<std::uint32_t> expected_psn;
  std::optional<std::array<std::uint8_t, 4>> dest_ip;
  std::optional<std::uint16_t> dest_port;
  std::optional<std::uint32_t> sq_psn;
  std::optional<std::uint32_t> rq_psn;
  std::optional<std::uint8_t> path_mtu;  // 1=256, 2=512, 3=1024, 4=2048, 5=4096
};

/// Pending operation for reliability tracking.
struct PendingOperation {
  SendWqe wqe;
  std::uint32_t psn;           // Starting PSN of this operation
  std::uint32_t num_packets;   // Number of packets for this WQE
  std::uint64_t timestamp_us;  // When the operation was sent
  std::uint8_t retry_count;    // Retries remaining
};

/// Queue Pair statistics.
struct RdmaQpStats {
  std::uint64_t send_wqes_posted{0};
  std::uint64_t recv_wqes_posted{0};
  std::uint64_t send_completions{0};
  std::uint64_t recv_completions{0};
  std::uint64_t packets_sent{0};
  std::uint64_t packets_received{0};
  std::uint64_t bytes_sent{0};
  std::uint64_t bytes_received{0};
  std::uint64_t retransmits{0};
  std::uint64_t rnr_naks_received{0};
  std::uint64_t sequence_errors{0};
  std::uint64_t local_errors{0};
  std::uint64_t remote_errors{0};
};

/// RDMA Queue Pair - manages send and receive queues with reliability.
class RdmaQueuePair {
public:
  RdmaQueuePair(std::uint32_t qp_number, const RdmaQpConfig& config);

  /// Modify QP state and parameters.
  /// @param params Modification parameters.
  /// @return true if modification succeeded.
  bool modify(const RdmaQpModifyParams& params);

  /// Post a send WQE.
  /// @param wqe The work queue entry to post.
  /// @return true if posted successfully.
  bool post_send(const SendWqe& wqe);

  /// Post a receive WQE.
  /// @param wqe The work queue entry to post.
  /// @return true if posted successfully.
  bool post_recv(const RecvWqe& wqe);

  /// Get next send WQE to process.
  /// @return WQE if available, nullopt otherwise.
  [[nodiscard]] std::optional<SendWqe> get_next_send();

  /// Get a receive WQE to consume incoming data.
  /// @return WQE if available, nullopt otherwise.
  [[nodiscard]] std::optional<RecvWqe> consume_recv();

  /// Record that a packet was sent.
  /// @param bytes Number of bytes in the packet.
  void record_packet_sent(std::size_t bytes);

  /// Record that a packet was received.
  /// @param bytes Number of bytes in the packet.
  void record_packet_received(std::size_t bytes);

  /// Handle ACK for PSN.
  /// @param acked_psn The acknowledged PSN.
  /// @param syndrome AETH syndrome (for NAK handling).
  void handle_ack(std::uint32_t acked_psn, AethSyndrome syndrome);

  /// Add a pending operation for reliability tracking.
  /// @param wqe The WQE being sent.
  /// @param num_packets Number of packets for this WQE.
  void add_pending_operation(const SendWqe& wqe, std::uint32_t num_packets);

  /// Check for timeout and retransmit if needed.
  /// @param current_time_us Current time in microseconds.
  /// @return Vector of WQEs to retransmit.
  [[nodiscard]] std::vector<SendWqe> check_timeouts(std::uint64_t current_time_us);

  /// Advance time for deterministic simulation.
  /// @param elapsed_us Microseconds elapsed.
  void advance_time(std::uint64_t elapsed_us);

  /// Generate next send PSN and advance.
  /// @return The current send PSN before advancing.
  [[nodiscard]] std::uint32_t next_send_psn();

  /// Get the last PSN that was sent (current sq_psn - 1).
  [[nodiscard]] std::uint32_t last_sent_psn() const noexcept {
    return (sq_psn_ == 0) ? kMaxPsn : (sq_psn_ - 1);
  }

  /// Get expected receive PSN.
  [[nodiscard]] std::uint32_t expected_recv_psn() const noexcept { return rq_psn_; }

  /// Advance expected receive PSN.
  void advance_recv_psn();

  // Accessors
  [[nodiscard]] std::uint32_t qp_number() const noexcept { return qp_number_; }
  [[nodiscard]] QpState state() const noexcept { return state_; }
  [[nodiscard]] QpType type() const noexcept { return config_.type; }
  [[nodiscard]] std::uint32_t pd_handle() const noexcept { return config_.pd_handle; }
  [[nodiscard]] std::uint32_t send_cq_number() const noexcept { return config_.send_cq_number; }
  [[nodiscard]] std::uint32_t recv_cq_number() const noexcept { return config_.recv_cq_number; }
  [[nodiscard]] std::uint32_t dest_qp_number() const noexcept { return dest_qp_number_; }
  [[nodiscard]] std::uint32_t sq_psn() const noexcept { return sq_psn_; }
  [[nodiscard]] std::uint32_t rq_psn() const noexcept { return rq_psn_; }
  [[nodiscard]] std::uint8_t path_mtu() const noexcept { return path_mtu_; }
  [[nodiscard]] std::size_t send_queue_size() const noexcept { return send_queue_.size(); }
  [[nodiscard]] std::size_t recv_queue_size() const noexcept { return recv_queue_.size(); }
  [[nodiscard]] std::size_t pending_count() const noexcept { return pending_operations_.size(); }
  [[nodiscard]] const RdmaQpConfig& config() const noexcept { return config_; }
  [[nodiscard]] const RdmaQpStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const std::array<std::uint8_t, 4>& dest_ip() const noexcept { return dest_ip_; }
  [[nodiscard]] std::uint16_t dest_port() const noexcept { return dest_port_; }

  /// Get MTU in bytes based on path_mtu setting.
  [[nodiscard]] std::uint32_t mtu_bytes() const noexcept;

  /// Reset QP to initial state.
  void reset();

  /// Check if QP can accept send WQEs.
  [[nodiscard]] bool can_post_send() const noexcept;

  /// Check if QP can accept recv WQEs.
  [[nodiscard]] bool can_post_recv() const noexcept;

  /// Check if QP is in an operational state for sending.
  [[nodiscard]] bool can_send() const noexcept { return state_ == QpState::Rts; }

  /// Check if QP is in an operational state for receiving.
  [[nodiscard]] bool can_receive() const noexcept {
    return state_ == QpState::Rtr || state_ == QpState::Rts;
  }

private:
  std::uint32_t qp_number_;
  RdmaQpConfig config_;
  QpState state_{QpState::Reset};

  // Remote QP info (set during transitions)
  std::uint32_t dest_qp_number_{0};
  std::array<std::uint8_t, 4> dest_ip_{};
  std::uint16_t dest_port_{kRoceUdpPort};
  std::uint8_t path_mtu_{3};  // Default 1024 bytes

  // PSN tracking
  std::uint32_t sq_psn_{0};  // Next PSN to send
  std::uint32_t rq_psn_{0};  // Next expected PSN to receive
  std::uint32_t last_acked_psn_{0};

  // Work queues
  std::deque<SendWqe> send_queue_;
  std::deque<RecvWqe> recv_queue_;

  // Reliability tracking
  std::deque<PendingOperation> pending_operations_;
  std::uint64_t current_time_us_{0};

  RdmaQpStats stats_;

  /// Validate state transition.
  [[nodiscard]] bool is_valid_transition(QpState from, QpState to) const;

  /// Get timeout in microseconds.
  [[nodiscard]] std::uint64_t timeout_us() const noexcept;
};

}  // namespace nic::rocev2
