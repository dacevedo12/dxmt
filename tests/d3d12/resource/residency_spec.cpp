#include <dxmt_test.hpp>

#include "../../../include/dxmt_d3d12_test_path.hpp"
#include "d3d12_test_context.hpp"
#include <dxmt_test_shader.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

using dxmt::test::ComPtr;
using dxmt::test::D3D12TestContext;

class LifetimeProbe final : public IUnknown {
public:
  explicit LifetimeProbe(std::shared_ptr<std::atomic_bool> destroyed)
      : destroyed_(std::move(destroyed)) {}

  ~LifetimeProbe() { destroyed_->store(true, std::memory_order_release); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (iid != __uuidof(IUnknown))
      return E_NOINTERFACE;
    *object = this;
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return references_.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG references =
        references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (!references)
      delete this;
    return references;
  }

private:
  std::atomic_ulong references_{1};
  std::shared_ptr<std::atomic_bool> destroyed_;
};

class ResidencySpec : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_EQ(context_.Initialize(), S_OK); }

  dxmt::d3d12::test::PersistentResidencyStats ReadPersistentStats() {
    dxmt::d3d12::test::PersistentResidencyStats stats = {};
    UINT size = sizeof(stats);
    EXPECT_EQ(context_.device()->GetPrivateData(
                  dxmt::d3d12::test::kPersistentResidencyStatsGuid,
                  &size, &stats),
              S_OK);
    EXPECT_EQ(size, sizeof(stats));
    EXPECT_EQ(stats.struct_size, sizeof(stats));
    return stats;
  }

  ComPtr<ID3D12Heap> CreateBufferHeap() {
    D3D12_HEAP_DESC desc = {};
    desc.SizeInBytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.Properties.CreationNodeMask = 1;
    desc.Properties.VisibleNodeMask = 1;
    desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ComPtr<ID3D12Heap> heap;
    EXPECT_EQ(context_.device()->CreateHeap(
                  &desc, __uuidof(ID3D12Heap),
                  reinterpret_cast<void **>(heap.put())),
              S_OK);
    return heap;
  }

  ComPtr<ID3D12Heap> CreateTextureHeap() {
    D3D12_HEAP_DESC desc = {};
    desc.SizeInBytes = 4 * 1024 * 1024;
    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.Properties.CreationNodeMask = 1;
    desc.Properties.VisibleNodeMask = 1;
    desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    ComPtr<ID3D12Heap> heap;
    EXPECT_EQ(context_.device()->CreateHeap(
                  &desc, __uuidof(ID3D12Heap),
                  reinterpret_cast<void **>(heap.put())),
              S_OK);
    return heap;
  }

  ComPtr<ID3D12QueryHeap> CreateQueryHeap() {
    D3D12_QUERY_HEAP_DESC desc = {};
    desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    desc.Count = 2;
    ComPtr<ID3D12QueryHeap> heap;
    EXPECT_EQ(context_.device()->CreateQueryHeap(
                  &desc, __uuidof(ID3D12QueryHeap),
                  reinterpret_cast<void **>(heap.put())),
              S_OK);
    return heap;
  }

  ComPtr<ID3D12Resource> CreatePlacedBuffer(ID3D12Heap *heap,
                                            UINT64 offset = 0) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 4096;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    EXPECT_EQ(context_.device()->CreatePlacedResource(
                  heap, offset, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                  IID_PPV_ARGS(resource.put())),
              S_OK);
    return resource;
  }

  ComPtr<ID3D12Resource> CreatePlacedTexture(ID3D12Heap *heap,
                                             UINT64 offset = 0) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 128;
    desc.Height = 128;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ComPtr<ID3D12Resource> resource;
    EXPECT_EQ(context_.device()->CreatePlacedResource(
                  heap, offset, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                  IID_PPV_ARGS(resource.put())),
              S_OK);
    return resource;
  }

  D3D12TestContext context_;
};

TEST_F(ResidencySpec, MakesAndEvictsEverySupportedObjectKind) {
  auto resource = context_.CreateBuffer(
      4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
      D3D12_RESOURCE_STATE_COMMON);
  auto descriptor_heap = context_.CreateDescriptorHeap(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, true);
  auto heap = CreateBufferHeap();
  auto query_heap = CreateQueryHeap();
  ASSERT_TRUE(resource);
  ASSERT_TRUE(descriptor_heap);
  ASSERT_TRUE(heap);
  ASSERT_TRUE(query_heap);
  std::array<ID3D12Pageable *, 4> objects = {
      resource.get(), descriptor_heap.get(), heap.get(), query_heap.get()};

  EXPECT_EQ(context_.device()->MakeResident(
                static_cast<UINT>(objects.size()), objects.data()),
            S_OK);
  EXPECT_EQ(context_.device()->Evict(static_cast<UINT>(objects.size()),
                                     objects.data()),
            S_OK);
  EXPECT_EQ(context_.device()->MakeResident(
                static_cast<UINT>(objects.size()), objects.data()),
            S_OK);
}

TEST_F(ResidencySpec, EnqueueMakeResidentSignalsFenceValuesInOrder) {
  ComPtr<ID3D12Device3> device3;
  ASSERT_EQ(context_.device()->QueryInterface(
                __uuidof(ID3D12Device3),
                reinterpret_cast<void **>(device3.put())),
            S_OK);
  auto first = context_.CreateBuffer(
      4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
      D3D12_RESOURCE_STATE_COMMON);
  auto second = context_.CreateBuffer(
      8192, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
      D3D12_RESOURCE_STATE_COMMON);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ComPtr<ID3D12Fence> fence;
  ASSERT_EQ(context_.device()->CreateFence(
                0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                reinterpret_cast<void **>(fence.put())),
            S_OK);

  ID3D12Pageable *first_object[] = {first.get()};
  std::array<ID3D12Pageable *, 2> both = {first.get(), second.get()};
  ASSERT_EQ(device3->EnqueueMakeResident(D3D12_RESIDENCY_FLAG_NONE, 1,
                                         first_object, fence.get(), 3),
            S_OK);
  ASSERT_EQ(device3->EnqueueMakeResident(
                D3D12_RESIDENCY_FLAG_NONE, static_cast<UINT>(both.size()),
                both.data(), fence.get(), 7),
            S_OK);
  ASSERT_EQ(context_.WaitForFence(fence.get(), 7), S_OK);
  EXPECT_GE(fence->GetCompletedValue(), 7u);
}

TEST_F(ResidencySpec, EnqueueMakeResidentAcceptsDenyOverbudgetFlag) {
  ComPtr<ID3D12Device3> device3;
  ASSERT_EQ(context_.device()->QueryInterface(IID_PPV_ARGS(device3.put())),
            S_OK);
  auto resource = context_.CreateBuffer(
      4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
      D3D12_RESOURCE_STATE_COMMON);
  ComPtr<ID3D12Fence> fence;
  ASSERT_EQ(context_.device()->CreateFence(
                0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())),
            S_OK);
  ASSERT_TRUE(resource);
  ID3D12Pageable *objects[] = {resource.get()};

  ASSERT_EQ(device3->EnqueueMakeResident(
                D3D12_RESIDENCY_FLAG_DENY_OVERBUDGET, 1, objects,
                fence.get(), 5),
            S_OK);
  EXPECT_GE(fence->GetCompletedValue(), 5u);
}

TEST_F(ResidencySpec, AcceptsPriorityBucketsAndApplicationValues) {
  ComPtr<ID3D12Device1> device1;
  ASSERT_EQ(context_.device()->QueryInterface(
                __uuidof(ID3D12Device1),
                reinterpret_cast<void **>(device1.put())),
            S_OK);
  auto resource = context_.CreateBuffer(
      4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
      D3D12_RESOURCE_STATE_COMMON);
  auto heap = CreateBufferHeap();
  ASSERT_TRUE(resource);
  ASSERT_TRUE(heap);
  std::array<ID3D12Pageable *, 2> objects = {resource.get(), heap.get()};
  const std::array<D3D12_RESIDENCY_PRIORITY, 2> priorities = {
      D3D12_RESIDENCY_PRIORITY_LOW,
      static_cast<D3D12_RESIDENCY_PRIORITY>(
          D3D12_RESIDENCY_PRIORITY_NORMAL + 1),
  };
  EXPECT_EQ(device1->SetResidencyPriority(
                static_cast<UINT>(objects.size()), objects.data(),
                priorities.data()),
            S_OK);
}

TEST_F(ResidencySpec,
       DuplicateEntriesCompleteWithoutRetainingPageableObjects) {
  ComPtr<ID3D12Device1> device1;
  ComPtr<ID3D12Device3> device3;
  ASSERT_EQ(context_.device()->QueryInterface(IID_PPV_ARGS(device1.put())),
            S_OK);
  ASSERT_EQ(context_.device()->QueryInterface(IID_PPV_ARGS(device3.put())),
            S_OK);
  auto resource = context_.CreateBuffer(
      4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
      D3D12_RESOURCE_STATE_COMMON);
  ComPtr<ID3D12Fence> fence;
  ASSERT_EQ(context_.device()->CreateFence(
                0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())),
            S_OK);
  ASSERT_TRUE(resource);

  auto destroyed = std::make_shared<std::atomic_bool>(false);
  auto *probe = new LifetimeProbe(destroyed);
  ASSERT_EQ(resource->SetPrivateDataInterface(__uuidof(IUnknown), probe),
            S_OK);
  probe->Release();
  ASSERT_FALSE(destroyed->load(std::memory_order_acquire));

  std::array<ID3D12Pageable *, 3> duplicates = {
      resource.get(), resource.get(), resource.get()};
  constexpr std::array priorities = {
      D3D12_RESIDENCY_PRIORITY_LOW, D3D12_RESIDENCY_PRIORITY_NORMAL,
      D3D12_RESIDENCY_PRIORITY_HIGH};
  EXPECT_EQ(context_.device()->MakeResident(
                static_cast<UINT>(duplicates.size()), duplicates.data()),
            S_OK);
  EXPECT_EQ(device1->SetResidencyPriority(
                static_cast<UINT>(duplicates.size()), duplicates.data(),
                priorities.data()),
            S_OK);
  EXPECT_EQ(context_.device()->Evict(
                static_cast<UINT>(duplicates.size()), duplicates.data()),
            S_OK);
  EXPECT_EQ(device3->EnqueueMakeResident(
                D3D12_RESIDENCY_FLAG_NONE,
                static_cast<UINT>(duplicates.size()), duplicates.data(),
                fence.get(), 9),
            S_OK);
  EXPECT_GE(fence->GetCompletedValue(), 9u);

  resource.reset();
  EXPECT_TRUE(destroyed->load(std::memory_order_acquire));
  EXPECT_EQ(context_.device()->GetDeviceRemovedReason(), S_OK);
}

TEST_F(ResidencySpec, TracksPlacedResourcesThroughBackingHeapResidency) {
  const auto initial = ReadPersistentStats();
  auto heap = CreateBufferHeap();
  ASSERT_TRUE(heap);
  auto first = CreatePlacedBuffer(heap.get());
  ASSERT_TRUE(first);
  const auto baseline = ReadPersistentStats();
  EXPECT_EQ(baseline.entry_count, initial.entry_count + 1);
  EXPECT_EQ(baseline.total_ref_count, initial.total_ref_count + 1);

  auto second = CreatePlacedBuffer(heap.get());
  ASSERT_TRUE(second);
  const auto after_second = ReadPersistentStats();
  EXPECT_EQ(after_second.entry_count, baseline.entry_count);
  EXPECT_EQ(after_second.total_ref_count, baseline.total_ref_count);

  second.reset();
  const auto after_second_remove = ReadPersistentStats();
  EXPECT_EQ(after_second_remove.entry_count, baseline.entry_count);
  EXPECT_EQ(after_second_remove.total_ref_count, baseline.total_ref_count);

  first.reset();
  const auto after_first_remove = ReadPersistentStats();
  EXPECT_EQ(after_first_remove.entry_count, baseline.entry_count);
  EXPECT_EQ(after_first_remove.total_ref_count, baseline.total_ref_count);

  heap.reset();
  const auto after_heap_remove = ReadPersistentStats();
  EXPECT_EQ(after_heap_remove.entry_count, initial.entry_count);
  EXPECT_EQ(after_heap_remove.total_ref_count, initial.total_ref_count);
}

TEST_F(ResidencySpec, DoesNotRegisterPlacedTexturesBesideTheirBackingHeap) {
  const auto initial = ReadPersistentStats();
  auto heap = CreateTextureHeap();
  ASSERT_TRUE(heap);
  auto first = CreatePlacedTexture(heap.get());
  ASSERT_TRUE(first);
  const auto after_first = ReadPersistentStats();
  EXPECT_EQ(after_first.entry_count, initial.entry_count + 1);
  EXPECT_EQ(after_first.total_ref_count, initial.total_ref_count + 1);

  auto second = CreatePlacedTexture(
      heap.get(), D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
  ASSERT_TRUE(second);
  const auto after_second = ReadPersistentStats();
  EXPECT_EQ(after_second.entry_count, after_first.entry_count);
  EXPECT_EQ(after_second.total_ref_count, after_first.total_ref_count);

  first.reset();
  second.reset();
  const auto after_resources = ReadPersistentStats();
  EXPECT_EQ(after_resources.entry_count, after_first.entry_count);
  EXPECT_EQ(after_resources.total_ref_count, after_first.total_ref_count);

  heap.reset();
  const auto after_heap = ReadPersistentStats();
  EXPECT_EQ(after_heap.entry_count, initial.entry_count);
  EXPECT_EQ(after_heap.total_ref_count, initial.total_ref_count);
}

TEST_F(ResidencySpec,
       SerializesBackendResidencyWhileResourceLifetimesRaceSubmission) {
  ASSERT_EQ(context_.ExecuteAndWait(), S_OK);
  ASSERT_EQ(context_.ResetCommandList(), S_OK);

  std::atomic_bool start = false;
  std::atomic_bool producer_done = false;
  std::atomic_bool producer_failed = false;
  std::thread producer([&] {
    while (!start.load(std::memory_order_acquire))
      std::this_thread::yield();

    std::vector<ComPtr<ID3D12Resource>> live_resources;
    live_resources.reserve(16);
    for (uint32_t index = 0; index < 512; ++index) {
      auto resource = context_.CreateBuffer(
          4096 + index * 16, D3D12_HEAP_TYPE_DEFAULT,
          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
      if (!resource) {
        producer_failed.store(true, std::memory_order_release);
        break;
      }
      live_resources.push_back(std::move(resource));
      if (live_resources.size() > 8)
        live_resources.erase(live_resources.begin());
      if ((index & 7u) == 0)
        std::this_thread::yield();
    }
    live_resources.clear();
    producer_done.store(true, std::memory_order_release);
  });

  start.store(true, std::memory_order_release);
  HRESULT submission_result = S_OK;
  for (uint32_t submission = 0;
       submission < 64 ||
       (!producer_done.load(std::memory_order_acquire) && submission < 512);
       ++submission) {
    if (FAILED(submission_result = context_.list()->Close()))
      break;
    ID3D12CommandList *lists[] = {context_.list()};
    context_.queue()->ExecuteCommandLists(1, lists);
    if (FAILED(submission_result = context_.SignalAndWait()))
      break;
    if (FAILED(submission_result = context_.ResetCommandList()))
      break;
  }
  producer.join();

  EXPECT_FALSE(producer_failed.load(std::memory_order_acquire));
  ASSERT_EQ(submission_result, S_OK);
  ASSERT_EQ(context_.list()->Close(), S_OK);
  ID3D12CommandList *final_lists[] = {context_.list()};
  context_.queue()->ExecuteCommandLists(1, final_lists);
  ASSERT_EQ(context_.SignalAndWait(), S_OK);

  const auto final_stats = ReadPersistentStats();
  EXPECT_EQ(final_stats.pending_removal_count, 0u);
  EXPECT_EQ(context_.device()->GetDeviceRemovedReason(), S_OK);
}

TEST_F(ResidencySpec,
       KeepsReplayAllocatorResidencyStableAcrossCommandBufferCompletions) {
  std::array<D3D12_ROOT_PARAMETER, 2> parameters = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  parameters[0].Descriptor.ShaderRegister = 0;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[1].Descriptor.ShaderRegister = 0;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC root_desc = {};
  root_desc.NumParameters = static_cast<UINT>(parameters.size());
  root_desc.pParameters = parameters.data();
  auto root_signature = context_.CreateRootSignature(root_desc);
  ASSERT_TRUE(root_signature);
  const auto shader = dxmt::test::CompileShader(
      "cbuffer C : register(b0) { uint value; };"
      "RWByteAddressBuffer output : register(u0);"
      "[numthreads(1, 1, 1)] void main() { output.Store(0, value); }",
      "cs_5_0");
  ASSERT_EQ(shader.result, S_OK) << shader.diagnostic_text();
  auto pipeline = context_.CreateComputePipeline(
      root_signature.get(),
      {shader.bytecode->GetBufferPointer(), shader.bytecode->GetBufferSize()});
  ASSERT_TRUE(pipeline);
  const std::array<std::uint32_t, 64> constants = {0x12345678};
  auto constant_buffer = context_.CreateUploadBuffer(
      sizeof(constants), constants.data(), sizeof(constants));
  auto output = context_.CreateBuffer(
      256, D3D12_HEAP_TYPE_DEFAULT,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ASSERT_TRUE(constant_buffer);
  ASSERT_TRUE(output);
  const auto baseline = ReadPersistentStats();

  const auto submit = [&] {
    context_.list()->SetComputeRootSignature(root_signature.get());
    context_.list()->SetPipelineState(pipeline.get());
    context_.list()->SetComputeRootConstantBufferView(
        0, constant_buffer->GetGPUVirtualAddress());
    context_.list()->SetComputeRootUnorderedAccessView(
        1, output->GetGPUVirtualAddress());
    context_.list()->Dispatch(1, 1, 1);
    ASSERT_EQ(context_.list()->Close(), S_OK);
    ID3D12CommandList *lists[] = {context_.list()};
    context_.queue()->ExecuteCommandLists(1, lists);
    ASSERT_EQ(context_.SignalAndWait(), S_OK);
  };
  submit();
  const auto first = ReadPersistentStats();
  ASSERT_GT(first.cached_allocation_count, 0u);
  EXPECT_GE(first.cached_allocation_count, baseline.cached_allocation_count);
  EXPECT_EQ(first.pending_removal_count, 0u);
  EXPECT_GT(first.commit_count, baseline.commit_count);
  ASSERT_EQ(context_.ResetCommandList(), S_OK);

  submit();
  const auto second = ReadPersistentStats();
  EXPECT_EQ(second.cached_allocation_count, first.cached_allocation_count);
  EXPECT_EQ(second.entry_count, first.entry_count);
  EXPECT_EQ(second.total_ref_count, first.total_ref_count);
  EXPECT_EQ(second.pending_removal_count, 0u);
  EXPECT_EQ(second.commit_count, first.commit_count);
}

TEST_F(ResidencySpec,
       SparseMappingMembershipRetainsHeapUntilOrderedUnmap) {
  D3D12_RESOURCE_DESC resource_desc = {};
  resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  resource_desc.Width = 256;
  resource_desc.Height = 256;
  resource_desc.DepthOrArraySize = 1;
  resource_desc.MipLevels = 1;
  resource_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  resource_desc.SampleDesc.Count = 1;
  resource_desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
  ComPtr<ID3D12Resource> resource;
  ASSERT_EQ(context_.device()->CreateReservedResource(
                &resource_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(resource.put())),
            S_OK);

  D3D12_PACKED_MIP_INFO packed_mips = {};
  D3D12_TILE_SHAPE tile_shape = {};
  D3D12_SUBRESOURCE_TILING tiling = {};
  UINT subresource_count = 1;
  UINT tile_count = 0;
  context_.device()->GetResourceTiling(
      resource.get(), &tile_count, &packed_mips, &tile_shape,
      &subresource_count, 0, &tiling);
  ASSERT_GT(tile_count, 0u);
  D3D12_HEAP_DESC heap_desc = {};
  heap_desc.SizeInBytes =
      UINT64(tile_count) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
  ComPtr<ID3D12Heap> heap;
  ASSERT_EQ(context_.device()->CreateHeap(&heap_desc,
                                          IID_PPV_ARGS(heap.put())),
            S_OK);
  const auto before = ReadPersistentStats();

  D3D12_TILED_RESOURCE_COORDINATE coordinate = {};
  D3D12_TILE_REGION_SIZE region = {};
  region.NumTiles = tile_count;
  D3D12_TILE_RANGE_FLAGS range_flag = D3D12_TILE_RANGE_FLAG_NONE;
  UINT heap_offset = 0;
  context_.queue()->UpdateTileMappings(
      resource.get(), 1, &coordinate, &region, heap.get(), 1, &range_flag,
      &heap_offset, &tile_count, D3D12_TILE_MAPPING_FLAG_NONE);
  ASSERT_EQ(context_.SignalAndWait(), S_OK);

  const auto mapped = ReadPersistentStats();
  // The first mapping lazily materializes the native reserved texture and
  // placement heap. Each native allocation gains exactly one lifetime
  // registration; the mapping record then retains the heap registration.
  EXPECT_GT(mapped.entry_count, before.entry_count);
  EXPECT_GT(mapped.total_ref_count, before.total_ref_count);
  heap.reset();
  const auto retained_by_mapping = ReadPersistentStats();
  EXPECT_EQ(retained_by_mapping.entry_count, mapped.entry_count);
  EXPECT_EQ(retained_by_mapping.total_ref_count, mapped.total_ref_count);

  range_flag = D3D12_TILE_RANGE_FLAG_NULL;
  context_.queue()->UpdateTileMappings(
      resource.get(), 1, &coordinate, &region, nullptr, 1, &range_flag,
      &heap_offset, &tile_count, D3D12_TILE_MAPPING_FLAG_NONE);
  ASSERT_EQ(context_.SignalAndWait(), S_OK);

  const auto unmapped = ReadPersistentStats();
  EXPECT_EQ(unmapped.entry_count + 1, mapped.entry_count);
  EXPECT_EQ(unmapped.total_ref_count + 1, mapped.total_ref_count);
}

TEST_F(ResidencySpec,
       ConcurrentLifetimeMutationSharesNativeLockWithSubmission) {
  constexpr UINT producer_count = 2;
  constexpr UINT allocations_per_producer = 128;
  std::atomic_bool start = false;
  std::atomic_bool failed = false;
  std::atomic_uint active_producers = producer_count;
  std::vector<std::thread> producers;
  producers.reserve(producer_count);

  for (UINT producer = 0; producer < producer_count; ++producer) {
    producers.emplace_back([&, producer]() {
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (UINT allocation = 0; allocation < allocations_per_producer;
           ++allocation) {
        auto resource = context_.CreateBuffer(
            4096 + 256 * ((producer + allocation) % 8),
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COMMON);
        if (!resource) {
          failed.store(true, std::memory_order_release);
          break;
        }
      }
      active_producers.fetch_sub(1, std::memory_order_acq_rel);
    });
  }

  start.store(true, std::memory_order_release);
  UINT submissions = 0;
  HRESULT submission_result = S_OK;
  do {
    submission_result = context_.SignalAndWait();
    if (FAILED(submission_result))
      break;
    ++submissions;
  } while (active_producers.load(std::memory_order_acquire) &&
           submissions < 64);

  for (auto &producer : producers)
    producer.join();
  ASSERT_EQ(submission_result, S_OK);
  ASSERT_FALSE(failed.load(std::memory_order_acquire));
  ASSERT_EQ(context_.SignalAndWait(), S_OK);

  const auto stats = ReadPersistentStats();
  EXPECT_EQ(stats.pending_removal_count, 0u);
}

TEST_F(ResidencySpec,
       SerializesSharedResidencySetAcrossIndependentCommandQueues) {
  constexpr std::array queue_types = {
      D3D12_COMMAND_LIST_TYPE_DIRECT,
      D3D12_COMMAND_LIST_TYPE_COMPUTE,
      D3D12_COMMAND_LIST_TYPE_COPY,
  };
  struct QueueWorker {
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
  };
  std::array<QueueWorker, queue_types.size()> workers;
  for (std::size_t index = 0; index < workers.size(); ++index) {
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = queue_types[index];
    ASSERT_EQ(context_.device()->CreateCommandQueue(
                  &desc, IID_PPV_ARGS(workers[index].queue.put())),
              S_OK);
    ASSERT_EQ(context_.device()->CreateFence(
                  0, D3D12_FENCE_FLAG_NONE,
                  IID_PPV_ARGS(workers[index].fence.put())),
              S_OK);
  }

  std::atomic_bool start = false;
  std::atomic_bool failed = false;
  std::vector<std::thread> submitters;
  submitters.reserve(workers.size());
  for (auto &worker : workers) {
    auto *worker_ptr = &worker;
    submitters.emplace_back([&start, &failed, worker_ptr]() {
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!event) {
        failed.store(true, std::memory_order_release);
        return;
      }
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (UINT64 value = 1; value <= 128; ++value) {
        HRESULT hr =
            worker_ptr->queue->Signal(worker_ptr->fence.get(), value);
        if (SUCCEEDED(hr) &&
            worker_ptr->fence->GetCompletedValue() < value)
          hr = worker_ptr->fence->SetEventOnCompletion(value, event);
        if (SUCCEEDED(hr) &&
            worker_ptr->fence->GetCompletedValue() < value &&
            WaitForSingleObject(event, 30000) != WAIT_OBJECT_0)
          hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if (FAILED(hr)) {
          failed.store(true, std::memory_order_release);
          break;
        }
      }
      CloseHandle(event);
    });
  }

  start.store(true, std::memory_order_release);
  std::vector<ComPtr<ID3D12Resource>> live_resources;
  live_resources.reserve(32);
  for (UINT index = 0; index < 1024; ++index) {
    auto resource = context_.CreateBuffer(
        4096 + 256 * (index % 16), D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
    if (!resource) {
      failed.store(true, std::memory_order_release);
      break;
    }
    live_resources.push_back(std::move(resource));
    if (live_resources.size() > 16)
      live_resources.erase(live_resources.begin());
    if ((index & 15u) == 0)
      std::this_thread::yield();
  }
  live_resources.clear();
  for (auto &submitter : submitters)
    submitter.join();

  ASSERT_FALSE(failed.load(std::memory_order_acquire));
  EXPECT_EQ(context_.device()->GetDeviceRemovedReason(), S_OK);
  const auto stats = ReadPersistentStats();
  EXPECT_EQ(stats.pending_removal_count, 0u);
}

} // namespace
