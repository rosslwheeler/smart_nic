#include "nic/queue_manager.h"

#include <sstream>

#include "nic/log.h"
#include "nic/trace.h"

using namespace nic;

QueueManager::QueueManager(QueueManagerConfig config, DMAEngine& dma_engine)
  : config_(std::move(config)), dma_engine_(dma_engine) {
  NIC_TRACE_SCOPED(__func__);
  queue_pairs_.reserve(config_.queue_configs.size());
  for (auto& qp_cfg : config_.queue_configs) {
    if (qp_cfg.weight == 0) {
      qp_cfg.weight = 1;
    }
    weights_.push_back(qp_cfg.weight);
    queue_pairs_.push_back(std::make_unique<QueuePair>(qp_cfg, dma_engine_));
  }
  if (queue_pairs_.empty()) {
    scheduler_credit_ = 0;
  } else {
    scheduler_credit_ = weights_.front();
  }
  scheduler_advances_ = 0;
  scheduler_skips_ = 0;
}

QueuePair* QueueManager::queue(std::size_t index) noexcept {
  NIC_TRACE_SCOPED(__func__);
  if (index >= queue_pairs_.size()) {
    return nullptr;
  }
  return queue_pairs_[index].get();
}

const QueuePair* QueueManager::queue(std::size_t index) const noexcept {
  NIC_TRACE_SCOPED(__func__);
  if (index >= queue_pairs_.size()) {
    return nullptr;
  }
  return queue_pairs_[index].get();
}

std::optional<QueuePairStats> QueueManager::queue_stats(std::size_t index) const noexcept {
  NIC_TRACE_SCOPED(__func__);
  if (index >= queue_pairs_.size()) {
    return std::nullopt;
  }
  return queue_pairs_[index]->stats();
}

bool QueueManager::process_once() {
  NIC_TRACE_SCOPED(__func__);
  if (queue_pairs_.empty()) {
    return false;
  }

  for (std::size_t i = 0; i < queue_pairs_.size(); ++i) {
    auto& qp = queue_pairs_[scheduler_index_];
    if (qp->process_once()) {
      if (scheduler_credit_ > 1) {
        --scheduler_credit_;
      } else {
        scheduler_index_ = (scheduler_index_ + 1) % queue_pairs_.size();
        scheduler_credit_ = weights_[scheduler_index_];
      }
      ++scheduler_advances_;
      return true;
    }
    // Skip blocked queue; move on.
    ++scheduler_skips_;
    scheduler_index_ = (scheduler_index_ + 1) % queue_pairs_.size();
    scheduler_credit_ = weights_[scheduler_index_];
  }
  return false;
}

void QueueManager::reset() {
  NIC_TRACE_SCOPED(__func__);
  scheduler_index_ = 0;
  if (queue_pairs_.empty()) {
    scheduler_credit_ = 0;
  } else {
    scheduler_credit_ = weights_.front();
  }
  scheduler_advances_ = 0;
  scheduler_skips_ = 0;
  for (auto& qp : queue_pairs_) {
    qp->reset();
  }
}

QueueManagerStats QueueManager::stats() const {
  NIC_TRACE_SCOPED(__func__);
  QueueManagerStats out{};
  aggregate_stats(out);
  return out;
}

std::string QueueManager::stats_summary() const {
  NIC_TRACE_SCOPED(__func__);
  const auto s = stats();
  std::ostringstream oss;
  oss << "qm tx_pkts=" << s.total_tx_packets << " rx_pkts=" << s.total_rx_packets
      << " tx_bytes=" << s.total_tx_bytes << " rx_bytes=" << s.total_rx_bytes
      << " drops_csum=" << s.total_drops_checksum
      << " drops_no_rx_desc=" << s.total_drops_no_rx_desc
      << " drops_buf_small=" << s.total_drops_buffer_small
      << " tx_tso_segs=" << s.total_tx_tso_segments << " tx_gso_segs=" << s.total_tx_gso_segments
      << " tx_vlan_ins=" << s.total_tx_vlan_insertions
      << " rx_vlan_strip=" << s.total_rx_vlan_strips
      << " rx_csum_ver=" << s.total_rx_checksum_verified << " rx_gro=" << s.total_rx_gro_aggregated
      << " sched_adv=" << s.scheduler_advances << " sched_skip=" << s.scheduler_skips;
  return oss.str();
}

void QueueManager::aggregate_stats(QueueManagerStats& out) const {
  NIC_TRACE_SCOPED(__func__);
  for (const auto& qp : queue_pairs_) {
    const QueuePairStats s = qp->stats();
    out.total_tx_packets += s.tx_packets;
    out.total_rx_packets += s.rx_packets;
    out.total_tx_bytes += s.tx_bytes;
    out.total_rx_bytes += s.rx_bytes;
    out.total_drops_checksum += s.drops_checksum;
    out.total_drops_no_rx_desc += s.drops_no_rx_desc;
    out.total_drops_buffer_small += s.drops_buffer_small;
    out.total_tx_tso_segments += s.tx_tso_segments;
    out.total_tx_gso_segments += s.tx_gso_segments;
    out.total_tx_vlan_insertions += s.tx_vlan_insertions;
    out.total_rx_vlan_strips += s.rx_vlan_strips;
    out.total_rx_checksum_verified += s.rx_checksum_verified;
    out.total_rx_gro_aggregated += s.rx_gro_aggregated;
  }
  out.scheduler_advances += scheduler_advances_;
  out.scheduler_skips += scheduler_skips_;
}
