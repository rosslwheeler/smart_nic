#include <cassert>

#include "nic/admin_queue.h"
#include "nic/error_injector.h"
#include "nic/log.h"
#include "nic/stats_collector.h"

using namespace nic;

int main() {
  // StatsCollector tests
  {
    StatsCollector stats;

    // Record TX packets on queue 0
    stats.record_tx_packet(0, 100);
    stats.record_tx_packet(0, 200);
    stats.record_tx_packet(0, 300);

    const auto& q0_stats = stats.queue_stats(0);
    assert(q0_stats.tx_packets.load() == 3);
    assert(q0_stats.tx_bytes.load() == 600);
    assert(q0_stats.tx_errors.load() == 0);

    // Record RX packets on queue 1
    stats.record_rx_packet(1, 512);
    stats.record_rx_packet(1, 1024);

    const auto& q1_stats = stats.queue_stats(1);
    assert(q1_stats.rx_packets.load() == 2);
    assert(q1_stats.rx_bytes.load() == 1536);

    // Record errors
    stats.record_error(0, StatsCollector::ErrorType::TxDMAError);
    stats.record_error(0, StatsCollector::ErrorType::TxChecksumError);
    assert(q0_stats.tx_errors.load() == 2);

    stats.record_error(1, StatsCollector::ErrorType::RxDroppedFull);
    assert(q1_stats.rx_errors.load() == 1);

    // Port stats (aggregated)
    auto port = stats.port_stats();
    assert(port.tx_packets == 3);
    assert(port.tx_bytes == 600);
    assert(port.tx_errors == 2);
    assert(port.rx_packets == 2);
    assert(port.rx_bytes == 1536);
    assert(port.rx_errors == 1);

    // VF stats
    stats.record_vf_tx_packet(0, 256);
    stats.record_vf_rx_packet(0, 512);
    stats.record_vf_mailbox_message(0);

    const auto& vf0_stats = stats.vf_stats(0);
    assert(vf0_stats.tx_packets.load() == 1);
    assert(vf0_stats.tx_bytes.load() == 256);
    assert(vf0_stats.rx_packets.load() == 1);
    assert(vf0_stats.rx_bytes.load() == 512);
    assert(vf0_stats.mailbox_messages.load() == 1);

    // Reset queue
    stats.reset_queue(0);
    const auto& q0_reset = stats.queue_stats(0);
    assert(q0_reset.tx_packets.load() == 0);
    assert(q0_reset.tx_bytes.load() == 0);
    assert(q0_reset.tx_errors.load() == 0);

    // Reset VF
    stats.reset_vf(0);
    const auto& vf0_reset = stats.vf_stats(0);
    assert(vf0_reset.tx_packets.load() == 0);

    // Reset all
    stats.reset_all();
    auto port_reset = stats.port_stats();
    assert(port_reset.tx_packets == 0);
    assert(port_reset.rx_packets == 0);
  }

  // ErrorInjector tests
  {
    ErrorInjector injector;

    // Configure immediate error injection
    ErrorInjector::ErrorConfig cfg1{
        .type = ErrorInjector::ErrorType::DMAReadFail,
        .target_queue = 0,
        .trigger_count = 0,  // Immediate
        .inject_count = 2,   // Inject twice
        .one_shot = true,
    };

    injector.configure(cfg1);

    // Should inject on first call
    assert(injector.should_inject(ErrorInjector::ErrorType::DMAReadFail, 0));

    // Should inject on second call
    assert(injector.should_inject(ErrorInjector::ErrorType::DMAReadFail, 0));

    // Should NOT inject on third call (reached limit)
    assert(!injector.should_inject(ErrorInjector::ErrorType::DMAReadFail, 0));

    // Configure delayed error injection
    ErrorInjector::ErrorConfig cfg2{
        .type = ErrorInjector::ErrorType::InvalidDescriptor,
        .target_queue = 1,
        .trigger_count = 5,  // After 5 operations
        .inject_count = 1,
        .one_shot = true,
    };

    injector.configure(cfg2);

    // First 5 calls should not inject
    for (int i = 0; i < 5; i++) {
      assert(!injector.should_inject(ErrorInjector::ErrorType::InvalidDescriptor, 1));
    }

    // 6th call should inject
    assert(injector.should_inject(ErrorInjector::ErrorType::InvalidDescriptor, 1));

    // 7th call should not inject (one-shot)
    assert(!injector.should_inject(ErrorInjector::ErrorType::InvalidDescriptor, 1));

    // Configure error for all queues
    ErrorInjector::ErrorConfig cfg3{
        .type = ErrorInjector::ErrorType::ChecksumError,
        .target_queue = 0xFFFF,  // All queues
        .trigger_count = 0,
        .inject_count = 1,
        .one_shot = false,  // Continuous
    };

    injector.configure(cfg3);

    // Should inject on any queue
    assert(injector.should_inject(ErrorInjector::ErrorType::ChecksumError, 0));
    assert(injector.should_inject(ErrorInjector::ErrorType::ChecksumError, 5));
    assert(injector.should_inject(ErrorInjector::ErrorType::ChecksumError, 10));

    // Disable all
    injector.disable_all();
    assert(!injector.should_inject(ErrorInjector::ErrorType::ChecksumError, 0));
  }

  // AdminQueue tests
  {
    AdminQueue admin;

    // Submit commands without handler
    AdminQueue::Command cmd1{
        .opcode = AdminQueue::AdminOpcode::GetStats,
        .flags = 0,
        .namespace_id = 0,
        .data = {1, 2, 3, 4},
    };

    std::uint16_t cmd1_id = admin.submit_command(cmd1);
    assert(admin.pending_count() == 1);

    AdminQueue::Command cmd2{
        .opcode = AdminQueue::AdminOpcode::ResetStats,
        .flags = 0,
        .namespace_id = 1,
    };

    std::uint16_t cmd2_id = admin.submit_command(cmd2);
    assert(admin.pending_count() == 2);
    assert(cmd2_id == cmd1_id + 1);

    // Process without handler (should return NotSupported)
    admin.process_commands();
    assert(admin.pending_count() == 0);
    assert(admin.completion_count() == 2);

    auto comp1 = admin.poll_completion();
    assert(comp1.has_value());
    assert(comp1->command_id == cmd1_id);
    assert(comp1->status == AdminQueue::StatusCode::NotSupported);

    auto comp2 = admin.poll_completion();
    assert(comp2.has_value());
    assert(comp2->command_id == cmd2_id);

    // No more completions
    auto comp3 = admin.poll_completion();
    assert(!comp3.has_value());

    // Register handler
    admin.register_handler([](const AdminQueue::Command& cmd, std::uint16_t cmd_id) {
      AdminQueue::Completion comp;
      comp.command_id = cmd_id;

      if (cmd.opcode == AdminQueue::AdminOpcode::GetStats) {
        comp.status = AdminQueue::StatusCode::Success;
        comp.result = 0x12345678;  // Mock stats value
      } else if (cmd.opcode == AdminQueue::AdminOpcode::ResetStats) {
        comp.status = AdminQueue::StatusCode::Success;
        comp.result = 0;
      } else {
        comp.status = AdminQueue::StatusCode::InvalidOpcode;
        comp.result = 0;
      }

      return comp;
    });

    // Submit and process with handler
    AdminQueue::Command cmd3{.opcode = AdminQueue::AdminOpcode::GetStats};
    std::uint16_t cmd3_id = admin.submit_command(cmd3);

    admin.process_commands();

    auto comp4 = admin.poll_completion();
    assert(comp4.has_value());
    assert(comp4->command_id == cmd3_id);
    assert(comp4->status == AdminQueue::StatusCode::Success);
    assert(comp4->result == 0x12345678);

    // Test invalid opcode handling
    AdminQueue::Command cmd4{.opcode = static_cast<AdminQueue::AdminOpcode>(0x9999)};
    [[maybe_unused]] auto cmd4_id = admin.submit_command(cmd4);
    admin.process_commands();

    auto comp5 = admin.poll_completion();
    assert(comp5.has_value());
    assert(comp5->status == AdminQueue::StatusCode::InvalidOpcode);
  }

  // LogController tests
  {
    auto& log_ctrl = LogController::instance();

    // Default level should be Info
    assert(log_ctrl.level() == LogLevel::Info);

    // Test level filtering
    assert(log_ctrl.is_enabled(LogLevel::Error));
    assert(log_ctrl.is_enabled(LogLevel::Warning));
    assert(log_ctrl.is_enabled(LogLevel::Info));
    assert(!log_ctrl.is_enabled(LogLevel::Debug));
    assert(!log_ctrl.is_enabled(LogLevel::Trace));

    // Change to Debug level
    log_ctrl.set_level(LogLevel::Debug);
    assert(log_ctrl.is_enabled(LogLevel::Debug));
    assert(!log_ctrl.is_enabled(LogLevel::Trace));

    // Change to Error level
    log_ctrl.set_level(LogLevel::Error);
    assert(log_ctrl.is_enabled(LogLevel::Error));
    assert(!log_ctrl.is_enabled(LogLevel::Warning));
    assert(!log_ctrl.is_enabled(LogLevel::Info));

    // Test log macros (should not crash)
    NIC_LOG_ERROR("Test error message");
    NIC_LOG_WARNING("Test warning message");
    NIC_LOG_INFO("Test info message");
    NIC_LOG_DEBUG("Test debug message");
    NIC_LOG_TRACE("Test trace message");

    // Test formatted log macros (should not crash)
    log_ctrl.set_level(LogLevel::Debug);
    NIC_LOGF_INFO("QP {} posted {} bytes", 5, 128);
    NIC_LOGF_ERROR("MR registration failed: pd={} addr={:#x} len={}", 1, 0x1000, 256);
    NIC_LOGF_WARNING("PSN mismatch: expected={} got={}", 42, 43);
    NIC_LOGF_DEBUG("DMA read: addr={:#x} size={}", 0x2000, 512);
    NIC_LOGF_TRACE("RSS hash={:#x} queue={}", 0xDEADBEEF, 3);

    // Test formatted macros with no extra args (just format string)
    NIC_LOGF_INFO("simple message with no args");

    // Reset to Info for other tests
    log_ctrl.set_level(LogLevel::Info);
  }

  return 0;
}
