#include <dxmt_test.hpp>

#include "d3d12_test_context.hpp"
#include "shaders/runtime_test_shaders.hpp"

#include <cstdint>
#include <cstring>
#include <array>
#include <algorithm>
#include <vector>

namespace {

using dxmt::test::ComPtr;
using dxmt::test::D3D12TestContext;
using dxmt::test::SparseTextureComputeShader;

enum class SparseCase {
  CreateOnly,
  WriteSrv,
  CopySrv,
  EmptySubmit,
  SampleUnmapped,
  SampleMapped,
  CopyToUnmapped,
  CopyToMapped,
};

class D3D12SparseResourceSpec : public ::testing::Test {
protected:
  struct SparseBacking {
    ComPtr<ID3D12Heap> heap;
    UINT tile_count = 0;
  };

  void SetUp() override {
    ASSERT_TRUE(SUCCEEDED(context_.Initialize()));
  }

  SparseBacking CreateAllTilesBacking(ID3D12Resource *resource) {
    D3D12_PACKED_MIP_INFO packed_mip_info = {};
    D3D12_TILE_SHAPE tile_shape = {};
    D3D12_SUBRESOURCE_TILING tiling = {};
    UINT subresource_count = 1;
    UINT total_tile_count = 0;
    context_.device()->GetResourceTiling(
        resource, &total_tile_count, &packed_mip_info, &tile_shape,
        &subresource_count, 0, &tiling);
    EXPECT_NE(total_tile_count, 0u);
    if (!total_tile_count)
      return {};

    D3D12_HEAP_DESC heap_desc = {};
    heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_desc.SizeInBytes =
        static_cast<UINT64>(total_tile_count) *
        D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap_desc.Flags =
        resource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
            ? D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS
            : D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    ComPtr<ID3D12Heap> heap;
    HRESULT hr = context_.device()->CreateHeap(
        &heap_desc, __uuidof(ID3D12Heap),
        reinterpret_cast<void **>(heap.put()));
    EXPECT_TRUE(SUCCEEDED(hr));
    if (FAILED(hr))
      return {};

    return {std::move(heap), total_tile_count};
  }

  void QueueAllTilesMapping(ID3D12Resource *resource,
                            const SparseBacking &backing) {
    ASSERT_TRUE(backing.heap);
    ASSERT_NE(backing.tile_count, 0u);
    D3D12_TILED_RESOURCE_COORDINATE coordinate = {};
    D3D12_TILE_REGION_SIZE region_size = {};
    region_size.NumTiles = backing.tile_count;
    D3D12_TILE_RANGE_FLAGS range_flag = D3D12_TILE_RANGE_FLAG_NONE;
    UINT heap_range_offset = 0;
    context_.queue()->UpdateTileMappings(
        resource, 1, &coordinate, &region_size, backing.heap.get(), 1,
        &range_flag, &heap_range_offset, &backing.tile_count,
        D3D12_TILE_MAPPING_FLAG_NONE);
  }

  ComPtr<ID3D12Heap> MapAllTiles(ID3D12Resource *resource) {
    SparseBacking backing = CreateAllTilesBacking(resource);
    if (!backing.heap)
      return {};
    QueueAllTilesMapping(resource, backing);
    return std::move(backing.heap);
  }

  ComPtr<ID3D12Resource>
  CreateSingleTileReservedBuffer(D3D12TestContext &context,
                                 D3D12_RESOURCE_STATES initial_state) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    EXPECT_EQ(context.device()->CreateReservedResource(
                  &desc, initial_state, nullptr, IID_PPV_ARGS(resource.put())),
              S_OK);
    return resource;
  }

  SparseBacking CreateSingleTileBacking(D3D12TestContext &context) {
    D3D12_HEAP_DESC desc = {};
    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.SizeInBytes = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ComPtr<ID3D12Heap> heap;
    EXPECT_EQ(context.device()->CreateHeap(&desc, IID_PPV_ARGS(heap.put())),
              S_OK);
    return {std::move(heap), 1};
  }

  void QueueSingleTileMapping(D3D12TestContext &context,
                              ID3D12Resource *resource,
                              const SparseBacking &backing) {
    ASSERT_TRUE(resource);
    ASSERT_TRUE(backing.heap);
    D3D12_TILED_RESOURCE_COORDINATE coordinate = {};
    D3D12_TILE_REGION_SIZE region = {};
    region.NumTiles = 1;
    D3D12_TILE_RANGE_FLAGS range_flag = D3D12_TILE_RANGE_FLAG_NONE;
    UINT heap_offset = 0;
    UINT tile_count = 1;
    context.queue()->UpdateTileMappings(
        resource, 1, &coordinate, &region, backing.heap.get(), 1,
        &range_flag, &heap_offset, &tile_count, D3D12_TILE_MAPPING_FLAG_NONE);
  }

  void ExpectSingleTileAlias(D3D12TestContext &context,
                             ID3D12Resource *write_resource,
                             ID3D12Resource *read_resource) {
    constexpr UINT64 size = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    std::vector<std::uint8_t> expected(size);
    for (UINT64 i = 0; i < size; ++i)
      expected[i] =
          static_cast<std::uint8_t>((i * 43 + (i >> 7) + 23) & 0xff);
    auto upload =
        context.CreateUploadBuffer(size, expected.data(), expected.size());
    auto output = context.CreateBuffer(
        size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    ASSERT_TRUE(upload);
    ASSERT_TRUE(output);

    D3D12_TILED_RESOURCE_COORDINATE coordinate = {};
    D3D12_TILE_REGION_SIZE region = {};
    region.NumTiles = 1;
    context.list()->CopyTiles(
        write_resource, &coordinate, &region, upload.get(), 0,
        D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
    D3D12_RESOURCE_BARRIER aliasing = {};
    aliasing.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    aliasing.Aliasing.pResourceBefore = write_resource;
    aliasing.Aliasing.pResourceAfter = read_resource;
    context.list()->ResourceBarrier(1, &aliasing);
    context.list()->CopyTiles(
        read_resource, &coordinate, &region, output.get(), 0,
        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    D3D12TestContext::Transition(
        context.list(), output.get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_COPY_SOURCE);

    std::vector<std::uint8_t> actual;
    ASSERT_EQ(context.ReadbackBuffer(output.get(), size, &actual), S_OK);
    ExpectBytesEqual(actual, expected);
  }

  void ExpectBytesEqual(const std::vector<std::uint8_t> &actual,
                        const std::vector<std::uint8_t> &expected) {
    ASSERT_EQ(actual.size(), expected.size());
    const auto mismatch =
        std::mismatch(actual.begin(), actual.end(), expected.begin());
    if (mismatch.first != actual.end()) {
      ADD_FAILURE() << "byte " << std::distance(actual.begin(), mismatch.first)
                    << ": expected " << static_cast<UINT>(*mismatch.second)
                    << ", got " << static_cast<UINT>(*mismatch.first);
    }
  }

  void SampleTexture(ID3D12DescriptorHeap *descriptor_heap,
                     std::array<std::uint32_t, 28> *results) {
    constexpr std::uint32_t sentinel = 0xf17e5a3cu;
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameters[2] = {};
    parameters[0].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &range;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[1].Descriptor.ShaderRegister = 0;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 2;
    root_desc.pParameters = parameters;

    ComPtr<ID3D12RootSignature> root_signature =
        context_.CreateRootSignature(root_desc);
    ASSERT_TRUE(root_signature);
    ComPtr<ID3D12PipelineState> pipeline = context_.CreateComputePipeline(
        root_signature.get(), SparseTextureComputeShader());
    ASSERT_TRUE(pipeline);
    std::array<std::uint32_t, 28> initial_values = {};
    initial_values.fill(sentinel);
    ComPtr<ID3D12Resource> initial = context_.CreateUploadBuffer(
        sizeof(initial_values), initial_values.data(), sizeof(initial_values));
    ComPtr<ID3D12Resource> output = context_.CreateBuffer(
        sizeof(initial_values), D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_DEST);
    ASSERT_TRUE(initial);
    ASSERT_TRUE(output);
    context_.list()->CopyBufferRegion(output.get(), 0, initial.get(), 0,
                                      sizeof(initial_values));
    D3D12TestContext::Transition(
        context_.list(), output.get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap *heaps[] = {descriptor_heap};
    context_.list()->SetDescriptorHeaps(1, heaps);
    context_.list()->SetComputeRootSignature(root_signature.get());
    context_.list()->SetPipelineState(pipeline.get());
    context_.list()->SetComputeRootDescriptorTable(
        0, context_.GpuDescriptorHandle(descriptor_heap, 1));
    context_.list()->SetComputeRootUnorderedAccessView(
        1, output->GetGPUVirtualAddress());
    context_.list()->Dispatch(1, 1, 1);
    D3D12TestContext::Transition(
        context_.list(), output.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(SUCCEEDED(
        context_.ReadbackBuffer(output.get(), sizeof(*results), &bytes)));
    ASSERT_EQ(bytes.size(), sizeof(*results));
    std::memcpy(results->data(), bytes.data(), sizeof(*results));
    EXPECT_TRUE(std::none_of(
        results->begin(), results->end(), [sentinel](std::uint32_t value) {
          return value == sentinel;
        }));
  }

  void CopyToTexture(ID3D12Resource *texture) {
    const D3D12_RESOURCE_DESC texture_desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT row_count = 0;
    UINT64 row_size = 0;
    UINT64 total_size = 0;
    context_.device()->GetCopyableFootprints(
        &texture_desc, 0, 1, 0, &footprint, &row_count, &row_size,
        &total_size);
    ComPtr<ID3D12Resource> upload = context_.CreateUploadBuffer(total_size);
    ASSERT_TRUE(upload);

    void *mapped = nullptr;
    D3D12_RANGE read_range = {0, 0};
    ASSERT_TRUE(SUCCEEDED(upload->Map(0, &read_range, &mapped)));
    std::memset(mapped, 0x5a, static_cast<std::size_t>(total_size));
    D3D12_RANGE written_range = {0, static_cast<SIZE_T>(total_size)};
    upload->Unmap(0, &written_range);

    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = upload.get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    context_.list()->CopyTextureRegion(&destination, 0, 0, 0, &source,
                                       nullptr);
    ASSERT_TRUE(SUCCEEDED(context_.ExecuteAndWait()));
  }

  ComPtr<ID3D12Resource>
  CreateStandardReservedTexture(D3D12_RESOURCE_STATES initial_state) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 512;
    desc.Height = 512;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    ComPtr<ID3D12Resource> texture;
    EXPECT_EQ(context_.device()->CreateReservedResource(
                  &desc, initial_state, nullptr, __uuidof(ID3D12Resource),
                  reinterpret_cast<void **>(texture.put())),
              S_OK);
    return texture;
  }

  void ExpectCopyTilesRoundTrip(UINT x, UINT y, UINT width, UINT height,
                                UINT64 buffer_offset) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    ASSERT_TRUE(SUCCEEDED(context_.device()->CheckFeatureSupport(
        D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))));
    if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_1)
      GTEST_SKIP() << "Tiled resources are not supported";

    auto texture =
        CreateStandardReservedTexture(D3D12_RESOURCE_STATE_COPY_DEST);
    ASSERT_TRUE(texture);

    D3D12_SUBRESOURCE_TILING tiling = {};
    UINT subresource_count = 1;
    UINT total_tiles = 0;
    context_.device()->GetResourceTiling(
        texture.get(), &total_tiles, nullptr, nullptr, &subresource_count, 0,
        &tiling);
    ASSERT_EQ(subresource_count, 1u);
    ASSERT_GT(total_tiles, 0u);
    ASSERT_LE(x + width, tiling.WidthInTiles);
    ASSERT_LE(y + height, tiling.HeightInTiles);

    auto backing_heap = MapAllTiles(texture.get());
    ASSERT_TRUE(backing_heap);

    const UINT tile_count = width * height;
    const UINT64 copy_size =
        UINT64(tile_count) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    constexpr UINT64 guard_size = 256;
    const UINT64 buffer_size = buffer_offset + copy_size + guard_size;
    std::vector<std::uint8_t> source(buffer_size, 0x5a);
    std::vector<std::uint8_t> expected(buffer_size, 0xa5);
    for (UINT64 i = 0; i < copy_size; ++i) {
      source[buffer_offset + i] =
          static_cast<std::uint8_t>((i * 37 + (i >> 8) + 19) & 0xff);
      expected[buffer_offset + i] = source[buffer_offset + i];
    }

    auto upload = context_.CreateUploadBuffer(buffer_size, source.data(),
                                               source.size());
    auto initial = context_.CreateUploadBuffer(buffer_size, expected.data(),
                                                expected.size());
    auto output = context_.CreateBuffer(
        buffer_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    ASSERT_TRUE(upload);
    ASSERT_TRUE(initial);
    ASSERT_TRUE(output);
    context_.list()->CopyBufferRegion(output.get(), 0, initial.get(), 0,
                                      buffer_size);

    D3D12_TILED_RESOURCE_COORDINATE coordinate = {x, y, 0, 0};
    D3D12_TILE_REGION_SIZE region = {};
    region.NumTiles = tile_count;
    region.UseBox = tile_count > 1;
    if (region.UseBox) {
      region.Width = width;
      region.Height = height;
      region.Depth = 1;
    }
    context_.list()->CopyTiles(
        texture.get(), &coordinate, &region, upload.get(), buffer_offset,
        D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
    D3D12TestContext::Transition(
        context_.list(), texture.get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    context_.list()->CopyTiles(
        texture.get(), &coordinate, &region, output.get(), buffer_offset,
        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    D3D12TestContext::Transition(
        context_.list(), output.get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_COPY_SOURCE);

    std::vector<std::uint8_t> actual;
    ASSERT_TRUE(SUCCEEDED(
        context_.ReadbackBuffer(output.get(), buffer_size, &actual)));
    ExpectBytesEqual(actual, expected);
  }

  void ExpectCopiedTileMappingsAlias(UINT source_x, UINT source_y,
                                     UINT destination_x, UINT destination_y,
                                     UINT width, UINT height,
                                     bool overwrite_existing_mapping) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    ASSERT_EQ(context_.device()->CheckFeatureSupport(
                  D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)),
              S_OK);
    if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_1)
      GTEST_SKIP() << "Tiled resources are not supported";

    auto source =
        CreateStandardReservedTexture(D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto destination =
        CreateStandardReservedTexture(D3D12_RESOURCE_STATE_COPY_DEST);
    ASSERT_TRUE(source);
    ASSERT_TRUE(destination);

    D3D12_SUBRESOURCE_TILING source_tiling = {};
    D3D12_SUBRESOURCE_TILING destination_tiling = {};
    UINT subresource_count = 1;
    context_.device()->GetResourceTiling(source.get(), nullptr, nullptr,
                                         nullptr, &subresource_count, 0,
                                         &source_tiling);
    ASSERT_EQ(subresource_count, 1u);
    subresource_count = 1;
    context_.device()->GetResourceTiling(destination.get(), nullptr, nullptr,
                                         nullptr, &subresource_count, 0,
                                         &destination_tiling);
    ASSERT_EQ(subresource_count, 1u);
    ASSERT_LE(source_x + width, source_tiling.WidthInTiles);
    ASSERT_LE(source_y + height, source_tiling.HeightInTiles);
    ASSERT_LE(destination_x + width, destination_tiling.WidthInTiles);
    ASSERT_LE(destination_y + height, destination_tiling.HeightInTiles);

    auto source_backing = MapAllTiles(source.get());
    ASSERT_TRUE(source_backing);
    ComPtr<ID3D12Heap> destination_backing;
    if (overwrite_existing_mapping) {
      destination_backing = MapAllTiles(destination.get());
      ASSERT_TRUE(destination_backing);
    }

    D3D12_TILED_RESOURCE_COORDINATE source_coordinate = {source_x, source_y,
                                                          0, 0};
    D3D12_TILED_RESOURCE_COORDINATE destination_coordinate = {
        destination_x, destination_y, 0, 0};
    const UINT tile_count = width * height;
    D3D12_TILE_REGION_SIZE region = {};
    region.NumTiles = tile_count;
    region.UseBox = tile_count > 1;
    if (region.UseBox) {
      region.Width = width;
      region.Height = height;
      region.Depth = 1;
    }
    context_.queue()->CopyTileMappings(
        destination.get(), &destination_coordinate, source.get(),
        &source_coordinate, &region, D3D12_TILE_MAPPING_FLAG_NONE);

    const UINT64 copy_size =
        UINT64(tile_count) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    std::vector<std::uint8_t> expected(copy_size);
    for (UINT64 i = 0; i < copy_size; ++i)
      expected[i] =
          static_cast<std::uint8_t>((i * 53 + (i >> 9) + 7) & 0xff);
    auto upload = context_.CreateUploadBuffer(copy_size, expected.data(),
                                               expected.size());
    auto output = context_.CreateBuffer(
        copy_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    ASSERT_TRUE(upload);
    ASSERT_TRUE(output);
    context_.list()->CopyTiles(
        destination.get(), &destination_coordinate, &region, upload.get(), 0,
        D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
    D3D12_RESOURCE_BARRIER aliasing = {};
    aliasing.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    aliasing.Aliasing.pResourceBefore = destination.get();
    aliasing.Aliasing.pResourceAfter = source.get();
    context_.list()->ResourceBarrier(1, &aliasing);
    context_.list()->CopyTiles(
        source.get(), &source_coordinate, &region, output.get(), 0,
        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    D3D12TestContext::Transition(
        context_.list(), output.get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_COPY_SOURCE);

    std::vector<std::uint8_t> actual;
    ASSERT_EQ(context_.ReadbackBuffer(output.get(), copy_size, &actual), S_OK);
    ExpectBytesEqual(actual, expected);
  }

  void RunCase(SparseCase sparse_case);

  D3D12TestContext context_;
};

void D3D12SparseResourceSpec::RunCase(SparseCase sparse_case) {
  D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
  HRESULT hr = context_.device()->CheckFeatureSupport(
      D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
  ASSERT_TRUE(SUCCEEDED(hr));
  if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_1)
    GTEST_SKIP() << "Tiled resources are not supported";

  const bool copy_case = sparse_case == SparseCase::CopyToUnmapped ||
                         sparse_case == SparseCase::CopyToMapped;
  const bool sample_case = sparse_case == SparseCase::SampleUnmapped ||
                           sparse_case == SparseCase::SampleMapped;
  D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON;
  if (copy_case)
    initial_state = D3D12_RESOURCE_STATE_COPY_DEST;
  else if (sample_case)
    initial_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

  D3D12_RESOURCE_DESC resource_desc = {};
  resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  resource_desc.Width = 512;
  resource_desc.Height = 512;
  resource_desc.DepthOrArraySize = 1;
  resource_desc.MipLevels = 10;
  resource_desc.Format = DXGI_FORMAT_R32_UINT;
  resource_desc.SampleDesc.Count = 1;
  resource_desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
  resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> texture;
  hr = context_.device()->CreateReservedResource(
      &resource_desc, initial_state, nullptr, __uuidof(ID3D12Resource),
      reinterpret_cast<void **>(texture.put()));
  ASSERT_TRUE(SUCCEEDED(hr));
  ASSERT_TRUE(texture);
  if (sparse_case == SparseCase::CreateOnly)
    return;

  ComPtr<ID3D12DescriptorHeap> descriptor_heap =
      context_.CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                    2, true);
  ASSERT_TRUE(descriptor_heap);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = resource_desc.Format;
  srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv_desc.Texture2D.MipLevels = resource_desc.MipLevels;
  context_.device()->CreateShaderResourceView(
      texture.get(), &srv_desc,
      context_.CpuDescriptorHandle(descriptor_heap.get(), 0));
  if (sparse_case == SparseCase::WriteSrv)
    return;

  context_.device()->CopyDescriptorsSimple(
      1, context_.CpuDescriptorHandle(descriptor_heap.get(), 1),
      context_.CpuDescriptorHandle(descriptor_heap.get(), 0),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  if (sparse_case == SparseCase::CopySrv)
    return;
  if (sparse_case == SparseCase::EmptySubmit) {
    ASSERT_TRUE(SUCCEEDED(context_.ExecuteAndWait()));
    return;
  }

  ComPtr<ID3D12Heap> backing_heap;
  if (sparse_case == SparseCase::SampleMapped ||
      sparse_case == SparseCase::CopyToMapped) {
    backing_heap = MapAllTiles(texture.get());
    ASSERT_TRUE(backing_heap);
  }

  if (sample_case) {
    std::array<std::uint32_t, 28> results = {};
    SampleTexture(descriptor_heap.get(), &results);
  } else
    CopyToTexture(texture.get());
}

TEST_F(D3D12SparseResourceSpec, CreatesReservedTexture) {
  RunCase(SparseCase::CreateOnly);
}

TEST_F(D3D12SparseResourceSpec, ReservedBufferAndCopyTiles) {
  D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
  ASSERT_EQ(context_.device()->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)),
            S_OK);
  if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_1)
    GTEST_SKIP() << "Tiled resources are not supported";

  constexpr UINT kResourceTileCount = 4;
  constexpr UINT kCopyTileCount = 2;
  constexpr UINT64 kTileSize =
      D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  constexpr UINT64 kBufferSize = kResourceTileCount * kTileSize;
  constexpr UINT64 kBufferOffset = kTileSize;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = kBufferSize;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> reserved;
  ASSERT_EQ(context_.device()->CreateReservedResource(
                &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                __uuidof(ID3D12Resource),
                reinterpret_cast<void **>(reserved.put())),
            S_OK);
  ASSERT_TRUE(reserved);
  EXPECT_NE(reserved->GetGPUVirtualAddress(), 0u);

  D3D12_PACKED_MIP_INFO packed = {};
  D3D12_TILE_SHAPE shape = {};
  D3D12_SUBRESOURCE_TILING tiling = {};
  UINT total_tiles = 0;
  UINT subresource_count = 1;
  context_.device()->GetResourceTiling(
      reserved.get(), &total_tiles, &packed, &shape, &subresource_count, 0,
      &tiling);
  EXPECT_EQ(total_tiles, kResourceTileCount);
  EXPECT_EQ(subresource_count, 1u);
  EXPECT_EQ(packed.NumPackedMips, 0u);
  EXPECT_EQ(shape.WidthInTexels, kTileSize);
  EXPECT_EQ(shape.HeightInTexels, 1u);
  EXPECT_EQ(shape.DepthInTexels, 1u);
  EXPECT_EQ(tiling.WidthInTiles, kResourceTileCount);
  EXPECT_EQ(tiling.HeightInTiles, 1u);
  EXPECT_EQ(tiling.DepthInTiles, 1u);

  auto backing = MapAllTiles(reserved.get());
  ASSERT_TRUE(backing);
  std::vector<std::uint8_t> source(kBufferSize);
  for (UINT64 i = 0; i < source.size(); ++i)
    source[i] = static_cast<std::uint8_t>((i * 29 + (i >> 8) + 7) & 0xff);
  std::vector<std::uint8_t> initial(kBufferSize, 0xa5);
  auto expected = initial;
  std::copy_n(source.begin() + kBufferOffset, kCopyTileCount * kTileSize,
              expected.begin() + kBufferOffset);
  auto upload =
      context_.CreateUploadBuffer(kBufferSize, source.data(), source.size());
  auto initial_upload =
      context_.CreateUploadBuffer(kBufferSize, initial.data(), initial.size());
  auto output = context_.CreateBuffer(
      kBufferSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
      D3D12_RESOURCE_STATE_COPY_DEST);
  ASSERT_TRUE(upload);
  ASSERT_TRUE(initial_upload);
  ASSERT_TRUE(output);
  context_.list()->CopyBufferRegion(output.get(), 0, initial_upload.get(), 0,
                                    kBufferSize);

  D3D12_TILED_RESOURCE_COORDINATE coordinate = {1, 0, 0, 0};
  D3D12_TILE_REGION_SIZE region = {};
  region.NumTiles = kCopyTileCount;
  context_.list()->CopyTiles(
      reserved.get(), &coordinate, &region, upload.get(), kBufferOffset,
      D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
  D3D12TestContext::Transition(
      context_.list(), reserved.get(), D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_STATE_COPY_SOURCE);
  context_.list()->CopyTiles(
      reserved.get(), &coordinate, &region, output.get(), kBufferOffset,
      D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
  D3D12TestContext::Transition(
      context_.list(), output.get(), D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_STATE_COPY_SOURCE);

  std::vector<std::uint8_t> actual;
  ASSERT_EQ(context_.ReadbackBuffer(output.get(), kBufferSize, &actual), S_OK);
  ExpectBytesEqual(actual, expected);
}

TEST_F(D3D12SparseResourceSpec, WritesReservedTextureSrv) {
  RunCase(SparseCase::WriteSrv);
}

TEST_F(D3D12SparseResourceSpec, CopiesReservedTextureSrv) {
  RunCase(SparseCase::CopySrv);
}

TEST_F(D3D12SparseResourceSpec, SubmitsCopiedReservedTextureSrv) {
  RunCase(SparseCase::EmptySubmit);
}

TEST_F(D3D12SparseResourceSpec, SamplesUnmappedReservedTexture) {
  RunCase(SparseCase::SampleUnmapped);
}

TEST_F(D3D12SparseResourceSpec, SamplesMappedReservedTexture) {
  RunCase(SparseCase::SampleMapped);
}

TEST_F(D3D12SparseResourceSpec, CopiesIntoUnmappedReservedTexture) {
  RunCase(SparseCase::CopyToUnmapped);
}

TEST_F(D3D12SparseResourceSpec, CopiesIntoMappedReservedTexture) {
  RunCase(SparseCase::CopyToMapped);
}

TEST_F(D3D12SparseResourceSpec, CopyTilesRoundTripsSingleMappedTile) {
  ExpectCopyTilesRoundTrip(0, 0, 1, 1, 0);
}

TEST_F(D3D12SparseResourceSpec,
       CopyTilesHonorsBufferOffsetAndNonzeroCoordinate) {
  ExpectCopyTilesRoundTrip(1, 1, 1, 1,
                           D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
}

TEST_F(D3D12SparseResourceSpec, CopyTilesRoundTripsMappedTileBox) {
  ExpectCopyTilesRoundTrip(1, 0, 2, 2,
                           D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
}


TEST_F(D3D12SparseResourceSpec,
       CopyTileMappingsAliasesNonzeroCoordinatesBetweenResources) {
  ExpectCopiedTileMappingsAlias(1, 1, 2, 0, 1, 1, false);
}

TEST_F(D3D12SparseResourceSpec,
       CopyTileMappingsCopiesBoxOverExistingMappings) {
  ExpectCopiedTileMappingsAlias(0, 1, 1, 0, 2, 2, true);
}

TEST_F(D3D12SparseResourceSpec,
       CopyTileMappingsOverlappingRangeUsesTemporaryMappings) {
  D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
  ASSERT_EQ(context_.device()->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)),
            S_OK);
  if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_1)
    GTEST_SKIP() << "Tiled resources are not supported";

  auto texture =
      CreateStandardReservedTexture(D3D12_RESOURCE_STATE_COPY_DEST);
  ASSERT_TRUE(texture);
  D3D12_SUBRESOURCE_TILING tiling = {};
  UINT subresource_count = 1;
  context_.device()->GetResourceTiling(texture.get(), nullptr, nullptr,
                                       nullptr, &subresource_count, 0,
                                       &tiling);
  ASSERT_EQ(subresource_count, 1u);
  ASSERT_GE(tiling.WidthInTiles, 4u);
  auto backing = MapAllTiles(texture.get());
  ASSERT_TRUE(backing);

  constexpr UINT tile_count = 3;
  constexpr UINT64 copy_size =
      UINT64(tile_count) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  std::vector<std::uint8_t> expected(copy_size);
  for (UINT64 i = 0; i < copy_size; ++i)
    expected[i] = static_cast<std::uint8_t>((i * 29 + (i >> 7) + 11) & 0xff);
  auto upload = context_.CreateUploadBuffer(copy_size, expected.data(),
                                             expected.size());
  ASSERT_TRUE(upload);

  D3D12_TILED_RESOURCE_COORDINATE source_coordinate = {};
  D3D12_TILE_REGION_SIZE region = {};
  region.NumTiles = tile_count;
  region.UseBox = TRUE;
  region.Width = tile_count;
  region.Height = 1;
  region.Depth = 1;
  context_.list()->CopyTiles(
      texture.get(), &source_coordinate, &region, upload.get(), 0,
      D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
  D3D12TestContext::Transition(
      context_.list(), texture.get(), D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_STATE_COPY_SOURCE);
  ASSERT_EQ(context_.ExecuteAndWait(), S_OK);
  ASSERT_EQ(context_.ResetCommandList(), S_OK);

  D3D12_TILED_RESOURCE_COORDINATE destination_coordinate = {1, 0, 0, 0};
  context_.queue()->CopyTileMappings(
      texture.get(), &destination_coordinate, texture.get(),
      &source_coordinate, &region, D3D12_TILE_MAPPING_FLAG_NONE);
  auto output = context_.CreateBuffer(
      copy_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
      D3D12_RESOURCE_STATE_COPY_DEST);
  ASSERT_TRUE(output);
  context_.list()->CopyTiles(
      texture.get(), &destination_coordinate, &region, output.get(), 0,
      D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
  D3D12TestContext::Transition(
      context_.list(), output.get(), D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_STATE_COPY_SOURCE);

  std::vector<std::uint8_t> actual;
  ASSERT_EQ(context_.ReadbackBuffer(output.get(), copy_size, &actual), S_OK);
  ExpectBytesEqual(actual, expected);
}

TEST_F(D3D12SparseResourceSpec,
       CopyTileMappingsAliasesPackedMipOnNonzeroArraySlice) {
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

  constexpr UINT16 array_size = 2;
  constexpr UINT16 mip_levels = 10;
  constexpr UINT array_slice = 1;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 512;
  desc.Height = 512;
  desc.DepthOrArraySize = array_size;
  desc.MipLevels = mip_levels;
  desc.Format = DXGI_FORMAT_R32_UINT;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
  ComPtr<ID3D12Resource> source;
  ComPtr<ID3D12Resource> destination;
  ASSERT_EQ(context_.device()->CreateReservedResource(
                &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(source.put())),
            S_OK);
  ASSERT_EQ(context_.device()->CreateReservedResource(
                &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(destination.put())),
            S_OK);
  ASSERT_TRUE(source);
  ASSERT_TRUE(destination);

  D3D12_PACKED_MIP_INFO packed = {};
  UINT total_tiles = 0;
  context_.device()->GetResourceTiling(source.get(), &total_tiles, &packed,
                                       nullptr, nullptr, 0, nullptr);
  if (!packed.NumPackedMips)
    GTEST_SKIP() << "Resource has no packed mip tail";
  ASSERT_GE(packed.NumTilesForPackedMips, array_size);
  const UINT first_packed_subresource =
      packed.NumStandardMips + array_slice * mip_levels;
  const UINT deepest_packed_subresource =
      mip_levels - 1 + array_slice * mip_levels;

  D3D12_HEAP_DESC heap_desc = {};
  heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap_desc.SizeInBytes = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
  ComPtr<ID3D12Heap> heap;
  ASSERT_EQ(context_.device()->CreateHeap(&heap_desc,
                                           IID_PPV_ARGS(heap.put())),
            S_OK);
  ASSERT_TRUE(heap);

  D3D12_TILED_RESOURCE_COORDINATE coordinate = {};
  coordinate.Subresource = first_packed_subresource;
  D3D12_TILE_REGION_SIZE region = {};
  region.NumTiles = 1;
  D3D12_TILE_RANGE_FLAGS range_flag = D3D12_TILE_RANGE_FLAG_NONE;
  UINT heap_offset = 0;
  UINT tile_count = 1;
  context_.queue()->UpdateTileMappings(
      source.get(), 1, &coordinate, &region, heap.get(), 1, &range_flag,
      &heap_offset, &tile_count, D3D12_TILE_MAPPING_FLAG_NONE);
  context_.queue()->CopyTileMappings(
      destination.get(), &coordinate, source.get(), &coordinate, &region,
      D3D12_TILE_MAPPING_FLAG_NONE);

  constexpr std::uint32_t expected = 0x4a93d27eu;
  D3D12_RESOURCE_BARRIER transition = {};
  transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  transition.Transition.pResource = destination.get();
  transition.Transition.Subresource = deepest_packed_subresource;
  transition.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  context_.list()->ResourceBarrier(1, &transition);
  ASSERT_EQ(context_.UploadTextureAndReset(
                destination.get(), &expected, sizeof(expected),
                sizeof(expected), deepest_packed_subresource),
            S_OK);
  D3D12_RESOURCE_BARRIER aliasing = {};
  aliasing.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
  aliasing.Aliasing.pResourceBefore = destination.get();
  aliasing.Aliasing.pResourceAfter = source.get();
  context_.list()->ResourceBarrier(1, &aliasing);
  transition.Transition.pResource = source.get();
  transition.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  context_.list()->ResourceBarrier(1, &transition);

  dxmt::test::TextureReadback readback;
  ASSERT_EQ(context_.ReadbackTexture(source.get(), &readback,
                                     deepest_packed_subresource),
            S_OK);
  ASSERT_GE(readback.data.size(), sizeof(expected));
  std::uint32_t actual = 0;
  std::memcpy(&actual, readback.data.data(), sizeof(actual));
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(context_.device()->GetDeviceRemovedReason(), S_OK);
}

TEST_F(D3D12SparseResourceSpec, WritesMappedDeepPackedMipTail) {
  D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
  ASSERT_TRUE(SUCCEEDED(context_.device()->CheckFeatureSupport(
      D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))));
  if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_1)
    GTEST_SKIP() << "Tiled resources are not supported";

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 256;
  desc.Height = 256;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 9;
  desc.Format = DXGI_FORMAT_R32_UINT;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
  ComPtr<ID3D12Resource> texture;
  ASSERT_TRUE(SUCCEEDED(context_.device()->CreateReservedResource(
      &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
      reinterpret_cast<void **>(texture.put()))));
  ASSERT_TRUE(texture);

  D3D12_PACKED_MIP_INFO packed = {};
  D3D12_TILE_SHAPE shape = {};
  UINT total_tiles = 0;
  context_.device()->GetResourceTiling(texture.get(), &total_tiles, &packed,
                                       &shape, nullptr, 0, nullptr);
  if (!packed.NumPackedMips)
    GTEST_SKIP() << "Resource has no packed mip tail";
  const UINT packed_subresource = desc.MipLevels - 1;
  ASSERT_GE(packed_subresource, packed.NumStandardMips);

  auto backing_heap = MapAllTiles(texture.get());
  ASSERT_TRUE(backing_heap);

  constexpr std::uint32_t expected = 0x89abcdefu;
  ASSERT_TRUE(SUCCEEDED(context_.UploadTextureAndReset(
      texture.get(), &expected, sizeof(expected), sizeof(expected),
      packed_subresource)));

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = texture.get();
  barrier.Transition.Subresource = packed_subresource;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  context_.list()->ResourceBarrier(1, &barrier);

  dxmt::test::TextureReadback readback;
  ASSERT_TRUE(SUCCEEDED(
      context_.ReadbackTexture(texture.get(), &readback, packed_subresource)));
  ASSERT_GE(readback.data.size(), sizeof(expected));
  std::uint32_t actual = 0;
  std::memcpy(&actual, readback.data.data(), sizeof(actual));
  EXPECT_EQ(actual, expected);
}

TEST_F(D3D12SparseResourceSpec,
       WritesPackedMipAfterMoreThanThirtyTwoQueuedMappings) {
  D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
  ASSERT_TRUE(SUCCEEDED(context_.device()->CheckFeatureSupport(
      D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))));
  if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_1)
    GTEST_SKIP() << "Tiled resources are not supported";

  // A burst of mapping updates must remain valid until the following queue
  // work consumes the mapped texture.
  constexpr std::size_t mapping_burst_size = 40;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 256;
  desc.Height = 256;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 9;
  desc.Format = DXGI_FORMAT_R32_UINT;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;

  std::vector<ComPtr<ID3D12Resource>> textures;
  std::vector<ComPtr<ID3D12Heap>> backing_heaps;
  textures.reserve(mapping_burst_size);
  backing_heaps.reserve(mapping_burst_size);
  for (std::size_t i = 0; i < mapping_burst_size; ++i) {
    ComPtr<ID3D12Resource> texture;
    ASSERT_TRUE(SUCCEEDED(context_.device()->CreateReservedResource(
        &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        __uuidof(ID3D12Resource),
        reinterpret_cast<void **>(texture.put()))));
    ASSERT_TRUE(texture);

    ComPtr<ID3D12Heap> backing_heap = MapAllTiles(texture.get());
    ASSERT_TRUE(backing_heap);
    textures.push_back(std::move(texture));
    backing_heaps.push_back(std::move(backing_heap));
  }

  D3D12_PACKED_MIP_INFO packed = {};
  D3D12_TILE_SHAPE shape = {};
  UINT total_tiles = 0;
  context_.device()->GetResourceTiling(textures.back().get(), &total_tiles,
                                       &packed, &shape, nullptr, 0, nullptr);
    if (!packed.NumPackedMips)
      GTEST_SKIP() << "Resource has no packed mip tail";
  const UINT packed_subresource = desc.MipLevels - 1;
  ASSERT_GE(packed_subresource, packed.NumStandardMips);

  constexpr std::uint32_t expected = 0x13579bdfu;
  ASSERT_TRUE(SUCCEEDED(context_.UploadTextureAndReset(
      textures.back().get(), &expected, sizeof(expected), sizeof(expected),
      packed_subresource)));

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = textures.back().get();
  barrier.Transition.Subresource = packed_subresource;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  context_.list()->ResourceBarrier(1, &barrier);

  dxmt::test::TextureReadback readback;
  ASSERT_TRUE(SUCCEEDED(context_.ReadbackTexture(
      textures.back().get(), &readback, packed_subresource)));
  ASSERT_GE(readback.data.size(), sizeof(expected));
  std::uint32_t actual = 0;
  std::memcpy(&actual, readback.data.data(), sizeof(actual));
  EXPECT_EQ(actual, expected);
}

DXMT_GROUP_SERIAL_TESTS("D3D12SparseResourceSpec.*", "d3d12-sparse");
DXMT_SERIAL_TEST_DOMAIN("D3D12SparseResourceSpec.*", "sparse");

DXMT_SERIAL_TEST_F(
    D3D12SparseResourceSpec,
    PreservesCompressedPackedMipWritesAcrossSeparateExecutes) {
  D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
  ASSERT_TRUE(SUCCEEDED(context_.device()->CheckFeatureSupport(
      D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))));
  if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_1)
    GTEST_SKIP() << "Tiled resources are not supported";

  constexpr UINT16 kMipLevels = 11;
  constexpr UINT kRowPitch = 256;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 1024;
  desc.Height = 1024;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = kMipLevels;
  desc.Format = DXGI_FORMAT_BC7_UNORM_SRGB;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;

  ComPtr<ID3D12Resource> texture;
  ASSERT_TRUE(SUCCEEDED(context_.device()->CreateReservedResource(
      &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
      __uuidof(ID3D12Resource),
      reinterpret_cast<void **>(texture.put()))));
  ASSERT_TRUE(texture);

  D3D12_PACKED_MIP_INFO packed = {};
  D3D12_TILE_SHAPE shape = {};
  UINT total_tiles = 0;
  context_.device()->GetResourceTiling(texture.get(), &total_tiles, &packed,
                                       &shape, nullptr, 0, nullptr);
  if (packed.NumPackedMips == 0)
    GTEST_SKIP() << "The selected adapter exposes no packed mip region";
  constexpr UINT kPackedSubresource = kMipLevels - 1;
  ASSERT_GE(kPackedSubresource, packed.NumStandardMips);

  D3D12_HEAP_DESC heap_desc = {};
  heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap_desc.SizeInBytes = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
  ComPtr<ID3D12Heap> backing_heap;
  ASSERT_TRUE(SUCCEEDED(context_.device()->CreateHeap(
      &heap_desc, __uuidof(ID3D12Heap),
      reinterpret_cast<void **>(backing_heap.put()))));
  ASSERT_TRUE(backing_heap);

  D3D12_TILED_RESOURCE_COORDINATE coordinate = {};
  coordinate.Subresource = packed.NumStandardMips;
  D3D12_TILE_REGION_SIZE region = {};
  region.NumTiles = 1;
  D3D12_TILE_RANGE_FLAGS range_flag = D3D12_TILE_RANGE_FLAG_NONE;
  UINT heap_offset = 0;
  UINT tile_count = 1;
  context_.queue()->UpdateTileMappings(
      texture.get(), 1, &coordinate, &region, backing_heap.get(), 1,
      &range_flag, &heap_offset, &tile_count, D3D12_TILE_MAPPING_FLAG_NONE);

  constexpr std::array<std::uint8_t, 16> producer_block = {
      0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
      0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
  constexpr std::array<std::uint8_t, 16> expected_block = {
      0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
      0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01};
  std::array<std::uint8_t, kRowPitch> producer_data = {};
  std::array<std::uint8_t, kRowPitch> consumer_data = {};
  std::copy(producer_block.begin(), producer_block.end(),
            producer_data.begin());
  std::copy(expected_block.begin(), expected_block.end(),
            consumer_data.begin());
  auto producer_upload = context_.CreateUploadBuffer(
      producer_data.size(), producer_data.data(), producer_data.size());
  auto consumer_upload = context_.CreateUploadBuffer(
      consumer_data.size(), consumer_data.data(), consumer_data.size());
  ASSERT_TRUE(producer_upload);
  ASSERT_TRUE(consumer_upload);

  auto create_copy_list = [&](ID3D12Resource *upload,
                              ComPtr<ID3D12CommandAllocator> *allocator,
                              ComPtr<ID3D12GraphicsCommandList> *list) {
    if (FAILED(context_.device()->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        __uuidof(ID3D12CommandAllocator),
        reinterpret_cast<void **>(allocator->put()))))
      return false;
    if (FAILED(context_.device()->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator->get(), nullptr,
        __uuidof(ID3D12GraphicsCommandList),
        reinterpret_cast<void **>(list->put()))))
      return false;
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = upload;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Footprint = {
        desc.Format, 1, 1, 1, kRowPitch};
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = texture.get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = kPackedSubresource;
    (*list)->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    return SUCCEEDED((*list)->Close());
  };

  ComPtr<ID3D12CommandAllocator> producer_allocator;
  ComPtr<ID3D12GraphicsCommandList> producer_list;
  ComPtr<ID3D12CommandAllocator> consumer_allocator;
  ComPtr<ID3D12GraphicsCommandList> consumer_list;
  ASSERT_TRUE(create_copy_list(producer_upload.get(), &producer_allocator,
                               &producer_list));
  ASSERT_TRUE(create_copy_list(consumer_upload.get(), &consumer_allocator,
                               &consumer_list));

  ID3D12CommandList *producer = producer_list.get();
  context_.queue()->ExecuteCommandLists(1, &producer);
  ID3D12CommandList *consumer = consumer_list.get();
  context_.queue()->ExecuteCommandLists(1, &consumer);
  ASSERT_TRUE(SUCCEEDED(context_.SignalAndWait()));
  EXPECT_EQ(context_.device()->GetDeviceRemovedReason(), S_OK);

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = texture.get();
  barrier.Transition.Subresource = kPackedSubresource;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  context_.list()->ResourceBarrier(1, &barrier);
  dxmt::test::TextureReadback readback;
  ASSERT_TRUE(SUCCEEDED(context_.ReadbackTexture(
      texture.get(), &readback, kPackedSubresource)));
  ASSERT_GE(readback.data.size(), expected_block.size());
  for (std::size_t i = 0; i < expected_block.size(); ++i)
    EXPECT_EQ(readback.data[i], expected_block[i]) << "byte " << i;
}

TEST_F(D3D12SparseResourceSpec, ReportsPackedMipTilesForEveryArraySlice) {
  D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
  ASSERT_TRUE(SUCCEEDED(context_.device()->CheckFeatureSupport(
      D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))));
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

  constexpr UINT16 array_size = 3;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 512;
  desc.Height = 512;
  desc.DepthOrArraySize = array_size;
  desc.MipLevels = 10;
  desc.Format = DXGI_FORMAT_R32_UINT;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
  ComPtr<ID3D12Resource> texture;
  ASSERT_TRUE(SUCCEEDED(context_.device()->CreateReservedResource(
      &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
      reinterpret_cast<void **>(texture.put()))));
  ASSERT_TRUE(texture);

  D3D12_PACKED_MIP_INFO packed = {};
  D3D12_TILE_SHAPE shape = {};
  UINT total_tiles = 0;
  context_.device()->GetResourceTiling(texture.get(), &total_tiles, &packed,
                                       &shape, nullptr, 0, nullptr);
  if (!packed.NumPackedMips)
    GTEST_SKIP() << "Resource has no packed mip tail";
  EXPECT_EQ(packed.NumTilesForPackedMips, array_size);
  EXPECT_GE(total_tiles, packed.NumTilesForPackedMips);
}

} // namespace
