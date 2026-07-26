#pragma once

// Namespace-level replay pass types.
//
// These definitions used to be nested inside the anonymous-namespace class
// CommandQueueImpl (d3d12_command_queue_replay_types.inc). They are pure data /
// policy types that never mention the queue class, so hoisting them to
// dxmt::d3d12 lets the queue implementation be split into independently
// compiled translation units.

#include "Metal.hpp"
#include "dxmt_context.hpp"
#include "dxmt_deptrack.hpp"
#include "dxmt_texture.hpp"

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

struct ReplayRenderTargetAttachment {
  Rc<Texture> texture;
  TextureViewKey view = {};
  UINT slot = 0;
  UINT array_length = 1;
  UINT depth_plane = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  WMTPixelFormat format = WMTPixelFormatInvalid;
};

struct ReplayDepthStencilAttachment {
  Rc<Texture> texture;
  TextureViewKey view = {};
  UINT array_length = 1;
  uint32_t width = 0;
  uint32_t height = 0;
  WMTPixelFormat format = WMTPixelFormatInvalid;
  int depth_access = ResourceAccess::ReadWrite;
  int stencil_access = ResourceAccess::ReadWrite;
  // Bit 0 = depth plane is read-only, bit 1 = stencil plane, matching the
  // dsv_readonly_flags ArgumentEncodingContext::startRenderPass() takes.
  //
  // Derived from the DSV descriptor flags alone, deliberately *not* from
  // depth_access/stencil_access: those also read Read when the pipeline simply
  // has DepthWriteMask=ZERO, which is a property of one draw rather than of the
  // pass, and BuildRenderPassAttachments() is called with graphics=nullptr on
  // some paths.  The descriptor flags are a property of the binding, so they
  // stay constant for as long as the attachment set does.
  uint8_t dsv_readonly_flags = 0;
};

struct ReplayRenderPassAttachments {
  std::vector<ReplayRenderTargetAttachment> colors;
  std::optional<ReplayDepthStencilAttachment> depth_stencil;
};

struct CompiledRenderPassRecipe {
  ReplayRenderPassAttachments attachments;
  UINT render_target_count = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t array_length = 1;
  uint32_t sample_count = 1;
  WMTPixelFormat dsv_format = WMTPixelFormatInvalid;
  bool use_geometry = false;
  bool use_tessellation = false;
};

enum class ReplayGraphicsCommandKind : uint8_t {
  Draw,
  DrawIndexed,
  DrawIndirect,
  DrawIndexedIndirect,
  Barrier,
};

inline bool
ReplayGraphicsCommandKindIsIndexed(ReplayGraphicsCommandKind kind) {
  return kind == ReplayGraphicsCommandKind::DrawIndexed ||
         kind == ReplayGraphicsCommandKind::DrawIndexedIndirect;
}

inline bool
ReplayGraphicsCommandKindIsIndirect(ReplayGraphicsCommandKind kind) {
  return kind == ReplayGraphicsCommandKind::DrawIndirect ||
         kind == ReplayGraphicsCommandKind::DrawIndexedIndirect;
}

inline bool
ReplayGraphicsCommandKindIsRealWork(ReplayGraphicsCommandKind kind) {
  return kind != ReplayGraphicsCommandKind::Barrier;
}

enum class ReplayComputeCommandKind : uint8_t {
  Dispatch,
  DispatchIndirect,
  Barrier,
};

struct ReplayGraphicsPassPlan {
  uint32_t command_count = 0;
  uint32_t indexed_count = 0;
  uint32_t indirect_count = 0;
  uint32_t parallel_candidate_count = 0;
  uint32_t compiled_candidate_count = 0;
  uint32_t compiled_legacy_count = 0;
  uint32_t compiled_barrier_count = 0;
  uint32_t compiled_gs_ts_count = 0;
  uint32_t compiled_indirect_count = 0;
  uint32_t current_parallel_candidate_run = 0;
  uint32_t largest_parallel_candidate_run = 0;
  uint64_t argument_buffer_size = 0;
  bool all_parallel_candidates = true;
};

using ReplayEncoderArena = std::pmr::monotonic_buffer_resource;

template <typename Base, typename Implementation, typename... Args>
std::shared_ptr<Base> AllocateReplayEncoder(
    std::shared_ptr<ReplayEncoderArena> &arena, Args &&...args) {
  if (!arena)
    arena = std::make_shared<ReplayEncoderArena>(64 * 1024);
  std::pmr::polymorphic_allocator<Implementation> allocator(arena.get());
  return std::allocate_shared<Implementation>(
      allocator, std::forward<Args>(args)...);
}

template <typename Command>
struct ReplayCommandStorage final {
  ReplayCommandStorage() = default;
  ReplayCommandStorage(std::shared_ptr<ReplayEncoderArena> encoder_arena,
                       std::vector<Command> replay_commands) noexcept
      : arena(std::move(encoder_arena)),
        commands(std::move(replay_commands)) {}
  ReplayCommandStorage(const ReplayCommandStorage &) = delete;
  ReplayCommandStorage &operator=(const ReplayCommandStorage &) = delete;
  ReplayCommandStorage(ReplayCommandStorage &&) noexcept = default;
  ReplayCommandStorage &operator=(ReplayCommandStorage &&) noexcept =
      default;
  ~ReplayCommandStorage() noexcept = default;

  // The arena is declared before the command vector so shared encoder
  // control blocks are destroyed before their backing storage.
  std::shared_ptr<ReplayEncoderArena> arena;
  std::vector<Command> commands;
};

class ReplayBlitEncodeCommand {
public:
  virtual ~ReplayBlitEncodeCommand() noexcept = default;
  virtual void Encode(ArgumentEncodingContext &enc) = 0;
};

template <typename EncoderFn>
class ReplayOwnedBlitEncodeCommand final
    : public ReplayBlitEncodeCommand {
public:
  explicit ReplayOwnedBlitEncodeCommand(EncoderFn encoder) noexcept
      : encoder_(std::move(encoder)) {}

  void Encode(ArgumentEncodingContext &enc) override {
    encoder_(enc);
  }

private:
  EncoderFn encoder_;
};

struct ReplayBlitPassCommand {
  ReplayBlitPassCommand() = default;
  ReplayBlitPassCommand(const ReplayBlitPassCommand &) = delete;
  ReplayBlitPassCommand &
  operator=(const ReplayBlitPassCommand &) = delete;
  ReplayBlitPassCommand(ReplayBlitPassCommand &&) noexcept = default;
  ReplayBlitPassCommand &
  operator=(ReplayBlitPassCommand &&) noexcept = default;
  ~ReplayBlitPassCommand() noexcept = default;

  std::shared_ptr<ReplayBlitEncodeCommand> encoder;
  uint64_t d3d_sequence = 0;
};

static_assert(std::is_nothrow_move_constructible_v<ReplayBlitPassCommand>);
static_assert(!std::is_copy_constructible_v<ReplayBlitPassCommand>);
static_assert(std::is_nothrow_move_constructible_v<
              ReplayCommandStorage<ReplayBlitPassCommand>>);

struct ReplayBlitBatch {
  std::shared_ptr<ReplayEncoderArena> encoder_arena;
  std::vector<ReplayBlitPassCommand> commands;
  std::unordered_set<ID3D12Resource *> reads;
  std::unordered_set<ID3D12Resource *> writes;
};

} // namespace dxmt::d3d12
