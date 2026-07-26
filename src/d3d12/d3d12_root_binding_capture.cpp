#include "d3d12_root_binding_capture.hpp"

#include "d3d12_binding_fingerprint.hpp"
#include "d3d12_queue_view_binding.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_shader_binding.hpp"
#include "log/log.hpp"

#include <algorithm>
#include <atomic>
#include <utility>

namespace dxmt::d3d12 {

namespace {

// Legacy shader ABI constant-buffer binding slots (b0..b13).
constexpr UINT kConstantBufferBindingSlots = 14;

constexpr UINT64 kConstantBufferPlacementAlignmentMask =
    D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1;

} // namespace

D3D12_DESCRIPTOR_RANGE_TYPE
RootDescriptorRangeType(DescriptorRecordType type) {
  return type == DescriptorRecordType::ConstantBufferView
             ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
             : type == DescriptorRecordType::ShaderResourceView
                   ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                   : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
}

const char *
RootDescriptorDebugKind(DescriptorRecordType type) {
  return type == DescriptorRecordType::ConstantBufferView
             ? "root-cbv"
             : type == DescriptorRecordType::ShaderResourceView ? "root-srv"
                                                                : "root-uav";
}

bool
BuildRootBufferDescriptor(D3D12_GPU_VIRTUAL_ADDRESS address,
                          DescriptorRecordType type,
                          RootBufferDescriptorBuild &out) {
  out = {};

  Resource *resource = nullptr;
  const auto offset = ResolveBufferGpuAddress(address, resource);
  if (!resource || !resource->GetBuffer())
    return false;
  if (type == DescriptorRecordType::ConstantBufferView &&
      (address & kConstantBufferPlacementAlignmentMask)) {
    WARN("D3D12CommandQueue: root CBV address is not 256-byte aligned");
    return false;
  }

  DescriptorRecord descriptor = {};
  descriptor.type = type;
  descriptor.resource = resource->GetD3D12Resource();
  descriptor.has_desc = true;
  if (type == DescriptorRecordType::ConstantBufferView) {
    const auto remaining = resource->GetResourceDesc().Width - offset;
    const auto size = std::min<UINT64>(remaining, UINT_MAX);
    if (size & kConstantBufferPlacementAlignmentMask) {
      WARN("D3D12CommandQueue: root CBV resolved size is not 256-byte aligned");
      return false;
    }
    descriptor.desc.cbv.BufferLocation = address;
    descriptor.desc.cbv.SizeInBytes = UINT(size);
  } else if (type == DescriptorRecordType::ShaderResourceView) {
    descriptor.desc.srv.Format = DXGI_FORMAT_UNKNOWN;
    descriptor.desc.srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    descriptor.desc.srv.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    descriptor.desc.srv.Buffer.FirstElement = offset;
    descriptor.desc.srv.Buffer.NumElements =
        UINT(std::min<UINT64>(resource->GetResourceDesc().Width - offset,
                              UINT_MAX));
    descriptor.desc.srv.Buffer.StructureByteStride = 1;
  } else {
    descriptor.desc.uav.Format = DXGI_FORMAT_UNKNOWN;
    descriptor.desc.uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    descriptor.desc.uav.Buffer.FirstElement = offset;
    descriptor.desc.uav.Buffer.NumElements =
        UINT(std::min<UINT64>(resource->GetResourceDesc().Width - offset,
                              UINT_MAX));
    descriptor.desc.uav.Buffer.StructureByteStride = 1;
  }

  out.resource = resource;
  out.offset = offset;
  out.descriptor = std::move(descriptor);
  return true;
}

void
CaptureGraphicsRootDescriptorAddress(GraphicsBindingSnapshot &snapshot,
                                     const PipelineState &pipeline,
                                     UINT root_index,
                                     const RootSignatureParameter &parameter,
                                     DescriptorRecordType type,
                                     D3D12_GPU_VIRTUAL_ADDRESS address,
                                     bool compute) {
  RootBufferDescriptorBuild build;
  if (!BuildRootBufferDescriptor(address, type, build))
    return;
  auto *resource = build.resource;
  const auto offset = build.offset;
  const auto &descriptor = build.descriptor;

  const auto range_type = RootDescriptorRangeType(type);
  ForEachVisibleStage(parameter.visibility, compute, [&](PipelineStage stage) {
    const auto *argument = ResolveShaderBindingArgument(
        pipeline, stage, BindingTypeForRange(range_type),
        parameter.descriptor.ShaderRegister,
        parameter.descriptor.RegisterSpace);
    if (!argument)
      return;

    GraphicsBindingSnapshotEntry entry = {};
    entry.kind = GraphicsBindingSnapshotEntry::Kind::Descriptor;
    entry.stage = stage;
    entry.range_type = range_type;
    entry.root_index = root_index;
    entry.slot = argument->SM50BindingSlot;
    entry.shader_register = parameter.descriptor.ShaderRegister;
    entry.register_space = parameter.descriptor.RegisterSpace;
    entry.debug_size = resource->GetResourceDesc().Width - offset;
    entry.debug_address = address;
    entry.debug_kind = RootDescriptorDebugKind(type);
    entry.has_descriptor = true;
    entry.descriptor_index = snapshot.descriptor_records->capture(descriptor);
    entry.argument = argument;
    HashGraphicsBindingValue(snapshot.content_fingerprint, entry.kind);
    HashGraphicsBindingValue(snapshot.content_fingerprint, entry.stage);
    HashGraphicsBindingValue(snapshot.content_fingerprint, entry.range_type);
    HashGraphicsBindingValue(snapshot.content_fingerprint, entry.root_index);
    HashGraphicsBindingValue(snapshot.content_fingerprint, entry.slot);
    HashGraphicsBindingValue(snapshot.content_fingerprint,
                             entry.shader_register);
    HashGraphicsBindingValue(snapshot.content_fingerprint,
                             entry.register_space);
    HashGraphicsBindingValue(snapshot.content_fingerprint, *entry.argument);
    HashGraphicsBindingValue(snapshot.content_fingerprint,
                             entry.debug_address);
    HashGraphicsBindingDescriptor(snapshot.content_fingerprint,
                                  SnapshotDescriptor(snapshot, entry));
    snapshot.entries.push_back(std::move(entry));
  });
}

void
CaptureGraphicsRootConstantsValues(GraphicsBindingSnapshot &snapshot,
                                   const PipelineState &pipeline,
                                   UINT root_index,
                                   const RootSignatureParameter &parameter,
                                   std::span<const UINT> values,
                                   UINT dst_offset, bool compute) {
  const auto declared_count = parameter.constants.Num32BitValues;
  // `dst_offset + values.size()` is a size_t sum, but the explicit uint32_t
  // template argument truncated it into `actual_count` while the resize below
  // used the untruncated value: the recorded constant_count and
  // constants.size() then disagreed, and the resize itself could ask for
  // gigabytes. A whole root signature is capped at D3D12_MAX_ROOT_COST DWORDs
  // (CreateRootSignatureFromBlob refuses anything above it) and each 32-bit
  // constant costs one DWORD, so no legal window can cross that cap; drop the
  // values beyond it the way the frozen-native appendRootConstants path does.
  // Everything inside the cap keeps the existing behaviour, including the
  // deliberate tolerance for a command list writing more words than the
  // parameter declares (see BindRootConstants in d3d12_root_parameter_apply.hpp).
  if (values.empty()) {
    dst_offset = 0;
  } else if (uint64_t(dst_offset) + values.size() > D3D12_MAX_ROOT_COST) {
    static std::atomic<uint32_t> log_count = 0;
    if (log_count.fetch_add(1, std::memory_order_relaxed) < 8) {
      WARN("D3D12CommandQueue: root constants destination range is outside the"
           " root signature and was dropped rootParameterIndex=", root_index,
           " num32BitValuesToSet=", values.size(),
           " destOffsetIn32BitValues=", dst_offset);
    }
    values = {};
    dst_offset = 0;
  }
  const auto actual_count = std::max<uint32_t>(
      declared_count, static_cast<uint32_t>(dst_offset + values.size()));
  if (!actual_count)
    return;

  ForEachVisibleStage(parameter.visibility, compute, [&](PipelineStage stage) {
    auto binding_slot = ResolveShaderBindingSlot(
        pipeline, stage, SM50BindingType::ConstantBuffer,
        parameter.constants.ShaderRegister,
        parameter.constants.RegisterSpace);
    if (!binding_slot)
      return;
    if (*binding_slot >= kConstantBufferBindingSlots) {
      WARN("D3D12CommandQueue: root constants target unsupported CBV slot b",
           *binding_slot);
      return;
    }

    GraphicsBindingSnapshotEntry entry = {};
    entry.kind = GraphicsBindingSnapshotEntry::Kind::RootConstants;
    entry.stage = stage;
    entry.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    entry.root_index = root_index;
    entry.slot = *binding_slot;
    entry.shader_register = parameter.constants.ShaderRegister;
    entry.register_space = parameter.constants.RegisterSpace;
    entry.debug_size = uint64_t(actual_count) * sizeof(UINT);
    entry.debug_kind = "root-constants";
    if (!values.empty()) {
      if (dst_offset) {
        entry.constants.resize(dst_offset + values.size(), 0);
        std::copy(values.begin(), values.end(),
                  entry.constants.begin() + dst_offset);
      } else {
        entry.constants.assign(values.begin(), values.end());
      }
    }
    entry.constant_count = actual_count;
    HashGraphicsBindingValue(snapshot.content_fingerprint, entry.kind);
    HashGraphicsBindingValue(snapshot.content_fingerprint, entry.stage);
    HashGraphicsBindingValue(snapshot.content_fingerprint, entry.root_index);
    HashGraphicsBindingValue(snapshot.content_fingerprint, entry.slot);
    HashGraphicsBindingValue(snapshot.content_fingerprint,
                             entry.shader_register);
    HashGraphicsBindingValue(snapshot.content_fingerprint,
                             entry.register_space);
    HashGraphicsBindingValue(snapshot.content_fingerprint,
                             entry.constant_count);
    for (const auto value : entry.constants)
      HashGraphicsBindingValue(snapshot.content_fingerprint, value);
    snapshot.entries.push_back(std::move(entry));
  });
}

} // namespace dxmt::d3d12
