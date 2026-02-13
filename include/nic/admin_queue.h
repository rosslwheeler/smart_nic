#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>

namespace nic {

/// Firmware-style admin command/completion queue for out-of-band control
class AdminQueue {
public:
  /// Admin command opcodes
  enum class AdminOpcode : std::uint16_t {
    GetStats = 0x0001,     ///< Query statistics
    ResetStats = 0x0002,   ///< Reset counters
    SetFeature = 0x0010,   ///< Enable/configure feature
    GetFeature = 0x0011,   ///< Query feature config
    InjectError = 0x0020,  ///< Inject test error
    Reset = 0x00FF,        ///< Device/queue reset
  };

  /// Command status codes
  enum class StatusCode : std::uint16_t {
    Success = 0x0000,
    InvalidOpcode = 0x0001,
    InvalidParameter = 0x0002,
    NotSupported = 0x0003,
    InternalError = 0x00FF,
  };

  /// Admin command descriptor
  struct Command {
    AdminOpcode opcode{AdminOpcode::GetStats};
    std::uint16_t flags{0};
    std::uint32_t namespace_id{0};        ///< 0 = global, >0 = queue/VF specific
    std::array<std::uint32_t, 4> data{};  ///< Opcode-specific parameters
  };

  /// Completion descriptor
  struct Completion {
    std::uint32_t result{0};
    StatusCode status{StatusCode::Success};
    std::uint16_t command_id{0};
  };

  /// Command handler function type
  using CommandHandler = std::function<Completion(const Command&, std::uint16_t command_id)>;

  AdminQueue() = default;

  /// Register command handler
  void register_handler(CommandHandler handler) noexcept;

  /// Submit command, returns command ID
  [[nodiscard]] std::uint16_t submit_command(const Command& cmd) noexcept;

  /// Poll for completion
  [[nodiscard]] std::optional<Completion> poll_completion() noexcept;

  /// Process pending commands (called by device tick)
  void process_commands() noexcept;

  /// Get number of pending commands
  [[nodiscard]] std::size_t pending_count() const noexcept { return pending_commands_.size(); }

  /// Get number of completions ready
  [[nodiscard]] std::size_t completion_count() const noexcept { return completions_.size(); }

private:
  struct PendingCommand {
    Command cmd;
    std::uint16_t command_id;
  };

  std::queue<PendingCommand> pending_commands_;
  std::queue<Completion> completions_;
  std::uint16_t next_command_id_{0};
  CommandHandler handler_;
};

}  // namespace nic