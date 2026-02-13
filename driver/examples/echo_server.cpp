#include <algorithm>
#include <array>
#include <cstdio>
#include <iostream>
#include <memory>

#include "nic/device.h"
#include "nic/simple_host_memory.h"
#include "nic_driver/driver.h"

using namespace nic_driver;

int main() {
  std::cout << "NIC Driver Echo Server Example\n";
  std::cout << "================================\n\n";

  // Create NIC device with default configuration
  nic::DeviceConfig config{};
  config.enable_queue_pair = true;  // Enable default queue pair
  auto device = std::make_unique<nic::Device>(config);
  device->reset();  // Bring device online

  std::cout << "Device created and reset\n";

  // Create and initialize driver
  NicDriver driver{};
  if (!driver.init(std::move(device))) {
    std::cerr << "Failed to initialize driver\n";
    return 1;
  }

  std::cout << "Driver initialized successfully\n";
  std::cout << "Device is " << (driver.device()->is_initialized() ? "ready" : "not ready")
            << "\n\n";

  // Get access to the queue pair for direct manipulation
  auto* qp = driver.device()->queue_pair();
  if (!qp) {
    std::cerr << "No queue pair available\n";
    return 1;
  }

  std::cout << "Queue pair ready\n";

  // Pre-populate some RX descriptors for receiving packets
  std::cout << "Adding RX descriptors...\n";

  for (std::size_t i = 0; i < 16; ++i) {
    nic::HostAddress rx_buffer_addr = 0x30000 + (i * 2048);  // 192KB + offset

    nic::RxDescriptor rx_desc{};
    rx_desc.buffer_address = rx_buffer_addr;
    rx_desc.buffer_length = 2048;
    rx_desc.checksum = nic::ChecksumMode::None;
    rx_desc.descriptor_index = static_cast<std::uint16_t>(i);

    std::vector<std::byte> rx_desc_bytes(sizeof(nic::RxDescriptor));
    std::memcpy(rx_desc_bytes.data(), &rx_desc, sizeof(nic::RxDescriptor));

    auto& rx_ring = qp->rx_ring();
    auto result = rx_ring.push_descriptor(rx_desc_bytes);

    if (!result.ok()) {
      std::cerr << "Failed to add RX descriptor " << i << "\n";
      return 1;
    }
  }

  std::cout << "RX descriptors added\n\n";
  std::cout << "Echo server ready. Sending test packets...\n";
  std::cout << "(This example sends packets and processes them internally)\n\n";

  std::uint64_t total_packets = 0;

  // Send some test packets
  for (std::size_t pkt_num = 0; pkt_num < 10; ++pkt_num) {
    // Create a test packet
    std::vector<std::byte> packet(64 + pkt_num * 10);  // Variable size packets

    // Ethernet header
    // Dst MAC: FF:FF:FF:FF:FF:FF (broadcast)
    for (std::size_t i = 0; i < 6; ++i) {
      packet[i] = std::byte{0xFF};
    }

    // Src MAC: AA:BB:CC:DD:EE:FF
    packet[6] = std::byte{0xAA};
    packet[7] = std::byte{0xBB};
    packet[8] = std::byte{0xCC};
    packet[9] = std::byte{0xDD};
    packet[10] = std::byte{0xEE};
    packet[11] = std::byte{0xFF};

    // EtherType: 0x0800 (IPv4)
    packet[12] = std::byte{0x08};
    packet[13] = std::byte{0x00};

    // Fill rest with test pattern
    for (std::size_t i = 14; i < packet.size(); ++i) {
      packet[i] = std::byte{static_cast<unsigned char>(i & 0xFF)};
    }

    total_packets++;

    // Display packet info
    std::cout << "Sending packet " << total_packets << ": " << packet.size() << " bytes [";
    for (std::size_t i = 0; i < std::min(packet.size(), std::size_t{16}); ++i) {
      std::printf("%02x ", static_cast<unsigned char>(packet[i]));
    }
    if (packet.size() > 16) {
      std::cout << "...";
    }
    std::cout << "]\n";

    // Send packet
    if (driver.send_packet(packet)) {
      std::cout << "  -> Sent successfully\n";
    } else {
      std::cout << "  -> Failed to send\n";
    }

    // Process device (moves packets from TX to RX)
    bool processed = driver.process();
    if (processed) {
      std::cout << "  -> Packet processed (looped back to RX)\n";
    }

    std::cout << "\n";
  }

  // Display final statistics
  auto stats = driver.get_stats();
  std::cout << "\n=============================\n";
  std::cout << "Final Statistics:\n";
  std::cout << "  TX: " << stats.tx_packets << " packets, " << stats.tx_bytes << " bytes\n";
  std::cout << "  RX: " << stats.rx_packets << " packets, " << stats.rx_bytes << " bytes\n";
  std::cout << "  Processed: " << stats.processed << " operations\n";
  std::cout << "=============================\n";

  return 0;
}