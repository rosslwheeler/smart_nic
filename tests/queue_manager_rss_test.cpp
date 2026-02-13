#include <cassert>
#include <numeric>
#include <vector>

#include "nic/checksum.h"
#include "nic/completion_queue.h"
#include "nic/descriptor_ring.h"
#include "nic/device.h"
#include "nic/dma_engine.h"
#include "nic/queue_manager.h"
#include "nic/rss.h"
#include "nic/simple_host_memory.h"
#include "nic/trace.h"
#include "nic/tx_rx.h"

using namespace nic;

namespace {

std::vector<std::byte> serialize_tx(const TxDescriptor& desc) {
  NIC_TRACE_SCOPED(__func__);
  std::vector<std::byte> bytes(sizeof(TxDescriptor));
  std::memcpy(bytes.data(), &desc, sizeof(TxDescriptor));
  return bytes;
}

std::vector<std::byte> serialize_rx(const RxDescriptor& desc) {
  NIC_TRACE_SCOPED(__func__);
  std::vector<std::byte> bytes(sizeof(RxDescriptor));
  std::memcpy(bytes.data(), &desc, sizeof(RxDescriptor));
  return bytes;
}

void test_rss_basic() {
  NIC_TRACE_SCOPED(__func__);
  RssConfig cfg{};
  cfg.table.assign(4, 2);  // all map to queue id 2
  RssEngine rss{cfg};

  std::uint8_t data[]{0x01, 0x02, 0x03, 0x04};
  auto selected = rss.select_queue(std::span<const std::uint8_t>(data));
  assert(selected.has_value());
  assert(*selected == 2);

  const auto& stats = rss.stats();
  assert(stats.hashes == 1);
  std::uint64_t total_hits =
      std::accumulate(stats.queue_hits.begin(), stats.queue_hits.end(), std::uint64_t{0});
  assert(total_hits == 1);
}

void test_queue_manager_multi_queue() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig mem_cfg{.size_bytes = 2048, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{mem_cfg};
  DMAEngine dma{mem};

  QueuePairConfig qp0{
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
      .tx_doorbell = nullptr,
      .rx_doorbell = nullptr,
      .tx_completion_doorbell = nullptr,
      .rx_completion_doorbell = nullptr,
      .weight = 2,
  };

  QueuePairConfig qp1 = qp0;
  qp1.queue_id = 1;
  qp1.tx_ring.queue_id = 1;
  qp1.rx_ring.queue_id = 1;
  qp1.tx_completion.queue_id = 1;
  qp1.rx_completion.queue_id = 1;
  qp1.weight = 1;

  QueueManagerConfig qm_cfg;
  qm_cfg.queue_configs.push_back(qp0);
  qm_cfg.queue_configs.push_back(qp1);

  QueueManager qm{qm_cfg, dma};

  // Prepare buffers
  std::vector<std::byte> payload0(8, std::byte{0xAA});
  std::vector<std::byte> payload1(8, std::byte{0xBB});
  assert(mem.write(100, payload0).ok());
  assert(mem.write(200, payload1).ok());

  TxDescriptor tx0{.buffer_address = 100,
                   .length = 8,
                   .checksum = ChecksumMode::None,
                   .descriptor_index = 0,
                   .checksum_value = 0};
  RxDescriptor rx0{.buffer_address = 300,
                   .buffer_length = 8,
                   .checksum = ChecksumMode::None,
                   .descriptor_index = 0};
  TxDescriptor tx1{.buffer_address = 200,
                   .length = 8,
                   .checksum = ChecksumMode::None,
                   .descriptor_index = 1,
                   .checksum_value = 0};
  RxDescriptor rx1{.buffer_address = 400,
                   .buffer_length = 8,
                   .checksum = ChecksumMode::None,
                   .descriptor_index = 1};

  assert(qm.queue(0)->tx_ring().push_descriptor(serialize_tx(tx0)).ok());
  assert(qm.queue(0)->rx_ring().push_descriptor(serialize_rx(rx0)).ok());
  assert(qm.queue(1)->tx_ring().push_descriptor(serialize_tx(tx1)).ok());
  assert(qm.queue(1)->rx_ring().push_descriptor(serialize_rx(rx1)).ok());

  // Process until both queues have completions.
  std::size_t iterations = 0;
  while (
      (qm.queue(0)->tx_completion().available() < 1 || qm.queue(1)->tx_completion().available() < 1)
      && iterations < 10) {
    qm.process_once();
    ++iterations;
  }

  assert(qm.queue(0)->tx_completion().available() == 1);
  assert(qm.queue(1)->tx_completion().available() == 1);
  assert(qm.queue(0)->rx_completion().available() == 1);
  assert(qm.queue(1)->rx_completion().available() == 1);

  auto txc0 = qm.queue(0)->tx_completion().poll_completion();
  auto txc1 = qm.queue(1)->tx_completion().poll_completion();
  auto rxc0 = qm.queue(0)->rx_completion().poll_completion();
  auto rxc1 = qm.queue(1)->rx_completion().poll_completion();
  assert(txc0.has_value() && txc1.has_value() && rxc0.has_value() && rxc1.has_value());
  assert(txc0->status == static_cast<std::uint32_t>(CompletionCode::Success));
  assert(txc1->status == static_cast<std::uint32_t>(CompletionCode::Success));
  assert(txc0->segments_produced == 1);
  assert(txc1->segments_produced == 1);
  assert(rxc0->status == static_cast<std::uint32_t>(CompletionCode::Success));
  assert(rxc1->status == static_cast<std::uint32_t>(CompletionCode::Success));

  auto stats = qm.stats();
  assert(stats.total_tx_packets == 2);
  assert(stats.total_rx_packets == 2);
  assert(stats.scheduler_advances > 0);
  assert(stats.scheduler_skips <= stats.scheduler_advances);
  auto qp0_stats = qm.queue_stats(0);
  auto qp1_stats = qm.queue_stats(1);
  assert(qp0_stats.has_value());
  assert(qp1_stats.has_value());
  assert(qp0_stats->tx_packets == 1);
  assert(qp1_stats->tx_packets == 1);
  assert(qp0_stats->tx_tso_segments == 0);
  assert(qp1_stats->tx_tso_segments == 0);
  assert(qp0_stats->tx_gso_segments == 0);
  assert(qp1_stats->tx_gso_segments == 0);
}

void test_weighted_scheduling_and_hol() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig mem_cfg{.size_bytes = 4096, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{mem_cfg};
  DMAEngine dma{mem};

  QueuePairConfig qp0{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 4,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 4,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 4, .queue_id = 0},
      .rx_completion = {.ring_size = 4, .queue_id = 0},
      .weight = 2,
  };
  QueuePairConfig qp1 = qp0;
  qp1.queue_id = 1;
  qp1.tx_ring.queue_id = 1;
  qp1.rx_ring.queue_id = 1;
  qp1.tx_completion.queue_id = 1;
  qp1.rx_completion.queue_id = 1;
  qp1.weight = 1;

  QueueManagerConfig qm_cfg;
  qm_cfg.queue_configs = {qp0, qp1};
  QueueManager qm{qm_cfg, dma};

  std::vector<std::byte> payload_a(4, std::byte{0xAA});
  std::vector<std::byte> payload_b(4, std::byte{0xBB});
  std::vector<std::byte> payload_c(4, std::byte{0xCC});
  assert(mem.write(100, payload_a).ok());
  assert(mem.write(200, payload_b).ok());
  assert(mem.write(300, payload_c).ok());

  // Queue 0 gets two TX packets, queue 1 gets one; weighted scheduler should service q0 twice
  // before q1.
  TxDescriptor tx0a{.buffer_address = 100,
                    .length = 4,
                    .checksum = ChecksumMode::None,
                    .descriptor_index = 0,
                    .checksum_value = 0};
  TxDescriptor tx0b{.buffer_address = 200,
                    .length = 4,
                    .checksum = ChecksumMode::None,
                    .descriptor_index = 1,
                    .checksum_value = 0};
  TxDescriptor tx1{.buffer_address = 300,
                   .length = 4,
                   .checksum = ChecksumMode::None,
                   .descriptor_index = 2,
                   .checksum_value = 0};
  RxDescriptor rx0a{.buffer_address = 400,
                    .buffer_length = 4,
                    .checksum = ChecksumMode::None,
                    .descriptor_index = 0};
  RxDescriptor rx0b{.buffer_address = 500,
                    .buffer_length = 4,
                    .checksum = ChecksumMode::None,
                    .descriptor_index = 1};
  RxDescriptor rx1{.buffer_address = 600,
                   .buffer_length = 4,
                   .checksum = ChecksumMode::None,
                   .descriptor_index = 2};

  assert(qm.queue(0)->tx_ring().push_descriptor(serialize_tx(tx0a)).ok());
  assert(qm.queue(0)->rx_ring().push_descriptor(serialize_rx(rx0a)).ok());
  assert(qm.queue(0)->tx_ring().push_descriptor(serialize_tx(tx0b)).ok());
  assert(qm.queue(0)->rx_ring().push_descriptor(serialize_rx(rx0b)).ok());
  assert(qm.queue(1)->tx_ring().push_descriptor(serialize_tx(tx1)).ok());
  assert(qm.queue(1)->rx_ring().push_descriptor(serialize_rx(rx1)).ok());

  for (std::size_t i = 0; i < 3; ++i) {
    qm.process_once();
  }

  assert(qm.queue(0)->tx_completion().available() == 2);
  assert(qm.queue(1)->tx_completion().available() == 1);
  auto stats = qm.stats();
  assert(stats.scheduler_advances >= 3);

  // Head-of-line skip: clear queue 0 descriptors, leave only queue 1 active.
  qm.reset();
  assert(qm.queue(1)->tx_ring().push_descriptor(serialize_tx(tx1)).ok());
  assert(qm.queue(1)->rx_ring().push_descriptor(serialize_rx(rx1)).ok());
  qm.process_once();
  auto stats_after = qm.stats();
  assert(stats_after.scheduler_skips >= 1);
  assert(qm.queue(1)->tx_completion().available() == 1);
}

void test_rss_stats_distribution() {
  NIC_TRACE_SCOPED(__func__);
  RssConfig cfg{};
  cfg.table = {0, 1, 2, 3};
  RssEngine rss{cfg};

  std::uint8_t d0[]{0xAA, 0xBB, 0xCC, 0xDD};
  std::uint8_t d1[]{0x10, 0x20, 0x30, 0x40};
  std::uint8_t d2[]{0x01, 0x00, 0x00, 0x01};

  auto q0 = rss.select_queue(std::span<const std::uint8_t>(d0));
  auto q1 = rss.select_queue(std::span<const std::uint8_t>(d1));
  auto q2 = rss.select_queue(std::span<const std::uint8_t>(d2));
  assert(q0.has_value());
  assert(q1.has_value());
  assert(q2.has_value());

  const auto& stats = rss.stats();
  assert(stats.hashes == 3);
  std::uint64_t total_hits =
      std::accumulate(stats.queue_hits.begin(), stats.queue_hits.end(), std::uint64_t{0});
  assert(total_hits == 3);
}

void test_rss_steering_to_queues() {
  NIC_TRACE_SCOPED(__func__);
  RssConfig cfg{};
  cfg.table = {0, 1};  // Even hashes -> 0, odd -> 1
  RssEngine rss{cfg};

  // Construct three different inputs; expect some to land on queue 0 and some on 1.
  std::uint8_t d0[]{0x00, 0x00, 0x00, 0x01};  // likely even hash
  std::uint8_t d1[]{0xFF, 0xEE, 0xDD, 0xCC};  // different hash
  std::uint8_t d2[]{0x12, 0x34, 0x56, 0x78};  // different hash

  auto q0 = rss.select_queue(std::span<const std::uint8_t>(d0));
  auto q1 = rss.select_queue(std::span<const std::uint8_t>(d1));
  auto q2 = rss.select_queue(std::span<const std::uint8_t>(d2));

  assert(q0.has_value());
  assert(q1.has_value());
  assert(q2.has_value());

  // Ensure at least one hit per queue id in the table.
  const auto& stats = rss.stats();
  assert(stats.queue_hits.size() == cfg.table.size());
  std::uint64_t hits0 = stats.queue_hits[0];
  std::uint64_t hits1 = stats.queue_hits[1];
  assert(hits0 >= 1);
  assert(hits1 >= 1);
}

void test_rss_defaults_and_reset() {
  NIC_TRACE_SCOPED(__func__);
  RssEngine rss;

  rss.set_key({});
  rss.set_table({});

  std::vector<std::uint8_t> empty;
  assert(rss.hash(empty) == 0);

  const auto& stats_before = rss.stats();
  assert(stats_before.hashes >= 1);
  rss.reset_stats();
  const auto& stats_after = rss.stats();
  assert(stats_after.hashes == 0);
}

void test_queue_manager_empty_config() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig mem_cfg{.size_bytes = 1024, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{mem_cfg};
  DMAEngine dma{mem};

  QueueManagerConfig qm_cfg;
  QueueManager qm{qm_cfg, dma};
  assert(qm.queue_count() == 0);
  assert(qm.queue(0) == nullptr);
  assert(!qm.queue_stats(0).has_value());
  assert(!qm.process_once());

  qm.reset();
  std::string summary = qm.stats_summary();
  assert(summary.find("qm tx_pkts=") != std::string::npos);
}

void test_queue_manager_edge_cases() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig mem_cfg{.size_bytes = 1024, .page_size = 64, .iommu_enabled = false};
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
      .weight = 0,
  };

  QueueManagerConfig qm_cfg;
  qm_cfg.queue_configs.push_back(qp_cfg);
  QueueManager qm{qm_cfg, dma};

  const QueueManager& cqm = qm;
  assert(cqm.queue(0) != nullptr);
  assert(cqm.queue(1) == nullptr);

  assert(!qm.process_once());
}

}  // namespace

int main() {
  NIC_TRACE_SCOPED(__func__);
  test_rss_basic();
  test_queue_manager_multi_queue();
  test_weighted_scheduling_and_hol();
  test_rss_stats_distribution();
  test_rss_steering_to_queues();
  test_rss_defaults_and_reset();
  test_queue_manager_empty_config();
  test_queue_manager_edge_cases();

  // Device wiring: two queues via QueueManager + RSS
  nic::DeviceConfig config{};
  config.enable_queue_pair = false;
  config.enable_queue_manager = true;
  QueuePairConfig qp0{
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
      .weight = 1,
  };
  QueuePairConfig qp1 = qp0;
  qp1.queue_id = 1;
  qp1.tx_ring.queue_id = 1;
  qp1.rx_ring.queue_id = 1;
  qp1.tx_completion.queue_id = 1;
  qp1.rx_completion.queue_id = 1;
  qp1.weight = 1;

  config.queue_manager_config.queue_configs = {qp0, qp1};
  config.rss_config.table = {0, 1};

  nic::Device device{config};
  device.reset();
  assert(device.queue_manager() != nullptr);
  assert(device.rss_engine() != nullptr);
  auto qm_stats = device.queue_manager_stats();
  auto rss_stats = device.rss_stats();
  (void) qm_stats;
  (void) rss_stats;
  return 0;
}
