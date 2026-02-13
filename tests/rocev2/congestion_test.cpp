#include "nic/rocev2/congestion.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "nic/rocev2/packet.h"
#include "nic/trace.h"

using namespace nic::rocev2;

static void WaitForTracyConnection();

namespace {

// Test ECN codepoint detection
void test_ecn_detection() {
  std::printf("  test_ecn_detection...\n");

  CongestionControlManager manager;

  assert(!manager.is_congestion_marked(EcnCodepoint::NonEct));
  assert(!manager.is_congestion_marked(EcnCodepoint::Ect0));
  assert(!manager.is_congestion_marked(EcnCodepoint::Ect1));
  assert(manager.is_congestion_marked(EcnCodepoint::Ce));

  std::printf("    PASSED\n");
}

// Test CNP generation
void test_cnp_generation() {
  std::printf("  test_cnp_generation...\n");

  DcqcnConfig config;
  config.cnp_timer_us = 100;  // 100us minimum between CNPs
  CongestionControlManager manager{config};

  // First CNP should be generated
  auto cnp = manager.generate_cnp(100, 200, 0);
  assert(cnp.has_value());
  assert(!cnp->empty());
  assert(manager.stats().cnps_generated == 1);

  // Parse the CNP
  RdmaPacketParser parser;
  assert(parser.parse(*cnp));
  assert(parser.bth().opcode == RdmaOpcode::kCnp);
  assert(parser.bth().dest_qp == 100);
  assert(parser.bth().becn);  // BECN should be set

  // Second CNP within timer should be rate-limited
  cnp = manager.generate_cnp(100, 200, 50);
  assert(!cnp.has_value());

  // CNP after timer expired should work
  cnp = manager.generate_cnp(100, 200, 150);
  assert(cnp.has_value());
  assert(manager.stats().cnps_generated == 2);

  // Different flow should not be rate-limited
  cnp = manager.generate_cnp(101, 200, 150);
  assert(cnp.has_value());
  assert(manager.stats().cnps_generated == 3);

  std::printf("    PASSED\n");
}

// Test CNP reception and rate decrease
void test_cnp_reception() {
  std::printf("  test_cnp_reception...\n");

  DcqcnConfig config;
  config.initial_rate_mbps = 100000;  // 100 Gbps
  config.min_rate_mbps = 10;
  CongestionControlManager manager{config};

  [[maybe_unused]] std::uint64_t initial_rate = manager.get_current_rate(1);
  assert(initial_rate == 100000);

  // Receive CNP - rate should decrease
  manager.handle_cnp_received(1, 0);
  [[maybe_unused]] std::uint64_t decreased_rate = manager.get_current_rate(1);
  assert(decreased_rate < initial_rate);
  assert(manager.stats().cnps_received == 1);
  assert(manager.stats().rate_decreases == 1);

  // Multiple CNPs should continue decreasing rate
  manager.handle_cnp_received(1, 100);
  [[maybe_unused]] std::uint64_t further_decreased = manager.get_current_rate(1);
  assert(further_decreased < decreased_rate);

  std::printf("    PASSED\n");
}

// Test rate recovery over time
void test_rate_recovery() {
  std::printf("  test_rate_recovery...\n");

  DcqcnConfig config;
  config.initial_rate_mbps = 100000;
  config.rate_increase_period_us = 50;
  config.alpha_update_period_us = 55;
  // Use a larger alpha_g for faster recovery in test (real DCQCN uses 1/256)
  config.alpha_g = 0.1;
  CongestionControlManager manager{config};

  // Decrease rate via CNP
  manager.handle_cnp_received(1, 0);
  [[maybe_unused]] std::uint64_t decreased_rate = manager.get_current_rate(1);
  assert(decreased_rate < config.initial_rate_mbps);

  // Advance time to trigger rate recovery
  manager.advance_time(100);  // Past rate increase period

  [[maybe_unused]] std::uint64_t recovered_rate = manager.get_current_rate(1);
  assert(recovered_rate >= decreased_rate);
  assert(manager.stats().rate_increases >= 1);

  // After significant time, rate should continue recovering toward initial
  // Each recovery step takes rate_increase_period_us (50us)
  // With alpha_g=0.1, recovery is much faster than default 1/256
  for (int time_idx = 0; time_idx < 500; ++time_idx) {
    manager.advance_time(100);
  }

  [[maybe_unused]] std::uint64_t final_rate = manager.get_current_rate(1);
  // Rate should have recovered to initial rate
  assert(final_rate == config.initial_rate_mbps);

  std::printf("    PASSED\n");
}

// Test disabled congestion control
void test_disabled_cc() {
  std::printf("  test_disabled_cc...\n");

  DcqcnConfig config;
  config.enabled = false;
  CongestionControlManager manager{config};

  // CNP generation should be disabled
  auto cnp = manager.generate_cnp(100, 200, 0);
  assert(!cnp.has_value());

  // CNP reception should be ignored
  manager.handle_cnp_received(1, 0);
  assert(manager.stats().cnps_received == 0);

  std::printf("    PASSED\n");
}

// Test multiple flows
void test_multiple_flows() {
  std::printf("  test_multiple_flows...\n");

  DcqcnConfig config;
  config.initial_rate_mbps = 100000;
  CongestionControlManager manager{config};

  // Each flow should have independent rate
  manager.handle_cnp_received(1, 0);
  manager.handle_cnp_received(2, 0);
  manager.handle_cnp_received(2, 50);  // Flow 2 gets extra CNP

  [[maybe_unused]] std::uint64_t rate1 = manager.get_current_rate(1);
  [[maybe_unused]] std::uint64_t rate2 = manager.get_current_rate(2);

  // Flow 2 should have lower rate due to extra CNP
  assert(rate2 < rate1);

  // Flow 3 should have initial rate (no CNP)
  [[maybe_unused]] std::uint64_t rate3 = manager.get_current_rate(3);
  assert(rate3 == config.initial_rate_mbps);

  std::printf("    PASSED\n");
}

// Test congestion control reset
void test_cc_reset() {
  std::printf("  test_cc_reset...\n");

  CongestionControlManager manager;

  manager.handle_cnp_received(1, 0);
  assert(manager.stats().cnps_received == 1);

  manager.reset();

  assert(manager.stats().cnps_received == 0);

  // Flows should be reset
  [[maybe_unused]] std::uint64_t rate = manager.get_current_rate(1);
  assert(rate == DcqcnConfig{}.initial_rate_mbps);

  std::printf("    PASSED\n");
}

// Test min rate clamping
void test_min_rate_clamping() {
  std::printf("  test_min_rate_clamping...\n");

  DcqcnConfig config;
  config.initial_rate_mbps = 1000;
  config.min_rate_mbps = 100;
  CongestionControlManager manager{config};

  // Send many CNPs to drive rate down
  for (int cnp_idx = 0; cnp_idx < 50; ++cnp_idx) {
    manager.handle_cnp_received(1, cnp_idx * 10);
  }

  [[maybe_unused]] std::uint64_t rate = manager.get_current_rate(1);
  assert(rate >= config.min_rate_mbps);

  std::printf("    PASSED\n");
}

}  // namespace

int main() {
  NIC_TRACE_SCOPED(__func__);
  WaitForTracyConnection();
  std::printf("Running congestion control tests...\n");

  test_ecn_detection();
  test_cnp_generation();
  test_cnp_reception();
  test_rate_recovery();
  test_disabled_cc();
  test_multiple_flows();
  test_cc_reset();
  test_min_rate_clamping();

  std::printf("All congestion control tests PASSED!\n");
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
