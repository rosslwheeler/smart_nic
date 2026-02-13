#include "nic/completion_queue.h"

using namespace nic;

CompletionQueue::CompletionQueue(CompletionQueueConfig config, Doorbell* doorbell)
  : config_(config), doorbell_(doorbell), entries_(config.ring_size) {
  NIC_TRACE_SCOPED(__func__);
}

bool CompletionQueue::is_full() const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return count_ == config_.ring_size;
}

bool CompletionQueue::is_empty() const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return count_ == 0;
}

std::size_t CompletionQueue::available() const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return count_;
}

std::size_t CompletionQueue::space() const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return config_.ring_size - count_;
}

bool CompletionQueue::post_completion(const CompletionEntry& entry) {
  NIC_TRACE_SCOPED(__func__);
  if (is_full()) {
    return false;
  }
  entries_[producer_index_] = entry;
  producer_index_ = (producer_index_ + 1) % config_.ring_size;
  ++count_;
  if (doorbell_ != nullptr) {
    doorbell_->ring(DoorbellPayload{config_.queue_id, producer_index_});
  }
  return true;
}

std::optional<CompletionEntry> CompletionQueue::poll_completion() {
  NIC_TRACE_SCOPED(__func__);
  if (is_empty()) {
    return std::nullopt;
  }
  CompletionEntry entry = entries_[consumer_index_];
  consumer_index_ = (consumer_index_ + 1) % config_.ring_size;
  --count_;
  return entry;
}

void CompletionQueue::reset() noexcept {
  NIC_TRACE_SCOPED(__func__);
  producer_index_ = 0;
  consumer_index_ = 0;
  count_ = 0;
}
