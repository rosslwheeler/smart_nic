#pragma once

#include <cstdint>

#ifdef TRACY_ENABLE
#include <Tracy.hpp>
#include <format>
#include <string>
#endif

namespace nic {

/// Log levels for Tracy-based logging
enum class LogLevel : std::uint8_t {
  Error = 0,    ///< Critical errors only
  Warning = 1,  ///< Warnings + errors
  Info = 2,     ///< Informational + warnings + errors
  Debug = 3,    ///< Debug + all above
  Trace = 4,    ///< All messages including trace
};

/// Simple logging controller using Tracy
class LogController {
public:
  /// Get singleton instance
  static LogController& instance() noexcept {
    static LogController ctrl;
    return ctrl;
  }

  /// Set global log level
  void set_level(LogLevel level) noexcept { level_ = level; }

  /// Get current level
  [[nodiscard]] LogLevel level() const noexcept { return level_; }

  /// Check if level is enabled
  [[nodiscard]] bool is_enabled(LogLevel level) const noexcept {
    return static_cast<std::uint8_t>(level) <= static_cast<std::uint8_t>(level_);
  }

private:
  LogLevel level_{LogLevel::Info};  // Default to Info level
};

/// Get Tracy color for log level
inline constexpr std::uint32_t log_level_color(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Error:
      return 0xFF0000;  // Red
    case LogLevel::Warning:
      return 0xFFA500;  // Orange
    case LogLevel::Info:
      return 0x00FF00;  // Green
    case LogLevel::Debug:
      return 0x00FFFF;  // Cyan
    case LogLevel::Trace:
      return 0xCCCCCC;  // Gray
    default:
      return 0xFFFFFF;  // White
  }
}

}  // namespace nic

/// Macro for conditional logging with Tracy
#ifdef TRACY_ENABLE
#define NIC_LOG(level, msg)                                               \
  do {                                                                    \
    if (::nic::LogController::instance().is_enabled(level)) {             \
      TracyMessageC(msg, sizeof(msg) - 1, ::nic::log_level_color(level)); \
    }                                                                     \
  } while (0)

#define NIC_LOG_ERROR(msg)   NIC_LOG(::nic::LogLevel::Error, msg)
#define NIC_LOG_WARNING(msg) NIC_LOG(::nic::LogLevel::Warning, msg)
#define NIC_LOG_INFO(msg)    NIC_LOG(::nic::LogLevel::Info, msg)
#define NIC_LOG_DEBUG(msg)   NIC_LOG(::nic::LogLevel::Debug, msg)
#define NIC_LOG_TRACE(msg)   NIC_LOG(::nic::LogLevel::Trace, msg)

/// Formatted logging macro - uses std::format for runtime values
#define NIC_LOGF(level, fmt_str, ...)                                                         \
  do {                                                                                        \
    if (::nic::LogController::instance().is_enabled(level)) {                                 \
      auto nic_log_msg_ = std::format(fmt_str __VA_OPT__(, ) __VA_ARGS__);                    \
      TracyMessageC(nic_log_msg_.data(), nic_log_msg_.size(), ::nic::log_level_color(level)); \
    }                                                                                         \
  } while (0)

#define NIC_LOGF_ERROR(fmt_str, ...) \
  NIC_LOGF(::nic::LogLevel::Error, fmt_str __VA_OPT__(, ) __VA_ARGS__)
#define NIC_LOGF_WARNING(fmt_str, ...) \
  NIC_LOGF(::nic::LogLevel::Warning, fmt_str __VA_OPT__(, ) __VA_ARGS__)
#define NIC_LOGF_INFO(fmt_str, ...) \
  NIC_LOGF(::nic::LogLevel::Info, fmt_str __VA_OPT__(, ) __VA_ARGS__)
#define NIC_LOGF_DEBUG(fmt_str, ...) \
  NIC_LOGF(::nic::LogLevel::Debug, fmt_str __VA_OPT__(, ) __VA_ARGS__)
#define NIC_LOGF_TRACE(fmt_str, ...) \
  NIC_LOGF(::nic::LogLevel::Trace, fmt_str __VA_OPT__(, ) __VA_ARGS__)
#else
   // No-op when Tracy is disabled
#define NIC_LOG(level, msg)            ((void) 0)
#define NIC_LOG_ERROR(msg)             ((void) 0)
#define NIC_LOG_WARNING(msg)           ((void) 0)
#define NIC_LOG_INFO(msg)              ((void) 0)
#define NIC_LOG_DEBUG(msg)             ((void) 0)
#define NIC_LOG_TRACE(msg)             ((void) 0)
#define NIC_LOGF(level, fmt_str, ...)  ((void) 0)
#define NIC_LOGF_ERROR(fmt_str, ...)   ((void) 0)
#define NIC_LOGF_WARNING(fmt_str, ...) ((void) 0)
#define NIC_LOGF_INFO(fmt_str, ...)    ((void) 0)
#define NIC_LOGF_DEBUG(fmt_str, ...)   ((void) 0)
#define NIC_LOGF_TRACE(fmt_str, ...)   ((void) 0)
#endif