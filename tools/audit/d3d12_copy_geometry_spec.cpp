// Host-native spec for the D3D12 copy-geometry boundary rules.
//
// Every value exercised here reaches DXMT straight from the application and
// then lands in a Metal blit descriptor, so the D3D12 spec's own statements
// about invalid input are what these assertions pin down:
//
//   * CopyTextureRegion: "The source box must be within the size of the source
//     resource... If you try and copy outside the destination resource or
//     specify a source box that is larger than the source resource, the
//     behavior of CopyTextureRegion is undefined... might result in incorrect
//     rendering, clipping, no copy, or even the removal of the rendering
//     device."  Validation lives in the debug layer only
//     (D3D12_MESSAGE_ID_COPYTEXTUREREGION_INVALIDSRCBOX / _EMPTYBOX /
//     _SRCREGIONOUTOFBOUNDS), never in the runtime or the driver, so a
//     translation layer has to defend itself.
//   * CopySubresourceRegion (same box semantics, stated explicitly there):
//     "An empty box results in a no-op. A box is empty if the top value is
//     greater than or equal to the bottom value, or the left value is greater
//     than or equal to the right value, or the front value is greater than or
//     equal to the back value."
//
// Half of the cases below are *legal* input asserted to be untouched. That is
// the point: the boundary checks must reject only what D3D12 already calls
// undefined, and must not clip, shrink or refuse anything an application is
// entitled to ask for.
//
// Built native (host) like d3d12_submission_model_spec.cpp. The functions under
// test are the real ones; only the logging sink, the winemetal device queries
// and dxmt::Buffer's refcount are link seams, none of which participate in the
// arithmetic being checked.

#include "winemetal.h"

#include "d3d12_copy_footprint.hpp"
#include "d3d12_indirect_topology.hpp"
#include "d3d12_subresource_geometry.hpp"
#include "d3d12_tile_copy_plan.hpp"
#include "dxmt_buffer.hpp"
#include "log/log.hpp"

#include <climits>
#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <vector>

namespace d3d12 = dxmt::d3d12;

// Every case here turns on 32-bit unsigned wraparound. If the host headers
// ever gave these a different type the spec would silently stop testing the
// thing it exists to test.
static_assert(std::is_unsigned_v<UINT> && sizeof(UINT) == 4,
              "the copy-geometry boundary rules assume a 32-bit unsigned UINT");
static_assert(std::is_unsigned_v<decltype(D3D12_BOX{}.left)>,
              "D3D12_BOX coordinates must be unsigned for these cases to bite");

// ---------------------------------------------------------------------------
// Link seams. The spec links the real d3d12_copy_footprint /
// d3d12_subresource_geometry / d3d12_tile_copy_plan / d3d12_indirect_topology
// translation units; these three symbols are the only things those objects
// reach for that would otherwise drag in Metal.
// ---------------------------------------------------------------------------
dxmt::Logger dxmt::Logger::s_instance("d3d12-copy-geometry-spec.log");

namespace dxmt {
void Buffer::incRef() {}
void Buffer::decRef() {}
void Allocation::incRef() {}
void Allocation::decRef() {}
} // namespace dxmt

WINEMETAL_API bool
MTLDevice_supportsFamily(obj_handle_t, enum WMTGPUFamily) {
  return false;
}
WINEMETAL_API bool
MTLDevice_supportsBCTextureCompression(obj_handle_t) {
  return false;
}
WINEMETAL_API void
NSObject_retain(obj_handle_t) {}

namespace {

[[noreturn]] void
Fail(const char *message) {
  std::cerr << "d3d12 copy geometry spec failed: " << message << '\n';
  std::abort();
}

void
Check(bool value, const char *message) {
  if (!value)
    Fail(message);
}

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

// d3d12::Resource is a pure interface, so the geometry helpers can be driven
// with nothing but a resource description, a tiling table and (for the
// reserved-buffer path) a backing dxmt::Buffer length.
class FakeResource final : public d3d12::Resource {
public:
  D3D12_RESOURCE_DESC desc = {};
  const d3d12::ResourceTiling *tiling = nullptr;
  dxmt::Buffer *buffer = nullptr;
  UINT64 heap_offset = 0;

  d3d12::ResourceKind GetKind() const override {
    return d3d12::ResourceKind::Committed;
  }
  bool IsReserved() const override { return tiling != nullptr; }
  bool IsReservedTexture() const override {
    return tiling != nullptr &&
           desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER;
  }
  bool UsesPlacementSparse() const override { return false; }
  const d3d12::ResourceTiling *GetTiling() const override { return tiling; }
  bool UpdateTileMapping(UINT, UINT, UINT, UINT, ID3D12Heap *, bool,
                         UINT64) override {
    return false;
  }
  bool UpdateTileMappingByIndex(UINT, ID3D12Heap *, bool, UINT64) override {
    return false;
  }
  // No heap mapping: the reserved-buffer path then falls back to the
  // resource's own backing buffer, which is what keeps this double small.
  bool GetTileMapping(UINT, UINT, UINT, UINT,
                      d3d12::ResourceTileMapping &) const override {
    return false;
  }
  bool GetTileMappingByIndex(UINT,
                             d3d12::ResourceTileMapping &) const override {
    return false;
  }
  const D3D12_RESOURCE_DESC &GetResourceDesc() const override { return desc; }
  IMTLD3D12Device *GetParentDevice() const override { return nullptr; }
  const D3D12_HEAP_PROPERTIES &GetResourceHeapProperties() const override {
    static const D3D12_HEAP_PROPERTIES properties = {};
    return properties;
  }
  D3D12_HEAP_FLAGS GetResourceHeapFlags() const override {
    return D3D12_HEAP_FLAG_NONE;
  }
  uint64_t GetDescriptorIdentity() const override { return 0; }
  bool HasLifetimeResidency() const override { return false; }
  uint64_t GetTileMappingGeneration() const override { return 0; }
  UINT64 GetHeapOffset() const override { return heap_offset; }
  D3D12_RESOURCE_STATES GetInitialState() const override {
    return D3D12_RESOURCE_STATE_COMMON;
  }
  D3D12_GPU_VIRTUAL_ADDRESS GetGpuVirtualAddress() const override { return 0; }
  dxmt::Buffer *GetBuffer() const override { return buffer; }
  dxmt::BufferAllocation *GetBufferAllocation() const override {
    return nullptr;
  }
  dxmt::Texture *GetTexture() const override { return nullptr; }
  dxmt::Texture *GetTexture(UINT) const override { return nullptr; }
  dxmt::TextureAllocation *GetTextureAllocation() const override {
    return nullptr;
  }
  dxmt::TextureAllocation *GetTextureAllocation(UINT) const override {
    return nullptr;
  }
  bool EnsureTextureAllocation(const char *) override { return false; }
  void AddPendingTimestampResolve(UINT64, UINT64, uint64_t) override {}
  bool CanDeferCpuQueryResolve() const override { return false; }
  bool AddPendingCpuQueryResolve(
      UINT64, UINT64, uint64_t,
      std::unique_ptr<d3d12::CpuQueryResolveTarget>) override {
    return false;
  }
  bool HasPendingCpuQueryResolves(UINT64, UINT64) override { return false; }
  bool MaterializePendingCpuQueryResolves(UINT64, UINT64,
                                          const char *) override {
    return false;
  }
  void SetPresentSourceView(dxmt::TextureViewKey) override {}
  dxmt::TextureViewKey GetPresentSourceView() const override { return {}; }
  ID3D12Resource *GetD3D12Resource() override { return nullptr; }
};

// PlanTextureTileCopies reads the linear side's length through the recorded
// ID3D12Resource, so the record needs a COM object rather than a d3d12::
// Resource. Never freed: AddRef/Release are inert and every instance is a
// stack object owned by the test.
class FakeD3D12Resource final : public ID3D12Resource {
public:
  D3D12_RESOURCE_DESC desc = {};

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {
    if (object)
      *object = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override {
    return E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT,
                                           const void *) override {
    return E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,
                                                    const IUnknown *) override {
    return E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_FAIL; }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **) override {
    return E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE Map(UINT, const D3D12_RANGE *, void **) override {
    return E_FAIL;
  }
  void STDMETHODCALLTYPE Unmap(UINT, const D3D12_RANGE *) override {}
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_RESOURCE_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_RESOURCE_DESC *out) override {
    *out = desc;
    return out;
  }
#else
  D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return desc; }
#endif
  D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE
  GetGPUVirtualAddress() override {
    return 0;
  }
  HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT, const D3D12_BOX *,
                                               const void *, UINT,
                                               UINT) override {
    return E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE ReadFromSubresource(void *, UINT, UINT, UINT,
                                                const D3D12_BOX *) override {
    return E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES *,
                                              D3D12_HEAP_FLAGS *) override {
    return E_FAIL;
  }
};

D3D12_RESOURCE_DESC
Texture2DDesc(UINT64 width, UINT height, UINT16 mip_levels,
              DXGI_FORMAT format) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = mip_levels;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  return desc;
}

D3D12_RESOURCE_DESC
BufferDesc(UINT64 width) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = width;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  return desc;
}

D3D12_BOX
Box(UINT left, UINT top, UINT front, UINT right, UINT bottom, UINT back) {
  D3D12_BOX box = {};
  box.left = left;
  box.top = top;
  box.front = front;
  box.right = right;
  box.bottom = bottom;
  box.back = back;
  return box;
}

bool
SizeEquals(const WMTSize &size, UINT64 width, UINT64 height, UINT64 depth) {
  return size.width == width && size.height == height && size.depth == depth;
}

// ---------------------------------------------------------------------------
// 1. D3D12_BOX handling in GetSubresourceSize
// ---------------------------------------------------------------------------

void
TestSubresourceSizeAcceptsLegalBoxesUnchanged() {
  FakeResource resource;
  resource.desc = Texture2DDesc(256, 128, 1, DXGI_FORMAT_R8G8B8A8_UNORM);

  Check(SizeEquals(d3d12::GetSubresourceSize(resource, 0, nullptr), 256, 128,
                   1),
        "a null box must still describe the whole subresource");

  const auto interior = Box(16, 32, 0, 80, 96, 1);
  Check(SizeEquals(d3d12::GetSubresourceSize(resource, 0, &interior), 64, 64,
                   1),
        "an interior box must be passed through exactly");

  const auto full = Box(0, 0, 0, 256, 128, 1);
  Check(SizeEquals(d3d12::GetSubresourceSize(resource, 0, &full), 256, 128, 1),
        "a box covering the whole subresource must be passed through exactly");

  const auto single_texel = Box(255, 127, 0, 256, 128, 1);
  Check(SizeEquals(d3d12::GetSubresourceSize(resource, 0, &single_texel), 1, 1,
                   1),
        "a one-texel box at the far corner is legal and must survive");

  // Mip 1 of the same resource: the box is clipped against the mip extent,
  // not against mip 0, so a legal mip-1 box has to come back untouched.
  FakeResource mipped;
  mipped.desc = Texture2DDesc(256, 128, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
  Check(SizeEquals(d3d12::GetSubresourceSize(mipped, 1, nullptr), 128, 64, 1),
        "mip 1 extent must halve");
  const auto mip_box = Box(0, 0, 0, 128, 64, 1);
  Check(SizeEquals(d3d12::GetSubresourceSize(mipped, 1, &mip_box), 128, 64, 1),
        "a box covering all of mip 1 must be passed through exactly");
}

void
TestSubresourceSizeRejectsEmptyAndInvertedBoxes() {
  FakeResource resource;
  resource.desc = Texture2DDesc(256, 128, 1, DXGI_FORMAT_R8G8B8A8_UNORM);

  // "A box is empty if ... the left value is greater than or equal to the
  // right value" - an empty box is a documented no-op, not a copy.
  const auto empty_x = Box(16, 0, 0, 16, 128, 1);
  Check(SizeEquals(d3d12::GetSubresourceSize(resource, 0, &empty_x), 0, 0, 0),
        "left == right must degenerate to an empty extent");

  const auto empty_y = Box(0, 64, 0, 256, 64, 1);
  Check(SizeEquals(d3d12::GetSubresourceSize(resource, 0, &empty_y), 0, 0, 0),
        "top == bottom must degenerate to an empty extent");

  const auto empty_z = Box(0, 0, 0, 256, 128, 0);
  Check(SizeEquals(d3d12::GetSubresourceSize(resource, 0, &empty_z), 0, 0, 0),
        "front == back must degenerate to an empty extent");

  // The regression that started all of this: right < left used to wrap the
  // UINT subtraction into a ~4e9 copy extent.
  const auto inverted_x = Box(80, 0, 0, 16, 128, 1);
  const auto inverted_size = d3d12::GetSubresourceSize(resource, 0, &inverted_x);
  Check(SizeEquals(inverted_size, 0, 0, 0),
        "an inverted box must not wrap into a huge extent");

  const auto inverted_y = Box(0, 96, 0, 256, 32, 1);
  Check(SizeEquals(d3d12::GetSubresourceSize(resource, 0, &inverted_y), 0, 0,
                   0),
        "an inverted box must not wrap on the Y axis");

  const auto inverted_z = Box(0, 0, 1, 256, 128, 0);
  Check(SizeEquals(d3d12::GetSubresourceSize(resource, 0, &inverted_z), 0, 0,
                   0),
        "an inverted box must not wrap on the Z axis");
}

void
TestSubresourceSizeClipsOutOfBoundsBoxes() {
  FakeResource resource;
  resource.desc = Texture2DDesc(256, 128, 1, DXGI_FORMAT_R8G8B8A8_UNORM);

  // Partially outside: D3D12 calls this undefined, and the outcome we pick is
  // the one that cannot run the blit past the subresource.
  const auto overhang = Box(128, 64, 0, 4096, 4096, 1);
  const auto clipped = d3d12::GetSubresourceSize(resource, 0, &overhang);
  Check(SizeEquals(clipped, 128, 64, 1),
        "an overhanging box must be clipped to the subresource");
  // The invariant the blit encoder depends on.
  Check(overhang.left + clipped.width <= resource.desc.Width &&
            overhang.top + clipped.height <= resource.desc.Height,
        "origin + clipped extent must stay inside the subresource");

  // Entirely outside: nothing left to copy.
  const auto beyond = Box(512, 0, 0, 1024, 128, 1);
  Check(SizeEquals(d3d12::GetSubresourceSize(resource, 0, &beyond), 0, 0, 0),
        "a box starting past the subresource must degenerate to empty");

  // A 2D subresource is one slice deep; a box claiming more must not hand the
  // blit encoder a depth its texture does not have.
  const auto deep = Box(0, 0, 0, 256, 128, 6);
  Check(SizeEquals(d3d12::GetSubresourceSize(resource, 0, &deep), 256, 128, 1),
        "box depth must be clipped to a 2D subresource's single slice");

  const auto huge = Box(0, 0, 0, UINT_MAX, UINT_MAX, UINT_MAX);
  Check(SizeEquals(d3d12::GetSubresourceSize(resource, 0, &huge), 256, 128, 1),
        "a box of UINT_MAX extents must clip rather than overflow");
}

// ---------------------------------------------------------------------------
// 2. Placed-footprint row layout
// ---------------------------------------------------------------------------

dxmt::MTL_DXGI_FORMAT_DESC
BlockCompressedFormatDesc() {
  dxmt::MTL_DXGI_FORMAT_DESC format = {};
  format.Flag = dxmt::MTL_DXGI_FORMAT_BC;
  format.BlockSize = 16;
  return format;
}

// 2^31, the largest row pitch that still fits a UINT. Spelled as a shift
// rather than a literal: 0x80000000 does not fit a signed int, and
// bugprone-narrowing-conversions flags the literal wherever it appears.
constexpr UINT64 kPitchTwoPow31 = UINT64(1) << 31;

// Takes 64-bit arguments so the overflow cases below can be written as the
// plain values they are, without a narrowing conversion at every call site.
D3D12_SUBRESOURCE_FOOTPRINT
Footprint(UINT64 row_pitch, UINT64 height) {
  D3D12_SUBRESOURCE_FOOTPRINT footprint = {};
  footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  footprint.Width = 1;
  footprint.Height = static_cast<UINT>(height);
  footprint.Depth = 1;
  footprint.RowPitch = static_cast<UINT>(row_pitch);
  return footprint;
}

void
TestPlacedFootprintAcceptsLegalLayouts() {
  const dxmt::MTL_DXGI_FORMAT_DESC plain = {};

  const auto typical = d3d12::ComputePlacedFootprintRowLayout(
      Footprint(1024, 64), false, plain);
  Check(typical.has_value(), "a typical footprint must be accepted");
  Check(typical->block_height == 1 && typical->row_count == 64 &&
            typical->image_pitch == 65536,
        "a typical footprint's row geometry must be unchanged");

  const auto compressed = d3d12::ComputePlacedFootprintRowLayout(
      Footprint(1024, 64), true, BlockCompressedFormatDesc());
  Check(compressed.has_value(),
        "a block-compressed footprint must be accepted");
  Check(compressed->block_height == 4 && compressed->row_count == 16 &&
            compressed->image_pitch == 16384,
        "block-compressed footprints must still count rows in blocks");

  // A zero height still describes one row, exactly as before.
  const auto degenerate =
      d3d12::ComputePlacedFootprintRowLayout(Footprint(256, 0), false, plain);
  Check(degenerate.has_value() && degenerate->row_count == 1 &&
            degenerate->image_pitch == 256,
        "a zero-height footprint must keep its single-row fallback");

  // The largest layout that still fits the Metal encoder's 32-bit pitch: one
  // below the rejection threshold, and it must not be refused.
  const auto at_limit = d3d12::ComputePlacedFootprintRowLayout(
      Footprint(kPitchTwoPow31, 1), false, plain);
  Check(at_limit.has_value() && at_limit->image_pitch == kPitchTwoPow31,
        "an image pitch of exactly 2^31 is representable and must be kept");

  const auto exact_max =
      d3d12::ComputePlacedFootprintRowLayout(Footprint(UINT_MAX, 1), false,
                                             plain);
  Check(exact_max.has_value() && exact_max->image_pitch == UINT_MAX,
        "an image pitch of exactly UINT_MAX must be kept");
}

void
TestPlacedFootprintRejectsOverflowingLayouts() {
  const dxmt::MTL_DXGI_FORMAT_DESC plain = {};

  // 0x80000000 * 2 == 2^32: the truncated product used to come out as 0.
  Check(!d3d12::ComputePlacedFootprintRowLayout(Footprint(kPitchTwoPow31, 2),
                                                false, plain),
        "an image pitch of exactly 2^32 must be rejected, not truncated to 0");

  Check(!d3d12::ComputePlacedFootprintRowLayout(Footprint(kPitchTwoPow31, 4),
                                                false, plain),
        "a wrapping image pitch must be rejected");

  Check(!d3d12::ComputePlacedFootprintRowLayout(Footprint(0x40000000u, 4),
                                                false, plain),
        "a wrapping image pitch must be rejected regardless of the factors");

  // Height + block_height - 1 used to wrap first, collapsing row_count to 1
  // and hiding the overflow entirely.
  Check(!d3d12::ComputePlacedFootprintRowLayout(Footprint(4, UINT_MAX), true,
                                                BlockCompressedFormatDesc()),
        "the block row-count rounding must not wrap before the pitch check");
}

// ---------------------------------------------------------------------------
// 3. Indirect argument sizes
// ---------------------------------------------------------------------------

D3D12_INDIRECT_ARGUMENT_DESC
ConstantArgument(UINT values) {
  D3D12_INDIRECT_ARGUMENT_DESC argument = {};
  argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
  argument.Constant.Num32BitValuesToSet = values;
  return argument;
}

D3D12_INDIRECT_ARGUMENT_DESC
TypedArgument(D3D12_INDIRECT_ARGUMENT_TYPE type) {
  D3D12_INDIRECT_ARGUMENT_DESC argument = {};
  argument.Type = type;
  return argument;
}

void
TestIndirectArgumentSizeKeepsLegalSignatures() {
  // The six fixed-size cases below repeat the production expression on
  // purpose, so read what they do and do not establish. They prove the switch
  // *dispatches* - routing DRAW through the DISPATCH arm changes 16 to 12 and
  // fails here. They do not prove the dispatch is *unique*:
  // D3D12_DRAW_ARGUMENTS, D3D12_VERTEX_BUFFER_VIEW and
  // D3D12_INDEX_BUFFER_VIEW are all 16 bytes, so swapping those three arms is
  // invisible to any assertion phrased over the return value - and invisible
  // to the rest of DXMT too, because the byte size is this function's entire
  // output. Asserting over field offsets instead would pin the Windows SDK's
  // struct layout rather than DXMT's behaviour, so the ambiguity is recorded
  // here rather than papered over.
  Check(d3d12::IndirectArgumentByteSize(
            TypedArgument(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)) ==
            sizeof(D3D12_DRAW_ARGUMENTS),
        "draw argument size must be unchanged");
  Check(d3d12::IndirectArgumentByteSize(
            TypedArgument(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED)) ==
            sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
        "indexed draw argument size must be unchanged");
  Check(d3d12::IndirectArgumentByteSize(
            TypedArgument(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH)) ==
            sizeof(D3D12_DISPATCH_ARGUMENTS),
        "dispatch argument size must be unchanged");
  Check(d3d12::IndirectArgumentByteSize(
            TypedArgument(D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW)) ==
            sizeof(D3D12_VERTEX_BUFFER_VIEW),
        "vertex buffer view argument size must be unchanged");
  Check(d3d12::IndirectArgumentByteSize(
            TypedArgument(D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW)) ==
            sizeof(D3D12_INDEX_BUFFER_VIEW),
        "index buffer view argument size must be unchanged");
  Check(d3d12::IndirectArgumentByteSize(TypedArgument(
            D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW)) ==
            sizeof(D3D12_GPU_VIRTUAL_ADDRESS),
        "CBV argument size must be unchanged");

  Check(d3d12::IndirectArgumentByteSize(ConstantArgument(1)) == 4,
        "one root constant must still be four bytes");
  // A root signature holds at most 64 DWORDs, so this is the largest root
  // constant payload an application can legally ask for.
  Check(d3d12::IndirectArgumentByteSize(ConstantArgument(64)) == 256,
        "a full root-constant payload must be unchanged");
  // One below the rejection threshold: still representable, still accepted.
  Check(d3d12::IndirectArgumentByteSize(ConstantArgument(0x3FFFFFFFu)) ==
            0xFFFFFFFCu,
        "the largest representable root-constant payload must be kept");
}

void
TestIndirectArgumentSizeRejectsOverflowingConstants() {
  // 0x40000000 * 4 == 2^32, which used to truncate to 0 anyway; the value
  // above it truncated to 4 while the consumer went on to memcpy ~4 GiB.
  Check(d3d12::IndirectArgumentByteSize(ConstantArgument(0x40000000u)) == 0,
        "a root-constant payload of exactly 2^32 bytes must be rejected");
  Check(d3d12::IndirectArgumentByteSize(ConstantArgument(0x40000001u)) == 0,
        "a truncating root-constant payload must be rejected, not reported "
        "as four bytes");
  Check(d3d12::IndirectArgumentByteSize(ConstantArgument(UINT_MAX)) == 0,
        "the largest root-constant payload must be rejected");

  // Pre-existing sentinels that must keep meaning "unsupported".
  Check(d3d12::IndirectArgumentByteSize(ConstantArgument(0)) == 0,
        "an empty root-constant payload must stay unsupported");
  Check(d3d12::IndirectArgumentByteSize(TypedArgument(
            static_cast<D3D12_INDIRECT_ARGUMENT_TYPE>(0x7fffffff))) == 0,
        "an unknown argument type must stay unsupported");
}

// ---------------------------------------------------------------------------
// 4. CopyTiles linear-buffer bounds
// ---------------------------------------------------------------------------

constexpr UINT64 kTileBytes = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;

d3d12::ResourceTiling
LinearTiling(UINT tiles_wide, UINT tiles_high) {
  d3d12::ResourceTiling tiling = {};
  tiling.total_tile_count = tiles_wide * tiles_high;
  tiling.tile_shape = {128, 128, 1};
  d3d12::SubresourceTiling subresource = {};
  subresource.width_in_tiles = tiles_wide;
  subresource.height_in_tiles = static_cast<UINT16>(tiles_high);
  subresource.depth_in_tiles = 1;
  subresource.start_tile_index = 0;
  tiling.subresources.push_back(subresource);
  return tiling;
}

d3d12::CopyTilesRecord
TileRecord(UINT num_tiles, UINT64 buffer_offset) {
  d3d12::CopyTilesRecord record = {};
  record.start = {};
  record.size.NumTiles = num_tiles;
  record.size.UseBox = FALSE;
  record.buffer_offset = buffer_offset;
  record.flags = D3D12_TILE_COPY_FLAG_NONE;
  return record;
}

void
TestBufferTileCopiesAcceptLegalRegions() {
  const auto tiling = LinearTiling(4, 1);
  auto *backing = new dxmt::Buffer(4 * kTileBytes, WMT::Device{});

  FakeResource tiled;
  tiled.desc = BufferDesc(4 * kTileBytes);
  tiled.tiling = &tiling;
  tiled.buffer = backing;

  FakeResource linear;
  linear.desc = BufferDesc(4 * kTileBytes);

  std::vector<d3d12::BufferTileCopy> copies;
  Check(d3d12::PlanBufferTileCopies(nullptr, TileRecord(2, 0), tiled, linear,
                                    copies),
        "a two-tile copy into a four-tile buffer must be planned");
  Check(copies.size() == 2, "a two-tile copy must emit two operations");
  Check(copies[0].linear_offset == 0 && copies[0].byte_count == kTileBytes,
        "the first tile must start at the recorded buffer offset");
  Check(copies[1].linear_offset == kTileBytes,
        "consecutive tiles must advance by one tile");

  // Exactly fits: the last tile ends on the final byte of the buffer.
  copies.clear();
  Check(d3d12::PlanBufferTileCopies(nullptr, TileRecord(2, 2 * kTileBytes),
                                    tiled, linear, copies),
        "a copy that ends exactly at the end of the buffer must be planned");
  Check(copies.size() == 2, "an exactly fitting copy must emit two operations");

  // A region holding no tiles is not a bounds problem, it is simply no work.
  copies.clear();
  Check(d3d12::PlanBufferTileCopies(nullptr, TileRecord(0, 0), tiled, linear,
                                    copies),
        "a zero-tile region must succeed");
  Check(copies.empty(), "a zero-tile region must emit no operations");
}

void
TestBufferTileCopiesRejectOutOfBoundsRegions() {
  const auto tiling = LinearTiling(4, 1);
  auto *backing = new dxmt::Buffer(4 * kTileBytes, WMT::Device{});

  FakeResource tiled;
  tiled.desc = BufferDesc(4 * kTileBytes);
  tiled.tiling = &tiling;
  tiled.buffer = backing;

  FakeResource linear;
  linear.desc = BufferDesc(4 * kTileBytes);

  std::vector<d3d12::BufferTileCopy> copies;
  Check(!d3d12::PlanBufferTileCopies(nullptr, TileRecord(2, 3 * kTileBytes),
                                     tiled, linear, copies),
        "a copy whose second tile leaves the buffer must be rejected");

  copies.clear();
  Check(!d3d12::PlanBufferTileCopies(nullptr, TileRecord(1, 4 * kTileBytes),
                                     tiled, linear, copies),
        "a start offset at the end of the buffer must be rejected");

  // The last tile would need a full 64 KiB but only part of it is backed.
  FakeResource short_linear;
  short_linear.desc = BufferDesc(2 * kTileBytes - 1);
  copies.clear();
  Check(!d3d12::PlanBufferTileCopies(nullptr, TileRecord(2, 0), tiled,
                                     short_linear, copies),
        "a partially backed final tile must be rejected");

  // A start offset near the top of the 64-bit range must not wrap into the
  // buffer once the per-tile stride is added.
  copies.clear();
  Check(!d3d12::PlanBufferTileCopies(
            nullptr, TileRecord(2, UINT64(0) - kTileBytes), tiled, linear,
            copies),
        "a start offset that would wrap the 64-bit sum must be rejected");
}

dxmt::MTL_DXGI_FORMAT_DESC
Rgba8FormatDesc() {
  dxmt::MTL_DXGI_FORMAT_DESC format = {};
  format.BytesPerTexel = 4;
  return format;
}

void
TestTextureTileCopiesAcceptLegalRegions() {
  const auto tiling = LinearTiling(2, 1);

  FakeResource tiled;
  tiled.desc = Texture2DDesc(256, 128, 1, DXGI_FORMAT_R8G8B8A8_UNORM);
  tiled.tiling = &tiling;

  FakeD3D12Resource linear;
  linear.desc = BufferDesc(2 * kTileBytes);

  auto record = TileRecord(2, 0);
  record.buffer = &linear;

  std::vector<d3d12::TextureTileCopy> ops;
  Check(d3d12::PlanTextureTileCopies(tiled, tiling, record, 0,
                                     Rgba8FormatDesc(), ops),
        "a two-tile texture copy into a two-tile buffer must be planned");
  Check(ops.size() == 2, "a two-tile texture copy must emit two operations");
  // 128 texels * 4 bytes = 512, already 256-aligned; 128 rows fill one tile.
  Check(ops[0].row_pitch == 512 && ops[0].image_pitch == 65536,
        "tile row geometry must be unchanged");
  Check(ops[0].buffer_offset == 0 && ops[1].buffer_offset == kTileBytes,
        "consecutive tiles must advance by one tile");

  // The heap offset is added on top and must not be counted against the
  // resource-relative bound.
  ops.clear();
  Check(d3d12::PlanTextureTileCopies(tiled, tiling, record, 4096,
                                     Rgba8FormatDesc(), ops),
        "a placed linear buffer must not have its heap offset double-counted");
  Check(ops.size() == 2 && ops[0].buffer_offset == 4096,
        "the heap offset must be added to every tile");

  // A region holding no tiles is simply no work.
  ops.clear();
  auto empty_record = TileRecord(0, 0);
  empty_record.buffer = &linear;
  Check(d3d12::PlanTextureTileCopies(tiled, tiling, empty_record, 0,
                                     Rgba8FormatDesc(), ops),
        "a zero-tile texture region must succeed");
  Check(ops.empty(), "a zero-tile texture region must emit no operations");
}

void
TestTextureTileCopiesRejectOutOfBoundsRegions() {
  const auto tiling = LinearTiling(2, 1);

  FakeResource tiled;
  tiled.desc = Texture2DDesc(256, 128, 1, DXGI_FORMAT_R8G8B8A8_UNORM);
  tiled.tiling = &tiling;

  // One tile short: the second tile's 64 KiB does not fit.
  FakeD3D12Resource undersized;
  undersized.desc = BufferDesc(100000);
  auto record = TileRecord(2, 0);
  record.buffer = &undersized;

  std::vector<d3d12::TextureTileCopy> ops;
  Check(!d3d12::PlanTextureTileCopies(tiled, tiling, record, 0,
                                      Rgba8FormatDesc(), ops),
        "a texture tile copy that leaves the linear buffer must be rejected");

  FakeD3D12Resource exact;
  exact.desc = BufferDesc(2 * kTileBytes);

  ops.clear();
  auto offset_record = TileRecord(2, kTileBytes);
  offset_record.buffer = &exact;
  Check(!d3d12::PlanTextureTileCopies(tiled, tiling, offset_record, 0,
                                      Rgba8FormatDesc(), ops),
        "a buffer offset that pushes the last tile out must be rejected");

  ops.clear();
  auto past_end = TileRecord(1, 2 * kTileBytes);
  past_end.buffer = &exact;
  Check(!d3d12::PlanTextureTileCopies(tiled, tiling, past_end, 0,
                                      Rgba8FormatDesc(), ops),
        "a start offset at the end of the linear buffer must be rejected");

  ops.clear();
  auto wrapping = TileRecord(2, UINT64(0) - kTileBytes);
  wrapping.buffer = &exact;
  Check(!d3d12::PlanTextureTileCopies(tiled, tiling, wrapping, 0,
                                      Rgba8FormatDesc(), ops),
        "a start offset that would wrap the 64-bit sum must be rejected");
}

} // namespace

int
main() {
  TestSubresourceSizeAcceptsLegalBoxesUnchanged();
  TestSubresourceSizeRejectsEmptyAndInvertedBoxes();
  TestSubresourceSizeClipsOutOfBoundsBoxes();
  TestPlacedFootprintAcceptsLegalLayouts();
  TestPlacedFootprintRejectsOverflowingLayouts();
  TestIndirectArgumentSizeKeepsLegalSignatures();
  TestIndirectArgumentSizeRejectsOverflowingConstants();
  TestBufferTileCopiesAcceptLegalRegions();
  TestBufferTileCopiesRejectOutOfBoundsRegions();
  TestTextureTileCopiesAcceptLegalRegions();
  TestTextureTileCopiesRejectOutOfBoundsRegions();
  return 0;
}
