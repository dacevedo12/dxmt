#include "d3d12_replay_buffer_immediate.hpp"

#include "d3d12_compiled_binding_tables.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_resource.hpp"
#include "log/log.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace dxmt::d3d12 {

void
ReplayWriteBufferImmediate(WMT::Device device, CommandChunk *chunk,
                           const WriteBufferImmediateRecord &record) {
  struct WriteBufferImmediateOp {
    Rc<Buffer> buffer;
    UINT64 byte_offset;
    UINT value;
  };
  std::vector<WriteBufferImmediateOp> ops;
  ops.reserve(record.operations.size());

  for (const auto &operation : record.operations) {
    auto *resource = GetResource(operation.resource.ptr());
    if (!resource || !resource->GetBufferAllocation()) {
      WARN("D3D12CommandQueue: WriteBufferImmediate skipped unknown "
           "destination");
      continue;
    }

    if (!resource->GetBuffer()) {
      WARN("D3D12CommandQueue: WriteBufferImmediate skipped destination "
           "without buffer resource");
      continue;
    }

    if (!ValidateBufferRange(resource, operation.offset, sizeof(UINT),
                             "WriteBufferImmediate")) {
      WARN("D3D12CommandQueue: WriteBufferImmediate skipped invalid "
           "destination");
      continue;
    }

    const UINT64 byte_offset =
        resource->GetHeapOffset() + operation.offset;
    const auto existing = std::find_if(
        ops.begin(), ops.end(), [&](const WriteBufferImmediateOp &entry) {
          return entry.buffer.ptr() == resource->GetBuffer() &&
                 entry.byte_offset == byte_offset;
        });
    if (existing != ops.end()) {
      // Metal does not define useful results for overlapping blits inside a
      // single encoder batch. D3D12 orders WriteBufferImmediate operations,
      // so collapse an identical destination to its last value.
      existing->value = operation.value;
    } else {
      ops.push_back({Rc<Buffer>(resource->GetBuffer()), byte_offset,
                     operation.value});
    }
  }

  if (ops.empty())
    return;

  WMTBufferInfo staging_info = {};
  staging_info.length = sizeof(UINT) * ops.size();
  staging_info.options = WMTResourceHazardTrackingModeUntracked |
                         WMTResourceOptionCPUCacheModeWriteCombined;
  auto staging = device.newBuffer(staging_info);
  auto *mapped = static_cast<UINT *>(staging_info.memory.get());
  if (!staging || !mapped) {
    WARN("D3D12CommandQueue: WriteBufferImmediate failed to allocate "
         "staging buffer");
    return;
  }

  for (size_t i = 0; i < ops.size(); ++i)
    mapped[i] = ops[i].value;

  chunk->emitcc([ops = std::move(ops),
                 staging = WMT::Reference<WMT::Buffer>(staging)](
                    ArgumentEncodingContext &enc) mutable {
    RetainDirectBufferForGpu(enc, staging);
    struct EncodedWrite {
      BufferAllocation *allocation;
      UINT64 dst_offset;
      UINT64 staging_offset;
    };
    enc.startBlitPass();
    std::vector<EncodedWrite> encoded;
    encoded.reserve(ops.size());
    for (size_t i = 0; i < ops.size(); ++i) {
      auto &op = ops[i];
      auto [allocation, suballocation_offset] =
          enc.access<PipelineStage::Compute>(
              op.buffer, op.byte_offset, sizeof(UINT),
              ResourceAccess::Write);
      encoded.push_back({allocation, suballocation_offset + op.byte_offset,
                         UINT64(i * sizeof(UINT))});
    }

    if (encoded.empty())
    {
      enc.endPass();
      return;
    }

    for (const auto &op : encoded) {
      enc.retainAllocation(op.allocation);
      auto &copy =
          enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      copy.type = WMTBlitCommandCopyFromBufferToBuffer;
      copy.src = staging;
      copy.src_offset = op.staging_offset;
      copy.dst = op.allocation->buffer();
      copy.dst_offset = op.dst_offset;
      copy.copy_length = sizeof(UINT);
    }
    enc.endPass();
  });
}

} // namespace dxmt::d3d12
