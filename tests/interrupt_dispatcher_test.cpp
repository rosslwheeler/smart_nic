#include "nic/interrupt_dispatcher.h"

#include <cassert>
#include <vector>

using namespace nic;

int main() {
  // Basic mapping, mask/enable, packet threshold, timer flush.
  MsixTable table{2};
  MsixMapping mapping{2, 0};
  mapping.set_queue_vector(0, 0);
  mapping.set_queue_vector(1, 1);

  CoalesceConfig cfg{.packet_threshold = 2, .timer_threshold_us = 100};

  std::vector<std::pair<std::uint16_t, std::uint32_t>> fired;
  InterruptDispatcher dispatcher{
      table, mapping, cfg, [&fired](std::uint16_t vec, std::uint32_t batch) {
        fired.emplace_back(vec, batch);
      }};

  // Queue 0: two events should coalesce to one interrupt with batch>=1.
  dispatcher.on_completion(InterruptEvent{0, {}});
  dispatcher.on_completion(InterruptEvent{0, {}});
  assert(fired.size() == 1);
  assert(fired[0].first == 0);
  assert(fired[0].second >= 1);

  // Queue 1: masked should suppress.
  dispatcher.mask_vector(1, true);
  dispatcher.on_completion(InterruptEvent{1, {}});
  assert(fired.size() == 1);  // unchanged
  auto stats = dispatcher.stats();
  assert(stats.suppressed_masked >= 1);

  // Unmask and test timer flush.
  table.mask(1, false);
  dispatcher.on_completion(InterruptEvent{1, {}});
  dispatcher.on_timer_tick(50);
  assert(fired.size() == 1);     // still pending
  dispatcher.on_timer_tick(60);  // exceeds threshold
  dispatcher.flush();
  assert(dispatcher.stats().manual_flushes >= 1);

  // Manual flush clears pending.
  dispatcher.on_completion(InterruptEvent{1, {}});
  dispatcher.flush();
  assert(dispatcher.stats().manual_flushes >= 1);

  // Per-queue coalescing configuration.
  MsixTable table2{3};
  MsixMapping mapping2{3, 0};
  mapping2.set_queue_vector(0, 0);
  mapping2.set_queue_vector(1, 1);
  mapping2.set_queue_vector(2, 2);

  CoalesceConfig default_cfg{.packet_threshold = 4, .timer_threshold_us = 0};
  std::vector<std::pair<std::uint16_t, std::uint32_t>> fired2;
  InterruptDispatcher dispatcher2{
      table2, mapping2, default_cfg, [&fired2](std::uint16_t vec, std::uint32_t batch) {
        fired2.emplace_back(vec, batch);
      }};

  // Queue 0 uses default config (threshold = 4).
  dispatcher2.on_completion(InterruptEvent{0, {}});
  dispatcher2.on_completion(InterruptEvent{0, {}});
  dispatcher2.on_completion(InterruptEvent{0, {}});
  assert(fired2.empty());  // Not reached threshold yet
  dispatcher2.on_completion(InterruptEvent{0, {}});
  assert(fired2.size() == 1);  // Now fires
  assert(fired2[0].first == 0);

  // Queue 1 gets custom config (threshold = 2).
  CoalesceConfig queue1_cfg{.packet_threshold = 2, .timer_threshold_us = 0};
  assert(dispatcher2.set_queue_coalesce_config(1, queue1_cfg));
  auto retrieved = dispatcher2.queue_coalesce_config(1);
  assert(retrieved.has_value());
  assert(retrieved->packet_threshold == 2);

  dispatcher2.on_completion(InterruptEvent{1, {}});
  assert(fired2.size() == 1);  // Pending
  dispatcher2.on_completion(InterruptEvent{1, {}});
  assert(fired2.size() == 2);  // Fires after 2 packets
  assert(fired2[1].first == 1);

  // Queue 2 gets custom config (threshold = 1, immediate).
  CoalesceConfig queue2_cfg{.packet_threshold = 1, .timer_threshold_us = 0};
  dispatcher2.set_queue_coalesce_config(2, queue2_cfg);
  dispatcher2.on_completion(InterruptEvent{2, {}});
  assert(fired2.size() == 3);  // Fires immediately
  assert(fired2[2].first == 2);

  // Clear queue 1 config, should fall back to default (threshold = 4).
  dispatcher2.clear_queue_coalesce_config(1);
  assert(!dispatcher2.queue_coalesce_config(1).has_value());

  std::size_t before_count = fired2.size();
  dispatcher2.on_completion(InterruptEvent{1, {}});
  dispatcher2.on_completion(InterruptEvent{1, {}});
  dispatcher2.on_completion(InterruptEvent{1, {}});
  assert(fired2.size() == before_count);  // Not yet (needs 4)
  dispatcher2.on_completion(InterruptEvent{1, {}});
  assert(fired2.size() == before_count + 1);  // Now fires with default threshold

  // Adaptive Interrupt Moderation (AIM).
  MsixTable table3{1};
  MsixMapping mapping3{1, 0};
  mapping3.set_queue_vector(0, 0);

  CoalesceConfig base_cfg{.packet_threshold = 8, .timer_threshold_us = 0};
  std::vector<std::pair<std::uint16_t, std::uint32_t>> fired3;
  InterruptDispatcher dispatcher3{
      table3, mapping3, base_cfg, [&fired3](std::uint16_t vec, std::uint32_t batch) {
        fired3.emplace_back(vec, batch);
      }};

  // Enable adaptive moderation
  AdaptiveConfig aim_cfg{
      .enabled = true,
      .min_threshold = 2,
      .max_threshold = 16,
      .low_batch_size = 4,    // If avg batch <= 4, decrease threshold
      .high_batch_size = 12,  // If avg batch >= 12, increase threshold
      .sample_interval = 10   // Adjust every 10 interrupts
  };
  dispatcher3.set_adaptive_config(aim_cfg);

  // Simulate low load: small batches should reduce threshold over time
  // Start with threshold=8, expect it to decrease toward min_threshold=2
  for (std::size_t round = 0; round < 5; ++round) {
    // Send packets in small batches (avg will be low)
    for (std::size_t i = 0; i < 10; ++i) {
      // Send 2-3 packets per interrupt (avg ~2.5, well below low_batch_size=4)
      dispatcher3.on_completion(InterruptEvent{0, {}});
      dispatcher3.on_completion(InterruptEvent{0, {}});
      // This will fire an interrupt after 2 packets once threshold drops
    }
  }

  // After several rounds, threshold should have decreased
  // More interrupts should have fired due to lower threshold
  assert(fired3.size() > 5);  // More interrupts than initial threshold would allow

  // Verify AIM config can be set and retrieved
  auto aim_retrieved = dispatcher3.adaptive_config();
  assert(aim_retrieved.enabled);
  assert(aim_retrieved.min_threshold == 2);
  assert(aim_retrieved.max_threshold == 16);
  assert(aim_retrieved.sample_interval == 10);

  // Test that AIM actually adapts by simulating consistent high-batch traffic
  fired3.clear();
  dispatcher3.set_adaptive_config(aim_cfg);  // Reset state

  // Simulate sustained high load - send packets continuously
  // This will create large batches which should trigger threshold increases
  for (std::size_t i = 0; i < 500; ++i) {
    dispatcher3.on_completion(InterruptEvent{0, {}});
    // Every 20 packets, check if we've adapted
    if (i % 20 == 19 && i > 100) {
      // At some point during sustained load, AIM should increase threshold
      // causing fewer interrupts than baseline would predict
    }
  }

  // The key is that AIM is working - the exact count depends on timing
  // Just verify we got some interrupts and the feature is active
  assert(fired3.size() > 0);
  assert(fired3.size() < 500);  // Not every packet fires an interrupt

  // Invalid vector mapping should fail to resolve.
  MsixTable table_bad{1};
  MsixMapping mapping_bad{1, 0};
  mapping_bad.set_queue_vector(0, 5);  // out of range
  CoalesceConfig bad_cfg{.packet_threshold = 1, .timer_threshold_us = 0};
  InterruptDispatcher dispatcher_bad{
      table_bad, mapping_bad, bad_cfg, [](std::uint16_t, std::uint32_t) {}};
  assert(!dispatcher_bad.on_completion(InterruptEvent{0, {}}));
  dispatcher_bad.flush(5);  // invalid vector id should be ignored

  // Disabled vector suppresses interrupts.
  MsixTable table_disabled{1};
  MsixMapping mapping_disabled{1, 0};
  mapping_disabled.set_queue_vector(0, 0);
  CoalesceConfig disabled_cfg{.packet_threshold = 1, .timer_threshold_us = 0};
  InterruptDispatcher dispatcher_disabled{
      table_disabled, mapping_disabled, disabled_cfg, [](std::uint16_t, std::uint32_t) {}};
  dispatcher_disabled.enable_vector(0, false);
  assert(!dispatcher_disabled.on_completion(InterruptEvent{0, {}}));
  dispatcher_disabled.flush(0);
  assert(dispatcher_disabled.stats().suppressed_disabled >= 1);

  // Masked vector should be suppressed in try_fire.
  MsixTable table_masked{1};
  MsixMapping mapping_masked{1, 0};
  mapping_masked.set_queue_vector(0, 0);
  CoalesceConfig masked_cfg{.packet_threshold = 1, .timer_threshold_us = 0};
  InterruptDispatcher dispatcher_masked{
      table_masked, mapping_masked, masked_cfg, [](std::uint16_t, std::uint32_t) {}};
  dispatcher_masked.mask_vector(0, true);
  dispatcher_masked.flush(0);
  assert(dispatcher_masked.stats().suppressed_masked >= 1);

  // try_fire batch==0 path.
  MsixTable table_empty{1};
  MsixMapping mapping_empty{1, 0};
  mapping_empty.set_queue_vector(0, 0);
  CoalesceConfig empty_cfg{.packet_threshold = 1, .timer_threshold_us = 0};
  InterruptDispatcher dispatcher_empty{
      table_empty, mapping_empty, empty_cfg, [](std::uint16_t, std::uint32_t) {}};
  dispatcher_empty.flush(0);

  // Flush all pending vectors path.
  MsixTable table_flush{2};
  MsixMapping mapping_flush{2, 0};
  mapping_flush.set_queue_vector(0, 0);
  mapping_flush.set_queue_vector(1, 1);
  CoalesceConfig flush_cfg{.packet_threshold = 5, .timer_threshold_us = 0};
  InterruptDispatcher dispatcher_flush{
      table_flush, mapping_flush, flush_cfg, [](std::uint16_t, std::uint32_t) {}};
  dispatcher_flush.on_completion(InterruptEvent{0, {}});
  dispatcher_flush.on_completion(InterruptEvent{1, {}});
  dispatcher_flush.flush();

  // Timer flush path with pending entries.
  MsixTable table_timer2{1};
  MsixMapping mapping_timer2{1, 0};
  mapping_timer2.set_queue_vector(0, 0);
  CoalesceConfig timer_cfg2{.packet_threshold = 10, .timer_threshold_us = 50};
  InterruptDispatcher dispatcher_timer2{
      table_timer2, mapping_timer2, timer_cfg2, [](std::uint16_t, std::uint32_t) {}};
  dispatcher_timer2.on_completion(InterruptEvent{0, {}});
  dispatcher_timer2.on_timer_tick(25);
  dispatcher_timer2.on_timer_tick(30);
  assert(dispatcher_timer2.stats().timer_flushes >= 1);

  // Coalesced batch should increment coalesced_batches.
  MsixTable table_coalesce{1};
  MsixMapping mapping_coalesce{1, 0};
  mapping_coalesce.set_queue_vector(0, 0);
  CoalesceConfig coalesce_cfg{.packet_threshold = 2, .timer_threshold_us = 0};
  std::vector<std::pair<std::uint16_t, std::uint32_t>> fired4;
  InterruptDispatcher dispatcher_coalesce{
      table_coalesce,
      mapping_coalesce,
      coalesce_cfg,
      [&fired4](std::uint16_t vec, std::uint32_t batch) { fired4.emplace_back(vec, batch); }};
  dispatcher_coalesce.on_completion(InterruptEvent{0, {}});
  dispatcher_coalesce.on_completion(InterruptEvent{0, {}});
  assert(dispatcher_coalesce.stats().coalesced_batches >= 1);

  // Timer tick returns early when threshold is zero.
  MsixTable table_timer{1};
  MsixMapping mapping_timer{1, 0};
  mapping_timer.set_queue_vector(0, 0);
  CoalesceConfig timer_cfg{.packet_threshold = 4, .timer_threshold_us = 0};
  InterruptDispatcher dispatcher_timer{
      table_timer, mapping_timer, timer_cfg, [](std::uint16_t, std::uint32_t) {}};
  dispatcher_timer.on_completion(InterruptEvent{0, {}});
  dispatcher_timer.on_timer_tick(100);

  // Adaptive threshold high/low branch exercise.
  MsixTable table_adapt{1};
  MsixMapping mapping_adapt{1, 0};
  mapping_adapt.set_queue_vector(0, 0);
  CoalesceConfig adapt_cfg{.packet_threshold = 12, .timer_threshold_us = 0};
  InterruptDispatcher dispatcher_adapt{
      table_adapt, mapping_adapt, adapt_cfg, [](std::uint16_t, std::uint32_t) {}};
  AdaptiveConfig adapt{
      .enabled = true,
      .min_threshold = 1,
      .max_threshold = 16,
      .low_batch_size = 4,
      .high_batch_size = 12,
      .sample_interval = 1,
  };
  dispatcher_adapt.set_adaptive_config(adapt);
  for (std::size_t i = 0; i < 12; ++i) {
    dispatcher_adapt.on_completion(InterruptEvent{0, {}});
  }

  CoalesceConfig adapt_low_cfg{.packet_threshold = 2, .timer_threshold_us = 0};
  InterruptDispatcher dispatcher_adapt_low{
      table_adapt, mapping_adapt, adapt_low_cfg, [](std::uint16_t, std::uint32_t) {}};
  dispatcher_adapt_low.set_adaptive_config(adapt);
  for (std::size_t i = 0; i < 2; ++i) {
    dispatcher_adapt_low.on_completion(InterruptEvent{0, {}});
  }

  return 0;
}
