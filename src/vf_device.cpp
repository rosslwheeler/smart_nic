#include "nic/vf_device.h"

#include "nic/trace.h"

using namespace nic;

VFDevice::VFDevice(Config config) : config_(std::move(config)) {
  NIC_TRACE_SCOPED(__func__);
  initialize_queue_pairs();
}

void VFDevice::initialize_queue_pairs() {
  NIC_TRACE_SCOPED(__func__);

  if ((config_.vf == nullptr) || (config_.dma_engine == nullptr)) {
    return;
  }

  auto vf_queue_ids = config_.vf->queue_ids();
  std::size_t num_qps =
      std::min(config_.num_queue_pairs, static_cast<std::uint16_t>(vf_queue_ids.size()));

  queue_pairs_.reserve(num_qps);
  tx_doorbells_.reserve(num_qps);
  rx_doorbells_.reserve(num_qps);
  tx_completion_doorbells_.reserve(num_qps);
  rx_completion_doorbells_.reserve(num_qps);

  for (std::size_t i = 0; i < num_qps; ++i) {
    std::uint16_t queue_id = vf_queue_ids[i];

    // Create doorbells for this queue pair
    auto tx_db = std::make_unique<Doorbell>();
    auto rx_db = std::make_unique<Doorbell>();
    auto tx_comp_db = std::make_unique<Doorbell>();
    auto rx_comp_db = std::make_unique<Doorbell>();

    // Configure queue pair
    QueuePairConfig qp_cfg{
        .queue_id = queue_id,
        .tx_ring = {.ring_size = config_.queue_depth},
        .rx_ring = {.ring_size = config_.queue_depth},
        .tx_completion = {.ring_size = config_.completion_queue_depth},
        .rx_completion = {.ring_size = config_.completion_queue_depth},
        .tx_doorbell = tx_db.get(),
        .rx_doorbell = rx_db.get(),
        .tx_completion_doorbell = tx_comp_db.get(),
        .rx_completion_doorbell = rx_comp_db.get(),
        .interrupt_dispatcher = config_.interrupt_dispatcher,
        .weight = 1,
        .max_mtu = kJumboMtu,
        .enable_tx_interrupts = false,
        .enable_rx_interrupts = true,
    };

    // Create queue pair
    auto qp = std::make_unique<QueuePair>(qp_cfg, *config_.dma_engine);

    // Store everything
    queue_pairs_.push_back(std::move(qp));
    tx_doorbells_.push_back(std::move(tx_db));
    rx_doorbells_.push_back(std::move(rx_db));
    tx_completion_doorbells_.push_back(std::move(tx_comp_db));
    rx_completion_doorbells_.push_back(std::move(rx_comp_db));
  }
}

QueuePair* VFDevice::queue_pair(std::size_t index) noexcept {
  if (index < queue_pairs_.size()) {
    return queue_pairs_[index].get();
  }
  return nullptr;
}

const QueuePair* VFDevice::queue_pair(std::size_t index) const noexcept {
  if (index < queue_pairs_.size()) {
    return queue_pairs_[index].get();
  }
  return nullptr;
}

bool VFDevice::process_queue_pair(std::size_t index) {
  NIC_TRACE_SCOPED(__func__);
  auto* qp = queue_pair(index);
  if (qp == nullptr) {
    return false;
  }
  return qp->process_once();
}

std::size_t VFDevice::process_all() {
  NIC_TRACE_SCOPED(__func__);
  std::size_t work_done = 0;
  for (std::size_t i = 0; i < queue_pairs_.size(); ++i) {
    if (process_queue_pair(i)) {
      ++work_done;
    }
  }
  return work_done;
}

Doorbell* VFDevice::tx_doorbell(std::size_t qp_index) noexcept {
  if (qp_index < tx_doorbells_.size()) {
    return tx_doorbells_[qp_index].get();
  }
  return nullptr;
}

Doorbell* VFDevice::rx_doorbell(std::size_t qp_index) noexcept {
  if (qp_index < rx_doorbells_.size()) {
    return rx_doorbells_[qp_index].get();
  }
  return nullptr;
}

Doorbell* VFDevice::tx_completion_doorbell(std::size_t qp_index) noexcept {
  if (qp_index < tx_completion_doorbells_.size()) {
    return tx_completion_doorbells_[qp_index].get();
  }
  return nullptr;
}

Doorbell* VFDevice::rx_completion_doorbell(std::size_t qp_index) noexcept {
  if (qp_index < rx_completion_doorbells_.size()) {
    return rx_completion_doorbells_[qp_index].get();
  }
  return nullptr;
}

VFDevice::Stats VFDevice::aggregate_stats() const noexcept {
  NIC_TRACE_SCOPED(__func__);
  Stats total{};

  for (const auto& qp : queue_pairs_) {
    const auto& qp_stats = qp->stats();
    total.total_tx_packets += qp_stats.tx_packets;
    total.total_rx_packets += qp_stats.rx_packets;
    total.total_tx_bytes += qp_stats.tx_bytes;
    total.total_rx_bytes += qp_stats.rx_bytes;
    total.total_drops += qp_stats.drops_checksum + qp_stats.drops_no_rx_desc
                         + qp_stats.drops_buffer_small + qp_stats.drops_mtu_exceeded
                         + qp_stats.drops_invalid_mss + qp_stats.drops_too_many_segments;
  }

  return total;
}

void VFDevice::reset_stats() noexcept {
  NIC_TRACE_SCOPED(__func__);
  for (auto& qp : queue_pairs_) {
    qp->reset_stats();
  }
}

void VFDevice::reset() {
  NIC_TRACE_SCOPED(__func__);

  // Reset all queue pairs
  for (auto& qp : queue_pairs_) {
    qp->reset();
  }

  // Reset doorbells
  for (auto& db : tx_doorbells_) {
    db->reset();
  }
  for (auto& db : rx_doorbells_) {
    db->reset();
  }
  for (auto& db : tx_completion_doorbells_) {
    db->reset();
  }
  for (auto& db : rx_completion_doorbells_) {
    db->reset();
  }
}