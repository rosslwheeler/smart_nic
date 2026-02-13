#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include "nic/dma_engine.h"
#include "nic/queue_pair.h"
#include "nic/simple_host_memory.h"
#include "nic/trace.h"
#include "nic/tx_rx.h"

using namespace nic;

// Custom priority scheduler
class PriorityQueueScheduler {
public:
  struct PriorityQueue {
    std::unique_ptr<QueuePair> qp;
    std::uint8_t priority;  // Higher = more important
    std::string name;
  };

  void add_queue(std::unique_ptr<QueuePair> qp, std::uint8_t priority, const std::string& name) {
    queues_.push_back({std::move(qp), priority, name});
    // Sort by priority (highest first)
    std::sort(queues_.begin(), queues_.end(), [](const auto& a, const auto& b) {
      return a.priority > b.priority;
    });
  }

  // Process highest priority queue that has work
  bool process_once() {
    for (auto& pq : queues_) {
      if (!pq.qp->tx_ring().is_empty()) {
        std::cout << "  [Scheduler] Processing queue '" << pq.name << "' (priority "
                  << static_cast<int>(pq.priority) << ")\n";
        return pq.qp->process_once();
      }
    }
    return false;  // No work
  }

  QueuePair& get_queue(const std::string& name) {
    for (auto& pq : queues_) {
      if (pq.name == name) {
        return *pq.qp;
      }
    }
    throw std::runtime_error("Queue not found: " + name);
  }

  void print_stats() const {
    std::cout << "\n=== Queue Statistics ===\n";
    for (const auto& pq : queues_) {
      const auto& stats = pq.qp->stats();
      std::cout << "Queue '" << pq.name << "' (pri=" << static_cast<int>(pq.priority)
                << "): TX=" << stats.tx_packets << " RX=" << stats.rx_packets << "\n";
    }
  }

private:
  std::vector<PriorityQueue> queues_;
};

void enqueue_packet(QueuePair& qp,
                    SimpleHostMemory& memory,
                    HostAddress tx_addr,
                    HostAddress rx_addr,
                    std::size_t size) {
  // Write packet
  std::vector<std::byte> packet(size, std::byte{0x42});
  (void) memory.write(tx_addr, packet);

  // TX descriptor
  TxDescriptor tx_desc{.buffer_address = tx_addr, .length = static_cast<std::uint32_t>(size)};
  std::vector<std::byte> tx_bytes(sizeof(tx_desc));
  std::memcpy(tx_bytes.data(), &tx_desc, sizeof(tx_desc));
  (void) qp.tx_ring().push_descriptor(tx_bytes);

  // RX descriptor
  RxDescriptor rx_desc{.buffer_address = rx_addr,
                       .buffer_length = static_cast<std::uint32_t>(size * 2)};
  std::vector<std::byte> rx_bytes(sizeof(rx_desc));
  std::memcpy(rx_bytes.data(), &rx_desc, sizeof(rx_desc));
  (void) qp.rx_ring().push_descriptor(rx_bytes);
}

int main() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "=== Lesson 5: Priority Queue Scheduler ===\n\n";

  HostMemoryConfig mem_config{.size_bytes = 65536};
  SimpleHostMemory memory{mem_config};
  DMAEngine dma{memory};

  PriorityQueueScheduler scheduler;

  // Create queues with different priorities
  auto make_qp = [&](std::uint16_t id) {
    QueuePairConfig config{
        .queue_id = id,
        .tx_ring = {.descriptor_size = sizeof(TxDescriptor), .ring_size = 8},
        .rx_ring = {.descriptor_size = sizeof(RxDescriptor), .ring_size = 8},
        .tx_completion = {.ring_size = 8},
        .rx_completion = {.ring_size = 8},
    };
    return std::make_unique<QueuePair>(config, dma);
  };

  scheduler.add_queue(make_qp(0), 1, "best-effort");
  scheduler.add_queue(make_qp(1), 5, "video");
  scheduler.add_queue(make_qp(2), 10, "voice");

  std::cout << "Created 3 queues:\n";
  std::cout << "  - 'voice' (priority 10) - highest\n";
  std::cout << "  - 'video' (priority 5)\n";
  std::cout << "  - 'best-effort' (priority 1) - lowest\n\n";

  // Enqueue packets in reverse priority order
  std::cout << "Enqueueing packets (lowest priority first):\n";

  std::cout << "  Enqueue to 'best-effort'\n";
  enqueue_packet(scheduler.get_queue("best-effort"), memory, 0x1000, 0x2000, 64);

  std::cout << "  Enqueue to 'video'\n";
  enqueue_packet(scheduler.get_queue("video"), memory, 0x3000, 0x4000, 64);

  std::cout << "  Enqueue to 'voice'\n";
  enqueue_packet(scheduler.get_queue("voice"), memory, 0x5000, 0x6000, 64);

  // Process - should handle voice first, then video, then best-effort
  std::cout << "\nProcessing (highest priority first):\n";
  while (scheduler.process_once()) {
    // Continue until no work
  }

  scheduler.print_stats();

  std::cout << "\n*** Lesson 5 Complete! ***\n";
  return 0;
}
