#include "nic/rocev2/completion_queue.h"

namespace nic::rocev2 {

RdmaCompletionQueue::RdmaCompletionQueue(std::uint32_t cq_number, RdmaCqConfig config)
  : cq_number_(cq_number), config_(config) {
  NIC_TRACE_SCOPED(__func__);
}

bool RdmaCompletionQueue::post(const RdmaCqe& cqe) {
  NIC_TRACE_SCOPED(__func__);

  if (is_full()) {
    ++stats_.overflows;
    return false;
  }

  cqes_.push_back(cqe);
  ++stats_.cqes_posted;
  has_new_completions_ = true;

  return true;
}

std::vector<RdmaCqe> RdmaCompletionQueue::poll(std::size_t max_cqes) {
  NIC_TRACE_SCOPED(__func__);

  std::vector<RdmaCqe> result;
  std::size_t to_poll = std::min(max_cqes, cqes_.size());
  result.reserve(to_poll);

  for (std::size_t poll_idx = 0; poll_idx < to_poll; ++poll_idx) {
    result.push_back(cqes_.front());
    cqes_.pop_front();
    ++stats_.cqes_polled;
  }

  if (!cqes_.empty()) {
    has_new_completions_ = false;
  }

  return result;
}

std::optional<RdmaCqe> RdmaCompletionQueue::poll_one() {
  NIC_TRACE_SCOPED(__func__);

  if (cqes_.empty()) {
    return std::nullopt;
  }

  RdmaCqe cqe = cqes_.front();
  cqes_.pop_front();
  ++stats_.cqes_polled;

  if (cqes_.empty()) {
    has_new_completions_ = false;
  }

  return cqe;
}

void RdmaCompletionQueue::arm() {
  NIC_TRACE_SCOPED(__func__);
  armed_ = true;
  has_new_completions_ = false;
  ++stats_.arm_count;
}

bool RdmaCompletionQueue::should_notify() const noexcept {
  return armed_ && has_new_completions_;
}

void RdmaCompletionQueue::reset() {
  NIC_TRACE_SCOPED(__func__);
  cqes_.clear();
  armed_ = false;
  has_new_completions_ = false;
  stats_ = RdmaCqStats{};
}

}  // namespace nic::rocev2
