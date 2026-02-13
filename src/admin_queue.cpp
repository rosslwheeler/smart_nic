#include "nic/admin_queue.h"

#include "nic/log.h"
#include "nic/trace.h"

using namespace nic;

void AdminQueue::register_handler(CommandHandler handler) noexcept {
  NIC_TRACE_SCOPED(__func__);
  handler_ = std::move(handler);
}

std::uint16_t AdminQueue::submit_command(const Command& cmd) noexcept {
  NIC_TRACE_SCOPED(__func__);

  std::uint16_t command_id = next_command_id_++;

  PendingCommand pending{.cmd = cmd, .command_id = command_id};
  pending_commands_.push(std::move(pending));
  NIC_LOGF_DEBUG("admin cmd submitted: id={} opcode={}", command_id, static_cast<int>(cmd.opcode));

  return command_id;
}

std::optional<AdminQueue::Completion> AdminQueue::poll_completion() noexcept {
  if (completions_.empty()) {
    return std::nullopt;
  }

  Completion comp = completions_.front();
  completions_.pop();
  return comp;
}

void AdminQueue::process_commands() noexcept {
  NIC_TRACE_SCOPED(__func__);

  // Process up to 16 commands per tick to avoid starvation
  constexpr std::size_t max_per_tick = 16;
  std::size_t processed = 0;

  while (!pending_commands_.empty() && processed < max_per_tick) {
    PendingCommand pending = pending_commands_.front();
    pending_commands_.pop();

    Completion comp;

    if (handler_) {
      // Execute handler
      comp = handler_(pending.cmd, pending.command_id);
    } else {
      // No handler registered - return error
      comp.command_id = pending.command_id;
      comp.status = StatusCode::NotSupported;
      comp.result = 0;
    }

    NIC_LOGF_DEBUG(
        "admin cmd completed: id={} status={}", comp.command_id, static_cast<int>(comp.status));
    completions_.push(comp);
    processed++;
  }
}