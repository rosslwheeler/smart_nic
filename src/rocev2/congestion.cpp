#include "nic/rocev2/congestion.h"

#include <algorithm>

#include "nic/log.h"

namespace nic::rocev2 {

// =============================================================================
// CongestionControlManager
// =============================================================================

CongestionControlManager::CongestionControlManager(DcqcnConfig config) : config_(config) {
  NIC_TRACE_SCOPED(__func__);
}

bool CongestionControlManager::is_congestion_marked(EcnCodepoint ecn_codepoint) const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return ecn_codepoint == EcnCodepoint::Ce;
}

std::optional<std::vector<std::byte>> CongestionControlManager::generate_cnp(
    std::uint32_t dest_qp, std::uint32_t /* src_qp */, std::uint64_t current_time_us) {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return std::nullopt;
  }

  // Rate limit CNP generation per flow
  auto timer_iter = cnp_timers_.find(dest_qp);
  if (timer_iter != cnp_timers_.end()) {
    if (current_time_us - timer_iter->second < config_.cnp_timer_us) {
      return std::nullopt;  // Rate limited
    }
  }

  // Update timer
  cnp_timers_[dest_qp] = current_time_us;

  // Build CNP packet
  // CNP format: BTH only + 16-byte reserved field
  // Source UDP port = 0, DSCP = 48
  RdmaPacketBuilder builder;
  builder.set_opcode(RdmaOpcode::kCnp)
      .set_dest_qp(dest_qp)
      .set_psn(0)  // PSN is 0 for CNP
      .set_ack_request(false)
      .set_becn(true);  // BECN bit set in CNP

  std::vector<std::byte> packet = builder.build();

  // Add 16-byte reserved field for CNP
  packet.resize(packet.size() + 16, std::byte{0});

  ++stats_.cnps_generated;
  ++stats_.ecn_marks_detected;
  NIC_LOGF_DEBUG("CNP generated: dest_qp={}", dest_qp);

  return packet;
}

void CongestionControlManager::handle_cnp_received(std::uint32_t qp_number,
                                                   std::uint64_t current_time_us) {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return;
  }

  DcqcnFlowState& state = get_flow_state(qp_number);
  [[maybe_unused]] std::uint64_t old_rate = state.current_rate_mbps;

  // Decrease rate on CNP reception
  decrease_rate(state);

  state.last_cnp_time_us = current_time_us;
  state.in_recovery = true;
  ++state.cnp_count;
  ++stats_.cnps_received;
  ++stats_.rate_decreases;
  NIC_LOGF_DEBUG(
      "CNP received: qp={} rate {} -> {} Mbps", qp_number, old_rate, state.current_rate_mbps);
}

std::uint64_t CongestionControlManager::get_current_rate(std::uint32_t qp_number) const {
  NIC_TRACE_SCOPED(__func__);

  auto iter = flow_states_.find(qp_number);
  if (iter == flow_states_.end()) {
    return config_.initial_rate_mbps;
  }
  return iter->second.current_rate_mbps;
}

void CongestionControlManager::advance_time(std::uint64_t elapsed_us) {
  NIC_TRACE_SCOPED(__func__);

  current_time_us_ += elapsed_us;

  if (!config_.enabled) {
    return;
  }

  // Update rate recovery for all flows
  for (auto& [qp_number, state] : flow_states_) {
    // Rate increase timer
    if (state.in_recovery
        && current_time_us_ - state.rate_increase_time_us >= config_.rate_increase_period_us) {
      recover_rate(state);
      state.rate_increase_time_us = current_time_us_;
    }

    // Alpha update timer
    if (current_time_us_ - state.alpha_update_time_us >= config_.alpha_update_period_us) {
      // No CNP received in this period
      bool cnp_in_period =
          (current_time_us_ - state.last_cnp_time_us < config_.alpha_update_period_us);
      update_alpha(state, cnp_in_period);
      state.alpha_update_time_us = current_time_us_;
    }
  }
}

void CongestionControlManager::reset() {
  NIC_TRACE_SCOPED(__func__);
  flow_states_.clear();
  cnp_timers_.clear();
  stats_ = CongestionStats{};
  current_time_us_ = 0;
}

void CongestionControlManager::clear_flow_state(std::uint32_t qp_number) {
  NIC_TRACE_SCOPED(__func__);
  flow_states_.erase(qp_number);
  cnp_timers_.erase(qp_number);
}

DcqcnFlowState& CongestionControlManager::get_flow_state(std::uint32_t qp_number) {
  NIC_TRACE_SCOPED(__func__);

  auto iter = flow_states_.find(qp_number);
  if (iter == flow_states_.end()) {
    DcqcnFlowState state;
    state.current_rate_mbps = config_.initial_rate_mbps;
    state.target_rate_mbps = config_.initial_rate_mbps;
    state.alpha = 1.0;
    state.rate_increase_time_us = current_time_us_;
    state.alpha_update_time_us = current_time_us_;
    iter = flow_states_.emplace(qp_number, state).first;
  }
  return iter->second;
}

void CongestionControlManager::decrease_rate(DcqcnFlowState& state) {
  NIC_TRACE_SCOPED(__func__);

  // DCQCN rate decrease: R_c = R_c * (1 - alpha/2)
  double factor = 1.0 - (state.alpha / 2.0);
  state.current_rate_mbps =
      std::max(config_.min_rate_mbps, static_cast<std::uint64_t>(state.current_rate_mbps * factor));

  // Set target rate for recovery
  state.target_rate_mbps = state.current_rate_mbps;
}

void CongestionControlManager::recover_rate(DcqcnFlowState& state) {
  NIC_TRACE_SCOPED(__func__);

  if (!state.in_recovery) {
    return;
  }

  // DCQCN rate recovery: R_t = R_t + R_ai (additive increase)
  // Using byte counter: R_c = (R_c + R_t) / 2
  std::uint64_t rate_increment =
      static_cast<std::uint64_t>(config_.initial_rate_mbps * config_.alpha_g);
  state.target_rate_mbps =
      std::min(config_.initial_rate_mbps, state.target_rate_mbps + rate_increment);

  // Use ceiling division to avoid getting stuck asymptotically approaching target
  state.current_rate_mbps = (state.current_rate_mbps + state.target_rate_mbps + 1) / 2;

  if (state.current_rate_mbps >= config_.initial_rate_mbps) {
    state.in_recovery = false;
    state.current_rate_mbps = config_.initial_rate_mbps;
  }

  ++stats_.rate_increases;
  NIC_LOGF_TRACE("rate recovery: rate={} Mbps target={} Mbps",
                 state.current_rate_mbps,
                 state.target_rate_mbps);
}

void CongestionControlManager::update_alpha(DcqcnFlowState& state, bool cnp_received) {
  NIC_TRACE_SCOPED(__func__);

  // DCQCN alpha update: alpha = (1 - g) * alpha + g * F
  // F = 1 if CNP received, 0 otherwise
  double cnp_factor = cnp_received ? 1.0 : 0.0;
  state.alpha = (1.0 - config_.alpha_g) * state.alpha + config_.alpha_g * cnp_factor;

  // Clamp alpha to valid range
  state.alpha = std::clamp(state.alpha, 0.0, 1.0);
}

// =============================================================================
// ReliabilityManager
// =============================================================================

ReliabilityManager::ReliabilityManager(ReliabilityConfig config) : config_(config) {
  NIC_TRACE_SCOPED(__func__);
}

void ReliabilityManager::add_pending(std::uint32_t qp_number,
                                     std::uint32_t start_psn,
                                     std::uint32_t end_psn,
                                     std::uint64_t wr_id,
                                     WqeOpcode opcode,
                                     std::uint64_t send_time_us) {
  NIC_TRACE_SCOPED(__func__);

  PendingAck pending;
  pending.start_psn = start_psn;
  pending.end_psn = end_psn;
  pending.send_time_us = send_time_us;
  pending.wr_id = wr_id;
  pending.opcode = opcode;
  pending.retry_count = 0;
  pending.waiting_for_ack = true;

  pending_ops_[qp_number].push_back(pending);
}

AckResult ReliabilityManager::process_ack(std::uint32_t qp_number, std::uint32_t ack_psn) {
  NIC_TRACE_SCOPED(__func__);

  AckResult result;

  auto iter = pending_ops_.find(qp_number);
  if (iter == pending_ops_.end()) {
    return result;
  }

  ++stats_.acks_received;

  // Complete all operations with PSN <= ack_psn
  complete_up_to(iter->second, ack_psn, result.completed_wr_ids);

  // Remove completed operations
  auto& pending = iter->second;
  pending.erase(
      std::remove_if(
          pending.begin(), pending.end(), [](const PendingAck& p) { return !p.waiting_for_ack; }),
      pending.end());

  result.success = true;
  return result;
}

AckResult ReliabilityManager::process_nak(std::uint32_t qp_number,
                                          std::uint32_t nak_psn,
                                          AethSyndrome syndrome) {
  NIC_TRACE_SCOPED(__func__);

  AckResult result;
  ++stats_.naks_received;

  auto iter = pending_ops_.find(qp_number);
  if (iter == pending_ops_.end()) {
    return result;
  }

  switch (syndrome) {
    case AethSyndrome::PsnSeqError: {
      // Need to retransmit from nak_psn
      result.needs_retransmit = true;
      for (auto& pending : iter->second) {
        if (psn_in_window(
                nak_psn, pending.start_psn, (pending.end_psn - pending.start_psn + 1) & kMaxPsn)) {
          ++pending.retry_count;
          pending.send_time_us = 0;  // Mark for immediate retransmit

          if (pending.retry_count > config_.max_retries) {
            result.error_status = WqeStatus::RetryExceededError;
            pending.waiting_for_ack = false;
            ++stats_.retry_exceeded;
            NIC_LOGF_ERROR("retry exceeded: qp={} psn={} retries={}",
                           qp_number,
                           pending.start_psn,
                           pending.retry_count);
          } else {
            ++stats_.retransmissions;
          }
        }
      }
      break;
    }

    case AethSyndrome::RnrNak: {
      // Receiver Not Ready - retry after RNR timeout
      ++stats_.rnr_retries;
      NIC_LOGF_WARNING("RNR NAK: qp={} psn={}", qp_number, nak_psn);
      for (auto& pending : iter->second) {
        if ((pending.start_psn == nak_psn) || (pending.end_psn == nak_psn)) {
          ++pending.retry_count;
          if (pending.retry_count > config_.rnr_retry_count) {
            result.error_status = WqeStatus::RnrRetryExceededError;
            pending.waiting_for_ack = false;
            ++stats_.retry_exceeded;
            NIC_LOGF_ERROR("RNR retry exceeded: qp={} psn={} retries={}",
                           qp_number,
                           pending.start_psn,
                           pending.retry_count);
          } else {
            // Schedule for later retransmission
            result.needs_retransmit = true;
          }
        }
      }
      break;
    }

    case AethSyndrome::InvalidRequest: {
      result.error_status = WqeStatus::RemoteInvalidRequestError;
      for (auto& pending : iter->second) {
        if (pending.start_psn == nak_psn) {
          pending.waiting_for_ack = false;
        }
      }
      break;
    }

    case AethSyndrome::RemoteAccessError: {
      result.error_status = WqeStatus::RemoteAccessError;
      for (auto& pending : iter->second) {
        if (pending.start_psn == nak_psn) {
          pending.waiting_for_ack = false;
        }
      }
      break;
    }

    case AethSyndrome::RemoteOpError: {
      result.error_status = WqeStatus::RemoteOperationError;
      for (auto& pending : iter->second) {
        if (pending.start_psn == nak_psn) {
          pending.waiting_for_ack = false;
        }
      }
      break;
    }

    default:
      break;
  }

  result.success = true;
  return result;
}

std::vector<std::uint32_t> ReliabilityManager::check_timeouts(std::uint32_t qp_number,
                                                              std::uint64_t current_time_us) {
  NIC_TRACE_SCOPED(__func__);

  std::vector<std::uint32_t> retransmit_psns;

  auto iter = pending_ops_.find(qp_number);
  if (iter == pending_ops_.end()) {
    return retransmit_psns;
  }

  for (auto& pending : iter->second) {
    if (!pending.waiting_for_ack) {
      continue;
    }

    std::uint64_t timeout = calculate_timeout(pending.retry_count);
    if (current_time_us - pending.send_time_us >= timeout) {
      ++pending.retry_count;
      pending.send_time_us = current_time_us;
      ++stats_.timeouts;

      if (pending.retry_count > config_.max_retries) {
        pending.waiting_for_ack = false;
        ++stats_.retry_exceeded;
      } else {
        retransmit_psns.push_back(pending.start_psn);
        ++stats_.retransmissions;
      }
    }
  }

  return retransmit_psns;
}

void ReliabilityManager::reset() {
  NIC_TRACE_SCOPED(__func__);
  pending_ops_.clear();
  stats_ = ReliabilityStats{};
}

void ReliabilityManager::clear_pending(std::uint32_t qp_number) {
  NIC_TRACE_SCOPED(__func__);
  pending_ops_.erase(qp_number);
}

std::uint64_t ReliabilityManager::calculate_timeout(std::uint32_t retry_count) const {
  NIC_TRACE_SCOPED(__func__);

  // Timeout = 4.096us * 2^(timeout_exponent + retry_count)
  // But cap at max reasonable value
  std::uint32_t exponent = config_.timeout_exponent + retry_count;
  if (exponent > 31) {
    exponent = 31;
  }

  // Base timeout is 4.096us (4096 nanoseconds)
  std::uint64_t timeout = config_.ack_timeout_us << retry_count;
  return timeout;
}

void ReliabilityManager::complete_up_to(std::vector<PendingAck>& pending,
                                        std::uint32_t ack_psn,
                                        std::vector<std::uint64_t>& completed) {
  NIC_TRACE_SCOPED(__func__);

  for (auto& op : pending) {
    if (!op.waiting_for_ack) {
      continue;
    }

    // Check if this operation is fully acknowledged
    // An ACK for PSN N acknowledges all PSNs < N
    // For cumulative ACK, the ack_psn is the last PSN successfully received
    std::uint32_t diff = (ack_psn - op.end_psn) & kMaxPsn;

    // If diff is small (within window), operation is complete
    if (diff < 0x800000) {  // Within half the PSN space
      op.waiting_for_ack = false;
      completed.push_back(op.wr_id);
    }
  }
}

}  // namespace nic::rocev2
