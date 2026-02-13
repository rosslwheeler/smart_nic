#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include "nic/trace.h"

namespace nic {

struct DoorbellPayload {
  std::uint16_t queue_id{0};
  std::uint32_t data{0};
};

using DoorbellCallback = std::function<void(const DoorbellPayload&)>;

/// Abstraction for queue notification doorbells.
class Doorbell {
public:
  Doorbell() = default;

  /// Ring the doorbell with a payload (e.g., queue id and producer index).
  void ring(const DoorbellPayload& payload);

  /// Set a callback invoked on each ring (no ownership of captured state).
  void set_callback(DoorbellCallback callback);

  /// Mask/unmask the doorbell (masked rings are ignored).
  void set_masked(bool masked);

  [[nodiscard]] bool is_masked() const noexcept;

  /// Inspect last payload for testing/telemetry.
  [[nodiscard]] std::optional<DoorbellPayload> last_payload() const noexcept;

  [[nodiscard]] std::size_t rings() const noexcept;

  /// Reset counters and last payload.
  void reset() noexcept;

private:
  DoorbellCallback callback_{};
  std::optional<DoorbellPayload> last_payload_{};
  std::size_t rings_{0};
  bool masked_{false};
};

}  // namespace nic
