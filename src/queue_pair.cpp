#include "nic/queue_pair.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>

#include "nic/checksum.h"
#include "nic/log.h"
#include "nic/trace.h"

using namespace nic;

QueuePair::QueuePair(QueuePairConfig config, DMAEngine& dma_engine)
  : config_(config), dma_engine_(dma_engine) {
  NIC_TRACE_SCOPED(__func__);

  tx_ring_ = std::make_unique<DescriptorRing>(config_.tx_ring, dma_engine_, config_.tx_doorbell);
  rx_ring_ = std::make_unique<DescriptorRing>(config_.rx_ring, dma_engine_, config_.rx_doorbell);

  tx_completion_ =
      std::make_unique<CompletionQueue>(config_.tx_completion, config_.tx_completion_doorbell);
  rx_completion_ =
      std::make_unique<CompletionQueue>(config_.rx_completion, config_.rx_completion_doorbell);
}

DescriptorRing& QueuePair::tx_ring() noexcept {
  NIC_TRACE_SCOPED(__func__);
  return *tx_ring_;
}

DescriptorRing& QueuePair::rx_ring() noexcept {
  NIC_TRACE_SCOPED(__func__);
  return *rx_ring_;
}

CompletionQueue& QueuePair::tx_completion() noexcept {
  NIC_TRACE_SCOPED(__func__);
  return *tx_completion_;
}

CompletionQueue& QueuePair::rx_completion() noexcept {
  NIC_TRACE_SCOPED(__func__);
  return *rx_completion_;
}

const DescriptorRing& QueuePair::tx_ring() const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return *tx_ring_;
}

const DescriptorRing& QueuePair::rx_ring() const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return *rx_ring_;
}

const CompletionQueue& QueuePair::tx_completion() const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return *tx_completion_;
}

const CompletionQueue& QueuePair::rx_completion() const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return *rx_completion_;
}

bool QueuePair::process_once() {
  NIC_TRACE_SCOPED(__func__);
  if (tx_ring_->is_empty()) {
    return false;
  }

  std::vector<std::byte> tx_bytes(config_.tx_ring.descriptor_size);
  if (!tx_ring_->pop_descriptor(tx_bytes).ok()) {
    trace_dma_error(DmaError::AccessError, "tx_pop_failed");
    return false;
  }

  TxDescriptor tx_desc{};
  if (!decode_tx_descriptor(tx_bytes, tx_desc)) {
    trace_dma_error(DmaError::AccessError, "tx_decode_failed");
    return false;
  }

  // Need an RX descriptor to deliver.
  if (rx_ring_->is_empty()) {
    CompletionEntry tx_entry =
        make_tx_completion(tx_desc, CompletionCode::NoDescriptor, 0, false, false);
    tx_completion_->post_completion(tx_entry);
    fire_tx_interrupt(tx_entry);
    stats_.drops_no_rx_desc += 1;
    NIC_LOGF_WARNING("tx drop: qp={} no RX descriptor", config_.queue_id);
    return true;
  }

  // DMA read TX buffer.
  std::vector<std::byte> packet(tx_desc.length);
  if (!dma_engine_.read(tx_desc.buffer_address, packet).ok()) {
    CompletionEntry tx_entry = make_tx_completion(tx_desc, CompletionCode::Fault, 0, false, false);
    tx_completion_->post_completion(tx_entry);
    fire_tx_interrupt(tx_entry);
    return true;
  }

  if (!tx_desc.checksum_offload && (tx_desc.checksum != ChecksumMode::None)) {
    std::uint16_t computed = compute_checksum(packet);
    if (computed != tx_desc.checksum_value) {
      CompletionEntry tx_entry =
          make_tx_completion(tx_desc, CompletionCode::ChecksumError, 0, false, false);
      tx_completion_->post_completion(tx_entry);
      fire_tx_interrupt(tx_entry);
      stats_.drops_checksum += 1;
      NIC_LOGF_WARNING("tx drop: qp={} checksum error", config_.queue_id);
      return true;
    }
  }

  return process_segments(tx_desc, packet);
}

void QueuePair::reset() {
  NIC_TRACE_SCOPED(__func__);
  tx_ring_->reset();
  rx_ring_->reset();
  tx_completion_->reset();
  rx_completion_->reset();
  reset_stats();
}

bool QueuePair::decode_tx_descriptor(std::span<const std::byte> bytes,
                                     TxDescriptor& out) const noexcept {
  NIC_TRACE_SCOPED(__func__);
  if (bytes.size() < sizeof(TxDescriptor)) {
    return false;
  }
  std::memcpy(static_cast<void*>(&out), bytes.data(), sizeof(TxDescriptor));
  return true;
}

bool QueuePair::decode_rx_descriptor(std::span<const std::byte> bytes,
                                     RxDescriptor& out) const noexcept {
  NIC_TRACE_SCOPED(__func__);
  if (bytes.size() < sizeof(RxDescriptor)) {
    return false;
  }
  std::memcpy(static_cast<void*>(&out), bytes.data(), sizeof(RxDescriptor));
  return true;
}

CompletionEntry QueuePair::make_completion(std::uint16_t descriptor_index,
                                           CompletionCode status) const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return CompletionEntry{
      .queue_id = config_.queue_id,
      .descriptor_index = descriptor_index,
      .status = static_cast<std::uint32_t>(status),
  };
}

CompletionEntry QueuePair::make_tx_completion(const TxDescriptor& tx_desc,
                                              CompletionCode status,
                                              std::size_t segments_count,
                                              bool performed_tso,
                                              bool performed_gso) const noexcept {
  NIC_TRACE_SCOPED(__func__);
  CompletionEntry entry = make_completion(tx_desc.descriptor_index, status);
  entry.checksum_offloaded = tx_desc.checksum_offload;
  entry.tso_performed = performed_tso;
  entry.gso_performed = performed_gso;
  entry.segments_produced = static_cast<std::uint16_t>(
      std::min<std::size_t>(segments_count, std::numeric_limits<std::uint16_t>::max()));
  if (tx_desc.vlan_insert) {
    entry.vlan_inserted = true;
    entry.vlan_tag = tx_desc.vlan_tag;
  }
  return entry;
}

std::string QueuePair::stats_summary() const {
  NIC_TRACE_SCOPED(__func__);
  std::ostringstream oss;
  oss << "qp=" << config_.queue_id << " tx_pkts=" << stats_.tx_packets
      << " rx_pkts=" << stats_.rx_packets << " tx_bytes=" << stats_.tx_bytes
      << " rx_bytes=" << stats_.rx_bytes << " drops_csum=" << stats_.drops_checksum
      << " drops_no_rx_desc=" << stats_.drops_no_rx_desc
      << " drops_buf_small=" << stats_.drops_buffer_small
      << " drops_mtu=" << stats_.drops_mtu_exceeded << " drops_mss=" << stats_.drops_invalid_mss
      << " drops_tso_segs=" << stats_.drops_too_many_segments
      << " tx_tso_segs=" << stats_.tx_tso_segments << " tx_gso_segs=" << stats_.tx_gso_segments
      << " tx_vlan_ins=" << stats_.tx_vlan_insertions << " rx_vlan_strip=" << stats_.rx_vlan_strips
      << " rx_csum_ver=" << stats_.rx_checksum_verified << " rx_gro=" << stats_.rx_gro_aggregated;
  return oss.str();
}

bool QueuePair::validate_mtu(const TxDescriptor& tx_desc, const std::vector<std::byte>& packet) {
  NIC_TRACE_SCOPED(__func__);
  if (packet.size() > config_.max_mtu) {
    CompletionEntry tx_entry =
        make_tx_completion(tx_desc, CompletionCode::MtuExceeded, 0, false, false);
    tx_completion_->post_completion(tx_entry);
    fire_tx_interrupt(tx_entry);
    stats_.drops_mtu_exceeded += 1;
    NIC_LOGF_WARNING("tx drop: qp={} MTU exceeded (pkt={} mtu={})",
                     config_.queue_id,
                     packet.size(),
                     config_.max_mtu);
    return false;
  }
  return true;
}

std::optional<std::vector<std::vector<std::byte>>> QueuePair::build_segments(
    const TxDescriptor& tx_desc, const std::vector<std::byte>& packet) {
  NIC_TRACE_SCOPED(__func__);

  std::vector<std::vector<std::byte>> segments;
  const bool segmentation_enabled = (tx_desc.tso_enabled || tx_desc.gso_enabled) && tx_desc.mss > 0
                                    && packet.size() > tx_desc.mss;

  if (!segmentation_enabled) {
    segments.emplace_back(packet);
    return segments;
  }

  // Validate MSS
  if ((tx_desc.mss < kMinMss) || (tx_desc.mss > kMaxMss)) {
    CompletionEntry tx_entry =
        make_tx_completion(tx_desc, CompletionCode::InvalidMss, 0, false, false);
    tx_completion_->post_completion(tx_entry);
    fire_tx_interrupt(tx_entry);
    stats_.drops_invalid_mss += 1;
    return std::nullopt;
  }

  // Validate header length
  if (tx_desc.header_length > packet.size()) {
    CompletionEntry tx_entry =
        make_tx_completion(tx_desc, CompletionCode::InvalidMss, 0, false, false);
    tx_completion_->post_completion(tx_entry);
    fire_tx_interrupt(tx_entry);
    stats_.drops_invalid_mss += 1;
    return std::nullopt;
  }

  const std::size_t header_len = std::min<std::size_t>(tx_desc.header_length, packet.size());
  const std::vector<std::byte> header(packet.begin(),
                                      packet.begin() + static_cast<std::ptrdiff_t>(header_len));
  std::size_t payload_offset = header_len;

  if (payload_offset >= packet.size()) {
    // Degenerate: header covers entire buffer; no segmentation.
    segments.emplace_back(packet);
  } else {
    while (payload_offset < packet.size()) {
      const std::size_t chunk = std::min<std::size_t>(tx_desc.mss, packet.size() - payload_offset);
      std::vector<std::byte> segment;
      segment.reserve(header.size() + chunk);
      segment.insert(segment.end(), header.begin(), header.end());
      segment.insert(segment.end(),
                     packet.begin() + static_cast<std::ptrdiff_t>(payload_offset),
                     packet.begin() + static_cast<std::ptrdiff_t>(payload_offset + chunk));
      segments.emplace_back(std::move(segment));
      payload_offset += chunk;
    }
  }

  // Check for too many segments
  if (segments.size() > kMaxTsoSegments) {
    CompletionEntry tx_entry =
        make_tx_completion(tx_desc, CompletionCode::TooManySegments, 0, false, false);
    tx_completion_->post_completion(tx_entry);
    fire_tx_interrupt(tx_entry);
    stats_.drops_too_many_segments += 1;
    return std::nullopt;
  }

  return segments;
}

void QueuePair::finalize_tx_success(const TxDescriptor& tx_desc,
                                    std::size_t total_segments,
                                    std::size_t packet_bytes,
                                    bool performed_tso,
                                    bool performed_gso) {
  NIC_TRACE_SCOPED(__func__);
  CompletionEntry tx_entry = make_tx_completion(
      tx_desc, CompletionCode::Success, total_segments, performed_tso, performed_gso);
  tx_completion_->post_completion(tx_entry);
  fire_tx_interrupt(tx_entry);
  stats_.tx_packets += total_segments;
  stats_.tx_bytes += packet_bytes;
  if (performed_tso) {
    stats_.tx_tso_segments += total_segments;
  }
  if (performed_gso) {
    stats_.tx_gso_segments += total_segments;
  }
  if (tx_desc.vlan_insert) {
    stats_.tx_vlan_insertions += total_segments;
  }
}

bool QueuePair::process_segments(const TxDescriptor& tx_desc,
                                 const std::vector<std::byte>& packet) {
  NIC_TRACE_SCOPED(__func__);

  if (!validate_mtu(tx_desc, packet)) {
    return true;
  }

  auto maybe_segments = build_segments(tx_desc, packet);
  if (!maybe_segments) {
    return true;
  }
  auto& segments = *maybe_segments;

  const std::size_t total_segments = segments.size();
  const bool performed_tso = tx_desc.tso_enabled && total_segments > 1;
  const bool performed_gso = tx_desc.gso_enabled && total_segments > 1;

  if (rx_ring_->available() < segments.size()) {
    CompletionEntry tx_entry =
        make_tx_completion(tx_desc, CompletionCode::NoDescriptor, 0, performed_tso, performed_gso);
    tx_completion_->post_completion(tx_entry);
    fire_tx_interrupt(tx_entry);
    stats_.drops_no_rx_desc += 1;
    NIC_LOGF_WARNING("tx drop: qp={} insufficient RX descriptors (need={} avail={})",
                     config_.queue_id,
                     segments.size(),
                     rx_ring_->available());
    return true;
  }

  for (const auto& base_segment : segments) {
    std::vector<std::byte> rx_bytes(config_.rx_ring.descriptor_size);
    if (!rx_ring_->pop_descriptor(rx_bytes).ok()) {
      trace_dma_error(DmaError::AccessError, "rx_pop_failed");
      return false;
    }

    RxDescriptor rx_desc{};
    if (!decode_rx_descriptor(rx_bytes, rx_desc)) {
      trace_dma_error(DmaError::AccessError, "rx_decode_failed");
      return false;
    }
    // Propagate VLAN presence when TX inserts a tag so RX strip logic is deterministic.
    if (tx_desc.vlan_insert) {
      rx_desc.vlan_present = true;
    }

    std::vector<std::byte> segment = base_segment;
    if (tx_desc.vlan_insert) {
      std::vector<std::byte> vlan_header(4);
      vlan_header[0] = std::byte{0x81};
      vlan_header[1] = std::byte{0x00};
      vlan_header[2] = static_cast<std::byte>((tx_desc.vlan_tag >> 8) & 0xFF);
      vlan_header[3] = static_cast<std::byte>(tx_desc.vlan_tag & 0xFF);
      segment.insert(segment.begin(), vlan_header.begin(), vlan_header.end());
    }

    if (!handle_rx_segment(
            std::move(segment), tx_desc, rx_desc, total_segments, performed_tso, performed_gso)) {
      return true;
    }
  }

  finalize_tx_success(tx_desc, total_segments, packet.size(), performed_tso, performed_gso);
  return true;
}

void QueuePair::fire_tx_interrupt(const CompletionEntry& entry) noexcept {
  NIC_TRACE_SCOPED(__func__);
  if (config_.enable_tx_interrupts && config_.interrupt_dispatcher != nullptr) {
    config_.interrupt_dispatcher->on_completion(InterruptEvent{config_.queue_id, entry});
  }
}

void QueuePair::fire_rx_interrupt(const CompletionEntry& entry) noexcept {
  NIC_TRACE_SCOPED(__func__);
  if (config_.enable_rx_interrupts && config_.interrupt_dispatcher != nullptr) {
    config_.interrupt_dispatcher->on_completion(InterruptEvent{config_.queue_id, entry});
  }
}

bool QueuePair::handle_rx_segment(std::vector<std::byte> segment,
                                  const TxDescriptor& tx_desc,
                                  RxDescriptor& rx_desc,
                                  std::size_t total_segments,
                                  bool performed_tso,
                                  bool performed_gso) {
  NIC_TRACE_SCOPED(__func__);
  const bool segment_has_vlan = tx_desc.vlan_insert || rx_desc.vlan_present;
  if (rx_desc.vlan_strip && segment_has_vlan && segment.size() >= 4) {
    segment.erase(segment.begin(), segment.begin() + 4);
  }

  if (rx_desc.buffer_length < segment.size()) {
    tx_completion_->post_completion(make_tx_completion(
        tx_desc, CompletionCode::Success, total_segments, performed_tso, performed_gso));
    CompletionEntry rx_entry =
        make_completion(rx_desc.descriptor_index, CompletionCode::BufferTooSmall);
    rx_entry.vlan_stripped = rx_desc.vlan_strip && segment_has_vlan;
    if (rx_entry.vlan_stripped) {
      rx_entry.vlan_tag = tx_desc.vlan_insert ? tx_desc.vlan_tag : rx_desc.vlan_tag;
    }
    rx_completion_->post_completion(rx_entry);
    fire_rx_interrupt(rx_entry);
    stats_.drops_buffer_small += 1;
    NIC_LOGF_WARNING("rx drop: qp={} buffer too small (seg={} buf={})",
                     config_.queue_id,
                     segment.size(),
                     rx_desc.buffer_length);
    return false;
  }

  if (!dma_engine_
           .write(rx_desc.buffer_address,
                  std::span<const std::byte>{segment.data(), segment.size()})
           .ok()) {
    tx_completion_->post_completion(make_tx_completion(
        tx_desc, CompletionCode::Fault, total_segments, performed_tso, performed_gso));
    CompletionEntry rx_entry = make_completion(rx_desc.descriptor_index, CompletionCode::Fault);
    rx_completion_->post_completion(rx_entry);
    fire_rx_interrupt(rx_entry);
    return false;
  }

  CompletionEntry rx_entry = make_completion(rx_desc.descriptor_index, CompletionCode::Success);
  rx_entry.gro_aggregated = rx_desc.gro_enabled;
  if (rx_entry.gro_aggregated) {
    stats_.rx_gro_aggregated += 1;
  }

  if (rx_desc.checksum_offload && rx_desc.checksum != ChecksumMode::None) {
    rx_entry.checksum_verified = true;
    stats_.rx_checksum_verified += 1;
    std::uint16_t rx_checksum = compute_checksum(segment);
    if (rx_checksum != 0) {
      rx_entry.status = static_cast<std::uint32_t>(CompletionCode::ChecksumError);
      rx_completion_->post_completion(rx_entry);
      fire_rx_interrupt(rx_entry);
      tx_completion_->post_completion(make_tx_completion(
          tx_desc, CompletionCode::Success, total_segments, performed_tso, performed_gso));
      stats_.drops_checksum += 1;
      return false;
    }
  }

  rx_entry.vlan_stripped = rx_desc.vlan_strip && segment_has_vlan;
  if (rx_entry.vlan_stripped) {
    rx_entry.vlan_tag = tx_desc.vlan_insert ? tx_desc.vlan_tag : rx_desc.vlan_tag;
    stats_.rx_vlan_strips += 1;
  }

  rx_completion_->post_completion(rx_entry);
  fire_rx_interrupt(rx_entry);
  stats_.rx_packets += 1;
  stats_.rx_bytes += segment.size();
  return true;
}
