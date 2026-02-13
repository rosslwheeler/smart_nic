#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nic {

/// IEEE 802.3x Pause Frame and IEEE 802.1Qbb PFC Frame support
struct PauseFrame {
  static constexpr std::uint16_t kEtherType = 0x8808;
  static constexpr std::uint16_t kOpcodeClassicPause = 0x0001;
  static constexpr std::uint16_t kOpcodePFC = 0x0101;

  std::uint16_t opcode{kOpcodeClassicPause};
  std::uint16_t pause_time{0};  ///< Pause duration in 512-bit time units (quanta)

  /// Check if packet is a pause frame (EtherType 0x8808)
  [[nodiscard]] static bool is_pause_frame(std::span<const std::byte> packet) noexcept;

  /// Parse pause frame from packet
  [[nodiscard]] static std::optional<PauseFrame> parse(std::span<const std::byte> packet) noexcept;

  /// Serialize pause frame to packet bytes
  [[nodiscard]] std::vector<std::byte> serialize() const noexcept;
};

/// Priority Flow Control frame (per-priority pause)
struct PFCFrame {
  std::uint16_t opcode{PauseFrame::kOpcodePFC};
  std::uint8_t enabled_priorities{0};          ///< Bitmap of paused priorities (bit 0 = priority 0)
  std::array<std::uint16_t, 8> pause_times{};  ///< Per-priority pause duration in quanta

  /// Parse PFC frame from packet
  [[nodiscard]] static std::optional<PFCFrame> parse(std::span<const std::byte> packet) noexcept;

  /// Serialize PFC frame to packet bytes
  [[nodiscard]] std::vector<std::byte> serialize() const noexcept;
};

/// Manages IEEE 802.3x flow control (classic pause)
class FlowControlManager {
public:
  struct Config {
    bool rx_pause_enabled{false};         ///< Can we process received pause frames?
    bool tx_pause_enabled{false};         ///< Can we send pause frames?
    std::uint16_t pause_threshold{0};     ///< Queue depth to trigger pause
    std::uint16_t resume_threshold{0};    ///< Queue depth to resume
    std::uint16_t default_pause_time{0};  ///< Default pause duration in quanta
  };

  explicit FlowControlManager(Config config);

  /// Process received pause frame
  void on_pause_frame_received(const PauseFrame& frame) noexcept;

  /// Check if currently paused
  [[nodiscard]] bool is_paused() const noexcept { return pause_timer_ > 0; }

  /// Get remaining pause time
  [[nodiscard]] std::uint16_t remaining_pause_time() const noexcept { return pause_timer_; }

  /// Tick to decrement pause timer (call periodically)
  void tick(std::uint16_t quanta) noexcept;

  /// Generate pause frame based on queue depth
  [[nodiscard]] std::optional<PauseFrame> generate_pause_frame(std::uint16_t queue_depth) noexcept;

  /// Statistics
  struct Stats {
    std::uint64_t pause_frames_received{0};
    std::uint64_t pause_frames_sent{0};
    std::uint64_t total_paused_time_quanta{0};
  };

  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
  void reset_stats() noexcept { stats_ = Stats{}; }

  [[nodiscard]] const Config& config() const noexcept { return config_; }

private:
  Config config_;
  std::uint16_t pause_timer_{0};  ///< Remaining pause time in quanta
  Stats stats_{};
  bool pause_sent_{false};  ///< Track if we sent pause to avoid duplicate sends
};

/// Manages IEEE 802.1Qbb Priority Flow Control
class PFCManager {
public:
  struct Config {
    bool pfc_enabled{false};
    std::array<bool, 8> priority_enabled{};              ///< Which priorities support PFC
    std::array<std::uint16_t, 8> pause_thresholds{};     ///< Per-priority queue depth thresholds
    std::array<std::uint16_t, 8> default_pause_times{};  ///< Default pause durations
  };

  explicit PFCManager(Config config);

  /// Process received PFC frame
  void on_pfc_frame_received(const PFCFrame& frame) noexcept;

  /// Check if a specific priority is paused
  [[nodiscard]] bool is_priority_paused(std::uint8_t priority) const noexcept;

  /// Tick all priority timers
  void tick(std::uint16_t quanta) noexcept;

  /// Generate PFC frame based on per-priority queue depths
  [[nodiscard]] std::optional<PFCFrame> generate_pfc_frame(
      const std::array<std::uint16_t, 8>& queue_depths) noexcept;

  /// Statistics
  struct Stats {
    std::uint64_t pfc_frames_received{0};
    std::uint64_t pfc_frames_sent{0};
    std::array<std::uint64_t, 8> per_priority_paused_time{};
  };

  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
  void reset_stats() noexcept { stats_ = Stats{}; }

  [[nodiscard]] const Config& config() const noexcept { return config_; }

private:
  Config config_;
  std::array<std::uint16_t, 8> pause_timers_{};  ///< Per-priority pause timers
  std::array<bool, 8> pause_sent_{};             ///< Track sent state per priority
  Stats stats_{};
};

/// Monitor queue backpressure and congestion state
class BackpressureMonitor {
public:
  /// Congestion level based on queue depth
  enum class CongestionLevel {
    None,     ///< Queue depth < 25%
    Low,      ///< Queue depth 25-50%
    Medium,   ///< Queue depth 50-75%
    High,     ///< Queue depth 75-90%
    Critical  ///< Queue depth > 90%
  };

  struct Config {
    std::uint16_t queue_capacity{1024};        ///< Total queue capacity
    std::uint16_t congestion_threshold{768};   ///< Depth to trigger congestion (75%)
    std::uint16_t critical_threshold{921};     ///< Depth to trigger critical state (90%)
    bool enable_head_of_line_detection{true};  ///< Detect head-of-line blocking
    std::uint16_t hol_timeout_quanta{1000};    ///< Time without drain = HOL blocking
  };

  explicit BackpressureMonitor(Config config);

  /// Update queue depth measurement
  void update_queue_depth(std::uint16_t depth) noexcept;

  /// Tick time forward (for HOL blocking detection)
  void tick(std::uint16_t quanta) noexcept;

  /// Get current congestion level
  [[nodiscard]] CongestionLevel congestion_level() const noexcept;

  /// Check if head-of-line blocking detected
  [[nodiscard]] bool has_head_of_line_blocking() const noexcept { return hol_blocked_; }

  /// Get current queue occupancy percentage (0-100)
  [[nodiscard]] std::uint8_t queue_occupancy_percent() const noexcept;

  /// Check if backpressure should be applied
  [[nodiscard]] bool should_apply_backpressure() const noexcept;

  /// Get recommended pause time based on congestion
  [[nodiscard]] std::uint16_t recommended_pause_time() const noexcept;

  /// Statistics
  struct Stats {
    std::uint64_t total_samples{0};
    std::uint64_t congestion_events{0};            ///< Times we entered congestion
    std::uint64_t critical_events{0};              ///< Times we hit critical threshold
    std::uint64_t hol_blocking_events{0};          ///< Times HOL blocking detected
    std::uint64_t total_congested_time_quanta{0};  ///< Total time in congestion
  };

  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
  void reset_stats() noexcept { stats_ = Stats{}; }

  [[nodiscard]] const Config& config() const noexcept { return config_; }

private:
  Config config_;
  std::uint16_t current_depth_{0};
  std::uint16_t previous_depth_{0};
  std::uint16_t quanta_since_drain_{0};  ///< Time since last queue drain
  bool hol_blocked_{false};
  bool was_congested_{false};  ///< Track state transitions
  Stats stats_{};
};

/// Energy Efficient Ethernet (IEEE 802.3az) management
class EEEManager {
public:
  /// EEE operational state
  enum class PowerState {
    Active,       ///< Full power, transmitting/receiving
    LPI,          ///< Low Power Idle - link in power save mode
    WakeTransit,  ///< Transitioning from LPI to Active
    SleepTransit  ///< Transitioning from Active to LPI
  };

  struct Config {
    bool eee_enabled{false};                     ///< EEE feature enabled
    std::uint16_t idle_threshold_quanta{100};    ///< Idle time before entering LPI
    std::uint16_t wake_time_quanta{50};          ///< Time to wake from LPI
    std::uint16_t sleep_time_quanta{20};         ///< Time to enter LPI
    std::uint16_t min_lpi_duration_quanta{500};  ///< Minimum LPI duration to be worthwhile
  };

  explicit EEEManager(Config config);

  /// Notify of traffic activity (TX or RX)
  void on_traffic_activity() noexcept;

  /// Check if link is idle (no traffic)
  void on_idle_period(std::uint16_t idle_quanta) noexcept;

  /// Tick time forward
  void tick(std::uint16_t quanta) noexcept;

  /// Get current power state
  [[nodiscard]] PowerState power_state() const noexcept { return state_; }

  /// Check if currently in low power mode
  [[nodiscard]] bool is_in_low_power() const noexcept { return state_ == PowerState::LPI; }

  /// Check if can transmit (not in LPI or transitioning)
  [[nodiscard]] bool can_transmit() const noexcept;

  /// Get estimated power savings (arbitrary units 0-100)
  [[nodiscard]] std::uint8_t power_savings_percent() const noexcept;

  /// Statistics
  struct Stats {
    std::uint64_t lpi_enter_count{0};           ///< Times entered LPI
    std::uint64_t lpi_exit_count{0};            ///< Times exited LPI
    std::uint64_t total_lpi_time_quanta{0};     ///< Total time in LPI
    std::uint64_t total_active_time_quanta{0};  ///< Total time active
    std::uint64_t wake_events{0};               ///< Times woken from LPI
  };

  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
  void reset_stats() noexcept { stats_ = Stats{}; }

  [[nodiscard]] const Config& config() const noexcept { return config_; }

private:
  void transition_to(PowerState new_state) noexcept;

  Config config_;
  PowerState state_{PowerState::Active};
  std::uint16_t idle_timer_{0};        ///< Time idle in Active state
  std::uint16_t transition_timer_{0};  ///< Time in transition state
  std::uint16_t lpi_duration_{0};      ///< Current LPI session duration
  Stats stats_{};
};

}  // namespace nic