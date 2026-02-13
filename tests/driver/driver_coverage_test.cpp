/// @file driver_coverage_test.cpp
/// @brief Coverage tests for NicDriver and PacketRouter edge cases.
///
/// Exercises untested paths in driver.cpp and packet_router.cpp:
/// - Null/double init, uninit operations, no-RDMA engine paths
/// - PacketRouter register nullptr, route to non-existent, unregister non-existent

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "nic/device.h"
#include "nic/trace.h"
#include "nic_driver/driver.h"
#include "nic_driver/packet_router.h"

using namespace nic_driver;

static void WaitForTracyConnection();

namespace {

/// Create a non-RDMA device (enable_rdma = false).
std::unique_ptr<nic::Device> create_non_rdma_device() {
  nic::DeviceConfig config{};
  config.enable_rdma = false;
  config.enable_queue_pair = true;
  auto device = std::make_unique<nic::Device>(config);
  device->reset();
  return device;
}

/// Create an RDMA-enabled device.
std::unique_ptr<nic::Device> create_rdma_device() {
  nic::DeviceConfig config{};
  config.enable_rdma = true;
  config.enable_queue_pair = false;
  config.rdma_config.mtu = 1024;
  auto device = std::make_unique<nic::Device>(config);
  device->reset();
  return device;
}

// ============================================================
// Driver tests
// ============================================================

/// init(nullptr) should return false.
void test_init_nullptr() {
  std::printf("  test_init_nullptr...\n");
  NIC_TRACE_SCOPED(__func__);

  NicDriver driver;
  bool result = driver.init(nullptr);
  assert(!result);
  assert(!driver.is_initialized());

  std::printf("    PASSED\n");
}

/// init twice should return false on second call.
void test_double_init() {
  std::printf("  test_double_init...\n");
  NIC_TRACE_SCOPED(__func__);

  NicDriver driver;
  auto device = create_non_rdma_device();
  bool first = driver.init(std::move(device));
  assert(first);
  assert(driver.is_initialized());

  auto device2 = create_non_rdma_device();
  bool second = driver.init(std::move(device2));
  assert(!second);
  assert(driver.is_initialized());

  std::printf("    PASSED\n");
}

/// send_packet on fresh (uninitialized) driver should return false.
void test_send_packet_uninitialized() {
  std::printf("  test_send_packet_uninitialized...\n");
  NIC_TRACE_SCOPED(__func__);

  NicDriver driver;
  std::vector<std::byte> packet(64, std::byte{0xAB});
  bool result = driver.send_packet(packet);
  assert(!result);

  std::printf("    PASSED\n");
}

/// process on fresh (uninitialized) driver should return false.
void test_process_uninitialized() {
  std::printf("  test_process_uninitialized...\n");
  NIC_TRACE_SCOPED(__func__);

  NicDriver driver;
  bool result = driver.process();
  assert(!result);

  std::printf("    PASSED\n");
}

/// rdma_enabled should return false for a non-RDMA device.
void test_rdma_enabled_no_rdma() {
  std::printf("  test_rdma_enabled_no_rdma...\n");
  NIC_TRACE_SCOPED(__func__);

  NicDriver driver;
  auto device = create_non_rdma_device();
  driver.init(std::move(device));
  assert(driver.is_initialized());
  assert(!driver.rdma_enabled());

  std::printf("    PASSED\n");
}

/// RDMA methods should return nullopt/false when no RDMA engine is present.
void test_rdma_methods_no_engine() {
  std::printf("  test_rdma_methods_no_engine...\n");
  NIC_TRACE_SCOPED(__func__);

  NicDriver driver;
  auto device = create_non_rdma_device();
  driver.init(std::move(device));
  assert(driver.is_initialized());

  // create_pd should return nullopt
  auto pd = driver.create_pd();
  assert(!pd.has_value());

  // destroy_pd should return false
  bool destroy_pd_result = driver.destroy_pd(PdHandle{1});
  assert(!destroy_pd_result);

  // register_mr should return nullopt
  AccessFlags access{};
  auto mr = driver.register_mr(PdHandle{1}, 0x1000, 4096, access);
  assert(!mr.has_value());

  // deregister_mr should return false
  bool dereg = driver.deregister_mr(MrHandle{1, 1});
  assert(!dereg);

  // create_cq should return nullopt
  auto cq = driver.create_cq(256);
  assert(!cq.has_value());

  // destroy_cq should return false
  bool destroy_cq_result = driver.destroy_cq(CqHandle{1});
  assert(!destroy_cq_result);

  // poll_cq should return empty
  auto cqes = driver.poll_cq(CqHandle{1}, 10);
  assert(cqes.empty());

  // create_qp should return nullopt
  RdmaQpConfig config;
  auto qp = driver.create_qp(config);
  assert(!qp.has_value());

  // destroy_qp should return false
  bool destroy_qp_result = driver.destroy_qp(QpHandle{1});
  assert(!destroy_qp_result);

  // modify_qp should return false
  RdmaQpModifyParams params;
  bool modify_result = driver.modify_qp(QpHandle{1}, params);
  assert(!modify_result);

  // post_send should return false
  SendWqe send_wqe;
  bool post_send_result = driver.post_send(QpHandle{1}, send_wqe);
  assert(!post_send_result);

  // post_recv should return false
  RecvWqe recv_wqe;
  bool post_recv_result = driver.post_recv(QpHandle{1}, recv_wqe);
  assert(!post_recv_result);

  std::printf("    PASSED\n");
}

/// deregister_mr should work through the full RDMA flow.
void test_deregister_mr() {
  std::printf("  test_deregister_mr...\n");
  NIC_TRACE_SCOPED(__func__);

  NicDriver driver;
  auto device = create_rdma_device();
  driver.init(std::move(device));
  assert(driver.rdma_enabled());

  auto pd = driver.create_pd();
  assert(pd.has_value());

  AccessFlags access{.local_read = true, .local_write = true};
  auto mr = driver.register_mr(*pd, 0x1000, 4096, access);
  assert(mr.has_value());

  bool dereg = driver.deregister_mr(*mr);
  assert(dereg);

  // Deregistering again should fail
  bool dereg_again = driver.deregister_mr(*mr);
  assert(!dereg_again);

  driver.destroy_pd(*pd);

  std::printf("    PASSED\n");
}

/// rdma_generate_packets on uninit driver should return empty.
void test_rdma_generate_packets_uninit() {
  std::printf("  test_rdma_generate_packets_uninit...\n");
  NIC_TRACE_SCOPED(__func__);

  NicDriver driver;
  auto packets = driver.rdma_generate_packets();
  assert(packets.empty());

  std::printf("    PASSED\n");
}

/// rdma_process_packet on uninit driver should return false.
void test_rdma_process_packet_uninit() {
  std::printf("  test_rdma_process_packet_uninit...\n");
  NIC_TRACE_SCOPED(__func__);

  NicDriver driver;
  std::vector<std::byte> payload(32, std::byte{0});
  std::array<std::uint8_t, 4> src_ip = {10, 0, 0, 1};
  std::array<std::uint8_t, 4> dst_ip = {10, 0, 0, 2};
  bool result = driver.rdma_process_packet(payload, src_ip, dst_ip, 4791);
  assert(!result);

  std::printf("    PASSED\n");
}

/// clear_stats on uninitialized driver should not crash.
void test_clear_stats_uninitialized() {
  std::printf("  test_clear_stats_uninitialized...\n");
  NIC_TRACE_SCOPED(__func__);

  NicDriver driver;
  driver.clear_stats();  // Should not crash
  auto stats = driver.get_stats();
  assert(stats.tx_packets == 0);
  assert(stats.rx_packets == 0);

  std::printf("    PASSED\n");
}

// ============================================================
// PacketRouter tests
// ============================================================

/// register_driver with nullptr should not add an entry.
void test_router_register_nullptr() {
  std::printf("  test_router_register_nullptr...\n");
  NIC_TRACE_SCOPED(__func__);

  PacketRouter router;
  PacketRouter::IpAddress ip = {10, 0, 0, 1};
  router.register_driver(ip, nullptr);
  assert(router.driver_count() == 0);

  std::printf("    PASSED\n");
}

/// route_packet to non-existent destination should return false.
void test_router_route_nonexistent() {
  std::printf("  test_router_route_nonexistent...\n");
  NIC_TRACE_SCOPED(__func__);

  PacketRouter router;
  nic::rocev2::OutgoingPacket packet;
  packet.dest_ip = {10, 0, 0, 99};
  packet.data.resize(32, std::byte{0});

  PacketRouter::IpAddress src_ip = {10, 0, 0, 1};
  bool result = router.route_packet(packet, src_ip);
  assert(!result);

  std::printf("    PASSED\n");
}

/// unregister_driver for non-existent IP should not crash.
void test_router_unregister_nonexistent() {
  std::printf("  test_router_unregister_nonexistent...\n");
  NIC_TRACE_SCOPED(__func__);

  PacketRouter router;
  PacketRouter::IpAddress ip = {10, 0, 0, 42};
  router.unregister_driver(ip);  // Should not crash
  assert(router.driver_count() == 0);

  std::printf("    PASSED\n");
}

/// process_all with no drivers should return 0.
void test_router_process_all_empty() {
  std::printf("  test_router_process_all_empty...\n");
  NIC_TRACE_SCOPED(__func__);

  PacketRouter router;
  std::size_t routed = router.process_all();
  assert(routed == 0);

  std::printf("    PASSED\n");
}

/// process_all with non-RDMA drivers should return 0 (skips them).
void test_router_process_all_non_rdma() {
  std::printf("  test_router_process_all_non_rdma...\n");
  NIC_TRACE_SCOPED(__func__);

  PacketRouter router;
  NicDriver driver;
  auto device = create_non_rdma_device();
  driver.init(std::move(device));
  assert(!driver.rdma_enabled());

  PacketRouter::IpAddress ip = {10, 0, 0, 1};
  router.register_driver(ip, &driver);
  assert(router.driver_count() == 1);

  std::size_t routed = router.process_all();
  assert(routed == 0);

  std::printf("    PASSED\n");
}

}  // namespace

int main() {
  NIC_TRACE_SCOPED(__func__);
  WaitForTracyConnection();
  std::printf("Running driver coverage tests...\n");

  // Driver tests
  test_init_nullptr();
  test_double_init();
  test_send_packet_uninitialized();
  test_process_uninitialized();
  test_rdma_enabled_no_rdma();
  test_rdma_methods_no_engine();
  test_deregister_mr();
  test_rdma_generate_packets_uninit();
  test_rdma_process_packet_uninit();
  test_clear_stats_uninitialized();

  // PacketRouter tests
  test_router_register_nullptr();
  test_router_route_nonexistent();
  test_router_unregister_nonexistent();
  test_router_process_all_empty();
  test_router_process_all_non_rdma();

  std::printf("All driver coverage tests PASSED!\n");
  return 0;
}

static void WaitForTracyConnection() {
#ifdef TRACY_ENABLE
  const char* wait_env = std::getenv("NIC_WAIT_FOR_TRACY");
  if (!wait_env || wait_env[0] == '\0' || wait_env[0] == '0') {
    return;
  }

  const auto timeout = std::chrono::seconds(2);
  const auto start = std::chrono::steady_clock::now();
  while (!tracy::GetProfiler().IsConnected()) {
    if (std::chrono::steady_clock::now() - start > timeout) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
#endif
}
