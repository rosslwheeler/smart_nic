#include "nic/rocev2/send_recv.h"

#include <algorithm>
#include <cstring>

#include "nic/log.h"

namespace nic::rocev2 {

SendRecvProcessor::SendRecvProcessor(HostMemory& host_memory, MemoryRegionTable& mr_table)
  : host_memory_(host_memory), mr_table_(mr_table) {
  NIC_TRACE_SCOPED(__func__);
}

std::vector<std::vector<std::byte>> SendRecvProcessor::generate_send_packets(RdmaQueuePair& qp,
                                                                             const SendWqe& wqe) {
  NIC_TRACE_SCOPED(__func__);

  std::vector<std::vector<std::byte>> packets;

  // Validate WQE opcode
  if ((wqe.opcode != WqeOpcode::Send) && (wqe.opcode != WqeOpcode::SendImm)) {
    return packets;
  }

  // Read data from scatter-gather list
  std::vector<std::byte> data = read_from_sgl(wqe.sgl, wqe.local_lkey, qp.pd_handle());
  if (data.size() != wqe.total_length) {
    return packets;
  }

  std::uint32_t mtu = qp.mtu_bytes();
  std::uint32_t num_packets = calculate_packet_count(wqe.total_length, mtu);
  bool has_immediate = (wqe.opcode == WqeOpcode::SendImm);

  ++stats_.sends_started;
  NIC_LOGF_DEBUG(
      "send: qp={} len={} mtu={} packets={}", qp.qp_number(), wqe.total_length, mtu, num_packets);

  // Handle zero-length send
  if (wqe.total_length == 0) {
    RdmaPacketBuilder builder;
    RdmaOpcode opcode = has_immediate ? RdmaOpcode::kRcSendOnlyImm : RdmaOpcode::kRcSendOnly;

    builder.set_opcode(opcode)
        .set_dest_qp(qp.dest_qp_number())
        .set_psn(qp.next_send_psn())
        .set_solicited_event(wqe.solicited)
        .set_ack_request(true);

    if (has_immediate) {
      builder.set_immediate(wqe.immediate_data);
    }

    packets.push_back(builder.build());
    ++stats_.send_packets_generated;

    // Add pending operation for reliability
    qp.add_pending_operation(wqe, 1);
    qp.record_packet_sent(packets.back().size());

    return packets;
  }

  // Generate packets with MTU segmentation
  std::size_t offset = 0;
  for (std::uint32_t pkt_idx = 0; pkt_idx < num_packets; ++pkt_idx) {
    bool is_first = (pkt_idx == 0);
    bool is_last = (pkt_idx == num_packets - 1);

    std::size_t payload_size = std::min(static_cast<std::size_t>(mtu), data.size() - offset);
    std::span<const std::byte> payload(data.data() + offset, payload_size);

    // Calculate padding
    std::uint8_t pad_count = 0;
    if (is_last) {
      std::size_t aligned_size = (payload_size + 3) & ~static_cast<std::size_t>(3);
      pad_count = static_cast<std::uint8_t>(aligned_size - payload_size);
    }

    RdmaOpcode opcode = get_send_opcode(is_first, is_last, has_immediate && is_last);

    RdmaPacketBuilder builder;
    builder.set_opcode(opcode)
        .set_dest_qp(qp.dest_qp_number())
        .set_psn(qp.next_send_psn())
        .set_pad_count(pad_count)
        .set_solicited_event(wqe.solicited && is_last)
        .set_ack_request(is_last)
        .set_payload(payload);

    if (has_immediate && is_last) {
      builder.set_immediate(wqe.immediate_data);
    }

    packets.push_back(builder.build());
    ++stats_.send_packets_generated;
    stats_.bytes_sent += payload_size;
    qp.record_packet_sent(packets.back().size());

    offset += payload_size;
  }

  // Add pending operation for reliability
  qp.add_pending_operation(wqe, num_packets);

  return packets;
}

RecvResult SendRecvProcessor::process_recv_packet(RdmaQueuePair& qp,
                                                  const RdmaPacketParser& parser) {
  NIC_TRACE_SCOPED(__func__);

  RecvResult result;
  const BthFields& bth = parser.bth();

  // Check if this is a SEND packet
  bool is_send = false;
  switch (bth.opcode) {
    case RdmaOpcode::kRcSendFirst:
    case RdmaOpcode::kRcSendMiddle:
    case RdmaOpcode::kRcSendLast:
    case RdmaOpcode::kRcSendLastImm:
    case RdmaOpcode::kRcSendOnly:
    case RdmaOpcode::kRcSendOnlyImm:
      is_send = true;
      break;
    default:
      return result;
  }

  if (!is_send) {
    return result;
  }

  // Check QP state
  if (!qp.can_receive()) {
    result.syndrome = AethSyndrome::InvalidRequest;
    result.needs_ack = true;
    result.ack_psn = bth.psn;
    return result;
  }

  // Check PSN
  std::uint32_t expected_psn = qp.expected_recv_psn();
  if (bth.psn != expected_psn) {
    result.syndrome = AethSyndrome::PsnSeqError;
    result.needs_ack = true;
    result.ack_psn = expected_psn;
    ++stats_.sequence_errors;
    NIC_LOGF_WARNING(
        "recv PSN mismatch: qp={} expected={} got={}", qp.qp_number(), expected_psn, bth.psn);
    return result;
  }

  // Get or create receiver state
  RecvMessageState& recv_state = recv_states_[qp.qp_number()];

  bool is_first = opcode_is_first(bth.opcode);
  bool is_only = opcode_is_only(bth.opcode);
  bool is_last = opcode_is_last(bth.opcode);

  // Handle first packet of a message
  if (is_first || is_only) {
    // Need a receive WQE
    std::optional<RecvWqe> recv_wqe = qp.consume_recv();
    if (!recv_wqe.has_value()) {
      result.syndrome = AethSyndrome::RnrNak;
      result.needs_ack = true;
      result.ack_psn = bth.psn;
      ++stats_.rnr_naks_sent;
      NIC_LOGF_WARNING("RNR: qp={} no recv WQE for incoming send", qp.qp_number());
      return result;
    }

    recv_state.wr_id = recv_wqe->wr_id;
    recv_state.sgl = recv_wqe->sgl;
    recv_state.bytes_received = 0;
    recv_state.expected_psn = bth.psn;
    recv_state.current_sge_idx = 0;
    recv_state.sge_offset = 0;
    recv_state.in_progress = true;
    recv_state.has_immediate = false;
    recv_state.immediate_data = 0;
  }

  // Validate we're in the right state
  if (!recv_state.in_progress) {
    result.syndrome = AethSyndrome::InvalidRequest;
    result.needs_ack = true;
    result.ack_psn = bth.psn;
    return result;
  }

  // Verify expected PSN for multi-packet messages
  if (!is_first && !is_only) {
    if (recv_state.expected_psn != bth.psn) {
      result.syndrome = AethSyndrome::PsnSeqError;
      result.needs_ack = true;
      result.ack_psn = recv_state.expected_psn;
      ++stats_.sequence_errors;
      return result;
    }
  }

  // Write payload to receive buffer
  std::span<const std::byte> payload = parser.payload();
  std::size_t bytes_written =
      write_to_sgl(recv_state.sgl, payload, recv_state.current_sge_idx, recv_state.sge_offset);

  if (bytes_written != payload.size()) {
    result.syndrome = AethSyndrome::RemoteAccessError;
    result.needs_ack = true;
    result.ack_psn = bth.psn;
    recv_state.in_progress = false;
    return result;
  }

  recv_state.bytes_received += static_cast<std::uint32_t>(bytes_written);
  recv_state.expected_psn = advance_psn(bth.psn);
  stats_.bytes_received += bytes_written;

  // Advance QP's expected PSN
  qp.advance_recv_psn();
  ++stats_.recv_packets_processed;
  qp.record_packet_received(bytes_written);

  // Handle immediate data
  if (parser.has_immediate()) {
    recv_state.has_immediate = true;
    recv_state.immediate_data = parser.immediate();
  }

  // Handle message completion
  if (is_last || is_only) {
    result.is_message_complete = true;
    recv_state.in_progress = false;

    // Generate completion
    RdmaCqe cqe;
    cqe.wr_id = recv_state.wr_id;
    cqe.status = WqeStatus::Success;
    cqe.opcode = recv_state.has_immediate ? WqeOpcode::SendImm : WqeOpcode::Send;
    cqe.qp_number = qp.qp_number();
    cqe.bytes_completed = recv_state.bytes_received;
    cqe.has_immediate = recv_state.has_immediate;
    cqe.immediate_data = recv_state.immediate_data;
    cqe.is_send = false;

    result.cqe = cqe;
    ++stats_.recvs_completed;
    NIC_LOGF_DEBUG("recv complete: qp={} wr_id={} bytes={}",
                   qp.qp_number(),
                   recv_state.wr_id,
                   recv_state.bytes_received);
  }

  // Send ACK if requested
  if (bth.ack_request || is_last || is_only) {
    result.needs_ack = true;
    result.ack_psn = bth.psn;
    result.syndrome = AethSyndrome::Ack;
  }

  result.success = true;
  return result;
}

std::vector<std::byte> SendRecvProcessor::generate_ack(const RdmaQueuePair& qp,
                                                       std::uint32_t psn,
                                                       AethSyndrome syndrome,
                                                       std::uint32_t msn) {
  NIC_TRACE_SCOPED(__func__);

  RdmaPacketBuilder builder;
  builder.set_opcode(RdmaOpcode::kRcAck)
      .set_dest_qp(qp.dest_qp_number())
      .set_psn(psn)
      .set_syndrome(syndrome)
      .set_msn(msn)
      .set_ack_request(false);

  return builder.build();
}

void SendRecvProcessor::reset() {
  NIC_TRACE_SCOPED(__func__);
  recv_states_.clear();
  stats_ = SendRecvStats{};
}

void SendRecvProcessor::clear_recv_state(std::uint32_t qp_number) {
  NIC_TRACE_SCOPED(__func__);
  recv_states_.erase(qp_number);
}

std::vector<std::byte> SendRecvProcessor::read_from_sgl(const std::vector<SglEntry>& sgl,
                                                        std::uint32_t lkey,
                                                        std::uint32_t /* pd_handle */) {
  NIC_TRACE_SCOPED(__func__);

  std::vector<std::byte> data;

  for (const auto& entry : sgl) {
    // Validate access
    if (!mr_table_.validate_lkey(lkey, entry.address, entry.length, false)) {
      return {};
    }

    // Read from host memory
    std::size_t old_size = data.size();
    data.resize(old_size + entry.length);
    std::span<std::byte> dest(data.data() + old_size, entry.length);
    if (!host_memory_.read(entry.address, dest).ok()) {
      return {};
    }
  }

  return data;
}

std::size_t SendRecvProcessor::write_to_sgl(const std::vector<SglEntry>& sgl,
                                            std::span<const std::byte> data,
                                            std::size_t& sge_idx,
                                            std::size_t& sge_offset) {
  NIC_TRACE_SCOPED(__func__);

  std::size_t total_written = 0;
  std::size_t data_offset = 0;

  while (data_offset < data.size() && sge_idx < sgl.size()) {
    const SglEntry& entry = sgl[sge_idx];
    std::size_t available = entry.length - sge_offset;
    std::size_t to_write = std::min(available, data.size() - data_offset);

    // Write to host memory
    HostAddress dest_addr = entry.address + sge_offset;
    std::span<const std::byte> src(data.data() + data_offset, to_write);
    if (!host_memory_.write(dest_addr, src).ok()) {
      return total_written;
    }

    total_written += to_write;
    data_offset += to_write;
    sge_offset += to_write;

    // Move to next SGE if current is full
    if (sge_offset >= entry.length) {
      ++sge_idx;
      sge_offset = 0;
    }
  }

  return total_written;
}

std::uint32_t SendRecvProcessor::calculate_packet_count(std::uint32_t total_length,
                                                        std::uint32_t mtu) const {
  NIC_TRACE_SCOPED(__func__);

  if (total_length == 0) {
    return 1;
  }
  return (total_length + mtu - 1) / mtu;
}

RdmaOpcode SendRecvProcessor::get_send_opcode(bool is_first,
                                              bool is_last,
                                              bool has_immediate) const {
  NIC_TRACE_SCOPED(__func__);

  if (is_first && is_last) {
    // Only packet
    return has_immediate ? RdmaOpcode::kRcSendOnlyImm : RdmaOpcode::kRcSendOnly;
  }
  if (is_first) {
    return RdmaOpcode::kRcSendFirst;
  }
  if (is_last) {
    return has_immediate ? RdmaOpcode::kRcSendLastImm : RdmaOpcode::kRcSendLast;
  }
  return RdmaOpcode::kRcSendMiddle;
}

}  // namespace nic::rocev2
