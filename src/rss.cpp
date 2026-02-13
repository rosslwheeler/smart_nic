#include "nic/rss.h"

#include <algorithm>

#include "nic/log.h"

using namespace nic;

namespace {
constexpr std::uint8_t kDefaultToeplitzKey[] = {
    0x6D, 0x5A, 0x56, 0x6B, 0x65, 0x4E, 0x67, 0x6E, 0x67, 0x55,
    0x6A, 0x6B, 0x61, 0x4F, 0x6B, 0x65, 0x6F, 0x49, 0x4D, 0x42,
};
constexpr std::size_t kDefaultTableSize = 128;
}  // namespace

RssEngine::RssEngine() {
  NIC_TRACE_SCOPED(__func__);
  ensure_defaults();
}

RssEngine::RssEngine(RssConfig config) : config_(std::move(config)) {
  NIC_TRACE_SCOPED(__func__);
  ensure_defaults();
}

void RssEngine::set_key(std::vector<std::uint8_t> key) {
  NIC_TRACE_SCOPED(__func__);
  config_.key = std::move(key);
  if (config_.key.empty()) {
    config_.key.assign(std::begin(kDefaultToeplitzKey), std::end(kDefaultToeplitzKey));
  }
}

void RssEngine::set_table(std::vector<std::uint16_t> table) {
  NIC_TRACE_SCOPED(__func__);
  config_.table = std::move(table);
  if (config_.table.empty()) {
    config_.table.resize(kDefaultTableSize, 0);
  }
}

std::uint32_t RssEngine::hash(std::span<const std::uint8_t> data) const {
  NIC_TRACE_SCOPED(__func__);
  stats_.hashes += 1;
  return toeplitz_hash(std::span<const std::uint8_t>(config_.key), data);
}

std::optional<std::uint16_t> RssEngine::select_queue(std::span<const std::uint8_t> data) const {
  NIC_TRACE_SCOPED(__func__);
  if (config_.table.empty()) {
    return std::nullopt;
  }
  std::uint32_t h = hash(data);
  std::size_t idx = static_cast<std::size_t>(h % static_cast<std::uint32_t>(config_.table.size()));
  if (idx < stats_.queue_hits.size()) {
    stats_.queue_hits[idx] += 1;
  }
  NIC_LOGF_TRACE("RSS: hash={:#x} queue={}", h, config_.table[idx]);
  return config_.table[idx];
}

std::uint32_t RssEngine::toeplitz_hash(std::span<const std::uint8_t> key,
                                       std::span<const std::uint8_t> data) const {
  NIC_TRACE_SCOPED(__func__);
  if (key.empty() || data.empty()) {
    return 0;
  }

  const std::size_t key_bits = key.size() * 8;
  const std::size_t data_bits = data.size() * 8;
  std::uint32_t hash_value = 0;

  for (std::size_t bit = 0; bit < data_bits; ++bit) {
    std::size_t byte_idx = bit / 8;
    std::size_t bit_idx = 7 - (bit % 8);
    bool data_bit = ((data[byte_idx] >> bit_idx) & 0x01U) != 0;
    if (!data_bit) {
      continue;
    }

    std::uint32_t segment = 0;
    for (std::size_t k = 0; k < 32; ++k) {
      std::size_t key_bit = (bit + k) % key_bits;
      std::size_t key_byte_idx = key_bit / 8;
      std::size_t key_bit_idx = 7 - (key_bit % 8);
      bool key_bit_val = ((key[key_byte_idx] >> key_bit_idx) & 0x01U) != 0;
      segment = (segment << 1) | static_cast<std::uint32_t>(key_bit_val);
    }
    hash_value ^= segment;
  }

  return hash_value;
}

void RssEngine::ensure_defaults() {
  NIC_TRACE_SCOPED(__func__);
  if (config_.key.empty()) {
    config_.key.assign(std::begin(kDefaultToeplitzKey), std::end(kDefaultToeplitzKey));
  }
  if (config_.table.empty()) {
    config_.table.resize(kDefaultTableSize);
    for (std::size_t i = 0; i < config_.table.size(); ++i) {
      config_.table[i] = static_cast<std::uint16_t>(i % 1);  // placeholder: single queue default
    }
  }
  stats_.queue_hits.assign(config_.table.size(), 0);
}

void RssEngine::reset_stats() noexcept {
  NIC_TRACE_SCOPED(__func__);
  stats_.hashes = 0;
  stats_.queue_hits.assign(config_.table.size(), 0);
}
