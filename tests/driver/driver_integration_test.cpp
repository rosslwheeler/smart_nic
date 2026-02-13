#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include "nic/device.h"
#include "nic/packet_generator.h"
#include "nic/simple_host_memory.h"
#include "nic_driver/driver.h"

using namespace nic_driver;

std::unique_ptr<nic::Device> create_test_device() {
  nic::DeviceConfig config{};
  config.host_memory = nullptr;     // Will use default SimpleHostMemory
  config.enable_queue_pair = true;  // Enable the default queue pair
  auto device = std::make_unique<nic::Device>(config);
  device->reset();  // Bring device online
  return device;
}

void test_driver_initialization() {
  std::cout << "Test: Driver initialization... ";

  NicDriver driver{};
  assert(!driver.is_initialized());

  auto device = create_test_device();
  [[maybe_unused]] bool init_result = driver.init(std::move(device));
  assert(init_result);
  assert(driver.is_initialized());

  driver.reset();
  assert(!driver.is_initialized());

  std::cout << "PASS\n";
}

void test_packet_send() {
  std::cout << "Test: Packet send... ";

  NicDriver driver{};
  auto device = create_test_device();
  driver.init(std::move(device));

  // Create a simple test packet (64 bytes)
  std::vector<std::byte> packet(64);
  // Destination MAC
  packet[0] = std::byte{0x00};
  packet[1] = std::byte{0x11};
  packet[2] = std::byte{0x22};
  packet[3] = std::byte{0x33};
  packet[4] = std::byte{0x44};
  packet[5] = std::byte{0x55};
  // Source MAC
  packet[6] = std::byte{0xAA};
  packet[7] = std::byte{0xBB};
  packet[8] = std::byte{0xCC};
  packet[9] = std::byte{0xDD};
  packet[10] = std::byte{0xEE};
  packet[11] = std::byte{0xFF};
  // EtherType (0x0800 = IPv4)
  packet[12] = std::byte{0x08};
  packet[13] = std::byte{0x00};

  // Send packet
  [[maybe_unused]] bool send_result = driver.send_packet(packet);
  assert(send_result);

  // Check driver stats
  [[maybe_unused]] auto stats = driver.get_stats();
  assert(stats.tx_packets == 1);
  assert(stats.tx_bytes == 64);

  std::cout << "PASS\n";
}

void test_packet_processing() {
  std::cout << "Test: Packet processing (TX -> RX loopback)... ";

  NicDriver driver{};
  auto device = create_test_device();

  // Need to add RX descriptors before processing
  auto* dev_ptr = device.get();
  driver.init(std::move(device));

  // Create and send a packet
  std::vector<std::byte> packet(128);
  packet[0] = std::byte{0xFF};  // Broadcast MAC
  packet[1] = std::byte{0xFF};
  packet[2] = std::byte{0xFF};
  packet[3] = std::byte{0xFF};
  packet[4] = std::byte{0xFF};
  packet[5] = std::byte{0xFF};

  driver.send_packet(packet);

  // Add RX descriptor for receiving
  auto* qp = dev_ptr->queue_pair();
  assert(qp != nullptr);

  // Allocate RX buffer
  nic::HostAddress rx_buffer_addr = 0x20000;  // 128KB offset
  nic::RxDescriptor rx_desc{};
  rx_desc.buffer_address = rx_buffer_addr;
  rx_desc.buffer_length = 2048;
  rx_desc.checksum = nic::ChecksumMode::None;
  rx_desc.descriptor_index = 0;

  // Push RX descriptor
  std::vector<std::byte> rx_desc_bytes(sizeof(nic::RxDescriptor));
  std::memcpy(rx_desc_bytes.data(), &rx_desc, sizeof(nic::RxDescriptor));
  auto& rx_ring = qp->rx_ring();
  [[maybe_unused]] auto rx_push_result = rx_ring.push_descriptor(rx_desc_bytes);
  assert(rx_push_result.ok());

  // Process device (should move packet from TX to RX)
  [[maybe_unused]] bool processed = driver.process();
  assert(processed);

  // Check stats
  [[maybe_unused]] auto stats = driver.get_stats();
  assert(stats.processed >= 1);
  assert(stats.rx_packets >= 1);

  std::cout << "PASS\n";
}

void test_statistics() {
  std::cout << "Test: Statistics tracking... ";

  NicDriver driver{};
  auto device = create_test_device();
  driver.init(std::move(device));

  auto stats = driver.get_stats();
  assert(stats.tx_packets == 0);
  assert(stats.tx_bytes == 0);

  // Send a packet
  std::vector<std::byte> packet(256);
  driver.send_packet(packet);

  stats = driver.get_stats();
  assert(stats.tx_packets == 1);
  assert(stats.tx_bytes == 256);

  // Clear stats
  driver.clear_stats();
  stats = driver.get_stats();
  assert(stats.tx_packets == 0);
  assert(stats.tx_bytes == 0);

  std::cout << "PASS\n";
}

void test_device_access() {
  std::cout << "Test: Device access... ";

  NicDriver driver{};
  auto device = create_test_device();
  [[maybe_unused]] auto* dev_ptr = device.get();

  driver.init(std::move(device));

  // Verify device pointer is accessible
  assert(driver.device() == dev_ptr);
  assert(driver.device()->is_initialized());

  // Access queue pair through device
  [[maybe_unused]] auto* qp = driver.device()->queue_pair();
  assert(qp != nullptr);

  std::cout << "PASS\n";
}

int main() {
  std::cout << "NIC Driver Integration Tests\n";
  std::cout << "=============================\n\n";

  test_driver_initialization();
  test_packet_send();
  test_packet_processing();
  test_statistics();
  test_device_access();

  std::cout << "\n=============================\n";
  std::cout << "All tests passed!\n";

  return 0;
}