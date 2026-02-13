#include "nic_driver/driver.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "nic/device.h"
#include "nic/host_memory.h"
#include "nic/log.h"
#include "nic/queue_pair.h"
#include "nic/rocev2/engine.h"
#include "nic/trace.h"
#include "nic/tx_rx.h"

namespace nic_driver {

NicDriver::NicDriver() = default;

NicDriver::~NicDriver() {
  NIC_TRACE_SCOPED(__func__);
  if (initialized_) {
    reset();
  }
}

bool NicDriver::init(std::unique_ptr<nic::Device> device) {
  NIC_TRACE_SCOPED(__func__);
  if (initialized_ || !device) {
    return false;
  }

  device_ = std::move(device);
  initialized_ = true;
  NIC_LOG_INFO("driver initialized");
  return true;
}

void NicDriver::reset() {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_) {
    return;
  }

  if (device_) {
    device_->reset();
  }

  initialized_ = false;
  device_.reset();
  NIC_LOG_INFO("driver reset");
}

bool NicDriver::send_packet(std::span<const std::byte> packet) {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_ || !device_) {
    return false;
  }

  auto* qp = device_->queue_pair();
  if (!qp) {
    return false;
  }

  // Get host memory for writing packet data
  auto& host_mem = device_->host_memory();

  // Allocate a buffer address in host memory (simplified - using packet size as address offset)
  // In a real driver, would manage a proper buffer pool
  static nic::HostAddress next_addr = 0x10000;  // Start at 64KB
  nic::HostAddress buffer_addr = next_addr;
  next_addr += packet.size() + 256;  // Add padding

  // Write packet to host memory
  std::vector<std::byte> packet_vec(packet.begin(), packet.end());
  auto write_result = host_mem.write(buffer_addr, packet_vec);
  if (!write_result.ok()) {
    return false;
  }

  // Create TX descriptor
  nic::TxDescriptor tx_desc{};
  tx_desc.buffer_address = buffer_addr;
  tx_desc.length = static_cast<std::uint32_t>(packet.size());
  tx_desc.checksum = nic::ChecksumMode::None;
  tx_desc.descriptor_index = 0;

  // Serialize descriptor to bytes
  std::vector<std::byte> desc_bytes(sizeof(nic::TxDescriptor));
  std::memcpy(desc_bytes.data(), &tx_desc, sizeof(nic::TxDescriptor));

  // Push descriptor to TX ring
  auto& tx_ring = qp->tx_ring();
  auto push_result = tx_ring.push_descriptor(desc_bytes);

  if (!push_result.ok()) {
    return false;
  }

  // Update stats
  stats_.tx_packets++;
  stats_.tx_bytes += packet.size();

  return true;
}

bool NicDriver::process() {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_ || !device_) {
    return false;
  }

  // Process the device (this moves packets from TX to RX)
  bool work_done = device_->process_queue_once();

  if (work_done) {
    stats_.processed++;

    // Update stats from device queue pair
    auto* qp = device_->queue_pair();
    if (qp) {
      auto qp_stats = qp->stats();
      stats_.rx_packets = qp_stats.rx_packets;
      stats_.rx_bytes = qp_stats.rx_bytes;
    }
  }

  return work_done;
}

NicDriver::Stats NicDriver::get_stats() const {
  NIC_TRACE_SCOPED(__func__);
  // Return our locally tracked stats
  // Note: In a real driver, might sync certain stats from hardware counters
  return stats_;
}

void NicDriver::clear_stats() {
  NIC_TRACE_SCOPED(__func__);
  stats_ = Stats{};

  if (initialized_ && device_) {
    auto* qp = device_->queue_pair();
    if (qp) {
      qp->reset_stats();
    }
  }
}

// ============================================
// RDMA API Implementation
// ============================================

bool NicDriver::rdma_enabled() const {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_ || !device_) {
    return false;
  }
  return device_->rdma_engine() != nullptr;
}

std::optional<PdHandle> NicDriver::create_pd() {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_ || !device_) {
    return std::nullopt;
  }

  auto* engine = device_->rdma_engine();
  if (!engine) {
    return std::nullopt;
  }

  auto result = engine->create_pd();
  if (!result) {
    return std::nullopt;
  }
  NIC_LOGF_INFO("driver create_pd: handle={}", *result);
  return PdHandle{*result};
}

bool NicDriver::destroy_pd(PdHandle pd) {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_ || !device_) {
    return false;
  }

  auto* engine = device_->rdma_engine();
  if (!engine) {
    return false;
  }

  return engine->destroy_pd(pd.value);
}

std::optional<MrHandle> NicDriver::register_mr(PdHandle pd,
                                               std::uint64_t virtual_address,
                                               std::size_t length,
                                               AccessFlags access) {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_ || !device_) {
    return std::nullopt;
  }

  auto* engine = device_->rdma_engine();
  if (!engine) {
    return std::nullopt;
  }

  auto result = engine->register_mr(pd.value, virtual_address, length, access);
  if (!result) {
    return std::nullopt;
  }

  // Get the actual rkey from the registered MR
  const auto* mr = engine->mr_table().get_by_lkey(*result);
  if (!mr) {
    return std::nullopt;
  }

  NIC_LOGF_INFO("driver register_mr: lkey={:#x} rkey={:#x} addr={:#x} len={}",
                mr->lkey,
                mr->rkey,
                virtual_address,
                length);
  return MrHandle{mr->lkey, mr->rkey};
}

bool NicDriver::deregister_mr(MrHandle mr) {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_ || !device_) {
    return false;
  }

  auto* engine = device_->rdma_engine();
  if (!engine) {
    return false;
  }

  return engine->deregister_mr(mr.lkey);
}

std::optional<CqHandle> NicDriver::create_cq(std::size_t depth) {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_ || !device_) {
    return std::nullopt;
  }

  auto* engine = device_->rdma_engine();
  if (!engine) {
    return std::nullopt;
  }

  auto result = engine->create_cq(depth);
  if (!result) {
    return std::nullopt;
  }
  return CqHandle{*result};
}

bool NicDriver::destroy_cq(CqHandle cq) {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_ || !device_) {
    return false;
  }

  auto* engine = device_->rdma_engine();
  if (!engine) {
    return false;
  }

  return engine->destroy_cq(cq.value);
}

std::vector<RdmaCqe> NicDriver::poll_cq(CqHandle cq, std::size_t max_cqes) {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_ || !device_) {
    return {};
  }

  auto* engine = device_->rdma_engine();
  if (!engine) {
    return {};
  }

  return engine->poll_cq(cq.value, max_cqes);
}

std::optional<QpHandle> NicDriver::create_qp(const RdmaQpConfig& config) {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_ || !device_) {
    return std::nullopt;
  }

  auto* engine = device_->rdma_engine();
  if (!engine) {
    return std::nullopt;
  }

  auto result = engine->create_qp(config);
  if (!result) {
    return std::nullopt;
  }
  NIC_LOGF_INFO("driver create_qp: handle={}", *result);
  return QpHandle{*result};
}

bool NicDriver::destroy_qp(QpHandle qp) {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_ || !device_) {
    return false;
  }

  auto* engine = device_->rdma_engine();
  if (!engine) {
    return false;
  }

  return engine->destroy_qp(qp.value);
}

bool NicDriver::modify_qp(QpHandle qp, const RdmaQpModifyParams& params) {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_ || !device_) {
    return false;
  }

  auto* engine = device_->rdma_engine();
  if (!engine) {
    return false;
  }

  return engine->modify_qp(qp.value, params);
}

bool NicDriver::post_send(QpHandle qp, const SendWqe& wqe) {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_ || !device_) {
    return false;
  }

  auto* engine = device_->rdma_engine();
  if (!engine) {
    return false;
  }

  return engine->post_send(qp.value, wqe);
}

bool NicDriver::post_recv(QpHandle qp, const RecvWqe& wqe) {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_ || !device_) {
    return false;
  }

  auto* engine = device_->rdma_engine();
  if (!engine) {
    return false;
  }

  return engine->post_recv(qp.value, wqe);
}

std::vector<OutgoingPacket> NicDriver::rdma_generate_packets() {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_ || !device_) {
    return {};
  }

  auto* engine = device_->rdma_engine();
  if (!engine) {
    return {};
  }

  return engine->generate_outgoing_packets();
}

bool NicDriver::rdma_process_packet(std::span<const std::byte> udp_payload,
                                    std::array<std::uint8_t, 4> src_ip,
                                    std::array<std::uint8_t, 4> dst_ip,
                                    std::uint16_t src_port) {
  NIC_TRACE_SCOPED(__func__);
  if (!initialized_ || !device_) {
    return false;
  }

  auto* engine = device_->rdma_engine();
  if (!engine) {
    return false;
  }

  return engine->process_incoming_packet(udp_payload, src_ip, dst_ip, src_port);
}

}  // namespace nic_driver