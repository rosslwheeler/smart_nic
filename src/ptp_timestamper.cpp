#include "nic/ptp_timestamper.h"

#include "nic/trace.h"

using namespace nic;

PTPTimestamper::PTPTimestamper(PTPClock& clock) : clock_(clock) {
  NIC_TRACE_SCOPED(__func__);
}

std::uint64_t PTPTimestamper::timestamp_tx_packet(std::uint16_t queue_id) noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (!tx_timestamping_enabled(queue_id)) {
    return 0;
  }

  std::uint64_t timestamp = clock_.read_time_ns();
  stats_.tx_timestamps += 1;
  return timestamp;
}

std::uint64_t PTPTimestamper::timestamp_rx_packet(std::uint16_t queue_id) noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (!rx_timestamping_enabled(queue_id)) {
    return 0;
  }

  std::uint64_t timestamp = clock_.read_time_ns();
  stats_.rx_timestamps += 1;
  return timestamp;
}

bool PTPTimestamper::is_ptp_packet(std::span<const std::byte> packet) const noexcept {
  NIC_TRACE_SCOPED(__func__);

  // Simple heuristic: Check for PTP over Ethernet (EtherType 0x88F7)
  // or PTP over UDP (port 319 or 320)

  if (packet.size() < 14) {
    return false;  // Too small for Ethernet header
  }

  // Check EtherType at bytes 12-13
  std::uint8_t ethertype_hi = static_cast<std::uint8_t>(packet[12]);
  std::uint8_t ethertype_lo = static_cast<std::uint8_t>(packet[13]);
  std::uint16_t ethertype = (static_cast<std::uint16_t>(ethertype_hi) << 8) | ethertype_lo;

  // PTP over Ethernet: EtherType 0x88F7
  if (ethertype == 0x88F7) {
    return true;
  }

  // Could add UDP port checks here for PTP over IPv4/IPv6
  // For now, keep it simple

  return false;
}

void PTPTimestamper::enable_tx_timestamping(std::uint16_t queue_id, bool enable) noexcept {
  NIC_TRACE_SCOPED(__func__);
  tx_enabled_[queue_id] = enable;
}

void PTPTimestamper::enable_rx_timestamping(std::uint16_t queue_id, bool enable) noexcept {
  NIC_TRACE_SCOPED(__func__);
  rx_enabled_[queue_id] = enable;
}

bool PTPTimestamper::tx_timestamping_enabled(std::uint16_t queue_id) const noexcept {
  auto it = tx_enabled_.find(queue_id);
  return it != tx_enabled_.end() && it->second;
}

bool PTPTimestamper::rx_timestamping_enabled(std::uint16_t queue_id) const noexcept {
  auto it = rx_enabled_.find(queue_id);
  return it != rx_enabled_.end() && it->second;
}