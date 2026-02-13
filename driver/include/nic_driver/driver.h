#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "nic_driver/rdma_types.h"

namespace nic {
class Device;
class HostMemory;
class QueuePair;
}  // namespace nic

namespace nic_driver {

// Simplified driver that wraps the NIC model API
class NicDriver {
public:
  NicDriver();
  ~NicDriver();

  // Disable copy/move
  NicDriver(const NicDriver&) = delete;
  NicDriver& operator=(const NicDriver&) = delete;
  NicDriver(NicDriver&&) = delete;
  NicDriver& operator=(NicDriver&&) = delete;

  // Initialization - takes ownership of device pointer
  bool init(std::unique_ptr<nic::Device> device);
  void reset();
  bool is_initialized() const { return initialized_; }

  // Get access to the underlying device
  [[nodiscard]] nic::Device* device() { return device_.get(); }
  [[nodiscard]] const nic::Device* device() const { return device_.get(); }

  // Packet transmission (uses device's queue pair)
  bool send_packet(std::span<const std::byte> packet);

  // Process device (polls for work)
  bool process();

  // Statistics
  struct Stats {
    std::uint64_t tx_packets{0};
    std::uint64_t tx_bytes{0};
    std::uint64_t rx_packets{0};
    std::uint64_t rx_bytes{0};
    std::uint64_t processed{0};
  };

  Stats get_stats() const;
  void clear_stats();

  // ============================================
  // RDMA API (hardware-style, mirrors libibverbs)
  // ============================================

  /// Check if RDMA is enabled on this driver.
  [[nodiscard]] bool rdma_enabled() const;

  // Protection Domain
  [[nodiscard]] std::optional<PdHandle> create_pd();
  bool destroy_pd(PdHandle pd);

  // Memory Region
  [[nodiscard]] std::optional<MrHandle> register_mr(PdHandle pd,
                                                    std::uint64_t virtual_address,
                                                    std::size_t length,
                                                    AccessFlags access);
  bool deregister_mr(MrHandle mr);

  // Completion Queue
  [[nodiscard]] std::optional<CqHandle> create_cq(std::size_t depth);
  bool destroy_cq(CqHandle cq);
  [[nodiscard]] std::vector<RdmaCqe> poll_cq(CqHandle cq, std::size_t max_cqes);

  // Queue Pair
  [[nodiscard]] std::optional<QpHandle> create_qp(const RdmaQpConfig& config);
  bool destroy_qp(QpHandle qp);
  bool modify_qp(QpHandle qp, const RdmaQpModifyParams& params);

  // Work Requests
  bool post_send(QpHandle qp, const SendWqe& wqe);
  bool post_recv(QpHandle qp, const RecvWqe& wqe);

  // Packet I/O for inter-driver routing
  [[nodiscard]] std::vector<OutgoingPacket> rdma_generate_packets();
  bool rdma_process_packet(std::span<const std::byte> udp_payload,
                           std::array<std::uint8_t, 4> src_ip,
                           std::array<std::uint8_t, 4> dst_ip,
                           std::uint16_t src_port);

private:
  bool initialized_{false};
  std::unique_ptr<nic::Device> device_;
  mutable Stats stats_;
};

}  // namespace nic_driver
