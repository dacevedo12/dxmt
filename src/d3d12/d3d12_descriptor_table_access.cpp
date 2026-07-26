#include "d3d12_descriptor_table_access.hpp"

#include "d3d12_descriptor_mirror.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_resource.hpp"
#include "dxmt_apitrace_d3d.hpp"

namespace dxmt::d3d12 {

namespace {

// 64-bit mixing step shared by the submission-plan identity hash.
constexpr uint64_t kSubmissionPlanHashPrime = 0x9e3779b97f4a7c15ull;

} // namespace

uint64_t
HashSubmissionPlanIdentity(uint64_t root_identity, uint64_t pipeline_identity,
                           PipelineStage stage, bool compute) {
  uint64_t key = root_identity * kSubmissionPlanHashPrime;
  key ^= pipeline_identity + kSubmissionPlanHashPrime + (key << 6) +
         (key >> 2);
  key ^= uint64_t(stage) + kSubmissionPlanHashPrime + (key << 6) +
         (key >> 2);
  key ^= uint64_t(compute) + kSubmissionPlanHashPrime + (key << 6) +
         (key >> 2);
  return key;
}

UINT
DescriptorRangeOffset(const RootSignatureRange &range, UINT running_offset) {
  return range.offset_in_descriptors_from_table_start == UINT_MAX
             ? running_offset
             : range.offset_in_descriptors_from_table_start;
}

const char *
DescriptorRangeTypeName(D3D12_DESCRIPTOR_RANGE_TYPE range_type) {
  switch (range_type) {
  case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
    return "table-cbv";
  case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
    return "table-srv";
  case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
    return "table-uav";
  case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
    return "table-sampler";
  default:
    return "table-unknown";
  }
}

D3D12_RESOURCE_STATES
ShaderReadStateForStage(PipelineStage stage) {
  return stage == PipelineStage::Pixel
             ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
             : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

UINT64
DescriptorRecordSizeBytes(const DescriptorRecord &descriptor) {
  if (!descriptor.has_desc)
    return 0;
  if (descriptor.type == DescriptorRecordType::ConstantBufferView)
    return descriptor.desc.cbv.SizeInBytes;
  auto *resource = GetResource(descriptor.resource.ptr());
  return resource ? resource->GetResourceDesc().Width : 0;
}

std::optional<DescriptorRecord>
GetBoundDescriptorRecordFromHeap(DescriptorHeap *descriptor_heap,
                                 D3D12_GPU_DESCRIPTOR_HANDLE handle,
                                 D3D12_DESCRIPTOR_HEAP_TYPE heap_type) {
  if (!descriptor_heap)
    return std::nullopt;

  auto *mirror = descriptor_heap->GetMirror();
  auto mirror_lock = mirror ? mirror->AcquireLock()
                            : DescriptorHeapMirror::ScopedLock{};
  const auto *descriptor = descriptor_heap->GetDescriptorRecord(handle);
  if (!descriptor) {
    const auto &heap_desc = descriptor_heap->GetDescriptorHeapDesc();
    WARN("D3D12CommandQueue: GPU descriptor handle does not belong to the currently bound heap type=",
         uint32_t(heap_type), " handle=", uint64_t(handle.ptr),
         " heap=", reinterpret_cast<uintptr_t>(descriptor_heap),
         " heapType=", uint32_t(heap_desc.Type),
         " heapCount=", heap_desc.NumDescriptors,
         " heapFlags=", uint32_t(heap_desc.Flags),
         " d3dSequence=", dxmt::apitrace::current_d3d_sequence());
    return std::nullopt;
  }
  if (!descriptor->shader_visible || descriptor->heap_type != heap_type) {
    WARN("D3D12CommandQueue: invalid GPU descriptor heap visibility/type");
    return std::nullopt;
  }
  return *descriptor;
}

std::optional<DescriptorRecord>
GetBoundDescriptorRecordInRangeFromHeap(DescriptorHeap *descriptor_heap,
                                        D3D12_GPU_DESCRIPTOR_HANDLE base,
                                        UINT range_offset,
                                        UINT descriptor_index,
                                        UINT descriptor_count,
                                        D3D12_DESCRIPTOR_HEAP_TYPE heap_type) {
  if (descriptor_count && descriptor_index >= descriptor_count)
    return std::nullopt;

  if (descriptor_index > UINT_MAX - range_offset) {
    WARN("D3D12CommandQueue: descriptor table offset overflow");
    return std::nullopt;
  }
  const auto total_offset = range_offset + descriptor_index;

  const auto handle =
      D3D12_GPU_DESCRIPTOR_HANDLE{base.ptr +
                                  sizeof(DescriptorRecord) * total_offset};
  const auto descriptor =
      GetBoundDescriptorRecordFromHeap(descriptor_heap, handle, heap_type);
  if (!descriptor)
    return std::nullopt;
  return descriptor;
}

const DescriptorRecord *
GetBoundDescriptorRecordInRangeFromLockedHeap(
    DescriptorHeap *descriptor_heap, D3D12_GPU_DESCRIPTOR_HANDLE base,
    UINT range_offset, UINT descriptor_index, UINT descriptor_count,
    D3D12_DESCRIPTOR_HEAP_TYPE heap_type) {
  if (!descriptor_heap ||
      (descriptor_count && descriptor_index >= descriptor_count))
    return nullptr;
  if (descriptor_index > UINT_MAX - range_offset)
    return nullptr;
  const auto total_offset = range_offset + descriptor_index;
  const auto handle = D3D12_GPU_DESCRIPTOR_HANDLE{
      base.ptr + sizeof(DescriptorRecord) * total_offset};
  const auto *descriptor = descriptor_heap->GetDescriptorRecord(handle);
  if (!descriptor || !descriptor->shader_visible ||
      descriptor->heap_type != heap_type)
    return nullptr;
  return descriptor;
}

} // namespace dxmt::d3d12
