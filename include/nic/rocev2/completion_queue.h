#pragma once

/// @file completion_queue.h
/// @brief RDMA Completion Queue for RoCEv2.

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "nic/rocev2/cqe.h"
#include "nic/trace.h"

namespace nic::rocev2 {

/// Completion Queue configuration.
struct RdmaCqConfig {
  std::size_t depth{256};  // Maximum number of CQEs
};

/// Completion Queue statistics.
struct RdmaCqStats {
  std::uint64_t cqes_posted{0};
  std::uint64_t cqes_polled{0};
  std::uint64_t overflows{0};
  std::uint64_t arm_count{0};
};

/// RDMA Completion Queue - holds completed work requests.
class RdmaCompletionQueue {
public:
  explicit RdmaCompletionQueue(std::uint32_t cq_number, RdmaCqConfig config = {});

  /// Post a CQE to the queue.
  /// @param cqe The completion entry to post.
  /// @return true if posted successfully, false if queue is full.
  bool post(const RdmaCqe& cqe);

  /// Poll CQEs from the queue.
  /// @param max_cqes Maximum number of CQEs to poll.
  /// @return Vector of CQEs (may be empty if queue is empty).
  [[nodiscard]] std::vector<RdmaCqe> poll(std::size_t max_cqes);

  /// Poll a single CQE from the queue.
  /// @return CQE if available, std::nullopt otherwise.
  [[nodiscard]] std::optional<RdmaCqe> poll_one();

  /// Arm the CQ for notification (e.g., interrupt on next completion).
  void arm();

  /// Check if CQ is armed.
  [[nodiscard]] bool is_armed() const noexcept { return armed_; }

  /// Clear the armed state (called after notification).
  void clear_arm() noexcept { armed_ = false; }

  /// Check if a notification should be triggered.
  /// Returns true if CQ was armed and has new completions.
  [[nodiscard]] bool should_notify() const noexcept;

  /// Get the CQ number.
  [[nodiscard]] std::uint32_t cq_number() const noexcept { return cq_number_; }

  /// Get current number of CQEs in queue.
  [[nodiscard]] std::size_t count() const noexcept { return cqes_.size(); }

  /// Check if CQ is empty.
  [[nodiscard]] bool is_empty() const noexcept { return cqes_.empty(); }

  /// Check if CQ is full.
  [[nodiscard]] bool is_full() const noexcept { return cqes_.size() >= config_.depth; }

  /// Get the CQ depth (capacity).
  [[nodiscard]] std::size_t depth() const noexcept { return config_.depth; }

  /// Get statistics.
  [[nodiscard]] const RdmaCqStats& stats() const noexcept { return stats_; }

  /// Reset the CQ.
  void reset();

private:
  std::uint32_t cq_number_;
  RdmaCqConfig config_;
  std::deque<RdmaCqe> cqes_;
  bool armed_{false};
  bool has_new_completions_{false};
  RdmaCqStats stats_;
};

}  // namespace nic::rocev2
