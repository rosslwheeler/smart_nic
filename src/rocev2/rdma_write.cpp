#include "nic/rocev2/rdma_write.h"

#include <algorithm>

#include "nic/log.h"

namespace nic::rocev2 {

WriteProcessor::WriteProcessor(HostMemory& host_memory, MemoryRegionTable& mr_table)
  : host_memory_(host_memory), mr_table_(mr_table) {
  NIC_TRACE_SCOPED(__func__);
}

std::vector<std::vector<std::byte>> WriteProcessor::generate_write_packets(RdmaQueuePair& qp,
                                                                           const SendWqe& wqe) {
  NIC_TRACE_SCOPED(__func__);

  std::vector<std::vector<std::byte>> packets;

  // Validate WQE opcode
  if ((wqe.opcode != WqeOpcode::RdmaWrite) && (wqe.opcode != WqeOpcode::RdmaWriteImm)) {
    return packets;
  }

  // Read data from local scatter-gather list
  std::vector<std::byte> data = read_from_sgl(wqe.sgl, wqe.local_lkey);
  if (data.size() != wqe.total_length) {
    return packets;
  }

  std::uint32_t mtu = qp.mtu_bytes();
  std::uint32_t num_packets = calculate_packet_count(wqe.total_length, mtu);
  bool has_immediate = (wqe.opcode == WqeOpcode::RdmaWriteImm);

  ++stats_.writes_started;
  NIC_LOGF_DEBUG("write: qp={} remote_addr={:#x} len={} packets={}",
                 qp.qp_number(),
                 wqe.remote_address,
                 wqe.total_length,
                 num_packets);

  // Handle zero-length write
  if (wqe.total_length == 0) {
    RdmaPacketBuilder builder;
    RdmaOpcode opcode = has_immediate ? RdmaOpcode::kRcWriteOnlyImm : RdmaOpcode::kRcWriteOnly;

    builder.set_opcode(opcode)
        .set_dest_qp(qp.dest_qp_number())
        .set_psn(qp.next_send_psn())
        .set_ack_request(true)
        .set_remote_address(wqe.remote_address)
        .set_rkey(wqe.rkey)
        .set_dma_length(0);

    if (has_immediate) {
      builder.set_immediate(wqe.immediate_data);
    }

    packets.push_back(builder.build());
    ++stats_.write_packets_generated;

    qp.add_pending_operation(wqe, 1);
    qp.record_packet_sent(packets.back().size());

    return packets;
  }

  // Generate packets with MTU segmentation
  std::size_t offset = 0;
  [[maybe_unused]] std::uint64_t current_remote_addr = wqe.remote_address;

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

    RdmaOpcode opcode = get_write_opcode(is_first, is_last, has_immediate && is_last);

    RdmaPacketBuilder builder;
    builder.set_opcode(opcode)
        .set_dest_qp(qp.dest_qp_number())
        .set_psn(qp.next_send_psn())
        .set_pad_count(pad_count)
        .set_ack_request(is_last)
        .set_payload(payload);

    // RETH only on first packet
    if (is_first) {
      builder.set_remote_address(wqe.remote_address)
          .set_rkey(wqe.rkey)
          .set_dma_length(wqe.total_length);
    }

    // Immediate data only on last packet
    if (has_immediate && is_last) {
      builder.set_immediate(wqe.immediate_data);
    }

    packets.push_back(builder.build());
    ++stats_.write_packets_generated;
    stats_.bytes_written += payload_size;
    qp.record_packet_sent(packets.back().size());

    offset += payload_size;
    current_remote_addr += payload_size;
  }

  // Add pending operation for reliability
  qp.add_pending_operation(wqe, num_packets);

  return packets;
}

WriteResult WriteProcessor::process_write_packet(RdmaQueuePair& qp,
                                                 const RdmaPacketParser& parser) {
  NIC_TRACE_SCOPED(__func__);

  WriteResult result;
  const BthFields& bth = parser.bth();

  // Check if this is a WRITE packet
  bool is_write = false;
  switch (bth.opcode) {
    case RdmaOpcode::kRcWriteFirst:
    case RdmaOpcode::kRcWriteMiddle:
    case RdmaOpcode::kRcWriteLast:
    case RdmaOpcode::kRcWriteLastImm:
    case RdmaOpcode::kRcWriteOnly:
    case RdmaOpcode::kRcWriteOnlyImm:
      is_write = true;
      break;
    default:
      return result;
  }

  if (!is_write) {
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
        "write PSN mismatch: qp={} expected={} got={}", qp.qp_number(), expected_psn, bth.psn);
    return result;
  }

  // Get or create write state
  WriteMessageState& write_state = write_states_[qp.qp_number()];

  bool is_first = opcode_is_first(bth.opcode);
  bool is_only = opcode_is_only(bth.opcode);
  bool is_last = opcode_is_last(bth.opcode);

  // Handle first packet of a write
  if (is_first || is_only) {
    // RETH must be present
    if (!parser.has_reth()) {
      result.syndrome = AethSyndrome::InvalidRequest;
      result.needs_ack = true;
      result.ack_psn = bth.psn;
      return result;
    }

    const RethFields& reth = parser.reth();

    // Validate rkey
    if (!mr_table_.validate_rkey(
            reth.rkey, qp.pd_handle(), reth.virtual_address, reth.dma_length, true)) {
      result.syndrome = AethSyndrome::RemoteAccessError;
      result.needs_ack = true;
      result.ack_psn = bth.psn;
      ++stats_.rkey_errors;
      NIC_LOGF_WARNING("write rkey error: qp={} rkey={:#x} addr={:#x}",
                       qp.qp_number(),
                       reth.rkey,
                       reth.virtual_address);
      return result;
    }

    write_state.remote_address = reth.virtual_address;
    write_state.rkey = reth.rkey;
    write_state.total_length = reth.dma_length;
    write_state.bytes_written = 0;
    write_state.expected_psn = bth.psn;
    write_state.in_progress = true;
    write_state.has_immediate = false;
    write_state.immediate_data = 0;
  }

  // Validate we're in the right state
  if (!write_state.in_progress) {
    result.syndrome = AethSyndrome::InvalidRequest;
    result.needs_ack = true;
    result.ack_psn = bth.psn;
    return result;
  }

  // Verify expected PSN for multi-packet writes
  if (!is_first && !is_only) {
    if (write_state.expected_psn != bth.psn) {
      result.syndrome = AethSyndrome::PsnSeqError;
      result.needs_ack = true;
      result.ack_psn = write_state.expected_psn;
      ++stats_.sequence_errors;
      return result;
    }
  }

  // Write payload to remote memory
  std::span<const std::byte> payload = parser.payload();
  std::uint64_t write_addr = write_state.remote_address + write_state.bytes_written;

  if (!payload.empty()) {
    if (!write_to_remote(write_addr, write_state.rkey, qp.pd_handle(), payload)) {
      result.syndrome = AethSyndrome::RemoteAccessError;
      result.needs_ack = true;
      result.ack_psn = bth.psn;
      write_state.in_progress = false;
      ++stats_.access_errors;
      return result;
    }
  }

  write_state.bytes_written += static_cast<std::uint32_t>(payload.size());
  write_state.expected_psn = advance_psn(bth.psn);

  // Advance QP's expected PSN
  qp.advance_recv_psn();
  ++stats_.write_packets_processed;
  qp.record_packet_received(payload.size());

  // Handle immediate data
  if (parser.has_immediate()) {
    write_state.has_immediate = true;
    write_state.immediate_data = parser.immediate();
  }

  // Handle write completion
  if (is_last || is_only) {
    result.is_message_complete = true;
    write_state.in_progress = false;
    ++stats_.writes_completed;
    NIC_LOGF_DEBUG("write complete: qp={} bytes={}", qp.qp_number(), write_state.bytes_written);

    // WRITE with immediate consumes a recv WQE and generates a CQE
    if (write_state.has_immediate) {
      std::optional<RecvWqe> recv_wqe = qp.consume_recv();
      if (!recv_wqe.has_value()) {
        // RNR NAK - no recv WQE for immediate
        result.syndrome = AethSyndrome::RnrNak;
        result.needs_ack = true;
        result.ack_psn = bth.psn;
        return result;
      }

      RdmaCqe cqe;
      cqe.wr_id = recv_wqe->wr_id;
      cqe.status = WqeStatus::Success;
      cqe.opcode = WqeOpcode::RdmaWriteImm;
      cqe.qp_number = qp.qp_number();
      cqe.bytes_completed = write_state.bytes_written;
      cqe.has_immediate = true;
      cqe.immediate_data = write_state.immediate_data;
      cqe.is_send = false;

      result.recv_cqe = cqe;
    }
  }

  // Send ACK if requested or on last packet
  if (bth.ack_request || is_last || is_only) {
    result.needs_ack = true;
    result.ack_psn = bth.psn;
    result.syndrome = AethSyndrome::Ack;
  }

  result.success = true;
  return result;
}

void WriteProcessor::reset() {
  NIC_TRACE_SCOPED(__func__);
  write_states_.clear();
  stats_ = WriteStats{};
}

void WriteProcessor::clear_write_state(std::uint32_t qp_number) {
  NIC_TRACE_SCOPED(__func__);
  write_states_.erase(qp_number);
}

std::vector<std::byte> WriteProcessor::read_from_sgl(const std::vector<SglEntry>& sgl,
                                                     std::uint32_t lkey) {
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

bool WriteProcessor::write_to_remote(std::uint64_t address,
                                     std::uint32_t rkey,
                                     std::uint32_t pd_handle,
                                     std::span<const std::byte> data) {
  NIC_TRACE_SCOPED(__func__);

  // Validate rkey for the write
  if (!mr_table_.validate_rkey(rkey, pd_handle, address, data.size(), true)) {
    return false;
  }

  // Write to host memory
  return host_memory_.write(address, data).ok();
}

std::uint32_t WriteProcessor::calculate_packet_count(std::uint32_t total_length,
                                                     std::uint32_t mtu) const {
  NIC_TRACE_SCOPED(__func__);

  if (total_length == 0) {
    return 1;
  }
  return (total_length + mtu - 1) / mtu;
}

RdmaOpcode WriteProcessor::get_write_opcode(bool is_first, bool is_last, bool has_immediate) const {
  NIC_TRACE_SCOPED(__func__);

  if (is_first && is_last) {
    return has_immediate ? RdmaOpcode::kRcWriteOnlyImm : RdmaOpcode::kRcWriteOnly;
  }
  if (is_first) {
    return RdmaOpcode::kRcWriteFirst;
  }
  if (is_last) {
    return has_immediate ? RdmaOpcode::kRcWriteLastImm : RdmaOpcode::kRcWriteLast;
  }
  return RdmaOpcode::kRcWriteMiddle;
}

}  // namespace nic::rocev2
