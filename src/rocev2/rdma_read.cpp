#include "nic/rocev2/rdma_read.h"

#include <algorithm>

#include "nic/log.h"

namespace nic::rocev2 {

ReadProcessor::ReadProcessor(HostMemory& host_memory, MemoryRegionTable& mr_table)
  : host_memory_(host_memory), mr_table_(mr_table) {
  NIC_TRACE_SCOPED(__func__);
}

std::vector<std::vector<std::byte>> ReadProcessor::generate_read_request(RdmaQueuePair& qp,
                                                                         const SendWqe& wqe) {
  NIC_TRACE_SCOPED(__func__);

  std::vector<std::vector<std::byte>> packets;

  // Validate WQE opcode
  if (wqe.opcode != WqeOpcode::RdmaRead) {
    return packets;
  }

  ++stats_.reads_started;
  NIC_LOGF_DEBUG("read request: qp={} remote_addr={:#x} rkey={:#x} len={}",
                 qp.qp_number(),
                 wqe.remote_address,
                 wqe.rkey,
                 wqe.total_length);

  // Get the PSN for this request (only call next_send_psn once!)
  std::uint32_t request_psn = qp.next_send_psn();

  // Create request state to track the outstanding read
  ReadRequestState& req_state = request_states_[qp.qp_number()];
  req_state.wr_id = wqe.wr_id;
  req_state.local_address = wqe.sgl.empty() ? 0 : wqe.sgl[0].address;
  req_state.local_lkey = wqe.local_lkey;
  req_state.total_length = wqe.total_length;
  req_state.bytes_received = 0;
  req_state.start_psn = request_psn;
  req_state.expected_psn = request_psn;  // Response starts at same PSN
  req_state.sgl = wqe.sgl;
  req_state.current_sge_idx = 0;
  req_state.sge_offset = 0;
  req_state.in_progress = true;

  // Build READ_REQUEST packet with RETH
  RdmaPacketBuilder builder;
  builder.set_opcode(RdmaOpcode::kRcReadRequest)
      .set_dest_qp(qp.dest_qp_number())
      .set_psn(request_psn)
      .set_ack_request(false)  // READ doesn't use ACK request bit
      .set_remote_address(wqe.remote_address)
      .set_rkey(wqe.rkey)
      .set_dma_length(wqe.total_length);

  packets.push_back(builder.build());
  ++stats_.read_requests_generated;

  // Add pending operation for reliability
  qp.add_pending_operation(wqe, 1);
  qp.record_packet_sent(packets.back().size());

  return packets;
}

ReadRequestResult ReadProcessor::process_read_request(RdmaQueuePair& qp,
                                                      const RdmaPacketParser& parser) {
  NIC_TRACE_SCOPED(__func__);

  ReadRequestResult result;
  const BthFields& bth = parser.bth();

  // Validate this is a READ_REQUEST
  if (bth.opcode != RdmaOpcode::kRcReadRequest) {
    return result;
  }

  // Check QP state
  if (!qp.can_receive()) {
    result.syndrome = AethSyndrome::InvalidRequest;
    result.needs_nak = true;
    result.nak_psn = bth.psn;
    return result;
  }

  // Check PSN
  std::uint32_t expected_psn = qp.expected_recv_psn();
  if (bth.psn != expected_psn) {
    result.syndrome = AethSyndrome::PsnSeqError;
    result.needs_nak = true;
    result.nak_psn = expected_psn;
    ++stats_.sequence_errors;
    return result;
  }

  // RETH must be present
  if (!parser.has_reth()) {
    result.syndrome = AethSyndrome::InvalidRequest;
    result.needs_nak = true;
    result.nak_psn = bth.psn;
    return result;
  }

  const RethFields& reth = parser.reth();

  // Validate rkey for remote read access
  if (!mr_table_.validate_rkey(
          reth.rkey, qp.pd_handle(), reth.virtual_address, reth.dma_length, false)) {
    result.syndrome = AethSyndrome::RemoteAccessError;
    result.needs_nak = true;
    result.nak_psn = bth.psn;
    ++stats_.rkey_errors;
    NIC_LOGF_WARNING("read rkey error: qp={} rkey={:#x} addr={:#x} len={}",
                     qp.qp_number(),
                     reth.rkey,
                     reth.virtual_address,
                     reth.dma_length);
    return result;
  }

  // Advance QP's expected PSN for the request
  qp.advance_recv_psn();

  // Generate response packets
  result.response_packets =
      generate_read_responses(qp, reth.virtual_address, reth.rkey, reth.dma_length, bth.psn);

  // Check if response generation succeeded
  if (result.response_packets.empty() && reth.dma_length > 0) {
    // This shouldn't happen if rkey validation above passed - indicates internal error
    result.syndrome = AethSyndrome::RemoteAccessError;
    result.needs_nak = true;
    result.nak_psn = bth.psn;
    ++stats_.access_errors;
    return result;
  }

  result.success = true;
  return result;
}

ReadResponseResult ReadProcessor::process_read_response(RdmaQueuePair& qp,
                                                        const RdmaPacketParser& parser) {
  NIC_TRACE_SCOPED(__func__);

  ReadResponseResult result;
  const BthFields& bth = parser.bth();

  // Validate this is a READ_RESPONSE
  bool is_response = false;
  switch (bth.opcode) {
    case RdmaOpcode::kRcReadResponseFirst:
    case RdmaOpcode::kRcReadResponseMiddle:
    case RdmaOpcode::kRcReadResponseLast:
    case RdmaOpcode::kRcReadResponseOnly:
      is_response = true;
      break;
    default:
      return result;
  }

  if (!is_response) {
    return result;
  }

  // Find the request state for this QP
  auto req_iter = request_states_.find(qp.qp_number());
  if ((req_iter == request_states_.end()) || !req_iter->second.in_progress) {
    // No outstanding read request
    return result;
  }

  ReadRequestState& req_state = req_iter->second;

  bool is_first = opcode_is_first(bth.opcode);
  bool is_only = opcode_is_only(bth.opcode);
  bool is_last = opcode_is_last(bth.opcode);

  // AETH is present in READ_RESPONSE (first and only packets)
  if (is_first || is_only) {
    if (!parser.has_aeth()) {
      return result;  // Invalid response format
    }

    const AethFields& aeth = parser.aeth();
    if (aeth.syndrome != AethSyndrome::Ack) {
      // NAK received - read failed
      RdmaCqe cqe;
      cqe.wr_id = req_state.wr_id;
      cqe.status = WqeStatus::RemoteAccessError;
      cqe.opcode = WqeOpcode::RdmaRead;
      cqe.qp_number = qp.qp_number();
      cqe.bytes_completed = 0;
      cqe.is_send = true;

      result.cqe = cqe;
      result.is_read_complete = true;
      req_state.in_progress = false;
      return result;
    }
  }

  // Write payload to local buffer using SGL
  std::span<const std::byte> payload = parser.payload();
  std::size_t bytes_written = write_to_sgl(req_state.sgl,
                                           payload,
                                           req_state.current_sge_idx,
                                           req_state.sge_offset,
                                           req_state.local_lkey);

  if (bytes_written != payload.size()) {
    // Local access error
    RdmaCqe cqe;
    cqe.wr_id = req_state.wr_id;
    cqe.status = WqeStatus::LocalAccessError;
    cqe.opcode = WqeOpcode::RdmaRead;
    cqe.qp_number = qp.qp_number();
    cqe.bytes_completed = req_state.bytes_received;
    cqe.is_send = true;

    result.cqe = cqe;
    result.is_read_complete = true;
    req_state.in_progress = false;
    ++stats_.access_errors;
    NIC_LOGF_WARNING("read local access error: qp={} bytes_written={} of {}",
                     qp.qp_number(),
                     bytes_written,
                     payload.size());
    return result;
  }

  req_state.bytes_received += static_cast<std::uint32_t>(bytes_written);
  req_state.expected_psn = advance_psn(bth.psn);
  stats_.bytes_read += bytes_written;
  ++stats_.read_responses_processed;
  qp.record_packet_received(bytes_written);

  // Handle read completion
  if (is_last || is_only) {
    result.is_read_complete = true;
    req_state.in_progress = false;
    ++stats_.reads_completed;
    NIC_LOGF_DEBUG("read complete: qp={} wr_id={} bytes={}",
                   qp.qp_number(),
                   req_state.wr_id,
                   req_state.bytes_received);

    // Generate success CQE
    RdmaCqe cqe;
    cqe.wr_id = req_state.wr_id;
    cqe.status = WqeStatus::Success;
    cqe.opcode = WqeOpcode::RdmaRead;
    cqe.qp_number = qp.qp_number();
    cqe.bytes_completed = req_state.bytes_received;
    cqe.is_send = true;

    result.cqe = cqe;
    NIC_LOGF_DEBUG("read complete: qp={} wr_id={} bytes={}",
                   qp.qp_number(),
                   req_state.wr_id,
                   req_state.bytes_received);
  }

  result.success = true;
  return result;
}

void ReadProcessor::reset() {
  NIC_TRACE_SCOPED(__func__);
  request_states_.clear();
  responder_states_.clear();
  stats_ = ReadStats{};
}

void ReadProcessor::clear_read_state(std::uint32_t qp_number) {
  NIC_TRACE_SCOPED(__func__);
  request_states_.erase(qp_number);
  responder_states_.erase(qp_number);
}

std::vector<std::byte> ReadProcessor::read_from_remote(std::uint64_t address,
                                                       std::uint32_t rkey,
                                                       std::uint32_t pd_handle,
                                                       std::size_t length) {
  NIC_TRACE_SCOPED(__func__);

  std::vector<std::byte> data;

  // Validate rkey for read access
  if (!mr_table_.validate_rkey(rkey, pd_handle, address, length, false)) {
    return data;
  }

  // Read from host memory
  data.resize(length);
  std::span<std::byte> dest(data.data(), length);
  if (!host_memory_.read(address, dest).ok()) {
    return {};
  }

  return data;
}

std::size_t ReadProcessor::write_to_sgl(const std::vector<SglEntry>& sgl,
                                        std::span<const std::byte> data,
                                        std::size_t& sge_idx,
                                        std::size_t& sge_offset,
                                        std::uint32_t lkey) {
  NIC_TRACE_SCOPED(__func__);

  std::size_t total_written = 0;
  std::size_t data_offset = 0;

  while (data_offset < data.size() && sge_idx < sgl.size()) {
    const SglEntry& entry = sgl[sge_idx];

    // Validate lkey for local write
    if (!mr_table_.validate_lkey(lkey,
                                 entry.address + sge_offset,
                                 std::min(entry.length - sge_offset, data.size() - data_offset),
                                 true)) {
      return total_written;
    }

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

std::vector<std::vector<std::byte>> ReadProcessor::generate_read_responses(
    RdmaQueuePair& qp,
    std::uint64_t address,
    std::uint32_t rkey,
    std::uint32_t length,
    std::uint32_t /* request_psn */) {
  NIC_TRACE_SCOPED(__func__);

  std::vector<std::vector<std::byte>> packets;

  // Read data from remote memory
  std::vector<std::byte> data = read_from_remote(address, rkey, qp.pd_handle(), length);
  if (data.size() != length) {
    // Read failed - would generate NAK, but that's handled in process_read_request
    return packets;
  }

  std::uint32_t mtu = qp.mtu_bytes();
  std::uint32_t num_packets = (length == 0) ? 1 : ((length + mtu - 1) / mtu);

  // Handle zero-length read
  if (length == 0) {
    RdmaPacketBuilder builder;
    builder.set_opcode(RdmaOpcode::kRcReadResponseOnly)
        .set_dest_qp(qp.dest_qp_number())
        .set_psn(qp.next_send_psn())
        .set_ack_request(false)
        .set_syndrome(AethSyndrome::Ack)
        .set_msn(0);

    packets.push_back(builder.build());
    ++stats_.read_responses_generated;
    qp.record_packet_sent(packets.back().size());

    return packets;
  }

  // Generate response packets with MTU segmentation
  std::size_t offset = 0;
  for (std::uint32_t pkt_idx = 0; pkt_idx < num_packets; ++pkt_idx) {
    bool is_first = (pkt_idx == 0);
    bool is_last = (pkt_idx == num_packets - 1);

    std::size_t payload_size = std::min(static_cast<std::size_t>(mtu), data.size() - offset);
    std::span<const std::byte> payload(data.data() + offset, payload_size);

    // Calculate padding for last packet
    std::uint8_t pad_count = 0;
    if (is_last) {
      std::size_t aligned_size = (payload_size + 3) & ~static_cast<std::size_t>(3);
      pad_count = static_cast<std::uint8_t>(aligned_size - payload_size);
    }

    RdmaOpcode opcode = get_read_response_opcode(is_first, is_last);

    RdmaPacketBuilder builder;
    builder.set_opcode(opcode)
        .set_dest_qp(qp.dest_qp_number())
        .set_psn(qp.next_send_psn())
        .set_pad_count(pad_count)
        .set_ack_request(false)
        .set_payload(payload);

    // AETH on first and only responses
    if (is_first || (is_first && is_last)) {
      builder.set_syndrome(AethSyndrome::Ack).set_msn(0);
    }

    packets.push_back(builder.build());
    ++stats_.read_responses_generated;
    qp.record_packet_sent(packets.back().size());

    offset += payload_size;
  }

  return packets;
}

RdmaOpcode ReadProcessor::get_read_response_opcode(bool is_first, bool is_last) const {
  NIC_TRACE_SCOPED(__func__);

  if (is_first && is_last) {
    return RdmaOpcode::kRcReadResponseOnly;
  }
  if (is_first) {
    return RdmaOpcode::kRcReadResponseFirst;
  }
  if (is_last) {
    return RdmaOpcode::kRcReadResponseLast;
  }
  return RdmaOpcode::kRcReadResponseMiddle;
}

}  // namespace nic::rocev2
