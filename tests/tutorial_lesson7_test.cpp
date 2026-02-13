#include <iostream>
#include <vector>

#include "nic/interrupt_dispatcher.h"
#include "nic/msix.h"
#include "nic/trace.h"

using namespace nic;

int main() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "=== Lesson 7: Interrupt Coalescing Tuning ===\n\n";

  // Create MSI-X table with 4 vectors
  MsixTable msix_table{4};
  for (int i = 0; i < 4; ++i) {
    msix_table.set_vector(i,
                          MsixVector{
                              .address = 0xFEE00000 + static_cast<std::uint64_t>(i) * 0x10,
                              .data = static_cast<std::uint32_t>(0x4000 + i),
                              .enabled = true,
                              .masked = false,
                          });
  }

  // Map queues to vectors
  MsixMapping mapping{4, 0};
  mapping.set_queue_vector(0, 0);  // Queue 0 -> Vector 0
  mapping.set_queue_vector(1, 1);  // Queue 1 -> Vector 1
  mapping.set_queue_vector(2, 2);  // Queue 2 -> Vector 2
  mapping.set_queue_vector(3, 3);  // Queue 3 -> Vector 3

  int total_interrupts = 0;
  auto deliver_fn = [&](std::uint16_t vector, std::uint32_t batch_size) {
    total_interrupts++;
    std::cout << "  [IRQ] Vector " << vector << " fired with batch size " << batch_size << "\n";
  };

  // --- Scenario 1: No Coalescing (threshold = 1) ---
  std::cout << "--- Scenario 1: No Coalescing (threshold=1) ---\n";
  {
    CoalesceConfig config{.packet_threshold = 1};
    InterruptDispatcher dispatcher{msix_table, mapping, config, deliver_fn};

    total_interrupts = 0;
    std::cout << "Sending 10 completions:\n";
    for (int i = 0; i < 10; ++i) {
      dispatcher.on_completion(InterruptEvent{0, {}});
    }
    std::cout << "Total interrupts: " << total_interrupts << "\n\n";
  }

  // --- Scenario 2: Moderate Coalescing (threshold = 4) ---
  std::cout << "--- Scenario 2: Moderate Coalescing (threshold=4) ---\n";
  {
    CoalesceConfig config{.packet_threshold = 4};
    InterruptDispatcher dispatcher{msix_table, mapping, config, deliver_fn};

    total_interrupts = 0;
    std::cout << "Sending 10 completions:\n";
    for (int i = 0; i < 10; ++i) {
      dispatcher.on_completion(InterruptEvent{0, {}});
    }
    std::cout << "Flushing remaining...\n";
    dispatcher.flush();
    std::cout << "Total interrupts: " << total_interrupts << "\n\n";
  }

  // --- Scenario 3: Aggressive Coalescing (threshold = 8) ---
  std::cout << "--- Scenario 3: Aggressive Coalescing (threshold=8) ---\n";
  {
    CoalesceConfig config{.packet_threshold = 8};
    InterruptDispatcher dispatcher{msix_table, mapping, config, deliver_fn};

    total_interrupts = 0;
    std::cout << "Sending 10 completions:\n";
    for (int i = 0; i < 10; ++i) {
      dispatcher.on_completion(InterruptEvent{0, {}});
    }
    std::cout << "Flushing remaining...\n";
    dispatcher.flush();
    std::cout << "Total interrupts: " << total_interrupts << "\n\n";
  }

  // --- Scenario 4: Per-Queue Configuration ---
  std::cout << "--- Scenario 4: Per-Queue Configuration ---\n";
  {
    // Start with default threshold = 4
    CoalesceConfig default_config{.packet_threshold = 4};
    InterruptDispatcher dispatcher{msix_table, mapping, default_config, deliver_fn};

    // Queue 0: Low latency (threshold = 1)
    dispatcher.set_queue_coalesce_config(0, CoalesceConfig{.packet_threshold = 1});
    // Queue 1: High throughput (threshold = 8)
    dispatcher.set_queue_coalesce_config(1, CoalesceConfig{.packet_threshold = 8});

    total_interrupts = 0;
    std::cout << "Sending 4 completions to each queue:\n";
    for (int i = 0; i < 4; ++i) {
      std::cout << "  Completion to queue 0 (low-latency):\n";
      dispatcher.on_completion(InterruptEvent{0, {}});
      std::cout << "  Completion to queue 1 (high-throughput):\n";
      dispatcher.on_completion(InterruptEvent{1, {}});
    }
    std::cout << "Flushing...\n";
    dispatcher.flush();
    std::cout << "Total interrupts: " << total_interrupts << "\n";
  }

  std::cout << "\n*** Lesson 7 Complete! ***\n";
  return 0;
}
