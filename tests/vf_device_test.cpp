#include "nic/vf_device.h"

#include <cassert>

#include "nic/pf_vf_manager.h"
#include "nic/simple_host_memory.h"

using namespace nic;

int main() {
  // Setup: Create PF/VF infrastructure
  PFConfig pf_cfg{.max_vfs = 4,
                  .total_queues = 32,
                  .total_vectors = 32,
                  .pf_reserved_queues = 4,
                  .pf_reserved_vectors = 4};
  PFVFManager manager{pf_cfg};

  // Create a VF with resources
  VFConfig vf_cfg{.vf_id = 1, .num_queues = 4, .num_vectors = 2, .enabled = false, .trust = false};
  assert(manager.create_vf(1, vf_cfg));

  auto* vf = manager.vf(1);
  assert(vf != nullptr);
  assert(manager.enable_vf(1));

  // Create DMA engine and interrupt dispatcher
  HostMemoryConfig host_cfg{.size_bytes = 1024 * 1024};  // 1MB
  SimpleHostMemory host_mem{host_cfg};
  DMAEngine dma{host_mem};

  MsixTable msix_table{4};
  MsixMapping msix_mapping{4, 0};
  for (std::uint16_t i = 0; i < 4; ++i) {
    msix_mapping.set_queue_vector(i, i);
  }

  CoalesceConfig coalesce_cfg{.packet_threshold = 1, .timer_threshold_us = 0};
  std::vector<std::pair<std::uint16_t, std::uint32_t>> interrupts_fired;
  InterruptDispatcher interrupt_dispatcher{
      msix_table,
      msix_mapping,
      coalesce_cfg,
      [&interrupts_fired](std::uint16_t vec, std::uint32_t batch) {
        interrupts_fired.emplace_back(vec, batch);
      }};

  // Create VF device interface
  VFDevice::Config vf_dev_cfg{.vf_id = 1,
                              .vf = vf,
                              .dma_engine = &dma,
                              .interrupt_dispatcher = &interrupt_dispatcher,
                              .num_queue_pairs = 2,
                              .queue_depth = 16,
                              .completion_queue_depth = 32};

  VFDevice vf_device{vf_dev_cfg};

  // Verify VF device was created correctly
  assert(vf_device.vf_id() == 1);
  assert(vf_device.vf() == vf);
  assert(vf_device.num_queue_pairs() == 2);

  // Access queue pairs
  auto* qp0 = vf_device.queue_pair(0);
  assert(qp0 != nullptr);

  auto* qp1 = vf_device.queue_pair(1);
  assert(qp1 != nullptr);

  auto* qp_invalid = vf_device.queue_pair(100);
  assert(qp_invalid == nullptr);

  // Access doorbells
  auto* tx_db = vf_device.tx_doorbell(0);
  assert(tx_db != nullptr);

  auto* rx_db = vf_device.rx_doorbell(0);
  assert(rx_db != nullptr);

  auto* tx_comp_db = vf_device.tx_completion_doorbell(0);
  assert(tx_comp_db != nullptr);

  auto* rx_comp_db = vf_device.rx_completion_doorbell(0);
  assert(rx_comp_db != nullptr);

  // Invalid doorbell access
  auto* invalid_db = vf_device.tx_doorbell(100);
  assert(invalid_db == nullptr);
  assert(vf_device.rx_doorbell(100) == nullptr);
  assert(vf_device.tx_completion_doorbell(100) == nullptr);
  assert(vf_device.rx_completion_doorbell(100) == nullptr);

  // Test VF device reset
  vf_device.reset();

  // Verify doorbells were reset (ring counters should be 0)
  assert(tx_db->rings() == 0);
  assert(rx_db->rings() == 0);

  // Test aggregate statistics
  auto stats = vf_device.aggregate_stats();
  assert(stats.total_tx_packets == 0);
  assert(stats.total_rx_packets == 0);
  assert(stats.total_tx_bytes == 0);
  assert(stats.total_rx_bytes == 0);
  assert(stats.total_drops == 0);

  // Test multiple VF devices
  VFConfig vf2_cfg{.vf_id = 2, .num_queues = 2, .num_vectors = 1};
  assert(manager.create_vf(2, vf2_cfg));

  auto* vf2 = manager.vf(2);
  assert(vf2 != nullptr);
  assert(manager.enable_vf(2));

  VFDevice::Config vf2_dev_cfg{.vf_id = 2,
                               .vf = vf2,
                               .dma_engine = &dma,
                               .interrupt_dispatcher = &interrupt_dispatcher,
                               .num_queue_pairs = 1,
                               .queue_depth = 8,
                               .completion_queue_depth = 16};

  VFDevice vf2_device{vf2_dev_cfg};

  assert(vf2_device.vf_id() == 2);
  assert(vf2_device.num_queue_pairs() == 1);

  // Verify isolation: VF devices have independent resources
  assert(vf_device.queue_pair(0) != vf2_device.queue_pair(0));
  assert(vf_device.tx_doorbell(0) != vf2_device.tx_doorbell(0));

  // Test reset stats
  vf_device.reset_stats();
  auto stats_after_reset = vf_device.aggregate_stats();
  assert(stats_after_reset.total_tx_packets == 0);

  // Exercise process_queue_pair/process_all and const accessors.
  assert(!vf_device.process_queue_pair(100));
  const VFDevice& const_vf_device = vf_device;
  assert(const_vf_device.queue_pair(0) == qp0);
  assert(vf_device.process_all() == 0);

  // VFDevice with missing dependencies should not initialize queue pairs.
  VFDevice::Config empty_cfg{.vf_id = 99,
                             .vf = nullptr,
                             .dma_engine = nullptr,
                             .interrupt_dispatcher = nullptr,
                             .num_queue_pairs = 4,
                             .queue_depth = 8,
                             .completion_queue_depth = 8};
  VFDevice empty_device{empty_cfg};
  assert(empty_device.queue_pair(0) == nullptr);
  assert(empty_device.process_all() == 0);

  return 0;
}
