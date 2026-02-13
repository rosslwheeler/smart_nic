#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nic/bar.h"
#include "nic/capability.h"
#include "nic/config_space.h"
#include "nic/dma_engine.h"
#include "nic/host_memory.h"
#include "nic/interrupt_dispatcher.h"
#include "nic/msix.h"
#include "nic/queue_manager.h"
#include "nic/queue_pair.h"
#include "nic/register.h"
#include "nic/rocev2/engine.h"
#include "nic/rss.h"
#include "nic/simple_host_memory.h"

namespace nic {

struct DeviceIdentity {
  std::uint16_t vendor_id;
  std::uint16_t device_id;
  std::uint8_t revision;
};

struct DeviceConfig {
  DeviceIdentity identity{};
  std::string name{"nic-model"};
  BarArray bars{MakeDefaultBars()};
  CapabilityList capabilities{MakeDefaultCapabilities()};
  HostMemoryConfig host_memory_config{
      .size_bytes = 1 << 20, .page_size = 4096, .iommu_enabled = false};
  HostMemory* host_memory{nullptr};  ///< Optional injected host memory
  DMAEngine* dma_engine{nullptr};    ///< Optional injected DMA engine
  bool enable_queue_pair{true};
  QueuePairConfig queue_pair_config{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 64,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 64,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 64, .queue_id = 0},
      .rx_completion = {.ring_size = 64, .queue_id = 0},
      .tx_doorbell = nullptr,
      .rx_doorbell = nullptr,
      .tx_completion_doorbell = nullptr,
      .rx_completion_doorbell = nullptr,
  };
  QueuePair* queue_pair{nullptr};  ///< Optional injected queue pair
  bool enable_queue_manager{false};
  QueueManagerConfig queue_manager_config{};
  QueueManager* queue_manager{nullptr};  ///< Optional injected queue manager
  RssConfig rss_config{};
  RssEngine* rss_engine{nullptr};  ///< Optional injected RSS engine
  MsixTable msix_table{};
  MsixMapping msix_mapping{};
  CoalesceConfig interrupt_coalesce{};
  InterruptDispatcher* interrupt_dispatcher{nullptr};
  bool enable_rdma{false};                   ///< Enable RoCEv2 RDMA engine
  rocev2::RdmaEngineConfig rdma_config{};    ///< RDMA engine configuration
  rocev2::RdmaEngine* rdma_engine{nullptr};  ///< Optional injected RDMA engine
};

/// Device state machine states.
enum class DeviceState : std::uint8_t {
  Uninitialized,  ///< Initial state after construction
  Resetting,      ///< Device is performing reset sequence
  Ready,          ///< Device is ready for operation
  Error,          ///< Device encountered fatal error
};

class Device {
public:
  explicit Device(DeviceConfig config);

  /// Perform device reset sequence.
  void reset();

  // Config accessors
  [[nodiscard]] const DeviceConfig& config() const noexcept { return config_; }
  [[nodiscard]] bool is_initialized() const noexcept { return state_ == DeviceState::Ready; }
  [[nodiscard]] DeviceState state() const noexcept { return state_; }

  // Config space access (PCIe configuration)
  [[nodiscard]] std::uint8_t read_config8(std::uint16_t offset) const noexcept;
  [[nodiscard]] std::uint16_t read_config16(std::uint16_t offset) const noexcept;
  [[nodiscard]] std::uint32_t read_config32(std::uint16_t offset) const noexcept;

  void write_config8(std::uint16_t offset, std::uint8_t value) noexcept;
  void write_config16(std::uint16_t offset, std::uint16_t value) noexcept;
  void write_config32(std::uint16_t offset, std::uint32_t value) noexcept;

  // Register file access (BAR0 MMIO)
  [[nodiscard]] std::uint32_t read_register(std::uint32_t offset) const noexcept;
  void write_register(std::uint32_t offset, std::uint32_t value);

  // Direct access for testing/debugging
  [[nodiscard]] const ConfigSpace& config_space() const noexcept { return config_space_; }
  [[nodiscard]] const RegisterFile& register_file() const noexcept { return register_file_; }
  [[nodiscard]] HostMemory& host_memory() noexcept { return *host_memory_; }
  [[nodiscard]] DMAEngine& dma_engine() noexcept { return *dma_engine_; }
  [[nodiscard]] const HostMemory& host_memory() const noexcept { return *host_memory_; }
  [[nodiscard]] const DMAEngine& dma_engine() const noexcept { return *dma_engine_; }
  [[nodiscard]] QueuePair* queue_pair() noexcept { return queue_pair_; }
  [[nodiscard]] const QueuePair* queue_pair() const noexcept { return queue_pair_; }
  [[nodiscard]] QueuePairStats queue_pair_stats() const {
    if (queue_pair_ == nullptr) {
      return QueuePairStats{};
    }
    return queue_pair_->stats();
  }
  [[nodiscard]] QueueManager* queue_manager() noexcept { return queue_manager_; }
  [[nodiscard]] const QueueManager* queue_manager() const noexcept { return queue_manager_; }
  [[nodiscard]] QueueManagerStats queue_manager_stats() const {
    if (queue_manager_ == nullptr) {
      return QueueManagerStats{};
    }
    return queue_manager_->stats();
  }
  [[nodiscard]] RssEngine* rss_engine() noexcept { return rss_engine_; }
  [[nodiscard]] const RssEngine* rss_engine() const noexcept { return rss_engine_; }
  [[nodiscard]] RssStats rss_stats() const {
    if (rss_engine_ == nullptr) {
      return RssStats{};
    }
    return rss_engine_->stats();
  }
  [[nodiscard]] InterruptStats interrupt_stats() const {
    if (interrupt_dispatcher_ == nullptr) {
      return InterruptStats{};
    }
    return interrupt_dispatcher_->stats();
  }
  [[nodiscard]] rocev2::RdmaEngine* rdma_engine() noexcept { return rdma_engine_; }
  [[nodiscard]] const rocev2::RdmaEngine* rdma_engine() const noexcept { return rdma_engine_; }

  bool set_msix_queue_vector(std::uint16_t queue_id, std::uint16_t vector_id);
  bool mask_msix_vector(std::uint16_t vector_id, bool masked = true);
  bool enable_msix_vector(std::uint16_t vector_id, bool enabled = true);

  /// Process one descriptor from the TX/RX queue pair. Returns true if work was done.
  bool process_queue_once();

private:
  DeviceConfig config_;
  DeviceState state_{DeviceState::Uninitialized};
  ConfigSpace config_space_;
  RegisterFile register_file_;
  HostMemory* host_memory_{nullptr};
  DMAEngine* dma_engine_{nullptr};
  std::unique_ptr<SimpleHostMemory> default_host_memory_;
  std::unique_ptr<DMAEngine> default_dma_engine_;
  QueuePair* queue_pair_{nullptr};
  std::unique_ptr<QueuePair> default_queue_pair_;
  QueueManager* queue_manager_{nullptr};
  std::unique_ptr<QueueManager> default_queue_manager_;
  RssEngine* rss_engine_{nullptr};
  std::unique_ptr<RssEngine> default_rss_engine_;
  InterruptDispatcher* interrupt_dispatcher_{nullptr};
  std::unique_ptr<InterruptDispatcher> default_interrupt_dispatcher_;
  std::uint64_t interrupt_counter_{0};
  rocev2::RdmaEngine* rdma_engine_{nullptr};
  std::unique_ptr<rocev2::RdmaEngine> default_rdma_engine_;

  void initialize_config_space();
  void initialize_register_file();
  void initialize_runtime();
};

}  // namespace nic
