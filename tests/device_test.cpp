#include "nic/device.h"

#include <cassert>

#include "nic/interrupt_dispatcher.h"
#include "nic/queue_manager.h"
#include "nic/simple_host_memory.h"

using namespace nic;

namespace {

void test_device_with_external_components() {
  HostMemoryConfig mem_cfg{.size_bytes = 4096, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{mem_cfg};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  MsixTable table{1};
  MsixMapping mapping{1, 0};
  mapping.set_queue_vector(0, 0);
  CoalesceConfig coalesce_cfg{.packet_threshold = 1, .timer_threshold_us = 0};
  InterruptDispatcher dispatcher{table, mapping, coalesce_cfg, [](std::uint16_t, std::uint32_t) {}};

  RssEngine rss;

  DeviceConfig config{};
  config.host_memory = &mem;
  config.dma_engine = &dma;
  config.queue_pair = &qp;
  config.enable_queue_pair = true;
  config.enable_queue_manager = false;
  config.rss_engine = &rss;
  config.interrupt_dispatcher = &dispatcher;

  Device device{config};
  device.reset();

  assert(device.queue_pair() == &qp);
  assert(device.queue_manager() == nullptr);
  assert(device.rss_engine() == &rss);

  device.write_config8(0x00, 0x12);
  device.write_config16(0x02, 0x3456);
  device.write_config32(0x04, 0x789ABCDE);

  assert(device.set_msix_queue_vector(0, 0));
  assert(device.mask_msix_vector(0, true));
  assert(device.enable_msix_vector(0, true));

  assert(!device.process_queue_once());
}

void test_device_with_queue_manager() {
  HostMemoryConfig mem_cfg{.size_bytes = 4096, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{mem_cfg};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };

  QueueManagerConfig qm_cfg;
  qm_cfg.queue_configs.push_back(qp_cfg);
  QueueManager qm{qm_cfg, dma};

  DeviceConfig config{};
  config.host_memory = &mem;
  config.dma_engine = &dma;
  config.enable_queue_manager = true;
  config.enable_queue_pair = true;
  config.queue_manager = &qm;
  config.queue_manager_config = qm_cfg;

  Device device{config};
  device.reset();

  assert(device.queue_manager() == &qm);
  assert(device.queue_pair() == nullptr);
  assert(!device.process_queue_once());
}

void test_device_without_queues() {
  DeviceConfig config{};
  config.enable_queue_pair = false;
  config.enable_queue_manager = false;

  Device device{config};
  device.reset();
  assert(device.queue_pair() == nullptr);
  assert(device.queue_manager() == nullptr);
  assert(!device.process_queue_once());
}

}  // namespace

int main() {
  test_device_with_external_components();
  test_device_with_queue_manager();
  test_device_without_queues();
  return 0;
}
