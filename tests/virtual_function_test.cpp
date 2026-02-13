#define private public
#include "nic/virtual_function.h"
#undef private

#include <cassert>

using namespace nic;

int main() {
  // Basic VF creation and state management.
  VFConfig cfg{.vf_id = 1, .num_queues = 4, .num_vectors = 2, .enabled = false, .trust = false};
  VirtualFunction vf{cfg};

  assert(vf.state() == VFState::Disabled);
  assert(vf.config().vf_id == 1);
  assert(vf.config().num_queues == 4);
  assert(vf.config().num_vectors == 2);

  // Disable when already disabled is idempotent.
  assert(vf.disable());

  // Enable VF.
  assert(vf.enable());
  assert(vf.state() == VFState::Enabled);
  assert(vf.config().enabled);

  // Double enable is idempotent.
  assert(vf.enable());
  assert(vf.state() == VFState::Enabled);

  // Disable VF.
  assert(vf.disable());
  assert(vf.state() == VFState::Disabled);
  assert(!vf.config().enabled);

  // Reset VF.
  assert(vf.enable());
  assert(vf.reset());
  assert(vf.state() == VFState::Reset);
  assert(vf.stats().resets == 1);

  // FLR in progress should block state changes.
  vf.state_ = VFState::FLRInProgress;
  assert(!vf.enable());
  assert(!vf.disable());
  vf.state_ = VFState::Disabled;

  // Cannot enable/disable during FLR.
  // After reset completes, VF is in Reset state (not FLRInProgress), so operations should work.

  // Resource assignment.
  VFConfig cfg2{.vf_id = 2, .num_queues = 2, .num_vectors = 1};
  VirtualFunction vf2{cfg2};

  std::vector<std::uint16_t> queues = {10, 11};
  std::vector<std::uint16_t> vectors = {5};
  vf2.set_queue_ids(queues);
  vf2.set_vector_ids(vectors);

  auto assigned_queues = vf2.queue_ids();
  assert(assigned_queues.size() == 2);
  assert(assigned_queues[0] == 10);
  assert(assigned_queues[1] == 11);

  auto assigned_vectors = vf2.vector_ids();
  assert(assigned_vectors.size() == 1);
  assert(assigned_vectors[0] == 5);

  // Statistics tracking.
  VFConfig cfg3{.vf_id = 3, .num_queues = 1, .num_vectors = 1};
  VirtualFunction vf3{cfg3};

  vf3.record_tx_packet(100);
  vf3.record_tx_packet(200);
  vf3.record_rx_packet(150);
  vf3.record_tx_drop();
  vf3.record_rx_drop();
  vf3.record_mailbox_message();

  const auto& stats = vf3.stats();
  assert(stats.tx_packets == 2);
  assert(stats.tx_bytes == 300);
  assert(stats.rx_packets == 1);
  assert(stats.rx_bytes == 150);
  assert(stats.tx_drops == 1);
  assert(stats.rx_drops == 1);
  assert(stats.mailbox_messages == 1);

  // Reset statistics.
  vf3.reset_stats();
  assert(vf3.stats().tx_packets == 0);
  assert(vf3.stats().tx_bytes == 0);

  return 0;
}
