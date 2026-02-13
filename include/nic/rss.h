#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "nic/trace.h"

namespace nic {

struct RssConfig {
  std::vector<std::uint8_t> key;     ///< Toeplitz key bytes
  std::vector<std::uint16_t> table;  ///< Indirection table mapping hash to queue id
};

struct RssStats {
  std::uint64_t hashes{0};
  std::vector<std::uint64_t> queue_hits;  ///< hit counts per queue id index in table
};

/// RSS engine implementing Toeplitz hashing and indirection lookup.
class RssEngine {
public:
  RssEngine();
  explicit RssEngine(RssConfig config);

  void set_key(std::vector<std::uint8_t> key);
  void set_table(std::vector<std::uint16_t> table);

  [[nodiscard]] std::uint32_t hash(std::span<const std::uint8_t> data) const;

  /// Select queue id from data; returns nullopt if table is empty.
  [[nodiscard]] std::optional<std::uint16_t> select_queue(std::span<const std::uint8_t> data) const;

  [[nodiscard]] const RssConfig& config() const noexcept { return config_; }
  [[nodiscard]] const RssStats& stats() const noexcept { return stats_; }
  void reset_stats() noexcept;

private:
  RssConfig config_;
  mutable RssStats stats_;

  [[nodiscard]] std::uint32_t toeplitz_hash(std::span<const std::uint8_t> key,
                                            std::span<const std::uint8_t> data) const;
  void ensure_defaults();
};

}  // namespace nic
