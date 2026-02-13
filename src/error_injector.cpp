#include "nic/error_injector.h"

#include "nic/log.h"
#include "nic/trace.h"

using namespace nic;

void ErrorInjector::configure(const ErrorConfig& config) noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (config.type == ErrorType::None) {
    return;
  }

  auto error = std::make_unique<ActiveError>();
  error->config = config;
  error->operation_count.store(0, std::memory_order_relaxed);
  error->inject_count.store(0, std::memory_order_relaxed);
  error->enabled.store(true, std::memory_order_relaxed);

  active_errors_.push_back(std::move(error));
}

bool ErrorInjector::should_inject(ErrorType type, std::uint16_t queue_id) noexcept {
  for (auto& error : active_errors_) {
    // Check if this error matches type
    if (error->config.type != type) {
      continue;
    }

    // Check if error is still enabled
    if (!error->enabled.load(std::memory_order_relaxed)) {
      continue;
    }

    // Check queue targeting
    if ((error->config.target_queue != 0xFFFF) && (error->config.target_queue != queue_id)) {
      continue;
    }

    // Increment operation count
    std::uint32_t ops = error->operation_count.fetch_add(1, std::memory_order_relaxed);

    // Check if we've reached trigger count
    if (ops < error->config.trigger_count) {
      continue;
    }

    // For one-shot errors, check inject count limit
    if (error->config.one_shot) {
      std::uint32_t injected = error->inject_count.fetch_add(1, std::memory_order_relaxed);

      // Check if we've reached inject limit
      if (injected >= error->config.inject_count) {
        // Disable and don't inject this time
        error->enabled.store(false, std::memory_order_relaxed);
        continue;
      }
    }
    // Continuous errors ignore inject_count and inject every time

    NIC_TRACE_SCOPED("ErrorInjector::injecting_error");
    NIC_LOGF_WARNING("error injected: type={} queue={}", static_cast<int>(type), queue_id);
    return true;
  }

  return false;
}

void ErrorInjector::disable_all() noexcept {
  NIC_TRACE_SCOPED(__func__);

  for (auto& error : active_errors_) {
    error->enabled.store(false, std::memory_order_relaxed);
  }

  active_errors_.clear();
}

std::vector<ErrorInjector::ErrorConfig> ErrorInjector::active_errors() const noexcept {
  std::vector<ErrorConfig> configs;

  for (const auto& error : active_errors_) {
    if (error->enabled.load(std::memory_order_relaxed)) {
      configs.push_back(error->config);
    }
  }

  return configs;
}