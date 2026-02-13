#include "nic/stats_collector.h"

#include "nic/log.h"
#include "nic/trace.h"

using namespace nic;

StatsCollector::StatsCollector() {
  NIC_TRACE_SCOPED(__func__);
}

void StatsCollector::record_tx_packet(std::uint16_t queue_id, std::uint64_t bytes) noexcept {
  auto& stats = get_queue_stats(queue_id);
  stats.tx_packets.fetch_add(1, std::memory_order_relaxed);
  stats.tx_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void StatsCollector::record_rx_packet(std::uint16_t queue_id, std::uint64_t bytes) noexcept {
  auto& stats = get_queue_stats(queue_id);
  stats.rx_packets.fetch_add(1, std::memory_order_relaxed);
  stats.rx_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void StatsCollector::record_error(std::uint16_t queue_id, ErrorType type) noexcept {
  NIC_TRACE_SCOPED(__func__);

  auto& stats = get_queue_stats(queue_id);

  NIC_LOGF_DEBUG("stats error: queue={} type={}", queue_id, static_cast<int>(type));

  switch (type) {
    case ErrorType::TxDescriptorError:
    case ErrorType::TxDMAError:
    case ErrorType::TxChecksumError:
      stats.tx_errors.fetch_add(1, std::memory_order_relaxed);
      break;

    case ErrorType::RxDescriptorError:
    case ErrorType::RxDMAError:
    case ErrorType::RxChecksumError:
    case ErrorType::RxDroppedFull:
      stats.rx_errors.fetch_add(1, std::memory_order_relaxed);
      break;
  }
}

void StatsCollector::record_vf_tx_packet(std::uint16_t vf_id, std::uint64_t bytes) noexcept {
  auto& stats = get_vf_stats(vf_id);
  stats.tx_packets.fetch_add(1, std::memory_order_relaxed);
  stats.tx_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void StatsCollector::record_vf_rx_packet(std::uint16_t vf_id, std::uint64_t bytes) noexcept {
  auto& stats = get_vf_stats(vf_id);
  stats.rx_packets.fetch_add(1, std::memory_order_relaxed);
  stats.rx_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void StatsCollector::record_vf_mailbox_message(std::uint16_t vf_id) noexcept {
  auto& stats = get_vf_stats(vf_id);
  stats.mailbox_messages.fetch_add(1, std::memory_order_relaxed);
}

StatsCollector::PortStats StatsCollector::port_stats() const noexcept {
  NIC_TRACE_SCOPED(__func__);

  PortStats port{};

  // Aggregate from all queues
  for (const auto& [queue_id, stats] : queue_stats_) {
    port.rx_bytes += stats.rx_bytes.load(std::memory_order_relaxed);
    port.rx_packets += stats.rx_packets.load(std::memory_order_relaxed);
    port.rx_errors += stats.rx_errors.load(std::memory_order_relaxed);
    port.tx_bytes += stats.tx_bytes.load(std::memory_order_relaxed);
    port.tx_packets += stats.tx_packets.load(std::memory_order_relaxed);
    port.tx_errors += stats.tx_errors.load(std::memory_order_relaxed);
  }

  return port;
}

const StatsCollector::QueueStats& StatsCollector::queue_stats(
    std::uint16_t queue_id) const noexcept {
  static const QueueStats empty_stats{};

  auto it = queue_stats_.find(queue_id);
  if (it == queue_stats_.end()) {
    return empty_stats;
  }

  return it->second;
}

const StatsCollector::VFStats& StatsCollector::vf_stats(std::uint16_t vf_id) const noexcept {
  static const VFStats empty_stats{};

  auto it = vf_stats_.find(vf_id);
  if (it == vf_stats_.end()) {
    return empty_stats;
  }

  return it->second;
}

void StatsCollector::reset_all() noexcept {
  NIC_TRACE_SCOPED(__func__);

  queue_stats_.clear();
  vf_stats_.clear();
}

void StatsCollector::reset_queue(std::uint16_t queue_id) noexcept {
  NIC_TRACE_SCOPED(__func__);

  auto it = queue_stats_.find(queue_id);
  if (it != queue_stats_.end()) {
    it->second.tx_bytes.store(0, std::memory_order_relaxed);
    it->second.tx_packets.store(0, std::memory_order_relaxed);
    it->second.tx_errors.store(0, std::memory_order_relaxed);
    it->second.rx_bytes.store(0, std::memory_order_relaxed);
    it->second.rx_packets.store(0, std::memory_order_relaxed);
    it->second.rx_errors.store(0, std::memory_order_relaxed);
  }
}

void StatsCollector::reset_vf(std::uint16_t vf_id) noexcept {
  NIC_TRACE_SCOPED(__func__);

  auto it = vf_stats_.find(vf_id);
  if (it != vf_stats_.end()) {
    it->second.tx_bytes.store(0, std::memory_order_relaxed);
    it->second.tx_packets.store(0, std::memory_order_relaxed);
    it->second.rx_bytes.store(0, std::memory_order_relaxed);
    it->second.rx_packets.store(0, std::memory_order_relaxed);
    it->second.mailbox_messages.store(0, std::memory_order_relaxed);
  }
}

StatsCollector::QueueStats& StatsCollector::get_queue_stats(std::uint16_t queue_id) noexcept {
  return queue_stats_[queue_id];
}

StatsCollector::VFStats& StatsCollector::get_vf_stats(std::uint16_t vf_id) noexcept {
  return vf_stats_[vf_id];
}