#include "nic/device.h"

#include "nic/log.h"
#include "nic/trace.h"

using namespace nic;

Device::Device(DeviceConfig config) : config_(std::move(config)) {
  NIC_TRACE_SCOPED(__func__);
  initialize_runtime();
  // Device boots in uninitialized state; explicit reset() brings it online.
}

void Device::reset() {
  NIC_TRACE_SCOPED(__func__);
  NIC_LOGF_INFO("device reset: vendor={:#06x} device={:#06x}",
                config_.identity.vendor_id,
                config_.identity.device_id);

  state_ = DeviceState::Resetting;

  // Initialize config space with device identity and capabilities
  initialize_config_space();

  // Initialize register file with default registers
  initialize_register_file();

  if (queue_pair_ != nullptr) {
    queue_pair_->reset();
    // Re-wire dispatcher after reset if present.
    queue_pair_->reset_stats();
  }
  if (queue_manager_ != nullptr) {
    queue_manager_->reset();
  }
  if (interrupt_dispatcher_ != nullptr) {
    // No explicit reset state needed yet.
  }

  state_ = DeviceState::Ready;
}

void Device::initialize_config_space() {
  NIC_TRACE_SCOPED(__func__);
  config_space_.initialize(config_.identity.vendor_id,
                           config_.identity.device_id,
                           config_.identity.revision,
                           config_.bars,
                           config_.capabilities);
}

void Device::initialize_register_file() {
  NIC_TRACE_SCOPED(__func__);
  register_file_.add_registers(MakeDefaultNicRegisters());
  register_file_.reset();
}

void Device::initialize_runtime() {
  NIC_TRACE_SCOPED(__func__);
  if (config_.host_memory != nullptr) {
    host_memory_ = config_.host_memory;
  } else {
    default_host_memory_ = std::make_unique<SimpleHostMemory>(config_.host_memory_config);
    host_memory_ = default_host_memory_.get();
  }

  if (config_.dma_engine != nullptr) {
    dma_engine_ = config_.dma_engine;
  } else {
    default_dma_engine_ = std::make_unique<DMAEngine>(*host_memory_);
    dma_engine_ = default_dma_engine_.get();
  }

  if (config_.rss_engine != nullptr) {
    rss_engine_ = config_.rss_engine;
  } else {
    default_rss_engine_ = std::make_unique<RssEngine>(config_.rss_config);
    rss_engine_ = default_rss_engine_.get();
  }

  if (config_.enable_queue_manager && !config_.queue_manager_config.queue_configs.empty()) {
    if (config_.queue_manager != nullptr) {
      queue_manager_ = config_.queue_manager;
    } else {
      for (auto& qp_cfg : config_.queue_manager_config.queue_configs) {
        qp_cfg.interrupt_dispatcher = config_.interrupt_dispatcher;
      }
      default_queue_manager_ =
          std::make_unique<QueueManager>(config_.queue_manager_config, *dma_engine_);
      queue_manager_ = default_queue_manager_.get();
    }
    queue_pair_ = nullptr;
  } else if (config_.enable_queue_pair) {
    if (config_.queue_pair != nullptr) {
      queue_pair_ = config_.queue_pair;
    } else {
      QueuePairConfig qp_cfg = config_.queue_pair_config;
      qp_cfg.queue_id = 0;
      qp_cfg.tx_ring.queue_id = 0;
      qp_cfg.rx_ring.queue_id = 0;
      qp_cfg.tx_completion.queue_id = 0;
      qp_cfg.rx_completion.queue_id = 0;
      qp_cfg.interrupt_dispatcher = config_.interrupt_dispatcher;
      default_queue_pair_ = std::make_unique<QueuePair>(qp_cfg, *dma_engine_);
      queue_pair_ = default_queue_pair_.get();
    }
  }

  if (config_.interrupt_dispatcher != nullptr) {
    interrupt_dispatcher_ = config_.interrupt_dispatcher;
  } else {
    default_interrupt_dispatcher_ = std::make_unique<InterruptDispatcher>(
        config_.msix_table,
        config_.msix_mapping,
        config_.interrupt_coalesce,
        [this](std::uint16_t /*vector_id*/, std::uint32_t /*batch*/) { interrupt_counter_++; });
    interrupt_dispatcher_ = default_interrupt_dispatcher_.get();
  }

  if (queue_pair_ != nullptr) {
    queue_pair_->reset_stats();
  }

  if (config_.enable_rdma) {
    if (config_.rdma_engine != nullptr) {
      rdma_engine_ = config_.rdma_engine;
    } else {
      default_rdma_engine_ =
          std::make_unique<rocev2::RdmaEngine>(config_.rdma_config, *dma_engine_, *host_memory_);
      rdma_engine_ = default_rdma_engine_.get();
    }
  }
}

// Config space access methods

std::uint8_t Device::read_config8(std::uint16_t offset) const noexcept {
  return config_space_.read8(offset);
}

std::uint16_t Device::read_config16(std::uint16_t offset) const noexcept {
  return config_space_.read16(offset);
}

std::uint32_t Device::read_config32(std::uint16_t offset) const noexcept {
  return config_space_.read32(offset);
}

void Device::write_config8(std::uint16_t offset, std::uint8_t value) noexcept {
  config_space_.write8(offset, value);
}

void Device::write_config16(std::uint16_t offset, std::uint16_t value) noexcept {
  config_space_.write16(offset, value);
}

void Device::write_config32(std::uint16_t offset, std::uint32_t value) noexcept {
  config_space_.write32(offset, value);
}

// Register file access methods

std::uint32_t Device::read_register(std::uint32_t offset) const noexcept {
  return register_file_.read32(offset);
}

void Device::write_register(std::uint32_t offset, std::uint32_t value) {
  register_file_.write32(offset, value);
}

bool Device::process_queue_once() {
  NIC_TRACE_SCOPED(__func__);
  if (queue_pair_ == nullptr) {
    return false;
  }
  return queue_pair_->process_once();
}

bool Device::set_msix_queue_vector(std::uint16_t queue_id, std::uint16_t vector_id) {
  NIC_TRACE_SCOPED(__func__);
  if (interrupt_dispatcher_ == nullptr) {
    return false;
  }
  return interrupt_dispatcher_->set_queue_vector(queue_id, vector_id);
}

bool Device::mask_msix_vector(std::uint16_t vector_id, bool masked) {
  NIC_TRACE_SCOPED(__func__);
  if (interrupt_dispatcher_ == nullptr) {
    return false;
  }
  return interrupt_dispatcher_->mask_vector(vector_id, masked);
}

bool Device::enable_msix_vector(std::uint16_t vector_id, bool enabled) {
  NIC_TRACE_SCOPED(__func__);
  if (interrupt_dispatcher_ == nullptr) {
    return false;
  }
  return interrupt_dispatcher_->enable_vector(vector_id, enabled);
}
