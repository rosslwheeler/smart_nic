#include "nic/checksum.h"

#include <cstddef>
#include <cstdint>

#include "nic/trace.h"

using namespace nic;

std::uint16_t nic::compute_checksum(std::span<const std::byte> buffer) {
  NIC_TRACE_SCOPED(__func__);
  std::uint32_t sum = 0;
  const std::size_t len = buffer.size();

  for (std::size_t i = 0; i + 1 < len; i += 2) {
    std::uint16_t word = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(buffer[i]) << 8)
                         | static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(buffer[i + 1]));
    sum += word;
    if (sum & 0x10000) {
      sum = (sum & 0xFFFF) + 1;  // fold carry
    }
  }

  if (len & 1) {
    std::uint16_t last =
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(buffer[len - 1]) << 8);
    sum += last;
    if (sum & 0x10000) {
      sum = (sum & 0xFFFF) + 1;
    }
  }

  return static_cast<std::uint16_t>(~sum);
}

bool nic::verify_checksum(std::span<const std::byte> buffer, std::uint16_t expected) {
  NIC_TRACE_SCOPED(__func__);
  return compute_checksum(buffer) == expected;
}
