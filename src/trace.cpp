#include "nic/trace.h"

#include <Tracy.hpp>

namespace nic::trace {

void initialize() {
  NIC_TRACE_SCOPED(__func__);
  // Tracy requires no explicit startup when linked as a client;
  // placeholder for future init hooks.
}

void set_thread_name(const char* name) {
  NIC_TRACE_SCOPED(__func__);
  tracy::SetThreadName(name);
}

}  // namespace nic::trace
