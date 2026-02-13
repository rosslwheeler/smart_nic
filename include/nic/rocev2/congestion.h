#pragma once

/// @file congestion.h
/// @brief DCQCN congestion control for RoCEv2.

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "nic/rocev2/packet.h"
#include "nic/rocev2/types.h"
#include "nic/trace.h"

namespace nic::rocev2 {

/// DSCP value for CNP packets (high priority).
inline constexpr std::uint8_t kCnpDscp = 48;

/// ECN codepoints in IP header.
enum class EcnCodepoint : std::uint8_t {
  NonEct = 0x00,  // Non-ECN-Capable Transport
  Ect0 = 0x02,    // ECN-Capable Transport (0)
  Ect1 = 0x01,    // ECN-Capable Transport (1)
  Ce = 0x03,      // Congestion Experienced
};

/// Configuration for DCQCN congestion control.
struct DcqcnConfig {
  std::uint64_t initial_rate_mbps{100000};  // Initial rate (100 Gbps default)
  std::uint64_t min_rate_mbps{10};          // Minimum rate
  double alpha_g{1.0 / 256.0};              // Alpha recovery factor
  double beta{0.5};                         // Rate decrease factor on CNP
  std::uint64_t rate_increase_period_us{50};
  std::uint64_t alpha_update_period_us{55};
  std::uint64_t cnp_timer_us{50};  // Min time between CNPs per flow
  bool enabled{true};
};

/// Per-flow DCQCN state.
struct DcqcnFlowState {
  std::uint64_t current_rate_mbps{0};  // Current sending rate
  std::uint64_t target_rate_mbps{0};   // Target rate after recovery
  double alpha{1.0};                   // Congestion feedback factor
  std::uint64_t last_cnp_time_us{0};   // Last CNP reception time
  std::uint64_t rate_increase_time_us{0};
  std::uint64_t alpha_update_time_us{0};
  std::uint32_t cnp_count{0};  // Count of CNPs received
  bool in_recovery{false};     // True if recovering from congestion
};

/// Statistics for congestion control.
struct CongestionStats {
  std::uint64_t cnps_generated{0};
  std::uint64_t cnps_received{0};
  std::uint64_t ecn_marks_detected{0};
  std::uint64_t rate_decreases{0};
  std::uint64_t rate_increases{0};
};

/// Congestion Control Manager for DCQCN implementation.
class CongestionControlManager {
public:
  explicit CongestionControlManager(DcqcnConfig config = {});

  /// Check if ECN congestion experienced is set in a packet.
  /// @param ecn_codepoint The ECN codepoint from IP header.
  /// @return true if congestion experienced (CE) bit is set.
  [[nodiscard]] bool is_congestion_marked(EcnCodepoint ecn_codepoint) const noexcept;

  /// Generate a CNP (Congestion Notification Packet) in response to ECN marking.
  /// @param dest_qp Destination QP number for the CNP.
  /// @param src_qp Source QP number.
  /// @return CNP packet bytes if CNP should be sent, nullopt if rate-limited.
  [[nodiscard]] std::optional<std::vector<std::byte>> generate_cnp(std::uint32_t dest_qp,
                                                                   std::uint32_t src_qp,
                                                                   std::uint64_t current_time_us);

  /// Handle reception of a CNP packet.
  /// @param qp_number QP that received the CNP.
  /// @param current_time_us Current time in microseconds.
  void handle_cnp_received(std::uint32_t qp_number, std::uint64_t current_time_us);

  /// Get the current sending rate for a flow.
  /// @param qp_number QP number identifying the flow.
  /// @return Current rate in Mbps.
  [[nodiscard]] std::uint64_t get_current_rate(std::uint32_t qp_number) const;

  /// Advance time and update rate recovery.
  /// @param elapsed_us Microseconds elapsed.
  void advance_time(std::uint64_t elapsed_us);

  /// Get statistics.
  [[nodiscard]] const CongestionStats& stats() const noexcept { return stats_; }

  /// Reset all state.
  void reset();

  /// Clear flow state for a QP.
  void clear_flow_state(std::uint32_t qp_number);

private:
  DcqcnConfig config_;
  CongestionStats stats_;
  std::uint64_t current_time_us_{0};

  // Per-flow state indexed by QP number
  std::unordered_map<std::uint32_t, DcqcnFlowState> flow_states_;

  // Per-flow CNP rate limiting (dest_qp -> last CNP time)
  std::unordered_map<std::uint32_t, std::uint64_t> cnp_timers_;

  /// Get or create flow state for a QP.
  DcqcnFlowState& get_flow_state(std::uint32_t qp_number);

  /// Update rate for a flow after CNP reception.
  void decrease_rate(DcqcnFlowState& state);

  /// Perform rate recovery for a flow.
  void recover_rate(DcqcnFlowState& state);

  /// Update alpha factor for a flow.
  void update_alpha(DcqcnFlowState& state, bool cnp_received);
};

/// Configuration for reliability/retransmission.
struct ReliabilityConfig {
  std::uint32_t max_retries{7};          // Max retransmission attempts
  std::uint32_t rnr_retry_count{7};      // RNR retry count
  std::uint64_t ack_timeout_us{4096};    // Initial ACK timeout (4.096 ms)
  std::uint64_t rnr_timeout_us{655360};  // RNR timeout (655.36 ms default)
  std::uint8_t timeout_exponent{14};     // Timeout = 4.096us * 2^exponent
};

/// State for a pending operation awaiting acknowledgment.
struct PendingAck {
  std::uint32_t start_psn{0};     // First PSN of the operation
  std::uint32_t end_psn{0};       // Last PSN of the operation
  std::uint64_t send_time_us{0};  // When the operation was sent
  std::uint64_t wr_id{0};         // Work request ID
  WqeOpcode opcode{WqeOpcode::Send};
  std::uint32_t retry_count{0};  // Number of retries so far
  bool waiting_for_ack{true};    // True if still awaiting ACK
};

/// Result of processing an ACK/NAK.
struct AckResult {
  bool success{false};
  bool needs_retransmit{false};
  std::vector<std::uint64_t> completed_wr_ids;  // WR IDs that completed
  std::optional<WqeStatus> error_status;        // Set if operation failed
};

/// Statistics for reliability.
struct ReliabilityStats {
  std::uint64_t acks_received{0};
  std::uint64_t naks_received{0};
  std::uint64_t retransmissions{0};
  std::uint64_t timeouts{0};
  std::uint64_t rnr_retries{0};
  std::uint64_t retry_exceeded{0};
};

/// Reliability Manager - handles ACK/NAK processing and retransmission.
class ReliabilityManager {
public:
  explicit ReliabilityManager(ReliabilityConfig config = {});

  /// Add a pending operation awaiting acknowledgment.
  /// @param qp_number QP that sent the operation.
  /// @param start_psn First PSN of the operation.
  /// @param end_psn Last PSN of the operation.
  /// @param wr_id Work request ID.
  /// @param opcode Operation type.
  /// @param send_time_us Time when operation was sent.
  void add_pending(std::uint32_t qp_number,
                   std::uint32_t start_psn,
                   std::uint32_t end_psn,
                   std::uint64_t wr_id,
                   WqeOpcode opcode,
                   std::uint64_t send_time_us);

  /// Process an ACK packet.
  /// @param qp_number QP that received the ACK.
  /// @param ack_psn PSN being acknowledged.
  /// @return Result indicating completed operations or needed retransmissions.
  [[nodiscard]] AckResult process_ack(std::uint32_t qp_number, std::uint32_t ack_psn);

  /// Process a NAK packet.
  /// @param qp_number QP that received the NAK.
  /// @param nak_psn PSN of the NAK.
  /// @param syndrome NAK syndrome (type of error).
  /// @return Result indicating needed actions.
  [[nodiscard]] AckResult process_nak(std::uint32_t qp_number,
                                      std::uint32_t nak_psn,
                                      AethSyndrome syndrome);

  /// Check for timeouts and trigger retransmissions.
  /// @param qp_number QP to check.
  /// @param current_time_us Current time.
  /// @return List of PSNs that need retransmission.
  [[nodiscard]] std::vector<std::uint32_t> check_timeouts(std::uint32_t qp_number,
                                                          std::uint64_t current_time_us);

  /// Get statistics.
  [[nodiscard]] const ReliabilityStats& stats() const noexcept { return stats_; }

  /// Reset all state.
  void reset();

  /// Clear pending state for a QP.
  void clear_pending(std::uint32_t qp_number);

private:
  ReliabilityConfig config_;
  ReliabilityStats stats_;

  // Per-QP pending operations
  std::unordered_map<std::uint32_t, std::vector<PendingAck>> pending_ops_;

  /// Calculate timeout in microseconds for given retry count.
  [[nodiscard]] std::uint64_t calculate_timeout(std::uint32_t retry_count) const;

  /// Mark operations as completed up to ack_psn.
  void complete_up_to(std::vector<PendingAck>& pending,
                      std::uint32_t ack_psn,
                      std::vector<std::uint64_t>& completed);
};

}  // namespace nic::rocev2
