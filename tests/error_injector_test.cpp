#include "nic/error_injector.h"

#include <cassert>

using namespace nic;

int main() {
  ErrorInjector injector;

  // No-op configure.
  injector.configure(ErrorInjector::ErrorConfig{.type = ErrorInjector::ErrorType::None});
  assert(injector.active_errors().empty());

  // One-shot error after trigger count.
  ErrorInjector::ErrorConfig one_shot_cfg{
      .type = ErrorInjector::ErrorType::DMAReadFail,
      .target_queue = 3,
      .trigger_count = 2,
      .inject_count = 1,
      .one_shot = true,
  };
  injector.configure(one_shot_cfg);
  assert(!injector.active_errors().empty());

  // Mismatched type and queue.
  assert(!injector.should_inject(ErrorInjector::ErrorType::DMAWriteFail, 3));
  assert(!injector.should_inject(ErrorInjector::ErrorType::DMAReadFail, 2));

  // Trigger count not reached.
  assert(!injector.should_inject(ErrorInjector::ErrorType::DMAReadFail, 3));
  assert(!injector.should_inject(ErrorInjector::ErrorType::DMAReadFail, 3));
  // Third matching operation triggers injection.
  assert(injector.should_inject(ErrorInjector::ErrorType::DMAReadFail, 3));
  // One-shot should disable further injections.
  assert(!injector.should_inject(ErrorInjector::ErrorType::DMAReadFail, 3));
  assert(injector.active_errors().empty());

  // Continuous error (no one-shot).
  ErrorInjector::ErrorConfig continuous_cfg{
      .type = ErrorInjector::ErrorType::Timeout,
      .target_queue = 0xFFFF,
      .trigger_count = 0,
      .inject_count = 0,
      .one_shot = false,
  };
  injector.configure(continuous_cfg);
  assert(!injector.active_errors().empty());
  assert(injector.should_inject(ErrorInjector::ErrorType::Timeout, 0));
  assert(injector.should_inject(ErrorInjector::ErrorType::Timeout, 1));

  // Disable all and ensure no active errors remain.
  injector.disable_all();
  assert(injector.active_errors().empty());

  return 0;
}
