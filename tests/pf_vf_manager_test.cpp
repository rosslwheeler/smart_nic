#include "nic/pf_vf_manager.h"

#include <cassert>

using namespace nic;

int main() {
  // Basic PF/VF manager creation.
  PFConfig pf_cfg{.max_vfs = 4,
                  .total_queues = 32,
                  .total_vectors = 32,
                  .pf_reserved_queues = 4,
                  .pf_reserved_vectors = 4};
  PFVFManager manager{pf_cfg};

  assert(manager.num_active_vfs() == 0);
  assert(manager.available_queues() == 28);  // 32 - 4 reserved
  assert(manager.available_vectors() == 28);

  // Create VF with resources.
  VFConfig vf1_cfg{.vf_id = 1, .num_queues = 4, .num_vectors = 2, .enabled = false, .trust = false};
  assert(manager.create_vf(1, vf1_cfg));

  auto* vf1 = manager.vf(1);
  assert(vf1 != nullptr);
  assert(vf1->config().vf_id == 1);
  assert(vf1->queue_ids().size() == 4);
  assert(vf1->vector_ids().size() == 2);

  // Check resources were allocated.
  assert(manager.available_queues() == 24);   // 28 - 4
  assert(manager.available_vectors() == 26);  // 28 - 2

  // Enable VF.
  assert(manager.enable_vf(1));
  assert(vf1->state() == VFState::Enabled);
  assert(manager.num_active_vfs() == 1);

  // Create another VF.
  VFConfig vf2_cfg{.vf_id = 2, .num_queues = 2, .num_vectors = 1};
  assert(manager.create_vf(2, vf2_cfg));

  auto* vf2 = manager.vf(2);
  assert(vf2 != nullptr);
  assert(vf2->queue_ids().size() == 2);
  assert(vf2->vector_ids().size() == 1);

  assert(manager.available_queues() == 22);   // 24 - 2
  assert(manager.available_vectors() == 25);  // 26 - 1

  // Cannot create VF with existing ID.
  assert(!manager.create_vf(1, vf1_cfg));

  // Disable and destroy VF.
  assert(manager.disable_vf(1));
  assert(vf1->state() == VFState::Disabled);
  assert(manager.num_active_vfs() == 0);

  assert(manager.destroy_vf(1));
  assert(manager.vf(1) == nullptr);

  // Resources should be freed.
  assert(manager.available_queues() == 26);   // 22 + 4
  assert(manager.available_vectors() == 27);  // 25 + 2

  // Reset VF.
  assert(manager.enable_vf(2));
  assert(manager.reset_vf(2));
  assert(vf2->state() == VFState::Reset);
  assert(vf2->stats().resets == 1);

  // Resource exhaustion.
  PFConfig small_cfg{.max_vfs = 2,
                     .total_queues = 8,
                     .total_vectors = 8,
                     .pf_reserved_queues = 2,
                     .pf_reserved_vectors = 2};
  PFVFManager small_manager{small_cfg};

  VFConfig big_vf_cfg{.vf_id = 10, .num_queues = 10, .num_vectors = 10};
  assert(!small_manager.create_vf(10, big_vf_cfg));  // Not enough resources

  VFConfig ok_vf_cfg{.vf_id = 11, .num_queues = 3, .num_vectors = 3};
  assert(small_manager.create_vf(11, ok_vf_cfg));

  VFConfig another_vf_cfg{.vf_id = 12, .num_queues = 3, .num_vectors = 3};
  assert(small_manager.create_vf(12, another_vf_cfg));

  // Now we should be at capacity.
  assert(small_manager.available_queues() == 0);  // 6 - 3 - 3
  assert(small_manager.available_vectors() == 0);

  VFConfig no_room_cfg{.vf_id = 13, .num_queues = 1, .num_vectors = 1};
  assert(!small_manager.create_vf(13, no_room_cfg));

  // Max VFs limit.
  assert(small_manager.destroy_vf(11));
  assert(small_manager.destroy_vf(12));

  VFConfig vf20_cfg{.vf_id = 20, .num_queues = 1, .num_vectors = 1};
  VFConfig vf21_cfg{.vf_id = 21, .num_queues = 1, .num_vectors = 1};
  VFConfig vf22_cfg{.vf_id = 22, .num_queues = 1, .num_vectors = 1};

  assert(small_manager.create_vf(20, vf20_cfg));
  assert(small_manager.create_vf(21, vf21_cfg));
  assert(!small_manager.create_vf(22, vf22_cfg));  // max_vfs = 2

  // Invalid VF operations.
  assert(!small_manager.enable_vf(99));
  assert(!small_manager.disable_vf(99));
  assert(!small_manager.reset_vf(99));
  assert(!small_manager.destroy_vf(99));

  // Resource checks.
  assert(!small_manager.has_available_resources(7, 1));
  assert(small_manager.has_available_resources(1, 1));
  assert(!small_manager.has_available_resources(0, 1));
  assert(!small_manager.has_available_resources(1, 0));

  return 0;
}
