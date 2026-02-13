#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "bit_fields/bit_fields.h"

namespace nic {

/// Deterministic packet generator for creating test vectors using bit_fields library
class PacketGenerator {
public:
  /// Ethernet frame configuration
  struct EthernetConfig {
    std::uint64_t dst_mac{0};                             // 48-bit MAC
    std::uint64_t src_mac{0};                             // 48-bit MAC
    std::uint16_t ethertype{0x0800};                      // IPv4 by default
    std::optional<std::uint16_t> vlan_tag{std::nullopt};  // Optional VLAN (PCP=0, DEI=0, VID)
  };

  /// IPv4 packet configuration
  struct IPv4Config {
    std::uint32_t src_ip{0};
    std::uint32_t dst_ip{0};
    std::uint8_t protocol{6};  // TCP by default
    std::uint8_t ttl{64};
    std::uint8_t tos{0};  // DSCP+ECN combined
    std::uint16_t identification{0};
    bool dont_fragment{true};
  };

  /// TCP segment configuration
  struct TCPConfig {
    std::uint16_t src_port{0};
    std::uint16_t dst_port{0};
    std::uint32_t seq_num{0};
    std::uint32_t ack_num{0};
    std::uint16_t flags{0};  // Combined: data_offset(4) + reserved(3) + flags(9)
    std::uint16_t window{8192};
    std::uint16_t urgent_ptr{0};
  };

  /// UDP datagram configuration
  struct UDPConfig {
    std::uint16_t src_port{0};
    std::uint16_t dst_port{0};
  };

  PacketGenerator() = default;

  /// Generate complete Ethernet frame with IPv4/TCP packet
  [[nodiscard]] std::vector<std::byte> generate_eth_ipv4_tcp(
      const EthernetConfig& eth,
      const IPv4Config& ipv4,
      const TCPConfig& tcp,
      std::span<const std::byte> payload) noexcept;

  /// Generate complete Ethernet frame with IPv4/UDP packet
  [[nodiscard]] std::vector<std::byte> generate_eth_ipv4_udp(
      const EthernetConfig& eth,
      const IPv4Config& ipv4,
      const UDPConfig& udp,
      std::span<const std::byte> payload) noexcept;

  /// Generate Ethernet frame with custom payload (no IP)
  [[nodiscard]] std::vector<std::byte> generate_ethernet(
      const EthernetConfig& cfg, std::span<const std::byte> payload) noexcept;

  /// Generate IPv4 packet with payload
  [[nodiscard]] std::vector<std::byte> generate_ipv4(const IPv4Config& cfg,
                                                     std::span<const std::byte> payload) noexcept;

  /// Generate TCP segment with payload
  [[nodiscard]] std::vector<std::byte> generate_tcp(const TCPConfig& cfg,
                                                    std::uint32_t src_ip,
                                                    std::uint32_t dst_ip,
                                                    std::span<const std::byte> payload) noexcept;

  /// Generate UDP datagram with payload
  [[nodiscard]] std::vector<std::byte> generate_udp(const UDPConfig& cfg,
                                                    std::uint32_t src_ip,
                                                    std::uint32_t dst_ip,
                                                    std::span<const std::byte> payload) noexcept;

  /// Calculate IPv4 header checksum (used internally)
  [[nodiscard]] static std::uint16_t ipv4_checksum(std::span<const std::byte> header) noexcept;

  /// Calculate TCP checksum with pseudo-header
  [[nodiscard]] static std::uint16_t tcp_checksum(std::uint32_t src_ip,
                                                  std::uint32_t dst_ip,
                                                  std::span<const std::byte> tcp_segment) noexcept;

  /// Calculate UDP checksum with pseudo-header
  [[nodiscard]] static std::uint16_t udp_checksum(std::uint32_t src_ip,
                                                  std::uint32_t dst_ip,
                                                  std::span<const std::byte> udp_datagram) noexcept;

  /// Generate payload with specific pattern
  enum class PayloadPattern {
    Zeros,         ///< All zeros
    Ones,          ///< All 0xFF
    Incrementing,  ///< 0x00, 0x01, 0x02, ...
    Random,        ///< Pseudo-random (seeded)
  };

  [[nodiscard]] static std::vector<std::byte> generate_payload(std::size_t size,
                                                               PayloadPattern pattern,
                                                               std::uint32_t seed = 0) noexcept;

private:
  /// Helper: Calculate Internet checksum (RFC 1071)
  [[nodiscard]] static std::uint16_t internet_checksum(std::span<const std::byte> data) noexcept;
};

}  // namespace nic