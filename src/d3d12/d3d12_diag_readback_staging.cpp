#include "d3d12_diag_readback_staging.hpp"

#include "d3d12_compiled_binding_tables.hpp"
#include "wsi_platform.hpp"

namespace dxmt::d3d12 {

DiagnosticReadbackStaging
AllocateDiagnosticReadbackStaging(WMT::Device device, uint64_t size) {
  WMTBufferInfo info = {};
  info.length = size;
  info.options =
      WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked;
  info.memory.set(nullptr);
#ifdef __i386__
  info.memory.set(wsi::aligned_malloc(size, DXMT_PAGE_SIZE));
#endif
  auto staging = device.newBuffer(info);
  auto *mapped = static_cast<uint8_t *>(info.memory.get_accessible_or_null());
  if (!staging || !mapped) {
#ifdef __i386__
    wsi::aligned_free(info.memory.get_accessible_or_null());
#endif
    return {};
  }
  return {staging, mapped};
}

void
EncodeDiagnosticReadbackBlit(CommandChunk *chunk,
                             const Rc<BufferAllocation> &allocation,
                             WMT::Buffer staging, uint64_t src_offset,
                             uint64_t size) {
  chunk->emitcc([allocation, staging = WMT::Reference<WMT::Buffer>(staging),
                 src_offset, size](ArgumentEncodingContext &enc) {
    RetainDirectBufferForGpu(enc, staging);
    enc.retainAllocation(allocation.ptr());
    enc.startBlitPass();
    auto &copy =
        enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
    copy.type = WMTBlitCommandCopyFromBufferToBuffer;
    copy.src = allocation->buffer();
    copy.src_offset = src_offset;
    copy.dst = staging;
    copy.dst_offset = 0;
    copy.copy_length = size;
    enc.endPass();
  });
}

} // namespace dxmt::d3d12
