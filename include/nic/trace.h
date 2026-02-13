#pragma once

#include <cstring>

#ifdef TRACY_ENABLE
#include <Tracy.hpp>
#endif

namespace nic::trace {

// Initializes tracing backend (Tracy when enabled). No-op if tracing is disabled.
void initialize();

// Sets a friendly thread name for trace viewers.
void set_thread_name(const char* name);

// Emits a Tracy message if enabled; otherwise no-op.
inline void message(const char* text) {
#ifdef TRACY_ENABLE
  TracyMessage(text, std::strlen(text));
#else
  (void) text;
#endif
}

}  // namespace nic::trace

#ifdef TRACY_ENABLE
#define NIC_TRACE_SCOPED(name) ZoneScopedN(name)
#define NIC_TRACE_FRAME_MARK() FrameMark
#else
#define NIC_TRACE_SCOPED(name) ((void) 0)
#define NIC_TRACE_FRAME_MARK() ((void) 0)
#endif
