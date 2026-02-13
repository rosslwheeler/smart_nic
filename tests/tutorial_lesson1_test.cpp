#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#include "nic/dma_engine.h"
#include "nic/queue_pair.h"
#include "nic/simple_host_memory.h"
#include "nic/trace.h"
#include "nic/tx_rx.h"

using namespace nic;

int main() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "=== Lesson 1: Your First Packet ===\n\n";

  // Step 1: Create host memory (simulates system RAM)
  HostMemoryConfig mem_config{
      .size_bytes = 4096,
      .page_size = 64,
      .iommu_enabled = false,
  };
  SimpleHostMemory memory{mem_config};
  std::cout << "1. Created host memory: " << mem_config.size_bytes << " bytes\n";

  // Step 2: Create DMA engine (moves data between NIC and host memory)
  DMAEngine dma{memory};
  std::cout << "2. Created DMA engine\n";

  // Step 3: Configure a queue pair (TX + RX rings)
  QueuePairConfig qp_config{
      .queue_id = 0,
      .tx_ring =
          {
              .descriptor_size = sizeof(TxDescriptor),
              .ring_size = 4,
              .host_backed = false,
          },
      .rx_ring =
          {
              .descriptor_size = sizeof(RxDescriptor),
              .ring_size = 4,
              .host_backed = false,
          },
      .tx_completion = {.ring_size = 4},
      .rx_completion = {.ring_size = 4},
  };
  QueuePair qp{qp_config, dma};
  std::cout << "3. Created queue pair with 4-entry rings\n";

  // Step 4: Create a packet in host memory
  std::vector<std::byte> packet(64);
  for (size_t i = 0; i < packet.size(); ++i) {
    packet[i] = std::byte{static_cast<unsigned char>('A' + (i % 26))};
  }

  HostAddress tx_addr = 0x100;  // Where we put the TX packet
  HostAddress rx_addr = 0x200;  // Where we want the RX packet

  [[maybe_unused]] auto write_result = memory.write(tx_addr, packet);
  assert(write_result.ok());
  std::cout << "4. Wrote " << packet.size() << " byte packet to address 0x" << std::hex << tx_addr
            << std::dec << "\n";

  // Step 5: Create TX descriptor (tells NIC where packet is)
  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(packet.size()),
      .checksum = ChecksumMode::None,
      .descriptor_index = 0,
  };

  std::vector<std::byte> tx_bytes(sizeof(tx_desc));
  std::memcpy(tx_bytes.data(), &tx_desc, sizeof(tx_desc));
  auto push_result = qp.tx_ring().push_descriptor(tx_bytes);
  assert(push_result.ok());
  std::cout << "5. Pushed TX descriptor to ring\n";

  // Step 6: Create RX descriptor (tells NIC where to put received packet)
  RxDescriptor rx_desc{
      .buffer_address = rx_addr,
      .buffer_length = 128,  // Must be >= packet size
      .descriptor_index = 0,
  };

  std::vector<std::byte> rx_bytes(sizeof(rx_desc));
  std::memcpy(rx_bytes.data(), &rx_desc, sizeof(rx_desc));
  push_result = qp.rx_ring().push_descriptor(rx_bytes);
  assert(push_result.ok());
  std::cout << "6. Pushed RX descriptor to ring\n";

  // Step 7: Process the packet (NIC does its work)
  std::cout << "7. Processing packet...\n";
  bool work_done = qp.process_once();
  assert(work_done);
  std::cout << "   Work done: " << (work_done ? "yes" : "no") << "\n";

  // Step 8: Check completions
  [[maybe_unused]] auto tx_cpl = qp.tx_completion().poll_completion();
  [[maybe_unused]] auto rx_cpl = qp.rx_completion().poll_completion();

  assert(tx_cpl.has_value());
  assert(rx_cpl.has_value());
  assert(tx_cpl->status == static_cast<std::uint32_t>(CompletionCode::Success));
  assert(rx_cpl->status == static_cast<std::uint32_t>(CompletionCode::Success));
  std::cout << "8. TX and RX completions received (both Success)\n";

  // Step 9: Verify the received packet matches
  std::vector<std::byte> received(64);
  [[maybe_unused]] auto read_result = memory.read(rx_addr, received);
  assert(read_result.ok());
  assert(received == packet);
  std::cout << "9. Received packet matches sent packet!\n";

  // Step 10: Print statistics
  const auto& stats = qp.stats();
  std::cout << "\n=== Statistics ===\n";
  std::cout << "TX packets: " << stats.tx_packets << "\n";
  std::cout << "RX packets: " << stats.rx_packets << "\n";
  std::cout << "TX bytes:   " << stats.tx_bytes << "\n";
  std::cout << "RX bytes:   " << stats.rx_bytes << "\n";

  std::cout << "\n*** Lesson 1 Complete! ***\n";
  return 0;
}
