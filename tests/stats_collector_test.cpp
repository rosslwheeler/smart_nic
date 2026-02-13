#include "nic/stats_collector.h"

#include <atomic>
#include <cassert>

using namespace nic;

int main() {
  StatsCollector collector;

  collector.record_tx_packet(1, 100);
  collector.record_rx_packet(2, 200);
  collector.record_error(1, StatsCollector::ErrorType::TxDMAError);
  collector.record_error(2, StatsCollector::ErrorType::RxDroppedFull);

  const auto& q1 = collector.queue_stats(1);
  assert(q1.tx_packets.load(std::memory_order_relaxed) == 1);
  assert(q1.tx_bytes.load(std::memory_order_relaxed) == 100);
  assert(q1.tx_errors.load(std::memory_order_relaxed) == 1);

  const auto& q2 = collector.queue_stats(2);
  assert(q2.rx_packets.load(std::memory_order_relaxed) == 1);
  assert(q2.rx_bytes.load(std::memory_order_relaxed) == 200);
  assert(q2.rx_errors.load(std::memory_order_relaxed) == 1);

  const auto& q3 = collector.queue_stats(3);
  assert(q3.tx_packets.load(std::memory_order_relaxed) == 0);

  collector.record_vf_tx_packet(1, 300);
  collector.record_vf_rx_packet(1, 400);
  collector.record_vf_mailbox_message(1);
  const auto& vf1 = collector.vf_stats(1);
  assert(vf1.tx_packets.load(std::memory_order_relaxed) == 1);
  assert(vf1.tx_bytes.load(std::memory_order_relaxed) == 300);
  assert(vf1.rx_packets.load(std::memory_order_relaxed) == 1);
  assert(vf1.rx_bytes.load(std::memory_order_relaxed) == 400);
  assert(vf1.mailbox_messages.load(std::memory_order_relaxed) == 1);

  const auto& vf2 = collector.vf_stats(2);
  assert(vf2.tx_packets.load(std::memory_order_relaxed) == 0);

  auto port = collector.port_stats();
  assert(port.tx_bytes == 100);
  assert(port.tx_packets == 1);
  assert(port.rx_bytes == 200);
  assert(port.rx_packets == 1);
  assert(port.tx_errors == 1);
  assert(port.rx_errors == 1);

  collector.reset_queue(1);
  const auto& q1_reset = collector.queue_stats(1);
  assert(q1_reset.tx_packets.load(std::memory_order_relaxed) == 0);

  collector.reset_vf(1);
  const auto& vf1_reset = collector.vf_stats(1);
  assert(vf1_reset.mailbox_messages.load(std::memory_order_relaxed) == 0);

  collector.reset_all();
  const auto& q1_empty = collector.queue_stats(1);
  const auto& vf1_empty = collector.vf_stats(1);
  assert(q1_empty.tx_packets.load(std::memory_order_relaxed) == 0);
  assert(vf1_empty.tx_packets.load(std::memory_order_relaxed) == 0);

  return 0;
}
