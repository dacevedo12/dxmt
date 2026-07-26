#include <dxmt_test.hpp>

#include "d3d12_test_context.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using dxmt::test::ComPtr;
using dxmt::test::D3D12TestContext;

struct SparseTilingCase {
  D3D12_RESOURCE_DIMENSION dimension;
  UINT64 width;
  UINT height;
  UINT16 depth_or_array_size;
  UINT16 mip_levels;
  DXGI_FORMAT format;
  const char *name;
};

class SparseTilingMatrixSpec
    : public ::testing::TestWithParam<SparseTilingCase> {
protected:
  void SetUp() override {
    ASSERT_EQ(context_.Initialize(), S_OK);
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    ASSERT_EQ(context_.device()->CheckFeatureSupport(
                  D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)),
              S_OK);
    if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_1)
      GTEST_SKIP() << "Tiled resources are not supported";
  }

  D3D12TestContext context_;
};

TEST_P(SparseTilingMatrixSpec, ReportsCoherentResourceTiling) {
  const auto &test = GetParam();
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = test.dimension;
  desc.Width = test.width;
  desc.Height = test.height;
  desc.DepthOrArraySize = test.depth_or_array_size;
  desc.MipLevels = test.mip_levels;
  desc.Format = test.format;
  desc.SampleDesc.Count = 1;
  desc.Layout = test.dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                    ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                    : D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;

  ComPtr<ID3D12Resource> resource;
  ASSERT_EQ(context_.device()->CreateReservedResource(
                &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(resource.put())),
            S_OK);
  ASSERT_TRUE(resource);

  const UINT expected_subresources =
      test.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
          ? test.mip_levels
          : test.mip_levels * test.depth_or_array_size;
  UINT total_tiles = 0;
  D3D12_PACKED_MIP_INFO packed = {};
  D3D12_TILE_SHAPE shape = {};
  UINT subresource_count = expected_subresources;
  std::vector<D3D12_SUBRESOURCE_TILING> tilings(expected_subresources);
  context_.device()->GetResourceTiling(resource.get(), &total_tiles, &packed,
                                       &shape, &subresource_count, 0,
                                       tilings.data());

  EXPECT_GT(total_tiles, 0u);
  EXPECT_EQ(subresource_count, expected_subresources);
  EXPECT_GT(shape.WidthInTexels, 0u);
  EXPECT_GT(shape.HeightInTexels, 0u);
  EXPECT_GT(shape.DepthInTexels, 0u);
  EXPECT_LE(packed.NumStandardMips, test.mip_levels);
  EXPECT_LE(packed.NumPackedMips, test.mip_levels);
  if (test.dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
    EXPECT_EQ(packed.NumStandardMips + packed.NumPackedMips, test.mip_levels);
  } else {
    EXPECT_EQ(packed.NumStandardMips, 0u);
    EXPECT_EQ(packed.NumPackedMips, 0u);
  }

  UINT previous_start = 0;
  bool saw_standard = false;
  for (UINT index = 0; index < subresource_count; ++index) {
    const auto &tiling = tilings[index];
    if (tiling.StartTileIndexInOverallResource == D3D12_PACKED_TILE)
      continue;
    EXPECT_GT(tiling.WidthInTiles, 0u) << "subresource " << index;
    EXPECT_GT(tiling.HeightInTiles, 0u) << "subresource " << index;
    EXPECT_GT(tiling.DepthInTiles, 0u) << "subresource " << index;
    if (saw_standard) {
      EXPECT_GE(tiling.StartTileIndexInOverallResource, previous_start);
    }
    previous_start = tiling.StartTileIndexInOverallResource;
    saw_standard = true;
  }
  EXPECT_TRUE(saw_standard);
}

std::string
SparseTilingCaseName(const ::testing::TestParamInfo<SparseTilingCase> &info) {
  return info.param.name;
}

INSTANTIATE_TEST_SUITE_P(
    ResourceDimensions, SparseTilingMatrixSpec,
    ::testing::Values(
        SparseTilingCase{D3D12_RESOURCE_DIMENSION_BUFFER, 4ull << 16, 1, 1, 1,
                         DXGI_FORMAT_UNKNOWN, "Buffer"},
        SparseTilingCase{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 512, 256, 1, 1,
                         DXGI_FORMAT_R32_UINT, "Texture2D"},
        SparseTilingCase{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 512, 512, 4, 1,
                         DXGI_FORMAT_R32_UINT, "Texture2DArray"},
        SparseTilingCase{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 512, 512, 1, 10,
                         DXGI_FORMAT_R32_UINT, "MipmappedTexture2D"},
        SparseTilingCase{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1024, 1024, 1, 11,
                         DXGI_FORMAT_BC7_UNORM, "BlockCompressedTexture"},
        SparseTilingCase{D3D12_RESOURCE_DIMENSION_TEXTURE3D, 256, 128, 32, 8,
                         DXGI_FORMAT_R32_UINT, "Texture3D"}),
    SparseTilingCaseName);

class SparseMappingMatrixSpec : public ::testing::Test {
protected:
  static constexpr UINT64 kTileSize = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;

  void SetUp() override {
    ASSERT_EQ(context_.Initialize(), S_OK);
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    ASSERT_EQ(context_.device()->CheckFeatureSupport(
                  D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)),
              S_OK);
    if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_1)
      GTEST_SKIP() << "Tiled resources are not supported";
  }

  ComPtr<ID3D12Resource> CreateTexture(D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 512;
    desc.Height = 512;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    ComPtr<ID3D12Resource> resource;
    EXPECT_EQ(context_.device()->CreateReservedResource(
                  &desc, state, nullptr, IID_PPV_ARGS(resource.put())),
              S_OK);
    return resource;
  }

  ComPtr<ID3D12Heap> CreateBacking(UINT tile_count) {
    D3D12_HEAP_DESC desc = {};
    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.SizeInBytes = UINT64(tile_count) * kTileSize;
    desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    ComPtr<ID3D12Heap> heap;
    EXPECT_EQ(context_.device()->CreateHeap(&desc, IID_PPV_ARGS(heap.put())),
              S_OK);
    return heap;
  }

  ComPtr<ID3D12Resource> CreateBuffer(UINT tile_count,
                                     D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = UINT64(tile_count) * kTileSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    EXPECT_EQ(context_.device()->CreateReservedResource(
                  &desc, state, nullptr, IID_PPV_ARGS(resource.put())),
              S_OK);
    return resource;
  }

  ComPtr<ID3D12Heap> CreateBufferBacking(UINT tile_count) {
    D3D12_HEAP_DESC desc = {};
    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.SizeInBytes = UINT64(tile_count) * kTileSize;
    desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ComPtr<ID3D12Heap> heap;
    EXPECT_EQ(context_.device()->CreateHeap(&desc, IID_PPV_ARGS(heap.put())),
              S_OK);
    return heap;
  }

  void AliasingBarrier(ID3D12Resource *before, ID3D12Resource *after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    barrier.Aliasing.pResourceBefore = before;
    barrier.Aliasing.pResourceAfter = after;
    context_.list()->ResourceBarrier(1, &barrier);
  }

  void MapRange(ID3D12Resource *resource, ID3D12Heap *heap, UINT x,
                UINT tile_count, D3D12_TILE_RANGE_FLAGS flag,
                UINT heap_offset = 0) {
    D3D12_TILED_RESOURCE_COORDINATE coordinate = {x, 0, 0, 0};
    D3D12_TILE_REGION_SIZE region = {};
    region.NumTiles = tile_count;
    region.UseBox = TRUE;
    region.Width = tile_count;
    region.Height = 1;
    region.Depth = 1;
    context_.queue()->UpdateTileMappings(
        resource, 1, &coordinate, &region, heap, 1, &flag, &heap_offset,
        &tile_count, D3D12_TILE_MAPPING_FLAG_NONE);
  }

  void WriteTile(ID3D12Resource *resource, UINT x,
                 const std::vector<std::uint8_t> &bytes) {
    ASSERT_EQ(bytes.size(), kTileSize);
    auto upload =
        context_.CreateUploadBuffer(kTileSize, bytes.data(), bytes.size());
    ASSERT_TRUE(upload);
    D3D12_TILED_RESOURCE_COORDINATE coordinate = {x, 0, 0, 0};
    D3D12_TILE_REGION_SIZE region = {};
    region.NumTiles = 1;
    context_.list()->CopyTiles(
        resource, &coordinate, &region, upload.get(), 0,
        D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
    ASSERT_EQ(context_.ExecuteAndWait(), S_OK);
    ASSERT_EQ(context_.ResetCommandList(), S_OK);
  }

  std::vector<std::uint8_t> ReadTile(ID3D12Resource *resource, UINT x,
                                     D3D12_RESOURCE_STATES before) {
    auto output = context_.CreateBuffer(kTileSize, D3D12_HEAP_TYPE_DEFAULT,
                                        D3D12_RESOURCE_FLAG_NONE,
                                        D3D12_RESOURCE_STATE_COPY_DEST);
    EXPECT_TRUE(output);
    if (!output)
      return {};
    if (before != D3D12_RESOURCE_STATE_COPY_SOURCE)
      D3D12TestContext::Transition(context_.list(), resource, before,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TILED_RESOURCE_COORDINATE coordinate = {x, 0, 0, 0};
    D3D12_TILE_REGION_SIZE region = {};
    region.NumTiles = 1;
    context_.list()->CopyTiles(
        resource, &coordinate, &region, output.get(), 0,
        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    D3D12TestContext::Transition(context_.list(), output.get(),
                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                 D3D12_RESOURCE_STATE_COPY_SOURCE);
    std::vector<std::uint8_t> actual;
    EXPECT_EQ(context_.ReadbackBuffer(output.get(), kTileSize, &actual), S_OK);
    return actual;
  }

  D3D12TestContext context_;
};

TEST_F(SparseMappingMatrixSpec, PaginatesSubresourceTilings) {
  D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
  ASSERT_EQ(context_.device()->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)),
            S_OK);
  // D3D12_TILED_RESOURCES_TIER tops out at _3, so its value range is 0..3 and
  // static_cast<D3D12_TILED_RESOURCES_TIER>(4) is undefined -- clang rejects it
  // outright in a constant expression. Compare the underlying integer instead.
  constexpr int kTiledResourcesTier4 = 4;
  if (static_cast<int>(options.TiledResourcesTier) < kTiledResourcesTier4)
    GTEST_SKIP() << "Array textures with packed mips require tiled resources tier 4";
  D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support = {DXGI_FORMAT_R32_UINT};
  ASSERT_EQ(context_.device()->CheckFeatureSupport(
                D3D12_FEATURE_FORMAT_SUPPORT, &format_support,
                sizeof(format_support)),
            S_OK);
  if (!(format_support.Support2 & D3D12_FORMAT_SUPPORT2_TILED))
    GTEST_SKIP() << "R32_UINT reserved textures are not supported";

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 512;
  desc.Height = 512;
  desc.DepthOrArraySize = 4;
  desc.MipLevels = 4;
  desc.Format = DXGI_FORMAT_R32_UINT;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
  ComPtr<ID3D12Resource> resource;
  ASSERT_EQ(context_.device()->CreateReservedResource(
                &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(resource.put())),
            S_OK);

  std::array<D3D12_SUBRESOURCE_TILING, 3> page = {};
  UINT count = page.size();
  context_.device()->GetResourceTiling(resource.get(), nullptr, nullptr,
                                       nullptr, &count, 2, page.data());
  EXPECT_EQ(count, page.size());

  count = 7;
  context_.device()->GetResourceTiling(resource.get(), nullptr, nullptr,
                                       nullptr, &count, 15, page.data());
  EXPECT_EQ(count, 1u);

  count = 1;
  context_.device()->GetResourceTiling(resource.get(), nullptr, nullptr,
                                       nullptr, &count, 16, page.data());
  EXPECT_EQ(count, 0u);
}

TEST_F(SparseMappingMatrixSpec, ReuseSinglePhysicalTileAliasesVirtualTiles) {
  auto texture = CreateTexture(D3D12_RESOURCE_STATE_COPY_DEST);
  auto heap = CreateBacking(1);
  ASSERT_TRUE(texture);
  ASSERT_TRUE(heap);
  MapRange(texture.get(), heap.get(), 0, 2,
           D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE);

  std::vector<std::uint8_t> expected(kTileSize);
  for (UINT64 index = 0; index < expected.size(); ++index)
    expected[index] = static_cast<std::uint8_t>((index * 41 + 13) & 0xff);
  WriteTile(texture.get(), 0, expected);
  const auto actual =
      ReadTile(texture.get(), 1, D3D12_RESOURCE_STATE_COPY_DEST);
  EXPECT_EQ(actual, expected);
}

TEST_F(SparseMappingMatrixSpec,
       MapsDisjointFirstAndLastBufferTilesInSingleUpdate) {
  constexpr UINT tile_count = 8;
  auto buffer = CreateBuffer(tile_count, D3D12_RESOURCE_STATE_COPY_DEST);
  auto heap = CreateBufferBacking(2);
  ASSERT_TRUE(buffer);
  ASSERT_TRUE(heap);

  const std::array<D3D12_TILED_RESOURCE_COORDINATE, 2> coordinates = {
      D3D12_TILED_RESOURCE_COORDINATE{0, 0, 0, 0},
      D3D12_TILED_RESOURCE_COORDINATE{tile_count - 1, 0, 0, 0}};
  std::array<D3D12_TILE_REGION_SIZE, 2> regions = {};
  regions[0].NumTiles = 1;
  regions[1].NumTiles = 1;
  const std::array<D3D12_TILE_RANGE_FLAGS, 2> range_flags = {
      D3D12_TILE_RANGE_FLAG_NONE, D3D12_TILE_RANGE_FLAG_NONE};
  const std::array<UINT, 2> heap_offsets = {0, 1};
  const std::array<UINT, 2> range_counts = {1, 1};
  context_.queue()->UpdateTileMappings(
      buffer.get(), coordinates.size(), coordinates.data(), regions.data(),
      heap.get(), range_flags.size(), range_flags.data(), heap_offsets.data(),
      range_counts.data(), D3D12_TILE_MAPPING_FLAG_NONE);

  std::vector<std::uint8_t> first(kTileSize);
  std::vector<std::uint8_t> last(kTileSize);
  for (UINT64 i = 0; i < kTileSize; ++i) {
    first[i] = static_cast<std::uint8_t>((i * 11 + 3) & 0xff);
    last[i] = static_cast<std::uint8_t>((i * 47 + 19) & 0xff);
  }
  WriteTile(buffer.get(), 0, first);
  WriteTile(buffer.get(), tile_count - 1, last);
  EXPECT_EQ(ReadTile(buffer.get(), 0, D3D12_RESOURCE_STATE_COPY_DEST), first);
  ASSERT_EQ(context_.ResetCommandList(), S_OK);
  EXPECT_EQ(ReadTile(buffer.get(), tile_count - 1,
                     D3D12_RESOURCE_STATE_COPY_SOURCE),
            last);
}

TEST_F(SparseMappingMatrixSpec, MapsMoreThanThirtyTwoDisjointRanges) {
  constexpr UINT tile_count = 40;
  constexpr UINT observed_tile = tile_count - 1;
  auto target = CreateBuffer(tile_count, D3D12_RESOURCE_STATE_COPY_DEST);
  auto probe = CreateBuffer(tile_count, D3D12_RESOURCE_STATE_COPY_DEST);
  auto baseline = CreateBufferBacking(tile_count);
  auto replacement = CreateBufferBacking(tile_count);
  ASSERT_TRUE(target);
  ASSERT_TRUE(probe);
  ASSERT_TRUE(baseline);
  ASSERT_TRUE(replacement);
  MapRange(target.get(), baseline.get(), 0, tile_count,
           D3D12_TILE_RANGE_FLAG_NONE);
  MapRange(probe.get(), replacement.get(), 0, tile_count,
           D3D12_TILE_RANGE_FLAG_NONE);

  std::vector<std::uint8_t> initial(kTileSize, 0x2d);
  WriteTile(probe.get(), observed_tile, initial);
  D3D12TestContext::Transition(
      context_.list(), probe.get(), D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_STATE_COPY_SOURCE);

  std::array<D3D12_TILED_RESOURCE_COORDINATE, tile_count> coordinates = {};
  std::array<D3D12_TILE_RANGE_FLAGS, tile_count> range_flags = {};
  std::array<UINT, tile_count> heap_offsets = {};
  std::array<UINT, tile_count> range_counts = {};
  for (UINT i = 0; i < tile_count; ++i) {
    coordinates[i].X = i;
    range_flags[i] = D3D12_TILE_RANGE_FLAG_NONE;
    heap_offsets[i] = i;
    range_counts[i] = 1;
  }
  context_.queue()->UpdateTileMappings(
      target.get(), tile_count, coordinates.data(), nullptr,
      replacement.get(), tile_count, range_flags.data(), heap_offsets.data(),
      range_counts.data(), D3D12_TILE_MAPPING_FLAG_NONE);

  std::vector<std::uint8_t> expected(kTileSize);
  for (UINT64 i = 0; i < kTileSize; ++i)
    expected[i] = static_cast<std::uint8_t>((i * 67 + 31) & 0xff);
  WriteTile(target.get(), observed_tile, expected);
  AliasingBarrier(target.get(), probe.get());
  EXPECT_EQ(ReadTile(probe.get(), observed_tile,
                     D3D12_RESOURCE_STATE_COPY_SOURCE),
            expected);
}

TEST_F(SparseMappingMatrixSpec,
       RemapsTileAcrossHeapOffsetsWithoutLosingPhysicalContents) {
  auto buffer = CreateBuffer(1, D3D12_RESOURCE_STATE_COPY_DEST);
  auto heap = CreateBufferBacking(2);
  ASSERT_TRUE(buffer);
  ASSERT_TRUE(heap);

  std::vector<std::uint8_t> first(kTileSize);
  std::vector<std::uint8_t> second(kTileSize);
  for (UINT64 i = 0; i < kTileSize; ++i) {
    first[i] = static_cast<std::uint8_t>((i * 13 + 7) & 0xff);
    second[i] = static_cast<std::uint8_t>((i * 59 + 41) & 0xff);
  }
  MapRange(buffer.get(), heap.get(), 0, 1, D3D12_TILE_RANGE_FLAG_NONE, 0);
  WriteTile(buffer.get(), 0, first);
  MapRange(buffer.get(), heap.get(), 0, 1, D3D12_TILE_RANGE_FLAG_NONE, 1);
  WriteTile(buffer.get(), 0, second);

  MapRange(buffer.get(), heap.get(), 0, 1, D3D12_TILE_RANGE_FLAG_NONE, 0);
  EXPECT_EQ(ReadTile(buffer.get(), 0, D3D12_RESOURCE_STATE_COPY_DEST), first);
  ASSERT_EQ(context_.ResetCommandList(), S_OK);
  MapRange(buffer.get(), heap.get(), 0, 1, D3D12_TILE_RANGE_FLAG_NONE, 1);
  EXPECT_EQ(ReadTile(buffer.get(), 0, D3D12_RESOURCE_STATE_COPY_SOURCE),
            second);
}

TEST_F(SparseMappingMatrixSpec, SkipRangePreservesExistingMapping) {
  auto texture = CreateTexture(D3D12_RESOURCE_STATE_COPY_DEST);
  auto heap = CreateBacking(1);
  ASSERT_TRUE(texture);
  ASSERT_TRUE(heap);
  MapRange(texture.get(), heap.get(), 0, 1, D3D12_TILE_RANGE_FLAG_NONE);

  std::vector<std::uint8_t> expected(kTileSize);
  for (UINT64 index = 0; index < expected.size(); ++index)
    expected[index] = static_cast<std::uint8_t>((index * 17 + 29) & 0xff);
  WriteTile(texture.get(), 0, expected);
  MapRange(texture.get(), nullptr, 0, 1, D3D12_TILE_RANGE_FLAG_SKIP);
  const auto actual =
      ReadTile(texture.get(), 0, D3D12_RESOURCE_STATE_COPY_DEST);
  EXPECT_EQ(actual, expected);
}

TEST_F(SparseMappingMatrixSpec, RepeatedNullAndMapRangesRecover) {
  auto texture = CreateTexture(D3D12_RESOURCE_STATE_COPY_DEST);
  auto heap = CreateBacking(1);
  ASSERT_TRUE(texture);
  ASSERT_TRUE(heap);

  for (UINT iteration = 0; iteration < 64; ++iteration) {
    MapRange(texture.get(), heap.get(), 0, 1, D3D12_TILE_RANGE_FLAG_NONE);
    MapRange(texture.get(), nullptr, 0, 1, D3D12_TILE_RANGE_FLAG_NULL);
  }
  MapRange(texture.get(), heap.get(), 0, 1, D3D12_TILE_RANGE_FLAG_NONE);

  std::vector<std::uint8_t> expected(kTileSize);
  for (UINT64 index = 0; index < expected.size(); ++index)
    expected[index] = static_cast<std::uint8_t>((index * 73 + 5) & 0xff);
  WriteTile(texture.get(), 0, expected);
  const auto actual =
      ReadTile(texture.get(), 0, D3D12_RESOURCE_STATE_COPY_DEST);
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(context_.device()->GetDeviceRemovedReason(), S_OK);
}

TEST_F(SparseMappingMatrixSpec, CrossQueueMappingIsOrderedBeforeDirectUse) {
  auto texture = CreateTexture(D3D12_RESOURCE_STATE_COPY_DEST);
  auto heap = CreateBacking(1);
  ASSERT_TRUE(texture);
  ASSERT_TRUE(heap);

  D3D12_COMMAND_QUEUE_DESC queue_desc = {};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
  ComPtr<ID3D12CommandQueue> copy_queue;
  ASSERT_EQ(context_.device()->CreateCommandQueue(
                &queue_desc, IID_PPV_ARGS(copy_queue.put())),
            S_OK);
  ComPtr<ID3D12Fence> fence;
  ASSERT_EQ(context_.device()->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                           IID_PPV_ARGS(fence.put())),
            S_OK);

  const D3D12_TILED_RESOURCE_COORDINATE coordinate = {0, 0, 0, 0};
  D3D12_TILE_REGION_SIZE region = {};
  region.NumTiles = 1;
  region.UseBox = TRUE;
  region.Width = 1;
  region.Height = 1;
  region.Depth = 1;
  const D3D12_TILE_RANGE_FLAGS range_flag = D3D12_TILE_RANGE_FLAG_NONE;
  const UINT heap_offset = 0;
  const UINT tile_count = 1;
  copy_queue->UpdateTileMappings(texture.get(), 1, &coordinate, &region,
                                 heap.get(), 1, &range_flag, &heap_offset,
                                 &tile_count, D3D12_TILE_MAPPING_FLAG_NONE);
  ASSERT_EQ(copy_queue->Signal(fence.get(), 1), S_OK);
  ASSERT_EQ(context_.queue()->Wait(fence.get(), 1), S_OK);

  std::vector<std::uint8_t> expected(kTileSize);
  for (UINT64 index = 0; index < expected.size(); ++index)
    expected[index] = static_cast<std::uint8_t>((index * 31 + 7) & 0xff);
  WriteTile(texture.get(), 0, expected);
  const auto actual =
      ReadTile(texture.get(), 0, D3D12_RESOURCE_STATE_COPY_DEST);
  EXPECT_EQ(actual, expected);
}

TEST_F(SparseMappingMatrixSpec, MappedTileRemainsUsableWhileBackingHeapIsAlive) {
  auto texture = CreateTexture(D3D12_RESOURCE_STATE_COPY_DEST);
  auto heap = CreateBacking(1);
  ASSERT_TRUE(texture);
  ASSERT_TRUE(heap);
  MapRange(texture.get(), heap.get(), 0, 1, D3D12_TILE_RANGE_FLAG_NONE);
  std::vector<std::uint8_t> expected(kTileSize, 0x5a);
  WriteTile(texture.get(), 0, expected);
  const auto actual =
      ReadTile(texture.get(), 0, D3D12_RESOURCE_STATE_COPY_DEST);
  EXPECT_EQ(actual, expected);
}

TEST_F(SparseMappingMatrixSpec, MultiPlaneTilingMatchesFormatCapability) {
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {DXGI_FORMAT_NV12};
  ASSERT_EQ(context_.device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                                   &support, sizeof(support)),
            S_OK);
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 256;
  desc.Height = 256;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
  ComPtr<ID3D12Resource> resource;
  const HRESULT result = context_.device()->CreateReservedResource(
      &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
      IID_PPV_ARGS(resource.put()));
  if (!(support.Support2 & D3D12_FORMAT_SUPPORT2_TILED)) {
    EXPECT_TRUE(FAILED(result));
    EXPECT_FALSE(resource);
    return;
  }
  ASSERT_EQ(result, S_OK);
  ASSERT_TRUE(resource);
  UINT subresource_count = 2;
  std::array<D3D12_SUBRESOURCE_TILING, 2> tilings = {};
  UINT total_tiles = 0;
  D3D12_PACKED_MIP_INFO packed = {};
  D3D12_TILE_SHAPE shape = {};
  context_.device()->GetResourceTiling(resource.get(), &total_tiles, &packed,
                                       &shape, &subresource_count, 0,
                                       tilings.data());
  EXPECT_EQ(subresource_count, 2u);
  EXPECT_GT(total_tiles, 0u);
}

} // namespace
