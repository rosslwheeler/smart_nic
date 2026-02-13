#include <cassert>
#include <chrono>

#include "bit_fields/bit_fields.h"
#include "nic/packet_generator.h"

using namespace nic;
using namespace bit_fields;

void test_packet_generation() {
  PacketGenerator gen;

  // Generate payload
  auto payload =
      PacketGenerator::generate_payload(100, PacketGenerator::PayloadPattern::Incrementing);
  assert(payload.size() == 100);
  assert(payload[0] == std::byte{0x00});
  assert(payload[99] == std::byte{0x63});  // 99 mod 256

  // Test Ethernet frame generation
  PacketGenerator::EthernetConfig eth_cfg{
      .dst_mac = 0xFF'FF'FF'FF'FF'FF,  // Broadcast
      .src_mac = 0x00'11'22'33'44'55,
      .ethertype = 0x0800,  // IPv4
  };

  auto eth_frame = gen.generate_ethernet(eth_cfg, payload);
  assert(eth_frame.size() == 14 + 100);  // Eth header + payload

  // Use bit_fields to parse and validate Ethernet header
  NetworkBitReader eth_reader{std::span<const std::byte>(eth_frame)};
  auto eth_parsed = eth_reader.deserialize(formats::kEthernetHeader);

  // Use verify_expected with runtime values
  std::array<ExpectedField, 3> eth_expected = {{
      {"dest_mac", eth_cfg.dst_mac},
      {"src_mac", eth_cfg.src_mac},
      {"ethertype", eth_cfg.ethertype},
  }};
  assert(eth_reader.verify_expected(eth_parsed, eth_expected));

  // Test IPv4 packet generation
  PacketGenerator::IPv4Config ipv4_cfg{
      .src_ip = 0xC0A80001,  // 192.168.0.1
      .dst_ip = 0xC0A80002,  // 192.168.0.2
      .protocol = 6,         // TCP
      .ttl = 64,
  };

  auto ipv4_packet = gen.generate_ipv4(ipv4_cfg, payload);
  assert(ipv4_packet.size() == 20 + 100);  // IPv4 header + payload

  // Use bit_fields to parse and validate IPv4 header
  NetworkBitReader ipv4_reader{std::span<const std::byte>(ipv4_packet)};
  auto ipv4_parsed = ipv4_reader.deserialize(formats::kIpv4Header);

  // Verify constant IPv4 fields using constexpr table
  constexpr ExpectedTable<3> ipv4_const_expected{{
      {"version", 4}, {"ihl", 5}, {"flags", 0x02},  // Don't fragment
  }};
  ipv4_reader.assert_expected(ipv4_parsed, ipv4_const_expected);

  // Verify runtime IPv4 fields
  std::array<ExpectedField, 5> ipv4_runtime_expected = {{
      {"total_length", 20 + 100},
      {"ttl", ipv4_cfg.ttl},
      {"protocol", ipv4_cfg.protocol},
      {"src_ip", ipv4_cfg.src_ip},
      {"dst_ip", ipv4_cfg.dst_ip},
  }};
  assert(ipv4_reader.verify_expected(ipv4_parsed, ipv4_runtime_expected));

  // Verify IPv4 checksum is correct (checksum of header should be 0)
  std::uint16_t checksum =
      PacketGenerator::ipv4_checksum(std::span<const std::byte>(ipv4_packet.data(), 20));
  assert(checksum == 0);  // Valid checksum should result in 0

  // Test TCP segment generation
  PacketGenerator::TCPConfig tcp_cfg{
      .src_port = 8080,
      .dst_port = 80,
      .seq_num = 1000,
      .ack_num = 0,
      .flags = 0x002,  // SYN
      .window = 8192,
  };

  auto tcp_segment = gen.generate_tcp(tcp_cfg, ipv4_cfg.src_ip, ipv4_cfg.dst_ip, payload);
  assert(tcp_segment.size() == 20 + 100);  // TCP header + payload

  // Use bit_fields to parse and validate TCP header
  NetworkBitReader tcp_reader{std::span<const std::byte>(tcp_segment)};
  auto tcp_parsed = tcp_reader.deserialize(formats::kTcpHeader);

  // Verify TCP fields
  std::array<ExpectedField, 6> tcp_expected = {{
      {"src_port", tcp_cfg.src_port},
      {"dst_port", tcp_cfg.dst_port},
      {"seq_num", tcp_cfg.seq_num},
      {"ack_num", tcp_cfg.ack_num},
      {"data_offset", 5},  // 20 bytes / 4
      {"window_size", tcp_cfg.window},
  }};
  assert(tcp_reader.verify_expected(tcp_parsed, tcp_expected));

  // Test complete packet generation
  auto complete_packet = gen.generate_eth_ipv4_tcp(eth_cfg, ipv4_cfg, tcp_cfg, payload);
  assert(complete_packet.size() == 14 + 20 + 20 + 100);

  // Parse complete packet layer by layer using bit_fields
  NetworkBitReader full_reader{std::span<const std::byte>(complete_packet)};

  auto eth_layer = full_reader.deserialize(formats::kEthernetHeader);
  constexpr ExpectedTable<1> eth_layer_expected{{"ethertype", 0x0800}};
  full_reader.assert_expected(eth_layer, eth_layer_expected);

  auto ipv4_layer = full_reader.deserialize(formats::kIpv4Header);
  constexpr ExpectedTable<1> ipv4_layer_expected{{"protocol", 6}};
  full_reader.assert_expected(ipv4_layer, ipv4_layer_expected);

  auto tcp_layer = full_reader.deserialize(formats::kTcpHeader);
  std::array<ExpectedField, 1> tcp_layer_expected = {{"src_port", 8080}};
  assert(full_reader.verify_expected(tcp_layer, tcp_layer_expected));

  // Test UDP generation
  PacketGenerator::UDPConfig udp_cfg{
      .src_port = 5353,
      .dst_port = 5353,
  };

  auto udp_datagram = gen.generate_udp(udp_cfg, ipv4_cfg.src_ip, ipv4_cfg.dst_ip, payload);
  assert(udp_datagram.size() == 8 + 100);  // UDP header + payload

  // Use bit_fields to parse and validate UDP header
  NetworkBitReader udp_reader{std::span<const std::byte>(udp_datagram)};
  auto udp_parsed = udp_reader.deserialize(formats::kUdpHeader);

  std::array<ExpectedField, 3> udp_expected = {{
      {"src_port", udp_cfg.src_port},
      {"dst_port", udp_cfg.dst_port},
      {"length", 8 + 100},
  }};
  assert(udp_reader.verify_expected(udp_parsed, udp_expected));

  auto complete_udp_packet = gen.generate_eth_ipv4_udp(eth_cfg, ipv4_cfg, udp_cfg, payload);
  assert(complete_udp_packet.size() == 14 + 20 + 8 + 100);

  // Test with VLAN tag
  eth_cfg.vlan_tag = 100;  // VID=100
  auto vlan_frame = gen.generate_ethernet(eth_cfg, payload);
  assert(vlan_frame.size() == 14 + 4 + 100);  // Eth header + VLAN + payload

  // Parse VLAN frame using bit_fields
  NetworkBitReader vlan_reader{std::span<const std::byte>(vlan_frame)};
  auto eth_vlan = vlan_reader.deserialize(formats::kEthernetHeader);

  constexpr ExpectedTable<1> vlan_ethertype_expected{{"ethertype", 0x8100}};
  vlan_reader.assert_expected(eth_vlan, vlan_ethertype_expected);

  auto vlan_tag = vlan_reader.deserialize(formats::kVlanTag);
  std::array<ExpectedField, 1> vlan_expected = {{"vid", 100}};
  assert(vlan_reader.verify_expected(vlan_tag, vlan_expected));
}

void test_payload_patterns() {
  // Test zeros pattern
  auto zeros = PacketGenerator::generate_payload(64, PacketGenerator::PayloadPattern::Zeros);
  assert(zeros.size() == 64);
  for (std::size_t i = 0; i < 64; ++i) {
    assert(zeros[i] == std::byte{0x00});
  }

  // Test ones pattern
  auto ones = PacketGenerator::generate_payload(64, PacketGenerator::PayloadPattern::Ones);
  assert(ones.size() == 64);
  for (std::size_t i = 0; i < 64; ++i) {
    assert(ones[i] == std::byte{0xFF});
  }

  // Test incrementing pattern
  auto inc = PacketGenerator::generate_payload(256, PacketGenerator::PayloadPattern::Incrementing);
  assert(inc.size() == 256);
  for (std::size_t i = 0; i < 256; ++i) {
    assert(inc[i] == static_cast<std::byte>(i));
  }

  // Test random pattern (same seed gives same result)
  auto rand1 = PacketGenerator::generate_payload(100, PacketGenerator::PayloadPattern::Random, 42);
  auto rand2 = PacketGenerator::generate_payload(100, PacketGenerator::PayloadPattern::Random, 42);
  assert(rand1 == rand2);

  // Different seed gives different result
  auto rand3 = PacketGenerator::generate_payload(100, PacketGenerator::PayloadPattern::Random, 43);
  assert(rand1 != rand3);
}

void test_checksum_validation() {
  PacketGenerator gen;

  // Create a known packet and verify checksums
  auto payload =
      PacketGenerator::generate_payload(1024, PacketGenerator::PayloadPattern::Incrementing);

  PacketGenerator::IPv4Config ipv4_cfg{
      .src_ip = 0x0A000001,  // 10.0.0.1
      .dst_ip = 0x0A000002,  // 10.0.0.2
      .protocol = 17,        // UDP
      .ttl = 128,
  };

  PacketGenerator::UDPConfig udp_cfg{
      .src_port = 12345,
      .dst_port = 80,
  };

  auto udp_datagram = gen.generate_udp(udp_cfg, ipv4_cfg.src_ip, ipv4_cfg.dst_ip, payload);

  // Parse and verify UDP header using bit_fields
  NetworkBitReader udp_reader{std::span<const std::byte>(udp_datagram)};
  auto udp_parsed = udp_reader.deserialize(formats::kUdpHeader);

  auto original_checksum = udp_parsed.get("checksum");

  // Zero out checksum field and recalculate
  auto udp_copy = udp_datagram;
  udp_copy[6] = std::byte{0};
  udp_copy[7] = std::byte{0};

  auto recalculated = PacketGenerator::udp_checksum(ipv4_cfg.src_ip, ipv4_cfg.dst_ip, udp_copy);

  // Should match original
  assert(original_checksum == recalculated);
}

void benchmark_packet_generation() {
  PacketGenerator gen;

  // Benchmark TCP packet generation
  constexpr std::size_t iterations = 10000;

  PacketGenerator::EthernetConfig eth_cfg{
      .dst_mac = 0x00'50'56'C0'00'08,
      .src_mac = 0x00'0C'29'1D'23'F0,
      .ethertype = 0x0800,
  };

  PacketGenerator::IPv4Config ipv4_cfg{
      .src_ip = 0xC0A80A01,
      .dst_ip = 0xC0A80A02,
      .protocol = 6,
      .ttl = 64,
  };

  PacketGenerator::TCPConfig tcp_cfg{
      .src_port = 443,
      .dst_port = 49152,
      .seq_num = 12345,
      .ack_num = 67890,
      .flags = 0x010,  // ACK
      .window = 65535,
  };

  auto payload = PacketGenerator::generate_payload(1460, PacketGenerator::PayloadPattern::Zeros);

  auto start = std::chrono::high_resolution_clock::now();

  for (std::size_t i = 0; i < iterations; ++i) {
    auto packet = gen.generate_eth_ipv4_tcp(eth_cfg, ipv4_cfg, tcp_cfg, payload);
    // Ensure packet isn't optimized away
    assert(packet.size() == 14 + 20 + 20 + 1460);
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

  double packets_per_sec = (iterations * 1'000'000.0) / duration;
  double mbps = (packets_per_sec * (14 + 20 + 20 + 1460) * 8) / 1'000'000;

  // Print benchmark results (visible in test output)
  printf("Packet generation benchmark:\n");
  printf("  Generated %zu packets in %lld us\n", iterations, static_cast<long long>(duration));
  printf("  Rate: %.2f packets/sec\n", packets_per_sec);
  printf("  Throughput: %.2f Mbps\n", mbps);
}

void test_field_validation_with_predicates() {
  PacketGenerator gen;

  auto payload = PacketGenerator::generate_payload(100, PacketGenerator::PayloadPattern::Zeros);

  PacketGenerator::IPv4Config ipv4_cfg{
      .src_ip = 0xC0A80001,
      .dst_ip = 0xC0A80002,
      .protocol = 6,
      .ttl = 64,
  };

  auto ipv4_packet = gen.generate_ipv4(ipv4_cfg, payload);

  NetworkBitReader reader{std::span<const std::byte>(ipv4_packet)};
  auto ipv4 = reader.deserialize(formats::kIpv4Header);

  // Define predicate check for TTL in valid range
  struct TtlInRange {
    bool operator()(std::uint64_t v) const { return v >= 1 && v <= 255; }
  };

  constexpr ExpectedChecks<TtlInRange, 1> ttl_checks{{
      {"ttl", TtlInRange{}},
  }};

  // This should pass (TTL=64 is in range)
  reader.assert_expected(ipv4, ttl_checks);

  // Define predicate check for total_length >= minimum
  struct LengthValid {
    bool operator()(std::uint64_t v) const { return v >= 20; }  // At least IPv4 header
  };

  constexpr ExpectedChecks<LengthValid, 1> length_checks{{
      {"total_length", LengthValid{}},
  }};

  reader.assert_expected(ipv4, length_checks);
}

int main() {
  test_payload_patterns();
  test_packet_generation();
  test_checksum_validation();
  test_field_validation_with_predicates();
  benchmark_packet_generation();

  return 0;
}