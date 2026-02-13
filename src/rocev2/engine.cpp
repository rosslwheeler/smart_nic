#include "nic/rocev2/engine.h"

#include <algorithm>

#include "nic/log.h"

namespace nic::rocev2 {

RdmaEngine::RdmaEngine(RdmaEngineConfig config, DMAEngine& dma_engine, HostMemory& host_memory)
  : config_(config),
    dma_engine_(dma_engine),
    host_memory_(host_memory),
    mr_table_(),
    send_recv_processor_(host_memory, mr_table_),
    write_processor_(host_memory, mr_table_),
    read_processor_(host_memory, mr_table_),
    congestion_manager_(config.dcqcn_config),
    reliability_manager_(config.reliability_config) {
  NIC_TRACE_SCOPED(__func__);
}

// ============================================
// Protection Domain Management
// ============================================

std::optional<std::uint32_t> RdmaEngine::create_pd() {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    NIC_LOG_WARNING("RDMA engine disabled, cannot create PD");
    return std::nullopt;
  }

  auto pd_handle = pd_table_.allocate();
  if (pd_handle.has_value()) {
    ++stats_.pds_created;
    NIC_LOGF_INFO("PD created: handle={}", *pd_handle);
  }
  return pd_handle;
}

bool RdmaEngine::destroy_pd(std::uint32_t pd_handle) {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return false;
  }

  return pd_table_.deallocate(pd_handle);
}

// ============================================
// Memory Region Management
// ============================================

std::optional<std::uint32_t> RdmaEngine::register_mr(std::uint32_t pd_handle,
                                                     std::uint64_t virtual_address,
                                                     std::size_t length,
                                                     AccessFlags access) {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return std::nullopt;
  }

  if (!pd_table_.is_valid(pd_handle)) {
    ++stats_.errors;
    NIC_LOGF_WARNING("MR registration failed: invalid PD {}", pd_handle);
    return std::nullopt;
  }

  auto lkey = mr_table_.register_mr(pd_handle, virtual_address, length, access);
  if (lkey.has_value()) {
    ++stats_.mrs_registered;
    NIC_LOGF_INFO("MR registered: pd={} addr={:#x} len={} lkey={}",
                  pd_handle,
                  virtual_address,
                  length,
                  *lkey);
  }
  return lkey;
}

bool RdmaEngine::deregister_mr(std::uint32_t lkey) {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return false;
  }

  return mr_table_.deregister_mr(lkey);
}

// ============================================
// Completion Queue Management
// ============================================

std::optional<std::uint32_t> RdmaEngine::create_cq(std::size_t depth) {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return std::nullopt;
  }

  if (cqs_.size() >= config_.max_cqs) {
    ++stats_.errors;
    NIC_LOGF_WARNING("CQ creation failed: limit reached ({}/{})", cqs_.size(), config_.max_cqs);
    return std::nullopt;
  }

  std::uint32_t cq_number = next_cq_number_++;
  RdmaCqConfig cq_config;
  cq_config.depth = depth;

  cqs_[cq_number] = std::make_unique<RdmaCompletionQueue>(cq_number, cq_config);
  ++stats_.cqs_created;
  NIC_LOGF_INFO("CQ created: cq={} depth={}", cq_number, depth);

  return cq_number;
}

bool RdmaEngine::destroy_cq(std::uint32_t cq_number) {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return false;
  }

  // Check if any QP is using this CQ
  for (const auto& [qp_number, qp] : qps_) {
    if ((qp->send_cq_number() == cq_number) || (qp->recv_cq_number() == cq_number)) {
      return false;  // CQ in use
    }
  }

  return cqs_.erase(cq_number) > 0;
}

std::vector<RdmaCqe> RdmaEngine::poll_cq(std::uint32_t cq_number, std::size_t max_cqes) {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return {};
  }

  auto iter = cqs_.find(cq_number);
  if (iter == cqs_.end()) {
    return {};
  }

  return iter->second->poll(max_cqes);
}

// ============================================
// Queue Pair Management
// ============================================

std::optional<std::uint32_t> RdmaEngine::create_qp(const RdmaQpConfig& config) {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return std::nullopt;
  }

  if (qps_.size() >= config_.max_qps) {
    ++stats_.errors;
    NIC_LOGF_WARNING("QP creation failed: limit reached ({}/{})", qps_.size(), config_.max_qps);
    return std::nullopt;
  }

  // Validate PD
  if (!pd_table_.is_valid(config.pd_handle)) {
    ++stats_.errors;
    NIC_LOGF_WARNING("QP creation failed: invalid PD {}", config.pd_handle);
    return std::nullopt;
  }

  // Validate CQs
  if ((cqs_.find(config.send_cq_number) == cqs_.end())
      || (cqs_.find(config.recv_cq_number) == cqs_.end())) {
    ++stats_.errors;
    NIC_LOGF_WARNING("QP creation failed: invalid CQ (send={} recv={})",
                     config.send_cq_number,
                     config.recv_cq_number);
    return std::nullopt;
  }

  std::uint32_t qp_number = next_qp_number_++;
  qps_[qp_number] = std::make_unique<RdmaQueuePair>(qp_number, config);
  ++stats_.qps_created;
  NIC_LOGF_INFO("QP created: qp={} pd={} send_cq={} recv_cq={}",
                qp_number,
                config.pd_handle,
                config.send_cq_number,
                config.recv_cq_number);

  return qp_number;
}

bool RdmaEngine::destroy_qp(std::uint32_t qp_number) {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return false;
  }

  auto iter = qps_.find(qp_number);
  if (iter == qps_.end()) {
    return false;
  }

  // Clear any pending state for this QP
  read_processor_.clear_read_state(qp_number);
  congestion_manager_.clear_flow_state(qp_number);
  reliability_manager_.clear_pending(qp_number);

  qps_.erase(iter);
  return true;
}

bool RdmaEngine::modify_qp(std::uint32_t qp_number, const RdmaQpModifyParams& params) {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return false;
  }

  auto iter = qps_.find(qp_number);
  if (iter == qps_.end()) {
    return false;
  }

  return iter->second->modify(params);
}

RdmaQueuePair* RdmaEngine::query_qp(std::uint32_t qp_number) {
  NIC_TRACE_SCOPED(__func__);

  auto iter = qps_.find(qp_number);
  if (iter == qps_.end()) {
    return nullptr;
  }
  return iter->second.get();
}

// ============================================
// Work Request Posting
// ============================================

bool RdmaEngine::post_send(std::uint32_t qp_number, const SendWqe& wqe) {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return false;
  }

  auto iter = qps_.find(qp_number);
  if (iter == qps_.end()) {
    return false;
  }

  RdmaQueuePair& qp = *iter->second;

  if (!qp.can_send()) {
    ++stats_.errors;
    NIC_LOGF_WARNING("post_send failed: qp={} not in sendable state", qp_number);
    return false;
  }

  // Capture starting PSN before generating packets (which advances PSN)
  std::uint32_t start_psn = qp.sq_psn();

  // Generate packets based on opcode
  std::vector<std::vector<std::byte>> packets;

  switch (wqe.opcode) {
    case WqeOpcode::Send:
    case WqeOpcode::SendImm: {
      packets = send_recv_processor_.generate_send_packets(qp, wqe);
      break;
    }
    case WqeOpcode::RdmaWrite:
    case WqeOpcode::RdmaWriteImm: {
      packets = write_processor_.generate_write_packets(qp, wqe);
      break;
    }
    case WqeOpcode::RdmaRead: {
      packets = read_processor_.generate_read_request(qp, wqe);
      break;
    }
    default:
      ++stats_.errors;
      return false;
  }

  if (packets.empty()) {
    ++stats_.errors;
    return false;
  }

  // Track for reliability - end_psn is start + num_packets - 1
  std::uint32_t end_psn = start_psn;
  if (packets.size() > 1) {
    end_psn = (start_psn + static_cast<std::uint32_t>(packets.size()) - 1) & 0xFFFFFF;
  }

  reliability_manager_.add_pending(qp_number, start_psn, end_psn, wqe.wr_id, wqe.opcode, 0);

  // Queue packets for sending
  for (auto& packet : packets) {
    stats_.bytes_sent += packet.size();
    queue_outgoing_packet(std::move(packet), qp);
  }

  ++stats_.send_wqes_posted;
  stats_.packets_sent += packets.size();
  NIC_LOGF_DEBUG("post_send: qp={} opcode={} len={} packets={}",
                 qp_number,
                 static_cast<int>(wqe.opcode),
                 wqe.total_length,
                 packets.size());

  return true;
}

bool RdmaEngine::post_recv(std::uint32_t qp_number, const RecvWqe& wqe) {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return false;
  }

  auto iter = qps_.find(qp_number);
  if (iter == qps_.end()) {
    return false;
  }

  if (!iter->second->post_recv(wqe)) {
    ++stats_.errors;
    return false;
  }

  ++stats_.recv_wqes_posted;
  return true;
}

// ============================================
// Packet Processing
// ============================================

bool RdmaEngine::process_incoming_packet(std::span<const std::byte> udp_payload,
                                         std::array<std::uint8_t, 4> src_ip,
                                         std::array<std::uint8_t, 4> /* dst_ip */,
                                         std::uint16_t /* src_port */) {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return false;
  }

  // Copy packet data - parser holds spans referencing this data
  std::vector<std::byte> packet_data(udp_payload.begin(), udp_payload.end());

  RdmaPacketParser parser;
  if (!parser.parse(packet_data)) {
    ++stats_.errors;
    return false;
  }

  const BthFields& bth = parser.bth();

  // Find destination QP
  auto qp_iter = qps_.find(bth.dest_qp);
  if (qp_iter == qps_.end()) {
    ++stats_.errors;
    NIC_LOGF_WARNING("incoming packet: unknown dest QP {}", bth.dest_qp);
    return false;
  }

  RdmaQueuePair& qp = *qp_iter->second;

  // Check for ECN marking (CE codepoint)
  if (congestion_manager_.is_congestion_marked(EcnCodepoint::Ce)) {
    // Generate CNP back to sender
    auto cnp = congestion_manager_.generate_cnp(bth.dest_qp, 0, 0);
    if (cnp.has_value()) {
      queue_outgoing_packet(std::move(*cnp), qp);
    }
  }

  ++stats_.packets_received;
  stats_.bytes_received += udp_payload.size();

  // Process based on opcode
  RdmaOpcode opcode = bth.opcode;

  if ((opcode == RdmaOpcode::kRcSendOnly) || (opcode == RdmaOpcode::kRcSendFirst)
      || (opcode == RdmaOpcode::kRcSendMiddle) || (opcode == RdmaOpcode::kRcSendLast)
      || (opcode == RdmaOpcode::kRcSendOnlyImm) || (opcode == RdmaOpcode::kRcSendLastImm)) {
    process_send_packet(qp, parser, src_ip);
  } else if ((opcode == RdmaOpcode::kRcWriteOnly) || (opcode == RdmaOpcode::kRcWriteFirst)
             || (opcode == RdmaOpcode::kRcWriteMiddle) || (opcode == RdmaOpcode::kRcWriteLast)
             || (opcode == RdmaOpcode::kRcWriteOnlyImm)
             || (opcode == RdmaOpcode::kRcWriteLastImm)) {
    process_write_packet(qp, parser);
  } else if (opcode == RdmaOpcode::kRcReadRequest) {
    process_read_request_packet(qp, parser);
  } else if ((opcode == RdmaOpcode::kRcReadResponseOnly)
             || (opcode == RdmaOpcode::kRcReadResponseFirst)
             || (opcode == RdmaOpcode::kRcReadResponseMiddle)
             || (opcode == RdmaOpcode::kRcReadResponseLast)) {
    process_read_response_packet(qp, parser);
  } else if (opcode == RdmaOpcode::kRcAck) {
    process_ack_packet(qp, parser);
  } else if (opcode == RdmaOpcode::kCnp) {
    process_cnp_packet(qp, parser);
  } else {
    ++stats_.errors;
    return false;
  }

  return true;
}

void RdmaEngine::process_send_packet(RdmaQueuePair& qp,
                                     const RdmaPacketParser& parser,
                                     std::array<std::uint8_t, 4> /* src_ip */) {
  NIC_TRACE_SCOPED(__func__);

  auto result = send_recv_processor_.process_recv_packet(qp, parser);

  if (!result.success) {
    if (result.syndrome != AethSyndrome::Ack) {
      generate_nak(qp, result.ack_psn, result.syndrome);
    }
    return;
  }

  // Generate ACK if requested
  if (result.needs_ack) {
    generate_ack(qp, result.ack_psn, result.syndrome);
  }

  // If complete, generate CQE
  if (result.cqe.has_value()) {
    deliver_cqe(qp.recv_cq_number(), *result.cqe);
  }
}

void RdmaEngine::process_write_packet(RdmaQueuePair& qp, const RdmaPacketParser& parser) {
  NIC_TRACE_SCOPED(__func__);

  auto result = write_processor_.process_write_packet(qp, parser);

  if (!result.success) {
    if (result.syndrome != AethSyndrome::Ack) {
      generate_nak(qp, result.ack_psn, result.syndrome);
    }
    return;
  }

  // Generate ACK if requested
  if (result.needs_ack) {
    generate_ack(qp, result.ack_psn, result.syndrome);
  }

  // If write with immediate completed, generate receive CQE
  if (result.recv_cqe.has_value()) {
    deliver_cqe(qp.recv_cq_number(), *result.recv_cqe);
  }
}

void RdmaEngine::process_read_request_packet(RdmaQueuePair& qp, const RdmaPacketParser& parser) {
  NIC_TRACE_SCOPED(__func__);

  auto result = read_processor_.process_read_request(qp, parser);

  if (result.needs_nak) {
    generate_nak(qp, result.nak_psn, result.syndrome);
    return;
  }

  // Queue response packets
  for (auto& packet : result.response_packets) {
    queue_outgoing_packet(std::move(packet), qp);
    ++stats_.packets_sent;
  }
}

void RdmaEngine::process_read_response_packet(RdmaQueuePair& qp, const RdmaPacketParser& parser) {
  NIC_TRACE_SCOPED(__func__);

  auto result = read_processor_.process_read_response(qp, parser);

  if (!result.success) {
    ++stats_.errors;
    return;
  }

  if (result.cqe.has_value()) {
    deliver_cqe(qp.send_cq_number(), *result.cqe);
  }
}

void RdmaEngine::process_ack_packet(RdmaQueuePair& qp, const RdmaPacketParser& parser) {
  NIC_TRACE_SCOPED(__func__);

  const AethFields& aeth = parser.aeth();
  std::uint32_t ack_psn = parser.bth().psn;

  if (aeth.syndrome == AethSyndrome::Ack) {
    // Normal ACK
    auto result = reliability_manager_.process_ack(qp.qp_number(), ack_psn);
    for (std::uint64_t wr_id : result.completed_wr_ids) {
      RdmaCqe cqe;
      cqe.wr_id = wr_id;
      cqe.status = WqeStatus::Success;
      cqe.qp_number = qp.qp_number();
      deliver_cqe(qp.send_cq_number(), cqe);
    }
  } else {
    // NAK
    AethSyndrome syndrome = aeth.syndrome;
    NIC_LOGF_WARNING("NAK received: qp={} psn={} syndrome={}",
                     qp.qp_number(),
                     ack_psn,
                     static_cast<int>(syndrome));
    auto result = reliability_manager_.process_nak(qp.qp_number(), ack_psn, syndrome);

    if (result.error_status.has_value()) {
      // Fatal error - generate error CQE
      RdmaCqe cqe;
      cqe.status = *result.error_status;
      cqe.qp_number = qp.qp_number();
      deliver_cqe(qp.send_cq_number(), cqe);

      // Transition QP to error state
      NIC_LOGF_ERROR("QP {} entering error state: status={}",
                     qp.qp_number(),
                     static_cast<int>(*result.error_status));
      RdmaQpModifyParams params;
      params.target_state = QpState::Error;
      qp.modify(params);
    }
  }
}

void RdmaEngine::process_cnp_packet(RdmaQueuePair& qp, const RdmaPacketParser& /* parser */) {
  NIC_TRACE_SCOPED(__func__);

  congestion_manager_.handle_cnp_received(qp.qp_number(), 0);
}

void RdmaEngine::generate_ack(RdmaQueuePair& qp, std::uint32_t psn, AethSyndrome syndrome) {
  NIC_TRACE_SCOPED(__func__);

  RdmaPacketBuilder builder;
  builder.set_opcode(RdmaOpcode::kRcAck)
      .set_dest_qp(qp.dest_qp_number())
      .set_psn(psn)
      .set_ack_request(false)
      .set_syndrome(syndrome)
      .set_msn(0);

  auto packet = builder.build();
  queue_outgoing_packet(std::move(packet), qp);
  ++stats_.packets_sent;
}

void RdmaEngine::generate_nak(RdmaQueuePair& qp, std::uint32_t psn, AethSyndrome syndrome) {
  NIC_TRACE_SCOPED(__func__);

  generate_ack(qp, psn, syndrome);
}

void RdmaEngine::queue_outgoing_packet(std::vector<std::byte> packet, RdmaQueuePair& qp) {
  NIC_TRACE_SCOPED(__func__);

  OutgoingPacket out;
  out.data = std::move(packet);
  out.dest_ip = qp.dest_ip();
  out.dest_port = kRoceUdpPort;

  outgoing_packets_.push_back(std::move(out));
}

void RdmaEngine::deliver_cqe(std::uint32_t cq_number, const RdmaCqe& cqe) {
  NIC_TRACE_SCOPED(__func__);

  auto iter = cqs_.find(cq_number);
  if (iter != cqs_.end()) {
    iter->second->post(cqe);
    ++stats_.cqes_generated;
  }
}

std::vector<OutgoingPacket> RdmaEngine::generate_outgoing_packets() {
  NIC_TRACE_SCOPED(__func__);

  std::vector<OutgoingPacket> result = std::move(outgoing_packets_);
  outgoing_packets_.clear();
  return result;
}

// ============================================
// Time and Housekeeping
// ============================================

void RdmaEngine::advance_time(std::uint64_t elapsed_us) {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return;
  }

  congestion_manager_.advance_time(elapsed_us);

  // Check for timeouts on all QPs
  for (auto& [qp_number, qp] : qps_) {
    auto retransmit_psns = reliability_manager_.check_timeouts(qp_number, elapsed_us);
    // Note: Actual retransmission would require re-fetching the original data
    // For now, we just track the timeout statistics
  }
}

void RdmaEngine::reset() {
  NIC_TRACE_SCOPED(__func__);

  qps_.clear();
  cqs_.clear();
  outgoing_packets_.clear();
  pd_table_.reset();
  mr_table_.reset();
  send_recv_processor_.reset();
  congestion_manager_.reset();
  reliability_manager_.reset();
  read_processor_.reset();
  write_processor_.reset();
  stats_ = RdmaEngineStats{};
  next_cq_number_ = 1;
  next_qp_number_ = 1;
  NIC_LOG_INFO("RDMA engine reset");
}

}  // namespace nic::rocev2
