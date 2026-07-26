#pragma once

#include "Metal.hpp"
#include "dxmt_occlusion_query.hpp"
#include <d3d12.h>
#include <cstdint>
#include <string>
#include <type_traits>

namespace dxmt::d3d12 {

struct DrawVisibilityRetirementWork final {
  Rc<VisibilityResultQuery> query;
  std::string kind;
  std::string pso;
  uint64_t d3d_sequence = 0;
  uint64_t record_serial = 0;
  uint32_t vertex_count = 0;
  uint32_t index_count = 0;
  uint32_t instance_count = 0;
};

class DiagnosticReadbackBuffer final {
public:
  DiagnosticReadbackBuffer() = default;
  DiagnosticReadbackBuffer(WMT::Buffer buffer, uint8_t *mapped) noexcept;
  DiagnosticReadbackBuffer(const DiagnosticReadbackBuffer &) = delete;
  DiagnosticReadbackBuffer &
  operator=(const DiagnosticReadbackBuffer &) = delete;
  DiagnosticReadbackBuffer(DiagnosticReadbackBuffer &&other) noexcept;
  DiagnosticReadbackBuffer &
  operator=(DiagnosticReadbackBuffer &&other) noexcept;
  ~DiagnosticReadbackBuffer() noexcept;

  [[nodiscard]] const uint8_t *mapped() const noexcept {
    return mapped_;
  }

private:
  void Reset() noexcept;

  WMT::Reference<WMT::Buffer> buffer_;
  uint8_t *mapped_ = nullptr;
};

struct IndexReadbackRetirementWork final {
  DiagnosticReadbackBuffer buffer;
  std::string key_prefix;
  std::string kind;
  uint64_t d3d_sequence = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint64_t size = 0;
  uint32_t start_index = 0;
  uint32_t index_count = 0;
  int32_t base_vertex = 0;
};

struct VertexReadbackRetirementWork final {
  DiagnosticReadbackBuffer buffer;
  std::string key_prefix;
  std::string kind;
  uint64_t d3d_sequence = 0;
  uint32_t slot = 0;
  uint32_t stride = 0;
  uint32_t view_size = 0;
  uint64_t resource_offset = 0;
  uint64_t vertex_offset = 0;
  uint64_t heap_offset = 0;
  uint64_t size = 0;
};

struct ConstantBufferReadbackRetirementWork final {
  DiagnosticReadbackBuffer buffer;
  std::string key_prefix;
  std::string kind;
  uint64_t d3d_sequence = 0;
  uint32_t root_index = 0;
  uint32_t slot = 0;
  uint32_t stage = 0;
  uint64_t address = 0;
  uint32_t declared_size = 0;
  uint64_t resource_offset = 0;
  uint64_t heap_offset = 0;
  uint64_t size = 0;
  bool log_readback = false;
  bool record_snapshot = false;
  uint64_t resource_object_id = 0;
  uint64_t draw_record_sequence = 0;
};

static_assert(
    std::is_nothrow_move_constructible_v<DiagnosticReadbackBuffer>);
static_assert(!std::is_copy_constructible_v<DiagnosticReadbackBuffer>);
static_assert(!std::is_copy_constructible_v<IndexReadbackRetirementWork>);
static_assert(!std::is_copy_constructible_v<VertexReadbackRetirementWork>);
static_assert(
    !std::is_copy_constructible_v<ConstantBufferReadbackRetirementWork>);

} // namespace dxmt::d3d12
