#include "nic/dma_engine.h"

#include "nic/log.h"
#include "nic/trace.h"

using namespace nic;

DMAEngine::DMAEngine(HostMemory& memory) : memory_(memory) {
  NIC_TRACE_SCOPED(__func__);
}

DmaResult DMAEngine::read(HostAddress address, std::span<std::byte> buffer) {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryResult host_result = memory_.read(address, buffer);
  DmaResult result = map_result(host_result, DmaDirection::Read, buffer.size(), "dma_read");
  if (result.ok()) {
    counters_.read_ops += 1;
    counters_.bytes_read += result.bytes_processed;
  }
  return result;
}

DmaResult DMAEngine::write(HostAddress address, std::span<const std::byte> data) {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryResult host_result = memory_.write(address, data);
  DmaResult result = map_result(host_result, DmaDirection::Write, data.size(), "dma_write");
  if (result.ok()) {
    counters_.write_ops += 1;
    counters_.bytes_written += result.bytes_processed;
  }
  return result;
}

DmaResult DMAEngine::read_burst(HostAddress address,
                                std::span<std::byte> buffer,
                                std::size_t beat_bytes,
                                std::size_t stride_bytes) {
  NIC_TRACE_SCOPED(__func__);
  if ((beat_bytes == 0) || (stride_bytes == 0)) {
    trace_dma_error(DmaError::AlignmentError, "dma_read_burst_invalid_stride");
    counters_.errors += 1;
    return {DmaError::AlignmentError, 0, "invalid_stride"};
  }
  if (buffer.size() % beat_bytes != 0) {
    trace_dma_error(DmaError::AlignmentError, "dma_read_burst_partial_beat");
    counters_.errors += 1;
    return {DmaError::AlignmentError, 0, "partial_beat"};
  }

  std::size_t beats = buffer.size() / beat_bytes;
  counters_.burst_read_ops += 1;

  std::size_t total_bytes = 0;
  for (std::size_t beat_index = 0; beat_index < beats; ++beat_index) {
    std::span<std::byte> beat = buffer.subspan(beat_index * beat_bytes, beat_bytes);
    HostAddress beat_addr = address + (beat_index * stride_bytes);
    HostMemoryResult host_result = memory_.read(beat_addr, beat);
    DmaResult result = map_result(host_result, DmaDirection::Read, beat_bytes, "dma_read_burst");
    if (!result.ok()) {
      return result;
    }
    total_bytes += result.bytes_processed;
  }

  counters_.bytes_read += total_bytes;
  return {DmaError::None, total_bytes, nullptr};
}

DmaResult DMAEngine::write_burst(HostAddress address,
                                 std::span<const std::byte> data,
                                 std::size_t beat_bytes,
                                 std::size_t stride_bytes) {
  NIC_TRACE_SCOPED(__func__);
  if ((beat_bytes == 0) || (stride_bytes == 0)) {
    trace_dma_error(DmaError::AlignmentError, "dma_write_burst_invalid_stride");
    counters_.errors += 1;
    return {DmaError::AlignmentError, 0, "invalid_stride"};
  }
  if (data.size() % beat_bytes != 0) {
    trace_dma_error(DmaError::AlignmentError, "dma_write_burst_partial_beat");
    counters_.errors += 1;
    return {DmaError::AlignmentError, 0, "partial_beat"};
  }

  std::size_t beats = data.size() / beat_bytes;
  counters_.burst_write_ops += 1;

  std::size_t total_bytes = 0;
  for (std::size_t beat_index = 0; beat_index < beats; ++beat_index) {
    std::span<const std::byte> beat = data.subspan(beat_index * beat_bytes, beat_bytes);
    HostAddress beat_addr = address + (beat_index * stride_bytes);
    HostMemoryResult host_result = memory_.write(beat_addr, beat);
    DmaResult result = map_result(host_result, DmaDirection::Write, beat_bytes, "dma_write_burst");
    if (!result.ok()) {
      return result;
    }
    total_bytes += result.bytes_processed;
  }

  counters_.bytes_written += total_bytes;
  return {DmaError::None, total_bytes, nullptr};
}

DmaResult DMAEngine::transfer_sgl(const SglView& sgl,
                                  DmaDirection direction,
                                  std::span<std::byte> buffer) {
  NIC_TRACE_SCOPED(__func__);

  if (sgl.empty()) {
    trace_dma_error(DmaError::AccessError, "dma_sgl_empty");
    counters_.errors += 1;
    return {DmaError::AccessError, 0, "empty_sgl"};
  }

  std::size_t total_length = sgl.total_length();
  if (buffer.size() < total_length) {
    trace_dma_error(DmaError::AccessError, "dma_sgl_buffer_too_small");
    counters_.errors += 1;
    return {DmaError::AccessError, 0, "buffer_too_small"};
  }

  std::size_t processed = 0;
  for (const auto& entry : sgl.entries()) {
    if (entry.length == 0) {
      continue;
    }

    std::span<std::byte> beat_buffer = buffer.subspan(processed, entry.length);
    HostMemoryResult host_result{};
    const char* context = "dma_sgl_read";
    if (direction == DmaDirection::Read) {
      host_result = memory_.read(entry.address, beat_buffer);
    } else {
      host_result = memory_.write(entry.address, beat_buffer);
      context = "dma_sgl_write";
    }

    DmaResult result = map_result(host_result, direction, entry.length, context);
    if (!result.ok()) {
      return result;
    }

    processed += result.bytes_processed;
  }

  if (direction == DmaDirection::Read) {
    counters_.read_ops += 1;
    counters_.bytes_read += processed;
  } else {
    counters_.write_ops += 1;
    counters_.bytes_written += processed;
  }

  return {DmaError::None, processed, nullptr};
}

DmaResult DMAEngine::map_result(const HostMemoryResult& host_result,
                                DmaDirection direction,
                                std::size_t requested_bytes,
                                const char* context) {
  NIC_TRACE_SCOPED(__func__);
  if (host_result.ok()) {
    return {DmaError::None, requested_bytes, nullptr};
  }

  DmaError error = ToDmaError(host_result.error);
  trace_dma_error(error, context);
  counters_.errors += 1;
  NIC_LOGF_WARNING("DMA error: context={} err={}", context, static_cast<int>(error));
  if (direction == DmaDirection::Read) {
    // No byte accumulation on error; counters updated by caller when successful.
  } else {
    // No-op for symmetry.
  }
  return {error, host_result.bytes_processed, context};
}
