#pragma once

#include <cstdint>
#include <span>

namespace nic {

/// Compute a simple ones-complement checksum over the buffer.
std::uint16_t compute_checksum(std::span<const std::byte> buffer);

/// Verify that the provided checksum matches the buffer.
bool verify_checksum(std::span<const std::byte> buffer, std::uint16_t expected);

}  // namespace nic
