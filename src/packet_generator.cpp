#include "nic/packet_generator.h"

#include <algorithm>
#include <cstring>
#include <random>

#include "nic/trace.h"

using namespace nic;
using namespace bit_fields;

std::vector<std::byte> PacketGenerator::generate_eth_ipv4_tcp(
    const EthernetConfig& eth,
    const IPv4Config& ipv4,
    const TCPConfig& tcp,
    std::span<const std::byte> payload) noexcept {
  NIC_TRACE_SCOPED(__func__);

  // Generate TCP segment
  auto tcp_segment = generate_tcp(tcp, ipv4.src_ip, ipv4.dst_ip, payload);

  // Generate IPv4 packet with TCP segment
  auto ipv4_packet = generate_ipv4(ipv4, tcp_segment);

  // Generate Ethernet frame with IPv4 packet
  return generate_ethernet(eth, ipv4_packet);
}

std::vector<std::byte> PacketGenerator::generate_eth_ipv4_udp(
    const EthernetConfig& eth,
    const IPv4Config& ipv4,
    const UDPConfig& udp,
    std::span<const std::byte> payload) noexcept {
  NIC_TRACE_SCOPED(__func__);

  // Generate UDP datagram
  auto udp_datagram = generate_udp(udp, ipv4.src_ip, ipv4.dst_ip, payload);

  // Generate IPv4 packet with UDP datagram
  auto ipv4_packet = generate_ipv4(ipv4, udp_datagram);

  // Generate Ethernet frame with IPv4 packet
  return generate_ethernet(eth, ipv4_packet);
}

std::vector<std::byte> PacketGenerator::generate_ethernet(
    const EthernetConfig& cfg, std::span<const std::byte> payload) noexcept {
  NIC_TRACE_SCOPED(__func__);

  // Calculate total size: Ethernet header (14) + optional VLAN (4) + payload
  std::size_t total_size = 14 + (cfg.vlan_tag.has_value() ? 4 : 0) + payload.size();
  std::vector<std::byte> frame(total_size);

  NetworkBitWriter writer{std::span<std::byte>(frame)};

  // Write Ethernet header
  writer.serialize(formats::kEthernetHeader,
                   cfg.dst_mac,                                         // dest_mac (48 bits)
                   cfg.src_mac,                                         // src_mac (48 bits)
                   cfg.vlan_tag.has_value() ? 0x8100 : cfg.ethertype);  // ethertype

  // Write VLAN tag if present
  if (cfg.vlan_tag.has_value()) {
    std::uint16_t vlan = *cfg.vlan_tag;
    std::uint8_t pcp = 0;
    std::uint8_t dei = 0;
    std::uint16_t vid = vlan & 0x0FFF;

    writer.serialize(formats::kVlanTag, pcp, dei, vid);

    // Write actual ethertype after VLAN tag
    writer.write_bits<16>(cfg.ethertype);
  }

  // Copy payload
  std::memcpy(frame.data() + (total_size - payload.size()), payload.data(), payload.size());

  return frame;
}

std::vector<std::byte> PacketGenerator::generate_ipv4(const IPv4Config& cfg,
                                                      std::span<const std::byte> payload) noexcept {
  NIC_TRACE_SCOPED(__func__);

  // IPv4 header is 20 bytes (160 bits)
  std::size_t total_size = 20 + payload.size();
  std::vector<std::byte> packet(total_size);

  NetworkBitWriter writer{std::span<std::byte>(packet)};

  // Extract DSCP and ECN from TOS
  std::uint8_t dscp = (cfg.tos >> 2) & 0x3F;
  std::uint8_t ecn = cfg.tos & 0x03;

  // Flags: bit 1 = DF (don't fragment), bit 0 = MF (more fragments)
  std::uint8_t flags = cfg.dont_fragment ? 0x02 : 0x00;

  // Write IPv4 header (checksum will be 0 initially)
  writer.serialize(formats::kIpv4Header,
                   4,                                       // version
                   5,                                       // ihl (5 = 20 bytes, no options)
                   dscp,                                    // dscp
                   ecn,                                     // ecn
                   static_cast<std::uint16_t>(total_size),  // total_length
                   cfg.identification,                      // identification
                   flags,                                   // flags
                   static_cast<std::uint16_t>(0),           // fragment_offset
                   cfg.ttl,                                 // ttl
                   cfg.protocol,                            // protocol
                   static_cast<std::uint16_t>(0),           // header_checksum (will calculate)
                   cfg.src_ip,                              // src_ip
                   cfg.dst_ip);                             // dst_ip

  // Calculate and insert checksum
  std::uint16_t checksum = ipv4_checksum(std::span<const std::byte>(packet.data(), 20));

  // Write checksum at offset 10 (bytes 10-11)
  packet[10] = static_cast<std::byte>(checksum >> 8);
  packet[11] = static_cast<std::byte>(checksum & 0xFF);

  // Copy payload
  std::memcpy(packet.data() + 20, payload.data(), payload.size());

  return packet;
}

std::vector<std::byte> PacketGenerator::generate_tcp(const TCPConfig& cfg,
                                                     std::uint32_t src_ip,
                                                     std::uint32_t dst_ip,
                                                     std::span<const std::byte> payload) noexcept {
  NIC_TRACE_SCOPED(__func__);

  // TCP header is 20 bytes (160 bits) without options
  std::size_t total_size = 20 + payload.size();
  std::vector<std::byte> segment(total_size);

  NetworkBitWriter writer{std::span<std::byte>(segment)};

  // Data offset = 5 (20 bytes / 4)
  std::uint8_t data_offset = 5;

  // Write TCP header (checksum will be 0 initially)
  writer.serialize(formats::kTcpHeader,
                   cfg.src_port,                   // src_port
                   cfg.dst_port,                   // dst_port
                   cfg.seq_num,                    // seq_num
                   cfg.ack_num,                    // ack_num
                   data_offset,                    // data_offset (4 bits)
                   static_cast<std::uint8_t>(0),   // reserved (3 bits)
                   cfg.flags,                      // flags (9 bits)
                   cfg.window,                     // window_size
                   static_cast<std::uint16_t>(0),  // checksum (will calculate)
                   cfg.urgent_ptr);                // urgent_ptr

  // Copy payload
  std::memcpy(segment.data() + 20, payload.data(), payload.size());

  // Calculate and insert checksum
  std::uint16_t checksum = tcp_checksum(src_ip, dst_ip, segment);

  // Write checksum at offset 16 (bytes 16-17)
  segment[16] = static_cast<std::byte>(checksum >> 8);
  segment[17] = static_cast<std::byte>(checksum & 0xFF);

  return segment;
}

std::vector<std::byte> PacketGenerator::generate_udp(const UDPConfig& cfg,
                                                     std::uint32_t src_ip,
                                                     std::uint32_t dst_ip,
                                                     std::span<const std::byte> payload) noexcept {
  NIC_TRACE_SCOPED(__func__);

  // UDP header is 8 bytes (64 bits)
  std::size_t total_size = 8 + payload.size();
  std::vector<std::byte> datagram(total_size);

  NetworkBitWriter writer{std::span<std::byte>(datagram)};

  // Write UDP header (checksum optional, can be 0)
  writer.serialize(formats::kUdpHeader,
                   cfg.src_port,                            // src_port
                   cfg.dst_port,                            // dst_port
                   static_cast<std::uint16_t>(total_size),  // length
                   static_cast<std::uint16_t>(0));          // checksum (will calculate)

  // Copy payload
  std::memcpy(datagram.data() + 8, payload.data(), payload.size());

  // Calculate and insert checksum
  std::uint16_t checksum = udp_checksum(src_ip, dst_ip, datagram);

  // Write checksum at offset 6 (bytes 6-7)
  datagram[6] = static_cast<std::byte>(checksum >> 8);
  datagram[7] = static_cast<std::byte>(checksum & 0xFF);

  return datagram;
}

std::uint16_t PacketGenerator::ipv4_checksum(std::span<const std::byte> header) noexcept {
  return internet_checksum(header);
}

std::uint16_t PacketGenerator::tcp_checksum(std::uint32_t src_ip,
                                            std::uint32_t dst_ip,
                                            std::span<const std::byte> tcp_segment) noexcept {
  NIC_TRACE_SCOPED(__func__);

  // TCP pseudo-header: src_ip(32) + dst_ip(32) + zero(8) + protocol(8) + tcp_length(16)
  std::array<std::byte, 12> pseudo_header{};
  pseudo_header[0] = static_cast<std::byte>(src_ip >> 24);
  pseudo_header[1] = static_cast<std::byte>(src_ip >> 16);
  pseudo_header[2] = static_cast<std::byte>(src_ip >> 8);
  pseudo_header[3] = static_cast<std::byte>(src_ip);

  pseudo_header[4] = static_cast<std::byte>(dst_ip >> 24);
  pseudo_header[5] = static_cast<std::byte>(dst_ip >> 16);
  pseudo_header[6] = static_cast<std::byte>(dst_ip >> 8);
  pseudo_header[7] = static_cast<std::byte>(dst_ip);

  pseudo_header[8] = std::byte{0};  // Zero
  pseudo_header[9] = std::byte{6};  // Protocol (TCP)

  std::uint16_t tcp_len = static_cast<std::uint16_t>(tcp_segment.size());
  pseudo_header[10] = static_cast<std::byte>(tcp_len >> 8);
  pseudo_header[11] = static_cast<std::byte>(tcp_len & 0xFF);

  // Calculate checksum over pseudo-header + TCP segment
  std::uint32_t sum = 0;

  // Add pseudo-header
  for (std::size_t i = 0; i < 12; i += 2) {
    sum += (static_cast<std::uint16_t>(pseudo_header[i]) << 8)
           | static_cast<std::uint16_t>(pseudo_header[i + 1]);
  }

  // Add TCP segment
  for (std::size_t i = 0; i < tcp_segment.size(); i += 2) {
    std::uint16_t word = static_cast<std::uint16_t>(tcp_segment[i]) << 8;
    if (i + 1 < tcp_segment.size()) {
      word |= static_cast<std::uint16_t>(tcp_segment[i + 1]);
    }
    sum += word;
  }

  // Fold 32-bit sum to 16 bits
  while (sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }

  return static_cast<std::uint16_t>(~sum);
}

std::uint16_t PacketGenerator::udp_checksum(std::uint32_t src_ip,
                                            std::uint32_t dst_ip,
                                            std::span<const std::byte> udp_datagram) noexcept {
  NIC_TRACE_SCOPED(__func__);

  // UDP pseudo-header: same as TCP
  std::array<std::byte, 12> pseudo_header{};
  pseudo_header[0] = static_cast<std::byte>(src_ip >> 24);
  pseudo_header[1] = static_cast<std::byte>(src_ip >> 16);
  pseudo_header[2] = static_cast<std::byte>(src_ip >> 8);
  pseudo_header[3] = static_cast<std::byte>(src_ip);

  pseudo_header[4] = static_cast<std::byte>(dst_ip >> 24);
  pseudo_header[5] = static_cast<std::byte>(dst_ip >> 16);
  pseudo_header[6] = static_cast<std::byte>(dst_ip >> 8);
  pseudo_header[7] = static_cast<std::byte>(dst_ip);

  pseudo_header[8] = std::byte{0};   // Zero
  pseudo_header[9] = std::byte{17};  // Protocol (UDP)

  std::uint16_t udp_len = static_cast<std::uint16_t>(udp_datagram.size());
  pseudo_header[10] = static_cast<std::byte>(udp_len >> 8);
  pseudo_header[11] = static_cast<std::byte>(udp_len & 0xFF);

  // Calculate checksum over pseudo-header + UDP datagram
  std::uint32_t sum = 0;

  // Add pseudo-header
  for (std::size_t i = 0; i < 12; i += 2) {
    sum += (static_cast<std::uint16_t>(pseudo_header[i]) << 8)
           | static_cast<std::uint16_t>(pseudo_header[i + 1]);
  }

  // Add UDP datagram
  for (std::size_t i = 0; i < udp_datagram.size(); i += 2) {
    std::uint16_t word = static_cast<std::uint16_t>(udp_datagram[i]) << 8;
    if (i + 1 < udp_datagram.size()) {
      word |= static_cast<std::uint16_t>(udp_datagram[i + 1]);
    }
    sum += word;
  }

  // Fold 32-bit sum to 16 bits
  while (sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }

  std::uint16_t result = static_cast<std::uint16_t>(~sum);

  // UDP checksum of 0 means no checksum; if calculated checksum is 0, use 0xFFFF
  return (result == 0) ? 0xFFFF : result;
}

std::vector<std::byte> PacketGenerator::generate_payload(std::size_t size,
                                                         PayloadPattern pattern,
                                                         std::uint32_t seed) noexcept {
  NIC_TRACE_SCOPED(__func__);

  std::vector<std::byte> payload(size);

  switch (pattern) {
    case PayloadPattern::Zeros:
      // Already zero-initialized
      break;

    case PayloadPattern::Ones:
      std::fill(payload.begin(), payload.end(), std::byte{0xFF});
      break;

    case PayloadPattern::Incrementing:
      for (std::size_t i = 0; i < size; ++i) {
        payload[i] = static_cast<std::byte>(i & 0xFF);
      }
      break;

    case PayloadPattern::Random: {
      std::mt19937 rng(seed);
      std::uniform_int_distribution<std::uint32_t> dist(0, 255);
      for (std::size_t i = 0; i < size; ++i) {
        payload[i] = static_cast<std::byte>(dist(rng));
      }
      break;
    }
  }

  return payload;
}

std::uint16_t PacketGenerator::internet_checksum(std::span<const std::byte> data) noexcept {
  std::uint32_t sum = 0;

  // Add 16-bit words
  for (std::size_t i = 0; i < data.size(); i += 2) {
    std::uint16_t word = static_cast<std::uint16_t>(data[i]) << 8;
    if (i + 1 < data.size()) {
      word |= static_cast<std::uint16_t>(data[i + 1]);
    }
    sum += word;
  }

  // Fold 32-bit sum to 16 bits
  while (sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }

  return static_cast<std::uint16_t>(~sum);
}