#include <cassert>
#include <cstring>
#include <iostream>

#include "nic/dma_engine.h"
#include "nic/queue_pair.h"
#include "nic/simple_host_memory.h"
#include "nic/trace.h"
#include "nic/tx_rx.h"

using namespace nic;

void setup_basic_qp(SimpleHostMemory& memory,
                    DMAEngine& /*dma*/,
                    QueuePair& qp,
                    HostAddress tx_addr,
                    std::size_t packet_size) {
  // Write packet data
  std::vector<std::byte> packet(packet_size, std::byte{0x42});
  (void) memory.write(tx_addr, packet);

  // Push TX descriptor
  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(packet_size),
      .checksum = ChecksumMode::None,
      .descriptor_index = 0,
  };
  std::vector<std::byte> tx_bytes(sizeof(tx_desc));
  std::memcpy(tx_bytes.data(), &tx_desc, sizeof(tx_desc));
  (void) qp.tx_ring().push_descriptor(tx_bytes);
}

int main() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "=== Lesson 4: Implementing Packet Drops ===\n\n";

  HostMemoryConfig mem_config{.size_bytes = 8192};
  SimpleHostMemory memory{mem_config};
  DMAEngine dma{memory};

  // --- Scenario 1: No RX Descriptor Available ---
  std::cout << "--- Scenario 1: No RX Descriptor (drop) ---\n";
  {
    QueuePairConfig qp_config{
        .queue_id = 0,
        .tx_ring = {.descriptor_size = sizeof(TxDescriptor), .ring_size = 4},
        .rx_ring = {.descriptor_size = sizeof(RxDescriptor), .ring_size = 4},
        .tx_completion = {.ring_size = 4},
        .rx_completion = {.ring_size = 4},
    };
    QueuePair qp{qp_config, dma};

    setup_basic_qp(memory, dma, qp, 0x100, 64);
    // Note: We DON'T push an RX descriptor!

    qp.process_once();

    auto tx_cpl = qp.tx_completion().poll_completion();
    assert(tx_cpl.has_value());
    std::cout << "TX Completion status: " << static_cast<int>(tx_cpl->status) << " (";
    if (tx_cpl->status == static_cast<std::uint32_t>(CompletionCode::Success)) {
      std::cout << "Success";
    } else if (tx_cpl->status == static_cast<std::uint32_t>(CompletionCode::NoDescriptor)) {
      std::cout << "NoDescriptor";
    } else {
      std::cout << "Other";
    }
    std::cout << ")\n";
    std::cout << "Drops (no RX desc): " << qp.stats().drops_no_rx_desc << "\n\n";
  }

  // --- Scenario 2: RX Buffer Too Small ---
  std::cout << "--- Scenario 2: RX Buffer Too Small (drop) ---\n";
  {
    QueuePairConfig qp_config{
        .queue_id = 1,
        .tx_ring = {.descriptor_size = sizeof(TxDescriptor), .ring_size = 4},
        .rx_ring = {.descriptor_size = sizeof(RxDescriptor), .ring_size = 4},
        .tx_completion = {.ring_size = 4},
        .rx_completion = {.ring_size = 4},
    };
    QueuePair qp{qp_config, dma};

    // Send 100 byte packet
    setup_basic_qp(memory, dma, qp, 0x200, 100);

    // But provide only 50 byte RX buffer
    RxDescriptor rx_desc{
        .buffer_address = 0x300,
        .buffer_length = 50,  // Too small!
        .descriptor_index = 0,
    };
    std::vector<std::byte> rx_bytes(sizeof(rx_desc));
    std::memcpy(rx_bytes.data(), &rx_desc, sizeof(rx_desc));
    (void) qp.rx_ring().push_descriptor(rx_bytes);

    qp.process_once();

    auto rx_cpl = qp.rx_completion().poll_completion();
    if (rx_cpl.has_value()) {
      std::cout << "RX Completion status: " << static_cast<int>(rx_cpl->status) << " (";
      if (rx_cpl->status == static_cast<std::uint32_t>(CompletionCode::Success)) {
        std::cout << "Success";
      } else if (rx_cpl->status == static_cast<std::uint32_t>(CompletionCode::BufferTooSmall)) {
        std::cout << "BufferTooSmall";
      } else {
        std::cout << "Other";
      }
      std::cout << ")\n";
    }
    std::cout << "Drops (buffer small): " << qp.stats().drops_buffer_small << "\n\n";
  }

  // --- Scenario 3: Successful Transfer ---
  std::cout << "--- Scenario 3: Successful Transfer ---\n";
  {
    QueuePairConfig qp_config{
        .queue_id = 2,
        .tx_ring = {.descriptor_size = sizeof(TxDescriptor), .ring_size = 4},
        .rx_ring = {.descriptor_size = sizeof(RxDescriptor), .ring_size = 4},
        .tx_completion = {.ring_size = 4},
        .rx_completion = {.ring_size = 4},
    };
    QueuePair qp{qp_config, dma};

    setup_basic_qp(memory, dma, qp, 0x400, 64);

    // Provide adequate RX buffer
    RxDescriptor rx_desc{
        .buffer_address = 0x500,
        .buffer_length = 128,  // Plenty of space
        .descriptor_index = 0,
    };
    std::vector<std::byte> rx_bytes(sizeof(rx_desc));
    std::memcpy(rx_bytes.data(), &rx_desc, sizeof(rx_desc));
    (void) qp.rx_ring().push_descriptor(rx_bytes);

    qp.process_once();

    auto tx_cpl = qp.tx_completion().poll_completion();
    auto rx_cpl = qp.rx_completion().poll_completion();

    std::cout << "TX status: "
              << (tx_cpl->status == static_cast<std::uint32_t>(CompletionCode::Success) ? "Success"
                                                                                        : "Error")
              << "\n";
    std::cout << "RX status: "
              << (rx_cpl->status == static_cast<std::uint32_t>(CompletionCode::Success) ? "Success"
                                                                                        : "Error")
              << "\n";
    std::cout << "TX packets: " << qp.stats().tx_packets << "\n";
    std::cout << "RX packets: " << qp.stats().rx_packets << "\n";
  }

  std::cout << "\n*** Lesson 4 Complete! ***\n";
  return 0;
}
