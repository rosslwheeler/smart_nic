#include <cassert>
#include <chrono>
#include <client/TracyProfiler.hpp>
#include <cstdlib>
#include <thread>
#include <tracy/Tracy.hpp>

#include "nic/device.h"
#include "nic/trace.h"

static void WaitForTracyConnection();

int main() {
  NIC_TRACE_SCOPED(__func__);
  WaitForTracyConnection();

  nic::DeviceConfig config{};

  nic::Device device{config};

  assert(!device.is_initialized());
  assert(device.config().identity.vendor_id == config.identity.vendor_id);
  assert(device.config().identity.device_id == config.identity.device_id);

  device.reset();
  assert(device.is_initialized());

  return 0;
}

static void WaitForTracyConnection() {
  const char* wait_env = std::getenv("NIC_WAIT_FOR_TRACY");
  if (!wait_env || wait_env[0] == '\0' || wait_env[0] == '0') {
    return;
  }

  const auto timeout = std::chrono::seconds(2);
  const auto start = std::chrono::steady_clock::now();
  while (!tracy::GetProfiler().IsConnected()) {
    if (std::chrono::steady_clock::now() - start > timeout) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}
