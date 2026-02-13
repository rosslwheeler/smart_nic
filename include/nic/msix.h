#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace nic {

struct MsixVector {
  std::uint64_t address{0};
  std::uint32_t data{0};
  bool enabled{true};
  bool masked{false};
};

/// Lightweight MSI-X table model.
class MsixTable {
public:
  explicit MsixTable(std::size_t count = 0);

  [[nodiscard]] std::size_t size() const noexcept { return vectors_.size(); }
  [[nodiscard]] bool valid_index(std::size_t idx) const noexcept { return idx < vectors_.size(); }

  bool set_vector(std::size_t idx, const MsixVector& vec) noexcept;
  [[nodiscard]] std::optional<MsixVector> vector(std::size_t idx) const noexcept;

  bool mask(std::size_t idx, bool masked = true) noexcept;
  bool enable(std::size_t idx, bool enabled = true) noexcept;

private:
  std::vector<MsixVector> vectors_;
};

/// Mapping from queue/admin events to MSI-X vectors.
class MsixMapping {
public:
  explicit MsixMapping(std::size_t queue_count = 0, std::uint16_t default_vector = 0);

  bool set_queue_vector(std::size_t queue_id, std::uint16_t vector) noexcept;
  [[nodiscard]] std::uint16_t queue_vector(std::size_t queue_id) const noexcept;

  void set_admin_vector(std::uint16_t vector) noexcept { admin_vector_ = vector; }
  [[nodiscard]] std::uint16_t admin_vector() const noexcept { return admin_vector_; }

private:
  std::uint16_t admin_vector_{0};
  std::uint16_t default_vector_{0};
  std::vector<std::uint16_t> queue_vectors_;
};

}  // namespace nic
