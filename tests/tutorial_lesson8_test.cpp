#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <vector>

#include "nic/rss.h"
#include "nic/trace.h"

using namespace nic;

// Simulate packet 5-tuple
struct PacketTuple {
  std::uint32_t src_ip;
  std::uint32_t dst_ip;
  std::uint16_t src_port;
  std::uint16_t dst_port;

  std::vector<std::uint8_t> to_bytes() const {
    std::vector<std::uint8_t> bytes(12);
    bytes[0] = (src_ip >> 24) & 0xFF;
    bytes[1] = (src_ip >> 16) & 0xFF;
    bytes[2] = (src_ip >> 8) & 0xFF;
    bytes[3] = src_ip & 0xFF;
    bytes[4] = (dst_ip >> 24) & 0xFF;
    bytes[5] = (dst_ip >> 16) & 0xFF;
    bytes[6] = (dst_ip >> 8) & 0xFF;
    bytes[7] = dst_ip & 0xFF;
    bytes[8] = (src_port >> 8) & 0xFF;
    bytes[9] = src_port & 0xFF;
    bytes[10] = (dst_port >> 8) & 0xFF;
    bytes[11] = dst_port & 0xFF;
    return bytes;
  }
};

void print_distribution(const std::map<std::uint16_t, int>& dist, int total) {
  std::cout << "Queue distribution:\n";
  for (const auto& [queue, count] : dist) {
    double pct = 100.0 * count / total;
    std::cout << "  Queue " << queue << ": " << std::setw(5) << count << " (" << std::fixed
              << std::setprecision(1) << pct << "%) ";
    int bars = static_cast<int>(pct / 2);
    for (int i = 0; i < bars; ++i)
      std::cout << "#";
    std::cout << "\n";
  }
}

int main() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "=== Lesson 8: RSS Hash Distribution Analysis ===\n\n";

  // Standard Microsoft RSS key
  std::vector<std::uint8_t> rss_key = {
      0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2, 0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3,
      0x8f, 0xb0, 0xd0, 0xca, 0x2b, 0xcb, 0xae, 0x7b, 0x30, 0xb4, 0x77, 0xcb, 0x2d, 0xa3,
      0x80, 0x30, 0xf2, 0x0c, 0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa,
  };

  // --- Test 1: 4 Queues ---
  std::cout << "--- Test 1: 4 Queues, 1000 Flows ---\n";
  {
    RssConfig config;
    config.key = rss_key;
    config.table = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};  // 16-entry table

    RssEngine rss{config};

    std::map<std::uint16_t, int> distribution;
    std::mt19937 gen(42);  // Fixed seed for reproducibility
    std::uniform_int_distribution<std::uint32_t> ip_dist(0, 0xFFFFFFFF);
    std::uniform_int_distribution<std::uint16_t> port_dist(1024, 65535);

    for (int i = 0; i < 1000; ++i) {
      PacketTuple tuple{
          .src_ip = ip_dist(gen),
          .dst_ip = ip_dist(gen),
          .src_port = port_dist(gen),
          .dst_port = port_dist(gen),
      };
      auto bytes = tuple.to_bytes();
      auto queue = rss.select_queue(bytes);
      if (queue) {
        distribution[*queue]++;
      }
    }

    print_distribution(distribution, 1000);
  }

  // --- Test 2: Same Flow Always Goes to Same Queue ---
  std::cout << "\n--- Test 2: Flow Affinity (same flow -> same queue) ---\n";
  {
    RssConfig config;
    config.key = rss_key;
    config.table = {0, 1, 2, 3, 0, 1, 2, 3};

    RssEngine rss{config};

    // Create a specific flow
    PacketTuple flow{
        .src_ip = 0xC0A80164,  // 192.168.1.100
        .dst_ip = 0xC0A80101,  // 192.168.1.1
        .src_port = 8080,
        .dst_port = 80,
    };

    auto bytes = flow.to_bytes();
    auto hash_value = rss.hash(bytes);

    std::cout << "Flow: 192.168.1.100:8080 -> 192.168.1.1:80\n";
    std::cout << "Hash: 0x" << std::hex << hash_value << std::dec << "\n";

    // Check 10 times - should always be same queue
    std::cout << "Checking 10 lookups: ";
    auto first_queue = rss.select_queue(bytes);
    bool all_same = true;
    for (int i = 0; i < 10; ++i) {
      auto queue = rss.select_queue(bytes);
      std::cout << *queue << " ";
      if (queue != first_queue) {
        all_same = false;
      }
    }
    std::cout << "\nAll same queue: " << (all_same ? "YES" : "NO") << "\n";
  }

  // --- Test 3: Reverse Flow (bidirectional symmetry) ---
  std::cout << "\n--- Test 3: Bidirectional Flow ---\n";
  {
    RssConfig config;
    config.key = rss_key;
    config.table = {0, 1, 2, 3, 0, 1, 2, 3};

    RssEngine rss{config};

    PacketTuple forward{
        .src_ip = 0xC0A80164,
        .dst_ip = 0xC0A80101,
        .src_port = 8080,
        .dst_port = 80,
    };

    PacketTuple reverse{
        .src_ip = 0xC0A80101,  // Swapped
        .dst_ip = 0xC0A80164,
        .src_port = 80,  // Swapped
        .dst_port = 8080,
    };

    auto fwd_queue = rss.select_queue(forward.to_bytes());
    auto rev_queue = rss.select_queue(reverse.to_bytes());

    std::cout << "Forward (192.168.1.100:8080 -> 192.168.1.1:80): Queue " << *fwd_queue << "\n";
    std::cout << "Reverse (192.168.1.1:80 -> 192.168.1.100:8080): Queue " << *rev_queue << "\n";
    std::cout << "Note: Standard Toeplitz doesn't guarantee symmetry\n";
    std::cout << "(Symmetric RSS requires XOR of src/dst before hashing)\n";
  }

  // --- Test 4: Varying Source Ports ---
  std::cout << "\n--- Test 4: Same IPs, Varying Source Ports ---\n";
  {
    RssConfig config;
    config.key = rss_key;
    config.table = {0, 1, 2, 3, 0, 1, 2, 3};

    RssEngine rss{config};

    std::map<std::uint16_t, int> distribution;

    for (std::uint16_t port = 1024; port < 1024 + 100; ++port) {
      PacketTuple tuple{
          .src_ip = 0xC0A80164,
          .dst_ip = 0xC0A80101,
          .src_port = port,
          .dst_port = 80,
      };
      auto queue = rss.select_queue(tuple.to_bytes());
      if (queue) {
        distribution[*queue]++;
      }
    }

    std::cout << "100 connections from same client to same server:\n";
    print_distribution(distribution, 100);
  }

  std::cout << "\n*** Lesson 8 Complete! ***\n";
  return 0;
}
