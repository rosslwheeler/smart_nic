#include "nic/flow_control.h"

#include <cstring>

#include "nic/trace.h"

using namespace nic;

// PauseFrame implementation

bool PauseFrame::is_pause_frame(std::span<const std::byte> packet) noexcept {
  if (packet.size() < 14) {
    return false;
  }

  // Check EtherType at bytes 12-13 (0x8808 for MAC Control)
  std::uint8_t et_hi = static_cast<std::uint8_t>(packet[12]);
  std::uint8_t et_lo = static_cast<std::uint8_t>(packet[13]);
  std::uint16_t ethertype = (static_cast<std::uint16_t>(et_hi) << 8) | et_lo;

  return ethertype == kEtherType;
}

std::optional<PauseFrame> PauseFrame::parse(std::span<const std::byte> packet) noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (!is_pause_frame(packet)) {
    return std::nullopt;
  }

  // Pause frame format (after Ethernet header):
  // Bytes 14-15: Opcode (0x0001 for pause)
  // Bytes 16-17: Pause time in quanta
  if (packet.size() < 18) {
    return std::nullopt;
  }

  std::uint8_t opcode_hi = static_cast<std::uint8_t>(packet[14]);
  std::uint8_t opcode_lo = static_cast<std::uint8_t>(packet[15]);
  std::uint16_t opcode = (static_cast<std::uint16_t>(opcode_hi) << 8) | opcode_lo;

  std::uint8_t pause_hi = static_cast<std::uint8_t>(packet[16]);
  std::uint8_t pause_lo = static_cast<std::uint8_t>(packet[17]);
  std::uint16_t pause_time = (static_cast<std::uint16_t>(pause_hi) << 8) | pause_lo;

  return PauseFrame{.opcode = opcode, .pause_time = pause_time};
}

std::vector<std::byte> PauseFrame::serialize() const noexcept {
  NIC_TRACE_SCOPED(__func__);

  // Minimal Ethernet + MAC Control frame (64 bytes minimum)
  std::vector<std::byte> packet(64, std::byte{0});

  // Destination MAC: 01:80:C2:00:00:01 (Pause multicast)
  packet[0] = std::byte{0x01};
  packet[1] = std::byte{0x80};
  packet[2] = std::byte{0xC2};
  packet[3] = std::byte{0x00};
  packet[4] = std::byte{0x00};
  packet[5] = std::byte{0x01};

  // Source MAC: 00:00:00:00:00:00 (placeholder)
  // Bytes 6-11 remain zero

  // EtherType: 0x8808 (MAC Control)
  packet[12] = std::byte{0x88};
  packet[13] = std::byte{0x08};

  // Opcode
  packet[14] = std::byte{static_cast<std::uint8_t>(opcode >> 8)};
  packet[15] = std::byte{static_cast<std::uint8_t>(opcode & 0xFF)};

  // Pause time
  packet[16] = std::byte{static_cast<std::uint8_t>(pause_time >> 8)};
  packet[17] = std::byte{static_cast<std::uint8_t>(pause_time & 0xFF)};

  // Remaining bytes are padding (already zero)

  return packet;
}

// PFCFrame implementation

std::optional<PFCFrame> PFCFrame::parse(std::span<const std::byte> packet) noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (!PauseFrame::is_pause_frame(packet)) {
    return std::nullopt;
  }

  // PFC frame format:
  // Bytes 14-15: Opcode (0x0101 for PFC)
  // Byte 16: Enabled priority bitmap
  // Byte 17: Reserved
  // Bytes 18-33: 8 * 2-byte pause times
  if (packet.size() < 34) {
    return std::nullopt;
  }

  std::uint8_t opcode_hi = static_cast<std::uint8_t>(packet[14]);
  std::uint8_t opcode_lo = static_cast<std::uint8_t>(packet[15]);
  std::uint16_t opcode = (static_cast<std::uint16_t>(opcode_hi) << 8) | opcode_lo;

  if (opcode != PauseFrame::kOpcodePFC) {
    return std::nullopt;
  }

  PFCFrame frame;
  frame.opcode = opcode;
  frame.enabled_priorities = static_cast<std::uint8_t>(packet[16]);

  // Parse pause times for each priority
  for (std::size_t i = 0; i < 8; ++i) {
    std::size_t offset = 18 + (i * 2);
    std::uint8_t hi = static_cast<std::uint8_t>(packet[offset]);
    std::uint8_t lo = static_cast<std::uint8_t>(packet[offset + 1]);
    frame.pause_times[i] = (static_cast<std::uint16_t>(hi) << 8) | lo;
  }

  return frame;
}

std::vector<std::byte> PFCFrame::serialize() const noexcept {
  NIC_TRACE_SCOPED(__func__);

  std::vector<std::byte> packet(64, std::byte{0});

  // Destination MAC: 01:80:C2:00:00:01
  packet[0] = std::byte{0x01};
  packet[1] = std::byte{0x80};
  packet[2] = std::byte{0xC2};
  packet[3] = std::byte{0x00};
  packet[4] = std::byte{0x00};
  packet[5] = std::byte{0x01};

  // EtherType: 0x8808
  packet[12] = std::byte{0x88};
  packet[13] = std::byte{0x08};

  // Opcode: 0x0101
  packet[14] = std::byte{static_cast<std::uint8_t>(opcode >> 8)};
  packet[15] = std::byte{static_cast<std::uint8_t>(opcode & 0xFF)};

  // Enabled priorities bitmap
  packet[16] = std::byte{enabled_priorities};

  // Reserved byte (17) remains zero

  // Pause times for each priority
  for (std::size_t i = 0; i < 8; ++i) {
    std::size_t offset = 18 + (i * 2);
    packet[offset] = std::byte{static_cast<std::uint8_t>(pause_times[i] >> 8)};
    packet[offset + 1] = std::byte{static_cast<std::uint8_t>(pause_times[i] & 0xFF)};
  }

  return packet;
}

// FlowControlManager implementation

FlowControlManager::FlowControlManager(Config config) : config_(std::move(config)) {
  NIC_TRACE_SCOPED(__func__);
}

void FlowControlManager::on_pause_frame_received(const PauseFrame& frame) noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.rx_pause_enabled) {
    return;
  }

  pause_timer_ = frame.pause_time;
  stats_.pause_frames_received += 1;
}

void FlowControlManager::tick(std::uint16_t quanta) noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (pause_timer_ > 0) {
    if (quanta >= pause_timer_) {
      stats_.total_paused_time_quanta += pause_timer_;
      pause_timer_ = 0;
    } else {
      pause_timer_ -= quanta;
      stats_.total_paused_time_quanta += quanta;
    }
  }
}

std::optional<PauseFrame> FlowControlManager::generate_pause_frame(
    std::uint16_t queue_depth) noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.tx_pause_enabled) {
    return std::nullopt;
  }

  // Generate pause frame if above threshold and not already sent
  if ((queue_depth >= config_.pause_threshold) && !pause_sent_) {
    pause_sent_ = true;
    stats_.pause_frames_sent += 1;
    return PauseFrame{.opcode = PauseFrame::kOpcodeClassicPause,
                      .pause_time = config_.default_pause_time};
  }

  // Clear pause if below resume threshold
  if ((queue_depth < config_.resume_threshold) && pause_sent_) {
    pause_sent_ = false;
    stats_.pause_frames_sent += 1;
    return PauseFrame{.opcode = PauseFrame::kOpcodeClassicPause, .pause_time = 0};  // Resume
  }

  return std::nullopt;
}

// PFCManager implementation

PFCManager::PFCManager(Config config) : config_(std::move(config)) {
  NIC_TRACE_SCOPED(__func__);
}

void PFCManager::on_pfc_frame_received(const PFCFrame& frame) noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.pfc_enabled) {
    return;
  }

  stats_.pfc_frames_received += 1;

  for (std::size_t i = 0; i < 8; ++i) {
    if (!config_.priority_enabled[i]) {
      continue;
    }

    if ((frame.enabled_priorities & (1 << i)) != 0) {
      pause_timers_[i] = frame.pause_times[i];
    }
  }
}

bool PFCManager::is_priority_paused(std::uint8_t priority) const noexcept {
  if ((priority >= 8) || !config_.priority_enabled[priority]) {
    return false;
  }

  return pause_timers_[priority] > 0;
}

void PFCManager::tick(std::uint16_t quanta) noexcept {
  NIC_TRACE_SCOPED(__func__);

  for (std::size_t i = 0; i < 8; ++i) {
    if (pause_timers_[i] > 0) {
      if (quanta >= pause_timers_[i]) {
        stats_.per_priority_paused_time[i] += pause_timers_[i];
        pause_timers_[i] = 0;
      } else {
        pause_timers_[i] -= quanta;
        stats_.per_priority_paused_time[i] += quanta;
      }
    }
  }
}

std::optional<PFCFrame> PFCManager::generate_pfc_frame(
    const std::array<std::uint16_t, 8>& queue_depths) noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.pfc_enabled) {
    return std::nullopt;
  }

  PFCFrame frame;
  bool need_send = false;

  for (std::size_t i = 0; i < 8; ++i) {
    if (!config_.priority_enabled[i]) {
      continue;
    }

    // Trigger pause if above threshold
    if ((queue_depths[i] >= config_.pause_thresholds[i]) && !pause_sent_[i]) {
      frame.enabled_priorities |= (1 << i);
      frame.pause_times[i] = config_.default_pause_times[i];
      pause_sent_[i] = true;
      need_send = true;
    }
    // Clear pause if below threshold (using hysteresis - threshold/2)
    else if ((queue_depths[i] < (config_.pause_thresholds[i] / 2)) && pause_sent_[i]) {
      frame.enabled_priorities |= (1 << i);
      frame.pause_times[i] = 0;  // Resume
      pause_sent_[i] = false;
      need_send = true;
    }
  }

  if (need_send) {
    stats_.pfc_frames_sent += 1;
    return frame;
  }

  return std::nullopt;
}

// BackpressureMonitor implementation

BackpressureMonitor::BackpressureMonitor(Config config) : config_(std::move(config)) {
  NIC_TRACE_SCOPED(__func__);
}

void BackpressureMonitor::update_queue_depth(std::uint16_t depth) noexcept {
  NIC_TRACE_SCOPED(__func__);

  previous_depth_ = current_depth_;
  current_depth_ = depth;
  stats_.total_samples += 1;

  // Detect congestion state transitions
  bool is_congested = depth >= config_.congestion_threshold;
  if (is_congested && !was_congested_) {
    stats_.congestion_events += 1;
  }
  was_congested_ = is_congested;

  // Detect critical threshold
  if (depth >= config_.critical_threshold) {
    stats_.critical_events += 1;
  }

  // Reset HOL blocking if queue is draining
  if (depth < previous_depth_) {
    quanta_since_drain_ = 0;
    hol_blocked_ = false;
  }
}

void BackpressureMonitor::tick(std::uint16_t quanta) noexcept {
  NIC_TRACE_SCOPED(__func__);

  // Track time in congestion
  if (current_depth_ >= config_.congestion_threshold) {
    stats_.total_congested_time_quanta += quanta;
  }

  // Detect head-of-line blocking (queue not draining)
  if (config_.enable_head_of_line_detection) {
    if ((current_depth_ >= previous_depth_) && (current_depth_ > 0)) {
      quanta_since_drain_ += quanta;

      if ((quanta_since_drain_ >= config_.hol_timeout_quanta) && !hol_blocked_) {
        hol_blocked_ = true;
        stats_.hol_blocking_events += 1;
      }
    }
  }
}

BackpressureMonitor::CongestionLevel BackpressureMonitor::congestion_level() const noexcept {
  std::uint16_t threshold_25 = config_.queue_capacity / 4;
  std::uint16_t threshold_50 = config_.queue_capacity / 2;

  if (current_depth_ >= config_.critical_threshold) {
    return CongestionLevel::Critical;
  }
  if (current_depth_ >= config_.congestion_threshold) {
    return CongestionLevel::High;
  }
  if (current_depth_ >= threshold_50) {
    return CongestionLevel::Medium;
  }
  if (current_depth_ >= threshold_25) {
    return CongestionLevel::Low;
  }
  return CongestionLevel::None;
}

std::uint8_t BackpressureMonitor::queue_occupancy_percent() const noexcept {
  if (config_.queue_capacity == 0) {
    return 0;
  }

  std::uint32_t percent =
      (static_cast<std::uint32_t>(current_depth_) * 100) / config_.queue_capacity;
  return static_cast<std::uint8_t>(std::min(percent, 100u));
}

bool BackpressureMonitor::should_apply_backpressure() const noexcept {
  // Apply backpressure if we're congested or HOL blocked
  return current_depth_ >= config_.congestion_threshold || hol_blocked_;
}

std::uint16_t BackpressureMonitor::recommended_pause_time() const noexcept {
  // Scale pause time based on congestion level
  switch (congestion_level()) {
    case CongestionLevel::Critical:
      return 1000;  // Long pause
    case CongestionLevel::High:
      return 500;  // Medium pause
    case CongestionLevel::Medium:
      return 200;  // Short pause
    case CongestionLevel::Low:
      return 50;  // Very short pause
    case CongestionLevel::None:
    default:
      return 0;  // No pause
  }
}

// EEEManager implementation

EEEManager::EEEManager(Config config) : config_(std::move(config)) {
  NIC_TRACE_SCOPED(__func__);
}

void EEEManager::on_traffic_activity() noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.eee_enabled) {
    return;
  }

  idle_timer_ = 0;

  // Wake from LPI if needed
  if (state_ == PowerState::LPI) {
    stats_.wake_events += 1;
    transition_to(PowerState::WakeTransit);
  } else if (state_ == PowerState::SleepTransit) {
    // Cancel sleep transition
    transition_to(PowerState::Active);
  }
}

void EEEManager::on_idle_period(std::uint16_t idle_quanta) noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.eee_enabled || (state_ != PowerState::Active)) {
    return;
  }

  idle_timer_ += idle_quanta;

  // Enter sleep transition if idle long enough
  if (idle_timer_ >= config_.idle_threshold_quanta) {
    transition_to(PowerState::SleepTransit);
  }
}

void EEEManager::tick(std::uint16_t quanta) noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.eee_enabled) {
    stats_.total_active_time_quanta += quanta;
    return;
  }

  switch (state_) {
    case PowerState::Active:
      stats_.total_active_time_quanta += quanta;
      break;

    case PowerState::LPI:
      stats_.total_lpi_time_quanta += quanta;
      lpi_duration_ += quanta;
      break;

    case PowerState::WakeTransit:
      transition_timer_ += quanta;
      if (transition_timer_ >= config_.wake_time_quanta) {
        transition_to(PowerState::Active);
      }
      break;

    case PowerState::SleepTransit:
      transition_timer_ += quanta;
      if (transition_timer_ >= config_.sleep_time_quanta) {
        transition_to(PowerState::LPI);
      }
      break;
  }
}

bool EEEManager::can_transmit() const noexcept {
  return state_ == PowerState::Active;
}

std::uint8_t EEEManager::power_savings_percent() const noexcept {
  std::uint64_t total_time = stats_.total_active_time_quanta + stats_.total_lpi_time_quanta;
  if (total_time == 0) {
    return 0;
  }

  // LPI mode saves approximately 50% power (typical for EEE)
  std::uint64_t savings = (stats_.total_lpi_time_quanta * 50) / total_time;
  return static_cast<std::uint8_t>(std::min(savings, std::uint64_t{100}));
}

void EEEManager::transition_to(PowerState new_state) noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (state_ == new_state) {
    return;
  }

  // Track state transitions
  if (new_state == PowerState::LPI) {
    stats_.lpi_enter_count += 1;
    lpi_duration_ = 0;
  } else if (state_ == PowerState::LPI) {
    stats_.lpi_exit_count += 1;
  }

  state_ = new_state;
  transition_timer_ = 0;

  // Reset idle timer when entering active
  if (new_state == PowerState::Active) {
    idle_timer_ = 0;
  }
}
