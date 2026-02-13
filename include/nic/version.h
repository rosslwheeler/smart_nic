#pragma once

#include <cstdint>

namespace nic {

inline constexpr std::uint32_t kNicModelMajor = 0;
inline constexpr std::uint32_t kNicModelMinor = 1;
inline constexpr std::uint32_t kNicModelPatch = 0;

inline constexpr std::uint32_t MakeVersion(std::uint32_t major,
                                           std::uint32_t minor,
                                           std::uint32_t patch) {
  return (major << 24U) | (minor << 16U) | patch;
}

inline constexpr std::uint32_t kNicModelVersion =
    MakeVersion(kNicModelMajor, kNicModelMinor, kNicModelPatch);

}  // namespace nic
