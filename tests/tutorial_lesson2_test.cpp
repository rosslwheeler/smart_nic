#include <cassert>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <vector>

#include "nic/descriptor_ring.h"
#include "nic/dma_engine.h"
#include "nic/simple_host_memory.h"
#include "nic/trace.h"

using namespace nic;

void print_ring_state(const DescriptorRing& ring, const char* label) {
  std::cout << std::setw(20) << label << ": " << "prod=" << ring.producer_index() << " "
            << "cons=" << ring.consumer_index() << " " << "avail=" << ring.available() << " "
            << "space=" << ring.space() << " " << (ring.is_empty() ? "[EMPTY]" : "")
            << (ring.is_full() ? "[FULL]" : "") << "\n";
}

int main() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "=== Lesson 2: Understanding Descriptor Rings ===\n\n";

  // Create a small ring (4 slots) for easy visualization
  DescriptorRingConfig config{
      .descriptor_size = 16,  // Simple 16-byte descriptors
      .ring_size = 4,
      .host_backed = false,
  };

  DescriptorRing ring{config};

  std::cout << "Ring created with " << config.ring_size << " slots, " << config.descriptor_size
            << " bytes each\n\n";

  // Dummy descriptor data
  std::vector<std::byte> desc(16, std::byte{0xAA});
  std::vector<std::byte> read_buf(16);

  std::cout << "--- Initial State ---\n";
  print_ring_state(ring, "After creation");

  // Push 3 descriptors (producer advances)
  std::cout << "\n--- Pushing 3 Descriptors ---\n";
  for (int i = 0; i < 3; ++i) {
    desc[0] = std::byte{static_cast<unsigned char>(i)};  // Mark each one
    [[maybe_unused]] auto result = ring.push_descriptor(desc);
    assert(result.ok());
    print_ring_state(ring, ("After push " + std::to_string(i + 1)).c_str());
  }

  // Pop 2 descriptors (consumer advances)
  std::cout << "\n--- Popping 2 Descriptors ---\n";
  for (int i = 0; i < 2; ++i) {
    [[maybe_unused]] auto result = ring.pop_descriptor(read_buf);
    assert(result.ok());
    print_ring_state(ring, ("After pop " + std::to_string(i + 1)).c_str());
  }

  // Push until full
  std::cout << "\n--- Pushing Until Full ---\n";
  while (!ring.is_full()) {
    [[maybe_unused]] auto result = ring.push_descriptor(desc);
    assert(result.ok());
    print_ring_state(ring, "After push");
  }

  // Try to push when full (should still succeed but ring is at capacity)
  std::cout << "\n--- Ring is Full ---\n";
  std::cout << "Available slots: " << ring.space() << "\n";
  std::cout << "Descriptors ready: " << ring.available() << "\n";

  // Pop all remaining
  std::cout << "\n--- Popping All Remaining ---\n";
  while (!ring.is_empty()) {
    [[maybe_unused]] auto result = ring.pop_descriptor(read_buf);
    assert(result.ok());
    print_ring_state(ring, "After pop");
  }

  // Demonstrate wraparound
  std::cout << "\n--- Demonstrating Wraparound ---\n";
  std::cout << "Current indices: prod=" << ring.producer_index()
            << " cons=" << ring.consumer_index() << "\n";
  std::cout << "Pushing 2 more (will wrap around)...\n";

  for (int i = 0; i < 2; ++i) {
    (void) ring.push_descriptor(desc);
    print_ring_state(ring, ("Push " + std::to_string(i + 1)).c_str());
  }

  std::cout << "\n*** Lesson 2 Complete! ***\n";
  return 0;
}
