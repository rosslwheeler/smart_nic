#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace nic {

/// Controlled fault injection for testing error paths
class ErrorInjector {
public:
  /// Types of errors that can be injected
  enum class ErrorType {
    None,
    DMAReadFail,
    DMAWriteFail,
    InvalidDescriptor,
    ChecksumError,
    QueueFull,
    Timeout,
  };

  /// Configuration for a specific error injection
  struct ErrorConfig {
    ErrorType type{ErrorType::None};
    std::uint16_t target_queue{0};   ///< Which queue to affect (0xFFFF = all)
    std::uint32_t trigger_count{0};  ///< Trigger after N operations (0 = immediate)
    std::uint32_t inject_count{1};   ///< Number of times to inject
    bool one_shot{true};             ///< Auto-disable after inject_count
  };

  ErrorInjector() = default;

  /// Configure error injection
  void configure(const ErrorConfig& config) noexcept;

  /// Check if should inject error (called by subsystems)
  [[nodiscard]] bool should_inject(ErrorType type, std::uint16_t queue_id = 0) noexcept;

  /// Disable all error injection
  void disable_all() noexcept;

  /// Query active errors
  [[nodiscard]] std::vector<ErrorConfig> active_errors() const noexcept;

private:
  struct ActiveError {
    ErrorConfig config;
    std::atomic<std::uint32_t> operation_count{0};
    std::atomic<std::uint32_t> inject_count{0};
    std::atomic<bool> enabled{true};
  };

  std::vector<std::unique_ptr<ActiveError>> active_errors_;
};

}  // namespace nic
