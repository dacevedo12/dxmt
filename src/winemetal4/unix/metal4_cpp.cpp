#define METALCPP_SYMBOL_VISIBILITY_HIDDEN
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION

#include "Foundation/Foundation.hpp"
#include "Metal/Metal.hpp"
#include "Metal/MTL4CommandAllocator.hpp"
#include "Metal/MTL4CommandBuffer.hpp"
#include "Metal/MTL4CommandQueue.hpp"
#include "Metal/MTL4Counters.hpp"
#include "Metal/MTLDevice.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstring>
#include <stdint.h>

typedef uint64_t obj_handle_t;

extern "C" {

struct WMTMemoryPointer {
  uint64_t ptr;
};

struct unixcall_mtl4counterheap_newtimestampheap {
  obj_handle_t device;
  uint64_t count;
  obj_handle_t ret_error;
  obj_handle_t ret_heap;
};

struct unixcall_mtl4counterheap_resolvecounterrange {
  obj_handle_t heap;
  uint64_t start;
  uint64_t len;
  struct WMTMemoryPointer data_out;
  uint64_t data_length;
};

struct unixcall_mtl4commandbuffer_resolvecounterheap {
  obj_handle_t cmdbuf;
  obj_handle_t heap;
  uint64_t start;
  uint64_t count;
  obj_handle_t dst_buffer;
  uint64_t dst_offset;
  uint64_t dst_length;
  obj_handle_t wait_fence;
  obj_handle_t update_fence;
};

struct unixcall_mtl4timestampcontext_writetimestamp {
  obj_handle_t context;
  obj_handle_t mtl_command_buffer;
  obj_handle_t heap;
  uint64_t index;
};

struct unixcall_generic_obj_uint64_ret {
  obj_handle_t handle;
  uint64_t ret;
};

struct unixcall_generic_obj_noret {
  obj_handle_t handle;
};

struct unixcall_generic_obj_obj_ret {
  obj_handle_t handle;
  obj_handle_t ret;
};

typedef int NTSTATUS;
#define STATUS_SUCCESS 0

NTSTATUS _MTL4CounterHeap_newTimestampHeap(void *obj);
NTSTATUS _MTL4CounterHeap_resolveCounterRange(void *obj);
NTSTATUS _MTL4CommandBuffer_resolveCounterHeap(void *obj);
NTSTATUS _MTL4TimestampContext_create(void *obj);
NTSTATUS _MTL4TimestampContext_destroy(void *obj);
NTSTATUS _MTL4TimestampContext_writeTimestamp(void *obj);
NTSTATUS _MTLDevice_sizeOfTimestampHeapEntry(void *obj);
}

namespace {

template <typename T>
T *
fromHandle(obj_handle_t handle) {
  return reinterpret_cast<T *>(handle);
}

template <typename T>
obj_handle_t
toHandle(T *object) {
  return reinterpret_cast<obj_handle_t>(object);
}

struct TimestampContext {
};

extern "C" void
DXMTMetal4CommandBuffer_writeTimestampIntoHeap(obj_handle_t cmdbuf, obj_handle_t heap, uint64_t index);
extern "C" void
DXMTMetal4CommandBuffer_resolveCounterHeap(
    obj_handle_t cmdbuf, obj_handle_t heap, uint64_t start, uint64_t count,
    obj_handle_t dst_buffer, uint64_t dst_offset, uint64_t dst_length,
    obj_handle_t wait_fence, obj_handle_t update_fence);

} // namespace

extern "C" NTSTATUS
_MTL4CounterHeap_newTimestampHeap(void *obj) {
  auto *params = static_cast<unixcall_mtl4counterheap_newtimestampheap *>(obj);
  auto *device = fromHandle<MTL::Device>(params->device);

  // MTL_DEBUG_LAYER aborts when entryCount > 4096. Clamp here as a hard backstop
  // so a PE-side miscount cannot kill the process before the game window opens.
  constexpr uint64_t kMaxTimestampCounterHeapEntries = 4096;
  uint64_t count = params->count;
  if (count > kMaxTimestampCounterHeapEntries)
    count = kMaxTimestampCounterHeapEntries;
  if (count < 2)
    count = 2;

  auto *descriptor = MTL4::CounterHeapDescriptor::alloc()->init();
  descriptor->setType(MTL4::CounterHeapTypeTimestamp);
  descriptor->setCount(static_cast<NS::UInteger>(count));

  NS::Error *error = nullptr;
  auto *heap = device->newCounterHeap(descriptor, &error);
  descriptor->release();

  params->ret_heap = toHandle(heap);
  params->ret_error = toHandle(error ? error->retain() : nullptr);
  return STATUS_SUCCESS;
}

extern "C" NTSTATUS
_MTL4CounterHeap_resolveCounterRange(void *obj) {
  auto *params = static_cast<unixcall_mtl4counterheap_resolvecounterrange *>(obj);
  auto *heap = fromHandle<MTL4::CounterHeap>(params->heap);
  void *dst = reinterpret_cast<void *>(params->data_out.ptr);

  if (!heap || !dst || !params->data_length)
    return STATUS_SUCCESS;

  NS::Data *data = heap->resolveCounterRange(NS::Range::Make(
      static_cast<NS::UInteger>(params->start),
      static_cast<NS::UInteger>(params->len)));
  if (!data)
    return STATUS_SUCCESS;

  const auto copy_len = std::min<uint64_t>(params->data_length, data->length());
  const void *src = CFDataGetBytePtr(reinterpret_cast<CFDataRef>(data));
  if (copy_len && src)
    std::memcpy(dst, src, copy_len);

  return STATUS_SUCCESS;
}

extern "C" NTSTATUS
_MTL4CommandBuffer_resolveCounterHeap(void *obj) {
  auto *params = static_cast<unixcall_mtl4commandbuffer_resolvecounterheap *>(obj);
  auto *heap = fromHandle<MTL4::CounterHeap>(params->heap);

  if (!params->cmdbuf || !heap || !params->count || !params->dst_buffer || !params->dst_length)
    return STATUS_SUCCESS;

  DXMTMetal4CommandBuffer_resolveCounterHeap(
      params->cmdbuf, params->heap, params->start, params->count,
      params->dst_buffer, params->dst_offset, params->dst_length,
      params->wait_fence, params->update_fence);
  return STATUS_SUCCESS;
}

extern "C" NTSTATUS
_MTL4TimestampContext_create(void *obj) {
  auto *params = static_cast<unixcall_generic_obj_obj_ret *>(obj);
  params->ret = toHandle(new TimestampContext{});
  return STATUS_SUCCESS;
}

extern "C" NTSTATUS
_MTL4TimestampContext_destroy(void *obj) {
  auto *params = static_cast<unixcall_generic_obj_noret *>(obj);
  auto *context = fromHandle<TimestampContext>(params->handle);
  if (!context)
    return STATUS_SUCCESS;

  delete context;
  return STATUS_SUCCESS;
}

extern "C" NTSTATUS
_MTL4TimestampContext_writeTimestamp(void *obj) {
  auto *params = static_cast<unixcall_mtl4timestampcontext_writetimestamp *>(obj);
  auto *context = fromHandle<TimestampContext>(params->context);
  if (!context || !params->mtl_command_buffer || !params->heap)
    return STATUS_SUCCESS;

  DXMTMetal4CommandBuffer_writeTimestampIntoHeap(params->mtl_command_buffer, params->heap, params->index);
  return STATUS_SUCCESS;
}

extern "C" NTSTATUS
_MTLDevice_sizeOfTimestampHeapEntry(void *obj) {
  auto *params = static_cast<unixcall_generic_obj_uint64_ret *>(obj);
  auto *device = fromHandle<MTL::Device>(params->handle);

  params->ret = device->sizeOfCounterHeapEntry(MTL4::CounterHeapTypeTimestamp);
  return STATUS_SUCCESS;
}
