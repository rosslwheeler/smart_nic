#include "nic/rocev2/queue_pair.h"

#include <algorithm>

namespace nic::rocev2 {

RdmaQueuePair::RdmaQueuePair(std::uint32_t qp_number, const RdmaQpConfig& config)
  : qp_number_(qp_number), config_(config) {
  NIC_TRACE_SCOPED(__func__);
}

bool RdmaQueuePair::modify(const RdmaQpModifyParams& params) {
  NIC_TRACE_SCOPED(__func__);

  // Handle state transition if requested
  if (params.target_state.has_value()) {
    QpState target = params.target_state.value();
    if (!is_valid_transition(state_, target)) {
      ++stats_.local_errors;
      return false;
    }
    state_ = target;
  }

  // Apply other parameters
  if (params.dest_qp_number.has_value()) {
    dest_qp_number_ = params.dest_qp_number.value();
  }
  if (params.dest_ip.has_value()) {
    dest_ip_ = params.dest_ip.value();
  }
  if (params.dest_port.has_value()) {
    dest_port_ = params.dest_port.value();
  }
  if (params.sq_psn.has_value()) {
    sq_psn_ = params.sq_psn.value() & kMaxPsn;
  }
  if (params.rq_psn.has_value()) {
    rq_psn_ = params.rq_psn.value() & kMaxPsn;
  }
  if (params.path_mtu.has_value()) {
    path_mtu_ = params.path_mtu.value();
  }

  return true;
}

bool RdmaQueuePair::post_send(const SendWqe& wqe) {
  NIC_TRACE_SCOPED(__func__);

  if (!can_post_send()) {
    ++stats_.local_errors;
    return false;
  }

  send_queue_.push_back(wqe);
  ++stats_.send_wqes_posted;
  return true;
}

bool RdmaQueuePair::post_recv(const RecvWqe& wqe) {
  NIC_TRACE_SCOPED(__func__);

  if (!can_post_recv()) {
    ++stats_.local_errors;
    return false;
  }

  recv_queue_.push_back(wqe);
  ++stats_.recv_wqes_posted;
  return true;
}

std::optional<SendWqe> RdmaQueuePair::get_next_send() {
  NIC_TRACE_SCOPED(__func__);

  if (!can_send() || send_queue_.empty()) {
    return std::nullopt;
  }

  SendWqe wqe = send_queue_.front();
  send_queue_.pop_front();
  return wqe;
}

std::optional<RecvWqe> RdmaQueuePair::consume_recv() {
  NIC_TRACE_SCOPED(__func__);

  if (!can_receive() || recv_queue_.empty()) {
    return std::nullopt;
  }

  RecvWqe wqe = recv_queue_.front();
  recv_queue_.pop_front();
  return wqe;
}

void RdmaQueuePair::record_packet_sent(std::size_t bytes) {
  NIC_TRACE_SCOPED(__func__);
  ++stats_.packets_sent;
  stats_.bytes_sent += bytes;
}

void RdmaQueuePair::record_packet_received(std::size_t bytes) {
  NIC_TRACE_SCOPED(__func__);
  ++stats_.packets_received;
  stats_.bytes_received += bytes;
}

void RdmaQueuePair::handle_ack(std::uint32_t acked_psn, AethSyndrome syndrome) {
  NIC_TRACE_SCOPED(__func__);

  if (syndrome == AethSyndrome::Ack) {
    // Normal ACK - remove all pending operations up to acked_psn
    while (!pending_operations_.empty()) {
      const auto& front = pending_operations_.front();
      std::uint32_t op_last_psn = advance_psn(front.psn, front.num_packets - 1);

      // Check if this operation is fully acknowledged
      if (psn_in_window(op_last_psn, last_acked_psn_, acked_psn - last_acked_psn_ + 1)) {
        ++stats_.send_completions;
        pending_operations_.pop_front();
      } else {
        break;
      }
    }
    last_acked_psn_ = acked_psn;

  } else if (syndrome == AethSyndrome::RnrNak) {
    // Receiver Not Ready - wait and retry
    ++stats_.rnr_naks_received;

  } else if (syndrome == AethSyndrome::PsnSeqError) {
    // PSN sequence error - need to retransmit
    ++stats_.sequence_errors;
    ++stats_.retransmits;

  } else {
    // Other NAKs are errors
    ++stats_.remote_errors;
    state_ = QpState::Error;
  }
}

void RdmaQueuePair::add_pending_operation(const SendWqe& wqe, std::uint32_t num_packets) {
  NIC_TRACE_SCOPED(__func__);

  PendingOperation op{
      .wqe = wqe,
      .psn = sq_psn_,
      .num_packets = num_packets,
      .timestamp_us = current_time_us_,
      .retry_count = static_cast<std::uint8_t>(config_.retry_count),
  };
  pending_operations_.push_back(op);
}

std::vector<SendWqe> RdmaQueuePair::check_timeouts(std::uint64_t current_time_us) {
  NIC_TRACE_SCOPED(__func__);

  std::vector<SendWqe> retransmits;
  std::uint64_t timeout = timeout_us();

  for (auto& op : pending_operations_) {
    if (current_time_us - op.timestamp_us >= timeout) {
      if (op.retry_count > 0) {
        --op.retry_count;
        op.timestamp_us = current_time_us;
        retransmits.push_back(op.wqe);
        ++stats_.retransmits;
      } else {
        // Retry exhausted - move to error state
        state_ = QpState::Error;
        ++stats_.local_errors;
        break;
      }
    }
  }

  return retransmits;
}

void RdmaQueuePair::advance_time(std::uint64_t elapsed_us) {
  NIC_TRACE_SCOPED(__func__);
  current_time_us_ += elapsed_us;
}

std::uint32_t RdmaQueuePair::next_send_psn() {
  NIC_TRACE_SCOPED(__func__);
  std::uint32_t current = sq_psn_;
  sq_psn_ = advance_psn(sq_psn_);
  return current;
}

void RdmaQueuePair::advance_recv_psn() {
  NIC_TRACE_SCOPED(__func__);
  rq_psn_ = advance_psn(rq_psn_);
}

std::uint32_t RdmaQueuePair::mtu_bytes() const noexcept {
  // path_mtu: 1=256, 2=512, 3=1024, 4=2048, 5=4096
  switch (path_mtu_) {
    case 1:
      return 256;
    case 2:
      return 512;
    case 3:
      return 1024;
    case 4:
      return 2048;
    case 5:
      return 4096;
    default:
      return 1024;
  }
}

void RdmaQueuePair::reset() {
  NIC_TRACE_SCOPED(__func__);
  state_ = QpState::Reset;
  dest_qp_number_ = 0;
  dest_ip_ = {};
  dest_port_ = kRoceUdpPort;
  sq_psn_ = 0;
  rq_psn_ = 0;
  last_acked_psn_ = 0;
  send_queue_.clear();
  recv_queue_.clear();
  pending_operations_.clear();
  current_time_us_ = 0;
  stats_ = RdmaQpStats{};
}

bool RdmaQueuePair::can_post_send() const noexcept {
  // Can post send in Init, RTR, or RTS states (but only execute in RTS)
  if ((state_ != QpState::Init) && (state_ != QpState::Rtr) && (state_ != QpState::Rts)) {
    return false;
  }
  return send_queue_.size() < config_.send_queue_depth;
}

bool RdmaQueuePair::can_post_recv() const noexcept {
  // Can post recv in Init, RTR, or RTS states
  if ((state_ != QpState::Init) && (state_ != QpState::Rtr) && (state_ != QpState::Rts)) {
    return false;
  }
  return recv_queue_.size() < config_.recv_queue_depth;
}

bool RdmaQueuePair::is_valid_transition(QpState from, QpState to) const {
  NIC_TRACE_SCOPED(__func__);

  // Valid IB state machine transitions
  switch (from) {
    case QpState::Reset:
      return to == QpState::Init || to == QpState::Reset;

    case QpState::Init:
      return to == QpState::Rtr || to == QpState::Reset || to == QpState::Error;

    case QpState::Rtr:
      return to == QpState::Rts || to == QpState::Reset || to == QpState::Error;

    case QpState::Rts:
      return to == QpState::Sqd || to == QpState::Reset || to == QpState::Error
             || to == QpState::SqErr;

    case QpState::Sqd:
      return to == QpState::Rts || to == QpState::Reset || to == QpState::Error;

    case QpState::SqErr:
      return to == QpState::Reset || to == QpState::Error;

    case QpState::Error:
      return to == QpState::Reset;
  }

  return false;
}

std::uint64_t RdmaQueuePair::timeout_us() const noexcept {
  // Timeout = 4.096us * 2^timeout_exponent
  // Max timeout exponent is typically 31 (about 8800 seconds)
  std::uint32_t timeout_exp = std::min(config_.timeout, 31u);
  return static_cast<std::uint64_t>(4) << timeout_exp;
}

}  // namespace nic::rocev2
