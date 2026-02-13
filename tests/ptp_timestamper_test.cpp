#include "nic/ptp_timestamper.h"

#include <cassert>
#include <vector>

using namespace nic;

int main() {
  // Create PTP clock
  PTPClock::Config clock_cfg{.frequency_hz = 1'000'000'000, .drift_ppb = 0.0, .enabled = true};
  PTPClock clock{clock_cfg};

  // Create timestamper
  PTPTimestamper timestamper{clock};

  // Initially, no timestamping enabled
  assert(!timestamper.tx_timestamping_enabled(0));
  assert(!timestamper.rx_timestamping_enabled(0));

  // Enable TX timestamping for queue 0
  timestamper.enable_tx_timestamping(0, true);
  assert(timestamper.tx_timestamping_enabled(0));
  assert(!timestamper.tx_timestamping_enabled(1));  // Queue 1 still disabled

  // Enable RX timestamping for queue 0
  timestamper.enable_rx_timestamping(0, true);
  assert(timestamper.rx_timestamping_enabled(0));

  // Capture TX timestamp
  clock.tick(100);  // Advance clock to 100ns
  std::uint64_t tx_ts = timestamper.timestamp_tx_packet(0);
  assert(tx_ts == 100);
  assert(timestamper.stats().tx_timestamps == 1);

  // Capture RX timestamp
  clock.tick(50);  // Advance clock to 150ns
  std::uint64_t rx_ts = timestamper.timestamp_rx_packet(0);
  assert(rx_ts == 150);
  assert(timestamper.stats().rx_timestamps == 1);

  // Timestamp on disabled queue should return 0
  std::uint64_t disabled_ts = timestamper.timestamp_tx_packet(1);
  assert(disabled_ts == 0);
  assert(timestamper.stats().tx_timestamps == 1);  // Count unchanged

  // Disable and re-enable
  timestamper.enable_tx_timestamping(0, false);
  assert(!timestamper.tx_timestamping_enabled(0));

  disabled_ts = timestamper.timestamp_tx_packet(0);
  assert(disabled_ts == 0);

  timestamper.enable_tx_timestamping(0, true);
  clock.tick(50);  // Now at 200ns
  tx_ts = timestamper.timestamp_tx_packet(0);
  assert(tx_ts == 200);

  // Test multiple queues
  timestamper.enable_tx_timestamping(1, true);
  timestamper.enable_tx_timestamping(2, true);

  clock.tick(100);  // Now at 300ns
  std::uint64_t ts0 = timestamper.timestamp_tx_packet(0);
  std::uint64_t ts1 = timestamper.timestamp_tx_packet(1);
  std::uint64_t ts2 = timestamper.timestamp_tx_packet(2);

  assert(ts0 == 300);
  assert(ts1 == 300);
  assert(ts2 == 300);

  // PTP packet detection
  // Create a minimal Ethernet frame with PTP EtherType (0x88F7)
  std::vector<std::byte> ptp_packet(64);
  ptp_packet[12] = std::byte{0x88};  // EtherType high byte
  ptp_packet[13] = std::byte{0xF7};  // EtherType low byte

  assert(timestamper.is_ptp_packet(ptp_packet));

  // Non-PTP packet (IPv4 EtherType 0x0800)
  std::vector<std::byte> ipv4_packet(64);
  ipv4_packet[12] = std::byte{0x08};
  ipv4_packet[13] = std::byte{0x00};

  assert(!timestamper.is_ptp_packet(ipv4_packet));

  // Too small packet
  std::vector<std::byte> tiny_packet(10);
  assert(!timestamper.is_ptp_packet(tiny_packet));

  // Reset stats
  timestamper.reset_stats();
  assert(timestamper.stats().tx_timestamps == 0);
  assert(timestamper.stats().rx_timestamps == 0);

  return 0;
}