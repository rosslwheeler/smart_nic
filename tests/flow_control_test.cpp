#include "nic/flow_control.h"

#include <cassert>

using namespace nic;

int main() {
  // Test pause frame detection and parsing
  std::vector<std::byte> pause_pkt(64, std::byte{0});
  pause_pkt[12] = std::byte{0x88};  // EtherType 0x8808
  pause_pkt[13] = std::byte{0x08};
  pause_pkt[14] = std::byte{0x00};  // Opcode 0x0001
  pause_pkt[15] = std::byte{0x01};
  pause_pkt[16] = std::byte{0x01};  // Pause time 0x0164 (356 quanta)
  pause_pkt[17] = std::byte{0x64};

  assert(PauseFrame::is_pause_frame(pause_pkt));

  auto parsed = PauseFrame::parse(pause_pkt);
  assert(parsed.has_value());
  assert(parsed->opcode == 0x0001);
  assert(parsed->pause_time == 356);

  // Test pause frame serialization
  PauseFrame frame{.opcode = 0x0001, .pause_time = 1000};
  auto serialized = frame.serialize();
  assert(serialized.size() == 64);
  assert(serialized[12] == std::byte{0x88});
  assert(serialized[13] == std::byte{0x08});

  auto reparsed = PauseFrame::parse(serialized);
  assert(reparsed.has_value());
  assert(reparsed->pause_time == 1000);

  // FlowControlManager tests
  FlowControlManager::Config fc_cfg{
      .rx_pause_enabled = true,
      .tx_pause_enabled = true,
      .pause_threshold = 100,
      .resume_threshold = 50,
      .default_pause_time = 200,
  };

  FlowControlManager fc{fc_cfg};
  assert(!fc.is_paused());

  // Receive pause frame
  PauseFrame rx_pause{.opcode = 0x0001, .pause_time = 50};
  fc.on_pause_frame_received(rx_pause);
  assert(fc.is_paused());
  assert(fc.remaining_pause_time() == 50);
  assert(fc.stats().pause_frames_received == 1);

  // Tick down pause timer
  fc.tick(10);
  assert(fc.is_paused());
  assert(fc.remaining_pause_time() == 40);

  fc.tick(40);
  assert(!fc.is_paused());
  assert(fc.remaining_pause_time() == 0);

  // Generate pause frame when queue is full
  auto gen_pause = fc.generate_pause_frame(150);  // Above threshold
  assert(gen_pause.has_value());
  assert(gen_pause->pause_time == 200);
  assert(fc.stats().pause_frames_sent == 1);

  // Don't generate duplicate pause
  auto no_pause = fc.generate_pause_frame(150);
  assert(!no_pause.has_value());

  // Generate resume when queue drains
  auto resume = fc.generate_pause_frame(30);  // Below resume threshold
  assert(resume.has_value());
  assert(resume->pause_time == 0);  // Resume signal
  assert(fc.stats().pause_frames_sent == 2);

  // PFC frame tests
  PFCFrame pfc_frame;
  pfc_frame.enabled_priorities = 0b00000101;  // Priorities 0 and 2
  pfc_frame.pause_times[0] = 100;
  pfc_frame.pause_times[2] = 200;

  auto pfc_serialized = pfc_frame.serialize();
  assert(pfc_serialized.size() == 64);

  auto pfc_parsed = PFCFrame::parse(pfc_serialized);
  assert(pfc_parsed.has_value());
  assert(pfc_parsed->enabled_priorities == 0b00000101);
  assert(pfc_parsed->pause_times[0] == 100);
  assert(pfc_parsed->pause_times[2] == 200);

  // PFCManager tests
  PFCManager::Config pfc_cfg;
  pfc_cfg.pfc_enabled = true;
  pfc_cfg.priority_enabled[0] = true;
  pfc_cfg.priority_enabled[2] = true;
  pfc_cfg.pause_thresholds[0] = 80;
  pfc_cfg.pause_thresholds[2] = 120;
  pfc_cfg.default_pause_times[0] = 150;
  pfc_cfg.default_pause_times[2] = 250;

  PFCManager pfc_mgr{pfc_cfg};

  // Receive PFC frame
  pfc_mgr.on_pfc_frame_received(pfc_frame);
  assert(pfc_mgr.is_priority_paused(0));
  assert(!pfc_mgr.is_priority_paused(1));  // Not enabled
  assert(pfc_mgr.is_priority_paused(2));

  // Tick timers
  pfc_mgr.tick(50);
  assert(pfc_mgr.is_priority_paused(0));  // 100 - 50 = 50 remaining
  assert(pfc_mgr.is_priority_paused(2));  // 200 - 50 = 150 remaining

  pfc_mgr.tick(60);
  assert(!pfc_mgr.is_priority_paused(0));  // Expired
  assert(pfc_mgr.is_priority_paused(2));   // 150 - 60 = 90 remaining

  pfc_mgr.tick(100);
  assert(!pfc_mgr.is_priority_paused(2));  // Expired

  // Generate PFC frame
  std::array<std::uint16_t, 8> depths = {100, 0, 150, 0, 0, 0, 0, 0};
  auto gen_pfc = pfc_mgr.generate_pfc_frame(depths);
  assert(gen_pfc.has_value());
  assert((gen_pfc->enabled_priorities & 0x01) != 0);  // Priority 0 paused
  assert((gen_pfc->enabled_priorities & 0x04) != 0);  // Priority 2 paused
  assert(gen_pfc->pause_times[0] == 150);
  assert(gen_pfc->pause_times[2] == 250);

  // BackpressureMonitor tests
  BackpressureMonitor::Config bp_cfg{
      .queue_capacity = 1024,
      .congestion_threshold = 768,  // 75%
      .critical_threshold = 921,    // 90%
      .enable_head_of_line_detection = true,
      .hol_timeout_quanta = 100,
  };

  BackpressureMonitor bp_mon{bp_cfg};

  // Initial state - no congestion
  bp_mon.update_queue_depth(100);
  assert(bp_mon.congestion_level() == BackpressureMonitor::CongestionLevel::None);
  assert(!bp_mon.should_apply_backpressure());
  assert(bp_mon.queue_occupancy_percent() == 9);  // 100/1024 ~= 9%
  assert(bp_mon.recommended_pause_time() == 0);

  // Low congestion (25-50%)
  bp_mon.update_queue_depth(300);
  assert(bp_mon.congestion_level() == BackpressureMonitor::CongestionLevel::Low);
  assert(bp_mon.queue_occupancy_percent() == 29);
  assert(!bp_mon.should_apply_backpressure());  // Below congestion threshold

  // Medium congestion (50-75%)
  bp_mon.update_queue_depth(600);
  assert(bp_mon.congestion_level() == BackpressureMonitor::CongestionLevel::Medium);
  assert(bp_mon.queue_occupancy_percent() == 58);
  assert(!bp_mon.should_apply_backpressure());  // Still below threshold

  // High congestion (75-90%)
  bp_mon.update_queue_depth(800);
  assert(bp_mon.congestion_level() == BackpressureMonitor::CongestionLevel::High);
  assert(bp_mon.queue_occupancy_percent() == 78);
  assert(bp_mon.should_apply_backpressure());  // Above congestion threshold
  assert(bp_mon.recommended_pause_time() == 500);
  assert(bp_mon.stats().congestion_events == 1);

  // Critical congestion (> 90%)
  bp_mon.update_queue_depth(950);
  assert(bp_mon.congestion_level() == BackpressureMonitor::CongestionLevel::Critical);
  assert(bp_mon.queue_occupancy_percent() == 92);
  assert(bp_mon.should_apply_backpressure());
  assert(bp_mon.recommended_pause_time() == 1000);
  assert(bp_mon.stats().critical_events >= 1);

  // Drain queue - should clear backpressure
  bp_mon.update_queue_depth(700);  // Below congestion threshold
  assert(!bp_mon.should_apply_backpressure());

  // Head-of-line blocking detection
  bp_mon.update_queue_depth(400);
  bp_mon.update_queue_depth(400);  // No drain
  assert(!bp_mon.has_head_of_line_blocking());

  bp_mon.tick(50);  // 50 quanta, still not HOL blocked
  assert(!bp_mon.has_head_of_line_blocking());

  bp_mon.update_queue_depth(400);  // Still no drain
  bp_mon.tick(60);                 // Total 110 quanta > timeout
  assert(bp_mon.has_head_of_line_blocking());
  assert(bp_mon.should_apply_backpressure());  // HOL triggers backpressure
  assert(bp_mon.stats().hol_blocking_events == 1);

  // Drain should clear HOL blocking
  bp_mon.update_queue_depth(300);  // Queue draining
  assert(!bp_mon.has_head_of_line_blocking());

  // Congestion time tracking
  BackpressureMonitor time_mon{bp_cfg};
  time_mon.update_queue_depth(800);  // Congested
  time_mon.tick(100);
  assert(time_mon.stats().total_congested_time_quanta == 100);

  time_mon.update_queue_depth(500);  // Not congested
  time_mon.tick(50);
  assert(time_mon.stats().total_congested_time_quanta == 100);  // Should not increase

  // EEEManager tests
  EEEManager::Config eee_cfg{
      .eee_enabled = true,
      .idle_threshold_quanta = 100,
      .wake_time_quanta = 50,
      .sleep_time_quanta = 20,
      .min_lpi_duration_quanta = 500,
  };

  EEEManager eee{eee_cfg};

  // Initial state - active
  assert(eee.power_state() == EEEManager::PowerState::Active);
  assert(!eee.is_in_low_power());
  assert(eee.can_transmit());

  // Tick while active
  eee.tick(50);
  assert(eee.power_state() == EEEManager::PowerState::Active);
  assert(eee.stats().total_active_time_quanta == 50);

  // Idle period triggers sleep transition
  eee.on_idle_period(100);
  assert(eee.power_state() == EEEManager::PowerState::SleepTransit);
  assert(!eee.can_transmit());  // Cannot transmit during transition

  // Complete sleep transition
  eee.tick(20);
  assert(eee.power_state() == EEEManager::PowerState::LPI);
  assert(eee.is_in_low_power());
  assert(!eee.can_transmit());
  assert(eee.stats().lpi_enter_count == 1);

  // Accumulate LPI time
  eee.tick(100);
  assert(eee.stats().total_lpi_time_quanta == 100);

  // Traffic wakes from LPI
  eee.on_traffic_activity();
  assert(eee.power_state() == EEEManager::PowerState::WakeTransit);
  assert(eee.stats().wake_events == 1);
  assert(eee.stats().lpi_exit_count == 1);

  // Complete wake transition
  eee.tick(50);
  assert(eee.power_state() == EEEManager::PowerState::Active);
  assert(eee.can_transmit());

  // Traffic during sleep transition cancels it
  EEEManager eee2{eee_cfg};
  eee2.on_idle_period(100);
  assert(eee2.power_state() == EEEManager::PowerState::SleepTransit);
  eee2.on_traffic_activity();
  assert(eee2.power_state() == EEEManager::PowerState::Active);

  // Power savings calculation
  EEEManager eee3{eee_cfg};
  eee3.tick(100);  // 100 active
  eee3.on_idle_period(100);
  eee3.tick(20);   // 20 sleep transition (counts as active)
  eee3.tick(100);  // 100 LPI
  // Total: 120 active, 100 LPI = 220 total
  // Savings: (100 * 50) / 220 = 22.7%
  // But sleep transition time is counted as active, so actual calculation may differ
  std::uint8_t savings = eee3.power_savings_percent();
  assert(savings >= 20 && savings <= 25);  // Reasonable range

  // Disabled EEE stays active
  EEEManager::Config disabled_cfg = eee_cfg;
  disabled_cfg.eee_enabled = false;
  EEEManager eee_disabled{disabled_cfg};
  eee_disabled.on_idle_period(1000);
  eee_disabled.tick(100);
  assert(eee_disabled.power_state() == EEEManager::PowerState::Active);
  assert(eee_disabled.power_savings_percent() == 0);

  // Pause frame invalid sizes and types
  std::vector<std::byte> short_pkt(10, std::byte{0});
  assert(!PauseFrame::is_pause_frame(short_pkt));
  assert(!PauseFrame::parse(short_pkt).has_value());

  std::vector<std::byte> wrong_ethertype(64, std::byte{0});
  wrong_ethertype[12] = std::byte{0x12};
  wrong_ethertype[13] = std::byte{0x34};
  assert(!PauseFrame::parse(wrong_ethertype).has_value());

  // PFC invalid frames
  std::vector<std::byte> short_pfc(20, std::byte{0});
  short_pfc[12] = std::byte{0x88};
  short_pfc[13] = std::byte{0x08};
  assert(!PFCFrame::parse(short_pfc).has_value());

  std::vector<std::byte> bad_opcode(64, std::byte{0});
  bad_opcode[12] = std::byte{0x88};
  bad_opcode[13] = std::byte{0x08};
  bad_opcode[14] = std::byte{0x00};
  bad_opcode[15] = std::byte{0x02};
  assert(!PFCFrame::parse(bad_opcode).has_value());

  // FlowControlManager disabled paths
  FlowControlManager::Config fc_disabled_cfg{
      .rx_pause_enabled = false,
      .tx_pause_enabled = false,
      .pause_threshold = 100,
      .resume_threshold = 50,
      .default_pause_time = 200,
  };
  FlowControlManager fc_disabled{fc_disabled_cfg};
  fc_disabled.on_pause_frame_received(rx_pause);
  assert(!fc_disabled.is_paused());
  assert(!fc_disabled.generate_pause_frame(200).has_value());

  fc.on_pause_frame_received(PauseFrame{.opcode = 0x0001, .pause_time = 5});
  fc.tick(10);
  assert(!fc.is_paused());

  // PFCManager disabled and invalid priority
  PFCManager::Config pfc_disabled_cfg{};
  PFCManager pfc_disabled{pfc_disabled_cfg};
  pfc_disabled.on_pfc_frame_received(pfc_frame);
  assert(!pfc_disabled.generate_pfc_frame(depths).has_value());
  assert(!pfc_disabled.is_priority_paused(9));

  // PFC hysteresis clear path
  PFCManager::Config pfc_clear_cfg{};
  pfc_clear_cfg.pfc_enabled = true;
  pfc_clear_cfg.priority_enabled[0] = true;
  pfc_clear_cfg.pause_thresholds[0] = 100;
  pfc_clear_cfg.default_pause_times[0] = 120;
  PFCManager pfc_clear{pfc_clear_cfg};
  std::array<std::uint16_t, 8> high_depths = {120, 0, 0, 0, 0, 0, 0, 0};
  auto first_pfc = pfc_clear.generate_pfc_frame(high_depths);
  assert(first_pfc.has_value());
  std::array<std::uint16_t, 8> low_depths = {10, 0, 0, 0, 0, 0, 0, 0};
  auto clear_pfc = pfc_clear.generate_pfc_frame(low_depths);
  assert(clear_pfc.has_value());
  assert(clear_pfc->pause_times[0] == 0);

  // BackpressureMonitor disabled HOL detection and zero capacity
  BackpressureMonitor::Config bp_no_hol = bp_cfg;
  bp_no_hol.enable_head_of_line_detection = false;
  BackpressureMonitor no_hol{bp_no_hol};
  no_hol.update_queue_depth(400);
  no_hol.tick(1000);
  assert(!no_hol.has_head_of_line_blocking());

  BackpressureMonitor::Config bp_zero_cap = bp_cfg;
  bp_zero_cap.queue_capacity = 0;
  BackpressureMonitor zero_cap{bp_zero_cap};
  zero_cap.update_queue_depth(100);
  assert(zero_cap.queue_occupancy_percent() == 0);

  return 0;
}
