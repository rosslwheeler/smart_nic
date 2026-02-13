#include "nic/interrupt_dispatcher.h"

#include "nic/log.h"
#include "nic/trace.h"

using namespace nic;

InterruptDispatcher::InterruptDispatcher(MsixTable table,
                                         MsixMapping mapping,
                                         CoalesceConfig config,
                                         DeliverFn deliver)
  : table_(std::move(table)),
    mapping_(std::move(mapping)),
    coalesce_(config),
    deliver_(std::move(deliver)) {
  NIC_TRACE_SCOPED(__func__);
}

std::optional<std::uint16_t> InterruptDispatcher::resolve_vector(
    std::uint16_t queue_id) const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return mapping_.queue_vector(queue_id);
}

void InterruptDispatcher::try_fire(std::uint16_t vector_id) {
  NIC_TRACE_SCOPED(__func__);
  if (!table_.valid_index(vector_id)) {
    return;
  }
  auto vec = table_.vector(vector_id);
  if (!vec.has_value()) {
    return;
  }
  if (!vec->enabled) {
    stats_.suppressed_disabled += 1;
    stats_.per_vector_suppressed_disabled[vector_id] += 1;
    NIC_LOGF_TRACE("interrupt suppressed: vector={} reason=disabled", vector_id);
    return;
  }
  if (vec->masked) {
    stats_.suppressed_masked += 1;
    stats_.per_vector_suppressed_masked[vector_id] += 1;
    NIC_LOGF_TRACE("interrupt suppressed: vector={} reason=masked", vector_id);
    return;
  }

  auto it = pending_counts_.find(vector_id);
  std::uint32_t batch = 0;
  if (it != pending_counts_.end()) {
    batch = it->second;
    pending_counts_.erase(it);
  }
  if (batch == 0) {
    batch = 1;
  } else {
    stats_.coalesced_batches += 1;
  }

  // Update adaptive moderation based on batch size
  update_adaptive_threshold(vector_id, batch);

  NIC_LOGF_DEBUG("interrupt fired: vector={} batch={}", vector_id, batch);
  if (deliver_) {
    deliver_(vector_id, batch);
  }
  stats_.interrupts_fired += 1;
  stats_.per_vector_fired[vector_id] += 1;
}

bool InterruptDispatcher::on_completion(const InterruptEvent& ev) {
  NIC_TRACE_SCOPED(__func__);
  auto vector_id = resolve_vector(ev.queue_id);
  if (!vector_id.has_value()) {
    return false;
  }

  auto vec = table_.vector(*vector_id);
  if (!vec.has_value()) {
    return false;
  }
  if (!vec->enabled) {
    stats_.suppressed_disabled += 1;
    stats_.per_vector_suppressed_disabled[*vector_id] += 1;
    return false;
  }
  if (vec->masked) {
    stats_.suppressed_masked += 1;
    stats_.per_vector_suppressed_masked[*vector_id] += 1;
    return false;
  }

  const CoalesceConfig& cfg = get_coalesce_config(ev.queue_id);
  std::uint32_t& count = pending_counts_[*vector_id];
  ++count;
  pending_time_us_[*vector_id] = 0;

  // Determine threshold: use adaptive if enabled, otherwise use static config
  std::uint32_t threshold = cfg.packet_threshold;
  if (adaptive_.enabled) {
    auto it = adaptive_state_.find(*vector_id);
    if (it != adaptive_state_.end()) {
      threshold = it->second.current_threshold;
    } else {
      // Initialize adaptive state for this vector
      adaptive_state_[*vector_id].current_threshold = cfg.packet_threshold;
    }
  }

  if (count >= threshold) {
    try_fire(*vector_id);
    pending_time_us_.erase(*vector_id);
  }
  // Timer-based flush would go here; placeholder for future.
  return true;
}

void InterruptDispatcher::flush(std::optional<std::uint16_t> vector_id) {
  NIC_TRACE_SCOPED(__func__);
  if (vector_id.has_value()) {
    try_fire(*vector_id);
    pending_time_us_.erase(*vector_id);
  } else {
    // Flush all pending vectors.
    std::vector<std::uint16_t> keys;
    keys.reserve(pending_counts_.size());
    for (const auto& kv : pending_counts_) {
      keys.push_back(kv.first);
    }
    for (auto vid : keys) {
      try_fire(vid);
      pending_time_us_.erase(vid);
    }
  }
  stats_.manual_flushes += 1;
}

void InterruptDispatcher::on_timer_tick(std::uint32_t elapsed_us) {
  NIC_TRACE_SCOPED(__func__);
  if ((coalesce_.timer_threshold_us == 0) || pending_counts_.empty()) {
    return;
  }
  // Ensure we track time for all pending vectors.
  for (const auto& kv : pending_counts_) {
    pending_time_us_.try_emplace(kv.first, 0);
  }
  for (auto it = pending_time_us_.begin(); it != pending_time_us_.end();) {
    it->second += elapsed_us;
    if (it->second >= coalesce_.timer_threshold_us) {
      try_fire(it->first);
      stats_.timer_flushes += 1;
      it = pending_time_us_.erase(it);
    } else {
      ++it;
    }
  }
}

bool InterruptDispatcher::set_queue_vector(std::uint16_t queue_id,
                                           std::uint16_t vector_id) noexcept {
  NIC_TRACE_SCOPED(__func__);
  return mapping_.set_queue_vector(queue_id, vector_id);
}

bool InterruptDispatcher::mask_vector(std::uint16_t vector_id, bool masked) noexcept {
  NIC_TRACE_SCOPED(__func__);
  return table_.mask(vector_id, masked);
}

bool InterruptDispatcher::enable_vector(std::uint16_t vector_id, bool enabled) noexcept {
  NIC_TRACE_SCOPED(__func__);
  return table_.enable(vector_id, enabled);
}

const CoalesceConfig& InterruptDispatcher::get_coalesce_config(
    std::uint16_t queue_id) const noexcept {
  NIC_TRACE_SCOPED(__func__);
  auto it = per_queue_coalesce_.find(queue_id);
  if (it != per_queue_coalesce_.end()) {
    return it->second;
  }
  return coalesce_;
}

bool InterruptDispatcher::set_queue_coalesce_config(std::uint16_t queue_id,
                                                    const CoalesceConfig& config) noexcept {
  NIC_TRACE_SCOPED(__func__);
  per_queue_coalesce_[queue_id] = config;
  return true;
}

std::optional<CoalesceConfig> InterruptDispatcher::queue_coalesce_config(
    std::uint16_t queue_id) const noexcept {
  NIC_TRACE_SCOPED(__func__);
  auto it = per_queue_coalesce_.find(queue_id);
  if (it != per_queue_coalesce_.end()) {
    return it->second;
  }
  return std::nullopt;
}

void InterruptDispatcher::clear_queue_coalesce_config(std::uint16_t queue_id) noexcept {
  NIC_TRACE_SCOPED(__func__);
  per_queue_coalesce_.erase(queue_id);
}

void InterruptDispatcher::set_adaptive_config(const AdaptiveConfig& config) noexcept {
  NIC_TRACE_SCOPED(__func__);
  adaptive_ = config;
  // Reset adaptive state when config changes
  if (config.enabled) {
    for (auto& [vector_id, state] : adaptive_state_) {
      state.interrupt_count = 0;
      state.total_batch_size = 0;
      state.current_threshold = coalesce_.packet_threshold;
    }
  }
}

void InterruptDispatcher::update_adaptive_threshold(std::uint16_t vector_id,
                                                    std::uint32_t batch_size) noexcept {
  NIC_TRACE_SCOPED(__func__);
  if (!adaptive_.enabled) {
    return;
  }

  AdaptiveState& state = adaptive_state_[vector_id];
  state.interrupt_count++;
  state.total_batch_size += batch_size;

  // Check if we've reached the sample interval
  if (state.interrupt_count < adaptive_.sample_interval) {
    return;
  }

  // Calculate average batch size over the sample period
  std::uint32_t avg_batch =
      static_cast<std::uint32_t>(state.total_batch_size / state.interrupt_count);

  // Adjust threshold based on average batch size
  if ((avg_batch >= adaptive_.high_batch_size)
      && (state.current_threshold < adaptive_.max_threshold)) {
    // High load: increase threshold to reduce interrupt rate
    state.current_threshold = std::min(state.current_threshold + 1, adaptive_.max_threshold);
  } else if ((avg_batch <= adaptive_.low_batch_size)
             && (state.current_threshold > adaptive_.min_threshold)) {
    // Low load: decrease threshold for better latency
    state.current_threshold = std::max(state.current_threshold - 1, adaptive_.min_threshold);
  }

  // Reset counters for next sample period
  state.interrupt_count = 0;
  state.total_batch_size = 0;
}
