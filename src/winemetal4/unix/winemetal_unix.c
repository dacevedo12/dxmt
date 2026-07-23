#include <stdatomic.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#import <Cocoa/Cocoa.h>
#import <ColorSync/ColorSync.h>
#import <CoreFoundation/CFRunLoop.h>
#import <Metal/Metal.h>
#import <Metal/MTL4ArgumentTable.h>
#import <Metal/MTL4ComputeCommandEncoder.h>
#import <Metal/MTL4RenderPass.h>
#import <Metal/MTL4CommandQueue.h>
#import <Metal/MTL4Counters.h>
#import <Metal/MTLAllocation.h>
#import <Metal/MTLResourceViewPool.h>
#import <Metal/MTLResidencySet.h>
#import <Metal/MTLTextureViewPool.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/QuartzCore.h>
#include "objc/objc-runtime.h"
#include "iogpu_diagnostics.h"
#include <bootstrap.h>
#include <mach/mach_port.h>
#include <pthread.h>
#define WINEMETAL_API
#include "../winemetal_thunks.h"
#include "../airconv_thunks.h"

#if DXMT_APITRACE_METAL
#include "apitrace/metal_capi.h"
#endif

typedef int NTSTATUS;
#define STATUS_SUCCESS 0
#define STATUS_UNSUCCESSFUL 0xC0000001

@class DXMTMetal4CommandQueue;

@protocol MTLDrawableWithTexture <MTLDrawable>
@property(readonly) id<MTLTexture> texture;
@end

@protocol MTLDeviceMetal4SPI <MTLDevice>
- (nullable id<MTL4ArgumentTable>)newArgumentTableWithDescriptor:(MTL4ArgumentTableDescriptor *)descriptor
                                                           error:(NSError **)error;
@end

static NSObject *dxmt_metal4_compiler_lock;

struct DXMTSparseTextureTileKey {
  uint32_t level;
  uint32_t slice;
  uint32_t x;
  uint32_t y;
  uint32_t z;
};

@interface DXMTSparseTextureResidency : NSObject {
  NSLock *_lock;
  NSMutableDictionary *_tileHeaps;
  NSCountedSet *_mappedHeaps;
}
- (void)applyOperations:(const struct WMTSparseTextureMappingOperation *)operations
                  count:(uint64_t)count
                   heap:(id<MTLHeap>)heap;
- (void)addMappedHeapsToResidencySet:(id<MTLResidencySet>)residencySet;
- (NSArray *)mappedHeapsSnapshot;
@end

static NSLock *dxmt_sparse_texture_residency_lock;
static NSMapTable *dxmt_sparse_texture_residency;
static pthread_once_t dxmt_sparse_texture_residency_once = PTHREAD_ONCE_INIT;
/* Serializes ALL Metal MTLResidencySet userspace mutations (add/remove/commit
 * and the externalPersistentResidencySets list). Metal residency sets are not
 * thread-safe; one global ops lock matches GPTK "single mutex around set ops"
 * and avoids lock-order inversion with encode/register paths.
 */
static NSLock *dxmt_residency_set_ops_lock;
static pthread_once_t dxmt_residency_set_ops_once = PTHREAD_ONCE_INIT;

static void
dxmt_residency_set_ops_init(void) {
  dxmt_residency_set_ops_lock = [[NSLock alloc] init];
}

static NSLock *
dxmt_residency_set_ops_lock_get(void) {
  pthread_once(&dxmt_residency_set_ops_once, dxmt_residency_set_ops_init);
  return dxmt_residency_set_ops_lock;
}

/* Back-compat name used by older call sites in this file. */
static NSLock *
dxmt_residency_membership_lock_get(void) {
  return dxmt_residency_set_ops_lock_get();
}

static void
dxmt_sparse_texture_residency_init(void) {
  dxmt_sparse_texture_residency_lock = [[NSLock alloc] init];
  dxmt_sparse_texture_residency = [[NSMapTable alloc]
      initWithKeyOptions:NSPointerFunctionsWeakMemory |
                         NSPointerFunctionsObjectPointerPersonality
      valueOptions:NSPointerFunctionsStrongMemory
      capacity:0];
}

static DXMTSparseTextureResidency *
dxmt_sparse_texture_residency_for_texture(id<MTLTexture> texture,
                                          bool create) {
  if (!texture)
    return nil;
  pthread_once(&dxmt_sparse_texture_residency_once,
               dxmt_sparse_texture_residency_init);
  [dxmt_sparse_texture_residency_lock lock];
  DXMTSparseTextureResidency *residency =
      [dxmt_sparse_texture_residency objectForKey:texture];
  if (!residency && create) {
    residency = [[DXMTSparseTextureResidency alloc] init];
    [dxmt_sparse_texture_residency setObject:residency forKey:texture];
    [residency release];
  }
  [residency retain];
  [dxmt_sparse_texture_residency_lock unlock];
  return [residency autorelease];
}

@implementation DXMTSparseTextureResidency
- (instancetype)init {
  self = [super init];
  if (!self)
    return nil;
  _lock = [[NSLock alloc] init];
  _tileHeaps = [[NSMutableDictionary alloc] init];
  _mappedHeaps = [[NSCountedSet alloc] init];
  return self;
}

- (void)dealloc {
  [_mappedHeaps release];
  [_tileHeaps release];
  [_lock release];
  [super dealloc];
}

- (void)applyOperations:(const struct WMTSparseTextureMappingOperation *)operations
                  count:(uint64_t)count
                   heap:(id<MTLHeap>)heap {
  [_lock lock];
  for (uint64_t i = 0; i < count; ++i) {
    const struct WMTSparseTextureMappingOperation *operation = &operations[i];
    for (uint32_t z = 0; z < operation->depth; ++z) {
      for (uint32_t y = 0; y < operation->height; ++y) {
        for (uint32_t x = 0; x < operation->width; ++x) {
          const struct DXMTSparseTextureTileKey tile = {
              .level = operation->level,
              .slice = operation->slice,
              .x = operation->x + x,
              .y = operation->y + y,
              .z = operation->z + z,
          };
          NSData *key = [NSData dataWithBytes:&tile length:sizeof(tile)];
          id<MTLHeap> oldHeap = [_tileHeaps objectForKey:key];
          id<MTLHeap> newHeap =
              operation->mode == WMTSparseTextureMappingModeMap ? heap : nil;
          if (oldHeap == newHeap)
            continue;
          if (oldHeap) {
            [_mappedHeaps removeObject:oldHeap];
            [_tileHeaps removeObjectForKey:key];
          }
          if (newHeap) {
            [_tileHeaps setObject:newHeap forKey:key];
            [_mappedHeaps addObject:newHeap];
          }
        }
      }
    }
  }
  [_lock unlock];
}

- (void)addMappedHeapsToResidencySet:(id<MTLResidencySet>)residencySet {
  [_lock lock];
  for (id<MTLHeap> heap in _mappedHeaps)
    [residencySet addAllocation:(id<MTLAllocation>)heap];
  [_lock unlock];
}

- (NSArray *)mappedHeapsSnapshot {
  [_lock lock];
  NSArray *snapshot = [[_mappedHeaps allObjects] retain];
  [_lock unlock];
  return [snapshot autorelease];
}
@end

static bool
dxmt_truthy_env_value(const char *value) {
  if (!value || !value[0])
    return false;
  return !strcmp(value, "1") || !strcmp(value, "true") ||
         !strcmp(value, "yes") || !strcmp(value, "trace") ||
         !strcmp(value, "on");
}

static bool
dxmt_env_enabled_default(const char *name, bool default_value) {
  const char *value = getenv(name);
  if (!value || !value[0])
    return default_value;
  if (!strcmp(value, "0") || !strcmp(value, "false") ||
      !strcmp(value, "no") || !strcmp(value, "off"))
    return false;
  if (dxmt_truthy_env_value(value))
    return true;
  return default_value;
}

// Heap placement storage-mode diagnostics. Cheap on the happy path (a few
// integer comparisons per placed allocation). Mismatch logs are rate-limited;
// verbose success samples require DXMT_DIAG_HEAP_STORAGE=1.
static bool
dxmt_metal4_heap_storage_diag_verbose(void) {
  static bool initialized = false;
  static bool enabled = false;
  if (!initialized) {
    enabled = dxmt_truthy_env_value(getenv("DXMT_DIAG_HEAP_STORAGE")) ||
              dxmt_truthy_env_value(getenv("DXMT_DIAG_ROOT_CAUSE_DENSE"));
    initialized = true;
  }
  return enabled;
}

static MTLStorageMode
dxmt_storage_mode_from_resource_options(MTLResourceOptions options) {
  return (MTLStorageMode)((options & MTLResourceStorageModeMask) >>
                          MTLResourceStorageModeShift);
}

static const char *
dxmt_storage_mode_name(MTLStorageMode mode) {
  switch (mode) {
  case MTLStorageModeShared:
    return "Shared";
  case MTLStorageModePrivate:
    return "Private";
#if defined(MTLStorageModeManaged)
  case MTLStorageModeManaged:
    return "Managed";
#endif
#if defined(MTLStorageModeMemoryless)
  case MTLStorageModeMemoryless:
    return "Memoryless";
#endif
  default:
    return "Unknown";
  }
}

static void
dxmt_diag_heap_placement(const char *kind, id<MTLHeap> heap, uint64_t offset,
                         uint64_t length, MTLResourceOptions options,
                         const struct WMTTextureInfo *texture_info,
                         BOOL about_to_call_metal) {
  if (!heap)
    return;

  const MTLStorageMode heap_mode = heap.storageMode;
  const MTLStorageMode req_mode = dxmt_storage_mode_from_resource_options(options);
  const BOOL mismatch = heap_mode != req_mode;
  if (!mismatch && !dxmt_metal4_heap_storage_diag_verbose())
    return;

  static _Atomic uint32_t mismatch_count = 0;
  static _Atomic uint32_t sample_count = 0;
  uint32_t n = 0;
  if (mismatch) {
    n = atomic_fetch_add_explicit(&mismatch_count, 1, memory_order_relaxed) + 1;
    // Always keep early mismatches; then every 16th so a flood cannot dominate.
    if (n > 32 && (n % 16) != 0)
      return;
  } else {
    n = atomic_fetch_add_explicit(&sample_count, 1, memory_order_relaxed) + 1;
    if (n > 8 && (n % 256) != 0)
      return;
  }

  fprintf(stderr,
          "%s:  DXMT Metal4 heap placement %s: kind=%s heap=%p type=%lu "
          "heapSize=%llu heapStorage=%s(%lu) heapHazard=%lu "
          "reqOptions=0x%llx reqStorage=%s(%lu) offset=%llu length=%llu "
          "cpuCacheMode=%lu aboutToCallMetal=%u count=%u",
          mismatch ? "err" : "info",
          mismatch ? "STORAGE_MODE_MISMATCH" : "sample", kind, heap,
          (unsigned long)heap.type, (unsigned long long)heap.size,
          dxmt_storage_mode_name(heap_mode), (unsigned long)heap_mode,
          (unsigned long)heap.hazardTrackingMode,
          (unsigned long long)options, dxmt_storage_mode_name(req_mode),
          (unsigned long)req_mode, (unsigned long long)offset,
          (unsigned long long)length,
          (unsigned long)((options & MTLResourceCPUCacheModeMask) >>
                          MTLResourceCPUCacheModeShift),
          about_to_call_metal ? 1u : 0u, n);
  if (texture_info) {
    fprintf(stderr,
            " texType=%u format=%u size=%ux%ux%u mips=%u array=%u samples=%u "
            "usage=0x%x reserved=0x%x",
            (unsigned)texture_info->type, (unsigned)texture_info->pixel_format,
            (unsigned)texture_info->width, (unsigned)texture_info->height,
            (unsigned)texture_info->depth,
            (unsigned)texture_info->mipmap_level_count,
            (unsigned)texture_info->array_length,
            (unsigned)texture_info->sample_count, (unsigned)texture_info->usage,
            (unsigned)texture_info->reserved);
  }
  fputc('\n', stderr);
  fflush(stderr);
}

static uint64_t
dxmt_parse_u64_env(const char *name, uint64_t default_value) {
  const char *value = getenv(name);
  if (!value || !value[0])
    return default_value;
  char *end = NULL;
  unsigned long long parsed = strtoull(value, &end, 10);
  if (end == value)
    return default_value;
  return parsed;
}

static bool
dxmt_metal4_commit_feedback_enabled(uint64_t completion_value) {
  static bool initialized = false;
  static bool enabled = false;
  static bool sampled = false;
  static uint64_t sample_rate = 64;
  if (!initialized) {
    const char *value = getenv("DXMT_METAL4_COMMIT_FEEDBACK");
    enabled = dxmt_truthy_env_value(value);
    sampled = value && !strcmp(value, "sampled");
    sample_rate = dxmt_parse_u64_env("DXMT_METAL4_COMMIT_FEEDBACK_SAMPLE_RATE", 64);
    if (!sample_rate)
      sample_rate = 64;
    initialized = true;
  }
  if (enabled)
    return true;
  if (sampled)
    return completion_value == 0 || (completion_value % sample_rate) == 0;
  return false;
}

static bool
dxmt_metal4_present_ordering_enabled(void) {
  static bool initialized = false;
  static bool enabled = true;
  if (!initialized) {
    enabled = dxmt_env_enabled_default("DXMT_METAL4_PRESENT_ORDERING", true);
    initialized = true;
  }
  return enabled;
}

static bool
dxmt_metal4_wait_for_drawable_enabled(void) {
  static bool initialized = false;
  static bool enabled = true;
  if (!initialized) {
    enabled = dxmt_env_enabled_default("DXMT_METAL4_WAIT_FOR_DRAWABLE", true);
    initialized = true;
  }
  return enabled;
}

static bool
dxmt_metal4_perf_stats_enabled(void) {
  static bool initialized = false;
  static bool enabled = false;
  if (!initialized) {
    enabled = dxmt_truthy_env_value(getenv("DXMT_PERF_STATS"));
    initialized = true;
  }
  return enabled;
}

static bool
dxmt_metal4_pso_labels_enabled(void) {
  static bool initialized = false;
  static bool enabled = false;
  if (!initialized) {
    enabled = dxmt_truthy_env_value(getenv("DXMT_DIAG_METAL_PSO_LABELS")) ||
              dxmt_truthy_env_value(getenv("DXMT_DIAG_ROOT_CAUSE_DENSE"));
    initialized = true;
  }
  return enabled;
}

static bool
dxmt_metal4_residency_diag_enabled(void) {
  static bool initialized = false;
  static bool enabled = false;
  if (!initialized) {
    enabled = dxmt_truthy_env_value(getenv("DXMT_DIAG_METAL_RESIDENCY"));
    initialized = true;
  }
  return enabled;
}

static id<MTLAllocation>
dxmt_metal4_backing_allocation(id<MTLAllocation> allocation) {
  if (!allocation || ![(id)allocation conformsToProtocol:@protocol(MTLTexture)])
    return allocation;

  id<MTLTexture> texture = (id<MTLTexture>)allocation;
  for (unsigned depth = 0; depth < 64 && texture.parentTexture; depth++)
    texture = texture.parentTexture;
  if (texture.buffer)
    return (id<MTLAllocation>)texture.buffer;
  return (id<MTLAllocation>)texture;
}

static bool
dxmt_metal4_dense_hang_diagnostics_enabled(void) {
  static bool initialized = false;
  static bool enabled = false;
  if (!initialized) {
    enabled = dxmt_truthy_env_value(getenv("DXMT_DIAG_GPU_HANG_DENSE")) ||
              dxmt_truthy_env_value(getenv("DXMT_DIAG_ROOT_CAUSE_DENSE"));
    initialized = true;
  }
  return enabled;
}

/*
 * Hang / commit diagnostics.  PE-side WARN often never sees native fprintf
 * output when the wine-launch stream stalls; mirror critical lines into
 * $DXMT_LOG_PATH/dxmt-metal4-native.log so post-mortem always has a copy.
 */
enum {
  DXMT_METAL4_HANG_LOG_STDERR = 1u << 0,
  DXMT_METAL4_HANG_LOG_FILE = 1u << 1,
  DXMT_METAL4_HANG_LOG_BOTH =
      DXMT_METAL4_HANG_LOG_STDERR | DXMT_METAL4_HANG_LOG_FILE,
};

static FILE *dxmt_metal4_hang_log_file_handle = NULL;

static void
dxmt_metal4_hang_log_file_open(void) {
  const char *dir = getenv("DXMT_LOG_PATH");
  if (!dir || !dir[0])
    return;
  char path[PATH_MAX];
  if (snprintf(path, sizeof(path), "%s/dxmt-metal4-native.log", dir) >=
      (int)sizeof(path))
    return;
  dxmt_metal4_hang_log_file_handle = fopen(path, "a");
  if (dxmt_metal4_hang_log_file_handle) {
    setvbuf(dxmt_metal4_hang_log_file_handle, NULL, _IOLBF, 0);
    fprintf(dxmt_metal4_hang_log_file_handle,
            "info:  DXMT Metal4 native hang log opened path=%s pid=%d\n", path,
            (int)getpid());
    fflush(dxmt_metal4_hang_log_file_handle);
  }
}

static FILE *
dxmt_metal4_hang_log_file(void) {
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  pthread_once(&once, dxmt_metal4_hang_log_file_open);
  return dxmt_metal4_hang_log_file_handle;
}

static void
dxmt_metal4_hang_log(unsigned sinks, const char *fmt, ...) {
  va_list ap;
  if (sinks & DXMT_METAL4_HANG_LOG_STDERR) {
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
  }
  if (sinks & DXMT_METAL4_HANG_LOG_FILE) {
    FILE *file = dxmt_metal4_hang_log_file();
    if (file) {
      va_start(ap, fmt);
      vfprintf(file, fmt, ap);
      va_end(ap);
      fflush(file);
    }
  }
}

static bool
dxmt_metal4_queue_monitor_enabled(void) {
  static bool initialized = false;
  static bool enabled = false;
  if (!initialized) {
    enabled = dxmt_truthy_env_value(
                  getenv("DXMT_DIAG_METAL4_QUEUE_MONITOR")) ||
              dxmt_metal4_dense_hang_diagnostics_enabled();
    initialized = true;
  }
  return enabled;
}

static bool
dxmt_metal4_test_feedback_error_enabled(void) {
  const char *once =
      getenv("DXMT_TEST_METAL4_INJECT_FEEDBACK_ERROR_ONCE");
  return dxmt_truthy_env_value(
             getenv("DXMT_TEST_METAL4_INJECT_FEEDBACK_ERROR")) ||
         (once && once[0] && strcmp(once, "0") && strcmp(once, "false") &&
          strcmp(once, "no") && strcmp(once, "off"));
}

static uint64_t
dxmt_monotonic_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

typedef NS_ENUM(uint32_t, DXMTMetal4QueueMonitorPhase) {
  DXMTMetal4QueueMonitorPhaseIdle = 0,
  DXMTMetal4QueueMonitorPhaseCommitPreflight,
  DXMTMetal4QueueMonitorPhasePendingSparseResidency,
  DXMTMetal4QueueMonitorPhaseEnqueueWaitEvents,
  DXMTMetal4QueueMonitorPhaseWaitForDrawable,
  DXMTMetal4QueueMonitorPhasePresentOrderingWait,
  DXMTMetal4QueueMonitorPhasePrepareResidency,
  /* Sub-phases of prepareCommandResidencyForAllocations (submissionLock held). */
  DXMTMetal4QueueMonitorPhasePrepareResidencyLock,
  DXMTMetal4QueueMonitorPhasePrepareResidencyRetire,
  DXMTMetal4QueueMonitorPhasePrepareResidencyAdd,
  DXMTMetal4QueueMonitorPhasePrepareResidencySparseCommit,
  DXMTMetal4QueueMonitorPhasePrepareResidencyCommandCommit,
  DXMTMetal4QueueMonitorPhasePrepareResidencyUnlock,
  DXMTMetal4QueueMonitorPhaseCommitResidencySet,
  DXMTMetal4QueueMonitorPhaseRequestResidency,
  DXMTMetal4QueueMonitorPhaseUseResidencySet,
  DXMTMetal4QueueMonitorPhaseEndCommandBuffer,
  DXMTMetal4QueueMonitorPhaseCommandBufferThrottle,
  DXMTMetal4QueueMonitorPhaseFeedbackSetup,
  DXMTMetal4QueueMonitorPhaseMetalQueueCommit,
  DXMTMetal4QueueMonitorPhasePresent,
  DXMTMetal4QueueMonitorPhaseSignalEvents,
  DXMTMetal4QueueMonitorPhaseFinalize,
  DXMTMetal4QueueMonitorPhaseFeedbackInjection,
  DXMTMetal4QueueMonitorPhaseFeedbackErrorLatch,
  DXMTMetal4QueueMonitorPhaseReleaseSparseResidency,
  DXMTMetal4QueueMonitorPhaseReleaseSparseResidencyRefs,
  DXMTMetal4QueueMonitorPhaseReleaseSparseResidencyCommit,
  DXMTMetal4QueueMonitorPhaseReleaseSparseResidencyClear,
  DXMTMetal4QueueMonitorPhaseSparseMappingPreflight,
  DXMTMetal4QueueMonitorPhaseSparseMappingResidencyRefs,
  DXMTMetal4QueueMonitorPhaseSparseMappingResidencyCommit,
  DXMTMetal4QueueMonitorPhaseSparseMappingMetalUpdate,
  DXMTMetal4QueueMonitorPhaseSparseMappingApply,
};

/* subjectFlags bits for prepare-residency hang diagnostics. */
enum {
  DXMT_PREPARE_RESIDENCY_FLAG_SPARSE_CHANGED = 1u << 0,
  DXMT_PREPARE_RESIDENCY_FLAG_COMMAND_CHANGED = 1u << 1,
  DXMT_PREPARE_RESIDENCY_FLAG_STEP_LOCK = 1u << 8,
  DXMT_PREPARE_RESIDENCY_FLAG_STEP_RETIRE = 1u << 9,
  DXMT_PREPARE_RESIDENCY_FLAG_STEP_ADD = 1u << 10,
  DXMT_PREPARE_RESIDENCY_FLAG_STEP_SPARSE_COMMIT = 1u << 11,
  DXMT_PREPARE_RESIDENCY_FLAG_STEP_COMMAND_COMMIT = 1u << 12,
  DXMT_PREPARE_RESIDENCY_FLAG_STEP_UNLOCK = 1u << 13,
};

typedef NS_ENUM(uint32_t, DXMTMetal4QueueMonitorSource) {
  DXMTMetal4QueueMonitorSourceNone = 0,
  DXMTMetal4QueueMonitorSourceCommandBufferCommit,
  DXMTMetal4QueueMonitorSourceUnixCommitThunk,
  DXMTMetal4QueueMonitorSourceFeedbackInjection,
  DXMTMetal4QueueMonitorSourceFeedbackErrorLatch,
  DXMTMetal4QueueMonitorSourceReleaseSparseResidency,
  DXMTMetal4QueueMonitorSourceSparseTextureMappings,
};

enum {
  DXMT_METAL4_QUEUE_MONITOR_DIAGNOSTIC_CAPACITY = 16,
  DXMT_METAL4_QUEUE_MONITOR_WAITER_CAPACITY = 8,
};

struct DXMTMetal4QueueMonitorWaiterDiagnostic {
  atomic_uint_fast64_t thread;
  atomic_uint_fast64_t since_us;
  atomic_uint source;
  atomic_uintptr_t command_buffer;
};

/*
 * This tracker intentionally contains scalars only.  The watchdog must be able
 * to report a stuck Objective-C monitor without retaining or messaging the
 * queue, command buffer, or drawable that may be involved in the stall.
 */
struct DXMTMetal4QueueMonitorDiagnostic {
  atomic_uintptr_t queue;
  atomic_uintptr_t metal_queue;
  atomic_uint_fast64_t sequence;
  atomic_uint_fast64_t owner_thread;
  atomic_uint owner_depth;
  atomic_uint_fast64_t acquired_us;
  atomic_uint_fast64_t phase_us;
  atomic_uint phase;
  atomic_uint source;
  atomic_uintptr_t command_buffer;
  atomic_uintptr_t metal_buffer;
  atomic_uint_fast64_t completion_value;
  atomic_uint_fast64_t frame_id;
  atomic_uint_fast64_t chunk_id;
  atomic_uint_fast64_t d3d_sequence_begin;
  atomic_uint_fast64_t d3d_sequence_end;
  atomic_uintptr_t subject;
  atomic_uintptr_t subject_aux;
  atomic_uint_fast64_t subject_count;
  atomic_uint_fast64_t subject_flags;
  atomic_uint_fast64_t last_owner_thread;
  atomic_uint last_source;
  atomic_uint last_phase;
  atomic_uint_fast64_t last_duration_us;
  atomic_uint_fast64_t last_released_us;
  atomic_uint active_waiters;
  atomic_uint waiter_overflow;
  struct DXMTMetal4QueueMonitorWaiterDiagnostic
      waiters[DXMT_METAL4_QUEUE_MONITOR_WAITER_CAPACITY];
  atomic_uint report_count;
  atomic_uint_fast64_t last_report_us;
};

static struct DXMTMetal4QueueMonitorDiagnostic
    dxmt_metal4_queue_monitor_diagnostics
        [DXMT_METAL4_QUEUE_MONITOR_DIAGNOSTIC_CAPACITY];
static pthread_once_t dxmt_metal4_queue_monitor_watchdog_once =
    PTHREAD_ONCE_INIT;

static const char *
dxmt_metal4_queue_monitor_phase_name(uint32_t phase) {
  switch ((DXMTMetal4QueueMonitorPhase)phase) {
  case DXMTMetal4QueueMonitorPhaseCommitPreflight:
    return "commit-preflight";
  case DXMTMetal4QueueMonitorPhasePendingSparseResidency:
    return "pending-sparse-residency";
  case DXMTMetal4QueueMonitorPhaseEnqueueWaitEvents:
    return "enqueue-wait-events";
  case DXMTMetal4QueueMonitorPhaseWaitForDrawable:
    return "wait-for-drawable";
  case DXMTMetal4QueueMonitorPhasePresentOrderingWait:
    return "present-ordering-wait";
  case DXMTMetal4QueueMonitorPhasePrepareResidency:
    return "prepare-residency";
  case DXMTMetal4QueueMonitorPhasePrepareResidencyLock:
    return "prepare-residency-lock";
  case DXMTMetal4QueueMonitorPhasePrepareResidencyRetire:
    return "prepare-residency-retire";
  case DXMTMetal4QueueMonitorPhasePrepareResidencyAdd:
    return "prepare-residency-add";
  case DXMTMetal4QueueMonitorPhasePrepareResidencySparseCommit:
    return "prepare-residency-sparse-commit";
  case DXMTMetal4QueueMonitorPhasePrepareResidencyCommandCommit:
    return "prepare-residency-command-commit";
  case DXMTMetal4QueueMonitorPhasePrepareResidencyUnlock:
    return "prepare-residency-unlock";
  case DXMTMetal4QueueMonitorPhaseCommitResidencySet:
    return "commit-residency-set";
  case DXMTMetal4QueueMonitorPhaseRequestResidency:
    return "request-residency";
  case DXMTMetal4QueueMonitorPhaseUseResidencySet:
    return "use-residency-set";
  case DXMTMetal4QueueMonitorPhaseEndCommandBuffer:
    return "end-command-buffer";
  case DXMTMetal4QueueMonitorPhaseCommandBufferThrottle:
    return "command-buffer-throttle";
  case DXMTMetal4QueueMonitorPhaseFeedbackSetup:
    return "feedback-setup";
  case DXMTMetal4QueueMonitorPhaseMetalQueueCommit:
    return "metal-queue-commit";
  case DXMTMetal4QueueMonitorPhasePresent:
    return "present";
  case DXMTMetal4QueueMonitorPhaseSignalEvents:
    return "signal-events";
  case DXMTMetal4QueueMonitorPhaseFinalize:
    return "finalize";
  case DXMTMetal4QueueMonitorPhaseFeedbackInjection:
    return "feedback-injection";
  case DXMTMetal4QueueMonitorPhaseFeedbackErrorLatch:
    return "feedback-error-latch";
  case DXMTMetal4QueueMonitorPhaseReleaseSparseResidency:
    return "release-sparse-residency";
  case DXMTMetal4QueueMonitorPhaseReleaseSparseResidencyRefs:
    return "release-sparse-residency-refs";
  case DXMTMetal4QueueMonitorPhaseReleaseSparseResidencyCommit:
    return "release-sparse-residency-commit";
  case DXMTMetal4QueueMonitorPhaseReleaseSparseResidencyClear:
    return "release-sparse-residency-clear";
  case DXMTMetal4QueueMonitorPhaseSparseMappingPreflight:
    return "sparse-mapping-preflight";
  case DXMTMetal4QueueMonitorPhaseSparseMappingResidencyRefs:
    return "sparse-mapping-residency-refs";
  case DXMTMetal4QueueMonitorPhaseSparseMappingResidencyCommit:
    return "sparse-mapping-residency-commit";
  case DXMTMetal4QueueMonitorPhaseSparseMappingMetalUpdate:
    return "sparse-mapping-metal-update";
  case DXMTMetal4QueueMonitorPhaseSparseMappingApply:
    return "sparse-mapping-apply";
  default:
    return "idle-or-unknown";
  }
}

static const char *
dxmt_metal4_queue_monitor_source_name(uint32_t source) {
  switch ((DXMTMetal4QueueMonitorSource)source) {
  case DXMTMetal4QueueMonitorSourceCommandBufferCommit:
    return "command-buffer-commit";
  case DXMTMetal4QueueMonitorSourceUnixCommitThunk:
    return "unix-commit-thunk";
  case DXMTMetal4QueueMonitorSourceFeedbackInjection:
    return "feedback-injection";
  case DXMTMetal4QueueMonitorSourceFeedbackErrorLatch:
    return "feedback-error-latch";
  case DXMTMetal4QueueMonitorSourceReleaseSparseResidency:
    return "release-sparse-residency";
  case DXMTMetal4QueueMonitorSourceSparseTextureMappings:
    return "sparse-texture-mappings";
  default:
    return "none-or-unknown";
  }
}

static void *
dxmt_metal4_queue_monitor_watchdog(void *unused) {
  (void)unused;
  for (;;) {
    usleep(100000);
    const uint64_t now_us = dxmt_monotonic_us();
    for (uint32_t i = 0;
         i < DXMT_METAL4_QUEUE_MONITOR_DIAGNOSTIC_CAPACITY; i++) {
      struct DXMTMetal4QueueMonitorDiagnostic *diag =
          &dxmt_metal4_queue_monitor_diagnostics[i];
      const uintptr_t queue =
          atomic_load_explicit(&diag->queue, memory_order_acquire);
      if (!queue || queue == UINTPTR_MAX)
        continue;
      const uint64_t owner_thread = atomic_load_explicit(
          &diag->owner_thread, memory_order_acquire);
      const uint64_t acquired_us = atomic_load_explicit(
          &diag->acquired_us, memory_order_relaxed);
      uint64_t oldest_waiter_thread = 0;
      uint64_t oldest_waiter_since_us = 0;
      uint32_t oldest_waiter_source = DXMTMetal4QueueMonitorSourceNone;
      uintptr_t oldest_waiter_command_buffer = 0;
      uint32_t observed_waiters = 0;
      for (uint32_t waiter_index = 0;
           waiter_index < DXMT_METAL4_QUEUE_MONITOR_WAITER_CAPACITY;
           waiter_index++) {
        struct DXMTMetal4QueueMonitorWaiterDiagnostic *waiter =
            &diag->waiters[waiter_index];
        const uint64_t waiter_thread = atomic_load_explicit(
            &waiter->thread, memory_order_acquire);
        if (!waiter_thread || waiter_thread == UINT64_MAX)
          continue;
        const uint64_t waiter_since_us = atomic_load_explicit(
            &waiter->since_us, memory_order_relaxed);
        observed_waiters++;
        if (!oldest_waiter_since_us ||
            (waiter_since_us && waiter_since_us < oldest_waiter_since_us)) {
          oldest_waiter_thread = waiter_thread;
          oldest_waiter_since_us = waiter_since_us;
          oldest_waiter_source = atomic_load_explicit(
              &waiter->source, memory_order_relaxed);
          oldest_waiter_command_buffer = atomic_load_explicit(
              &waiter->command_buffer, memory_order_relaxed);
        }
      }

      const bool tracked_holder = owner_thread && acquired_us;
      const uint64_t blocked_since_us =
          tracked_holder ? acquired_us : oldest_waiter_since_us;
      if (!blocked_since_us || now_us <= blocked_since_us)
        continue;

      const uint64_t held_us = now_us - blocked_since_us;
      if (held_us < 250000)
        continue;

      const uint32_t report_count = atomic_load_explicit(
          &diag->report_count, memory_order_relaxed);
      const uint64_t last_report_us = atomic_load_explicit(
          &diag->last_report_us, memory_order_relaxed);
      if ((report_count == 1 && held_us < 1000000) ||
          (report_count >= 2 &&
           (held_us < 5000000 || now_us - last_report_us < 5000000)))
        continue;

      const uint64_t phase_us = atomic_load_explicit(
          &diag->phase_us, memory_order_relaxed);
      const uint32_t phase =
          atomic_load_explicit(&diag->phase, memory_order_relaxed);
      const uint32_t source =
          atomic_load_explicit(&diag->source, memory_order_relaxed);
      const uint64_t sequence = atomic_load_explicit(
          &diag->sequence, memory_order_relaxed);

      fprintf(stderr,
              "err:   DXMT Metal4 queue monitor blocked: queueObject=%p metalQueue=%p"
              " sequence=%" PRIu64 " holderTracked=%u holderTid=%" PRIu64
              " ownerDepth=%u"
              " source=%s phase=%s heldMs=%.3f phaseMs=%.3f"
              " commandBuffer=%p metalBuffer=%p completion=%" PRIu64
              " frame=%" PRIu64 " chunk=%" PRIu64
              " d3dSeq=%" PRIu64 "-%" PRIu64
              " subject=%p subjectAux=%p subjectCount=%" PRIu64
              " subjectFlags=0x%" PRIx64
              " waiterCount=%u waiterOverflow=%u"
              " oldestWaiterTid=%" PRIu64 " oldestWaiterSource=%s"
              " oldestWaiterMs=%.3f oldestWaiterCommandBuffer=%p"
              " lastHolderTid=%" PRIu64 " lastSource=%s lastPhase=%s"
              " lastDurationMs=%.3f lastReleasedAgoMs=%.3f report=%u\n",
              (void *)queue,
              (void *)atomic_load_explicit(&diag->metal_queue,
                                           memory_order_relaxed),
              sequence, tracked_holder, owner_thread,
              atomic_load_explicit(&diag->owner_depth,
                                   memory_order_relaxed),
              dxmt_metal4_queue_monitor_source_name(source),
              dxmt_metal4_queue_monitor_phase_name(phase),
              (double)held_us / 1000.0,
              phase_us && now_us > phase_us
                  ? (double)(now_us - phase_us) / 1000.0
                  : 0.0,
              (void *)atomic_load_explicit(&diag->command_buffer,
                                           memory_order_relaxed),
              (void *)atomic_load_explicit(&diag->metal_buffer,
                                           memory_order_relaxed),
              atomic_load_explicit(&diag->completion_value,
                                   memory_order_relaxed),
              atomic_load_explicit(&diag->frame_id, memory_order_relaxed),
              atomic_load_explicit(&diag->chunk_id, memory_order_relaxed),
              atomic_load_explicit(&diag->d3d_sequence_begin,
                                   memory_order_relaxed),
              atomic_load_explicit(&diag->d3d_sequence_end,
                                   memory_order_relaxed),
              (void *)atomic_load_explicit(&diag->subject,
                                           memory_order_relaxed),
              (void *)atomic_load_explicit(&diag->subject_aux,
                                           memory_order_relaxed),
              atomic_load_explicit(&diag->subject_count,
                                   memory_order_relaxed),
              atomic_load_explicit(&diag->subject_flags,
                                   memory_order_relaxed),
              observed_waiters,
              atomic_load_explicit(&diag->waiter_overflow,
                                   memory_order_relaxed),
              oldest_waiter_thread,
              dxmt_metal4_queue_monitor_source_name(oldest_waiter_source),
              oldest_waiter_thread && oldest_waiter_since_us &&
                      now_us > oldest_waiter_since_us
                  ? (double)(now_us - oldest_waiter_since_us) / 1000.0
                  : 0.0,
              (void *)oldest_waiter_command_buffer,
              atomic_load_explicit(&diag->last_owner_thread,
                                   memory_order_relaxed),
              dxmt_metal4_queue_monitor_source_name(atomic_load_explicit(
                  &diag->last_source, memory_order_relaxed)),
              dxmt_metal4_queue_monitor_phase_name(atomic_load_explicit(
                  &diag->last_phase, memory_order_relaxed)),
              (double)atomic_load_explicit(&diag->last_duration_us,
                                           memory_order_relaxed) /
                  1000.0,
              atomic_load_explicit(&diag->last_released_us,
                                   memory_order_relaxed) &&
                      now_us > atomic_load_explicit(
                                   &diag->last_released_us,
                                   memory_order_relaxed)
                  ? (double)(now_us - atomic_load_explicit(
                                          &diag->last_released_us,
                                          memory_order_relaxed)) /
                        1000.0
                  : 0.0,
              report_count + 1);
      for (uint32_t waiter_index = 0;
           waiter_index < DXMT_METAL4_QUEUE_MONITOR_WAITER_CAPACITY;
           waiter_index++) {
        struct DXMTMetal4QueueMonitorWaiterDiagnostic *waiter =
            &diag->waiters[waiter_index];
        const uint64_t waiter_thread = atomic_load_explicit(
            &waiter->thread, memory_order_acquire);
        if (!waiter_thread || waiter_thread == UINT64_MAX)
          continue;
        const uint64_t waiter_since_us = atomic_load_explicit(
            &waiter->since_us, memory_order_relaxed);
        fprintf(stderr,
                "err:   DXMT Metal4 queue monitor waiter: queueObject=%p"
                " slot=%u tid=%" PRIu64 " source=%s waitedMs=%.3f"
                " commandBuffer=%p\n",
                (void *)queue, waiter_index, waiter_thread,
                dxmt_metal4_queue_monitor_source_name(atomic_load_explicit(
                    &waiter->source, memory_order_relaxed)),
                waiter_since_us && now_us > waiter_since_us
                    ? (double)(now_us - waiter_since_us) / 1000.0
                    : 0.0,
                (void *)atomic_load_explicit(&waiter->command_buffer,
                                             memory_order_relaxed));
      }
      fflush(stderr);
      atomic_store_explicit(&diag->last_report_us, now_us,
                            memory_order_relaxed);
      atomic_store_explicit(&diag->report_count, report_count + 1,
                            memory_order_relaxed);
    }
  }
  return NULL;
}

static void
dxmt_metal4_queue_monitor_start_watchdog(void) {
  pthread_t thread;
  if (pthread_create(&thread, NULL, dxmt_metal4_queue_monitor_watchdog,
                     NULL) == 0)
    pthread_detach(thread);
}

static struct DXMTMetal4QueueMonitorDiagnostic *
dxmt_metal4_queue_monitor_register(uintptr_t queue, uintptr_t metal_queue) {
  if (!dxmt_metal4_queue_monitor_enabled())
    return NULL;
  pthread_once(&dxmt_metal4_queue_monitor_watchdog_once,
               dxmt_metal4_queue_monitor_start_watchdog);
  for (uint32_t i = 0;
       i < DXMT_METAL4_QUEUE_MONITOR_DIAGNOSTIC_CAPACITY; i++) {
    struct DXMTMetal4QueueMonitorDiagnostic *diag =
        &dxmt_metal4_queue_monitor_diagnostics[i];
    uintptr_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &diag->queue, &expected, UINTPTR_MAX, memory_order_acq_rel,
            memory_order_relaxed))
      continue;
    atomic_store_explicit(&diag->metal_queue, metal_queue,
                          memory_order_relaxed);
    atomic_store_explicit(&diag->sequence, 0, memory_order_relaxed);
    atomic_store_explicit(&diag->owner_thread, 0, memory_order_relaxed);
    atomic_store_explicit(&diag->owner_depth, 0, memory_order_relaxed);
    atomic_store_explicit(&diag->acquired_us, 0, memory_order_relaxed);
    atomic_store_explicit(&diag->phase_us, 0, memory_order_relaxed);
    atomic_store_explicit(&diag->phase,
                          DXMTMetal4QueueMonitorPhaseIdle,
                          memory_order_relaxed);
    atomic_store_explicit(&diag->source,
                          DXMTMetal4QueueMonitorSourceNone,
                          memory_order_relaxed);
    atomic_store_explicit(&diag->command_buffer, 0, memory_order_relaxed);
    atomic_store_explicit(&diag->metal_buffer, 0, memory_order_relaxed);
    atomic_store_explicit(&diag->completion_value, 0, memory_order_relaxed);
    atomic_store_explicit(&diag->frame_id, 0, memory_order_relaxed);
    atomic_store_explicit(&diag->chunk_id, 0, memory_order_relaxed);
    atomic_store_explicit(&diag->d3d_sequence_begin, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&diag->d3d_sequence_end, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&diag->subject, 0, memory_order_relaxed);
    atomic_store_explicit(&diag->subject_aux, 0, memory_order_relaxed);
    atomic_store_explicit(&diag->subject_count, 0, memory_order_relaxed);
    atomic_store_explicit(&diag->subject_flags, 0, memory_order_relaxed);
    atomic_store_explicit(&diag->last_owner_thread, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&diag->last_source,
                          DXMTMetal4QueueMonitorSourceNone,
                          memory_order_relaxed);
    atomic_store_explicit(&diag->last_phase,
                          DXMTMetal4QueueMonitorPhaseIdle,
                          memory_order_relaxed);
    atomic_store_explicit(&diag->last_duration_us, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&diag->last_released_us, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&diag->active_waiters, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&diag->waiter_overflow, 0,
                          memory_order_relaxed);
    for (uint32_t waiter_index = 0;
         waiter_index < DXMT_METAL4_QUEUE_MONITOR_WAITER_CAPACITY;
         waiter_index++) {
      struct DXMTMetal4QueueMonitorWaiterDiagnostic *waiter =
          &diag->waiters[waiter_index];
      atomic_store_explicit(&waiter->thread, 0, memory_order_relaxed);
      atomic_store_explicit(&waiter->since_us, 0, memory_order_relaxed);
      atomic_store_explicit(&waiter->source,
                            DXMTMetal4QueueMonitorSourceNone,
                            memory_order_relaxed);
      atomic_store_explicit(&waiter->command_buffer, 0,
                            memory_order_relaxed);
    }
    atomic_store_explicit(&diag->report_count, 0, memory_order_relaxed);
    atomic_store_explicit(&diag->last_report_us, 0, memory_order_relaxed);
    atomic_store_explicit(&diag->queue, queue, memory_order_release);
    return diag;
  }
  fprintf(stderr,
          "warn:  DXMT Metal4 queue monitor diagnostics capacity exhausted:"
          " queueObject=%p metalQueue=%p capacity=%u\n",
          (void *)queue, (void *)metal_queue,
          DXMT_METAL4_QUEUE_MONITOR_DIAGNOSTIC_CAPACITY);
  fflush(stderr);
  return NULL;
}

static void
dxmt_metal4_queue_monitor_unregister(
    struct DXMTMetal4QueueMonitorDiagnostic *diag) {
  if (!diag)
    return;
  atomic_store_explicit(&diag->queue, UINTPTR_MAX, memory_order_release);
  atomic_store_explicit(&diag->owner_thread, 0, memory_order_relaxed);
  atomic_store_explicit(&diag->owner_depth, 0, memory_order_relaxed);
  atomic_store_explicit(&diag->active_waiters, 0, memory_order_relaxed);
  atomic_store_explicit(&diag->metal_queue, 0, memory_order_relaxed);
  atomic_store_explicit(&diag->queue, 0, memory_order_release);
}

static uint64_t
dxmt_metal4_current_thread_id(void) {
  uint64_t thread_id = 0;
  pthread_threadid_np(NULL, &thread_id);
  return thread_id;
}

typedef NS_ENUM(uint64_t, DXMTMetal4CommandBufferState) {
  DXMTMetal4CommandBufferStateNotEnqueued = 0,
  DXMTMetal4CommandBufferStateCommitted = 2,
  DXMTMetal4CommandBufferStateCompleted = 4,
  DXMTMetal4CommandBufferStateError = 5,
};

enum { DXMT_METAL4_PRESENT_DIAGNOSTIC_CAPACITY = 32 };

/*
 * Scalar-only snapshot of the presentation hand-off.  Keeping Objective-C
 * objects in this ring would extend their lifetime and could hide the MMU
 * fault we are trying to diagnose, so every object is recorded as an address
 * and is never dereferenced after capture.
 */
struct DXMTMetal4PresentDiagnostic {
  uint64_t serial;
  uint64_t capture_us;
  uintptr_t command_buffer;
  uintptr_t metal_buffer;
  uintptr_t drawable;
  uintptr_t texture;
  uintptr_t parent_texture;
  uintptr_t buffer;
  uintptr_t heap;
  uintptr_t backing_allocation;
  uintptr_t layer;
  uintptr_t layer_device;
  uintptr_t layer_residency_set;
  uint64_t texture_gpu_resource_id;
  uint64_t completion_value;
  uint64_t previous_present_value;
  uint64_t current_present_value;
  uint64_t frame_id;
  uint64_t chunk_id;
  uint64_t d3d_sequence_begin;
  uint64_t d3d_sequence_end;
  uint64_t width;
  uint64_t height;
  uint64_t depth;
  uint64_t array_length;
  uint64_t mipmap_level_count;
  uint64_t sample_count;
  uint64_t pixel_format;
  uint64_t texture_type;
  uint64_t usage;
  uint64_t storage_mode;
  uint64_t hazard_tracking_mode;
  uint64_t layer_pixel_format;
  uint64_t layer_maximum_drawable_count;
  uint64_t layer_allocation_count;
  uint64_t command_buffer_allocation_count;
  double layer_width;
  double layer_height;
  double present_duration;
  uint8_t texture_framebuffer_only;
  uint8_t layer_framebuffer_only;
  uint8_t layer_contains_texture;
  uint8_t layer_contains_backing;
  uint8_t command_buffer_contains_texture;
  uint8_t command_buffer_contains_backing;
  uint8_t wait_for_drawable;
  uint8_t present_ordering;
  uint8_t has_present_duration;
};

@interface DXMTMetal4CommandBuffer : NSObject
@property(nonatomic, retain) DXMTMetal4CommandQueue *owner;
@property(nonatomic, retain) id<MTL4CommandAllocator> allocator;
@property(nonatomic, retain) id<MTL4CommandBuffer> metal4Buffer;
@property(nonatomic, retain) NSMutableArray *pendingWaitEvents;
@property(nonatomic, retain) NSMutableArray *pendingSignalEvents;
@property(nonatomic, retain) NSMutableArray *retainedTemporaryResources;
// Lifetime snapshot handed off by the Metal completion callback and drained by
// the waiter (dxmt-finish-thr). Never released on com.Metal4.CompletionQueue:
// that GCD thread has no Wine TEB, and ObjC free of GPU resources there has
// caused EXC_BAD_ACCESS + ntdll save_context faults.
@property(nonatomic, retain) NSArray *pendingLifetimeSnapshot;
@property(nonatomic, retain) NSMutableArray *queueResidencyAllocations;
@property(nonatomic, retain) NSMutableSet *queueResidencyAllocationSet;
@property(nonatomic, retain) NSCondition *feedbackCondition;
@property(nonatomic, assign) BOOL feedbackComplete;
@property(nonatomic, retain) id<MTLDrawable> pendingDrawable;
@property(nonatomic, assign) BOOL hasPresentDuration;
@property(nonatomic, assign) double presentDuration;
@property(nonatomic, assign) uint64_t completionValue;
@property(nonatomic, assign) DXMTMetal4CommandBufferState internalStatus;
@property(nonatomic, retain) NSError *feedbackError;
@property(nonatomic, assign) double feedbackGPUStartTime;
@property(nonatomic, assign) double feedbackGPUEndTime;
@property(nonatomic, assign) uint64_t completionCurrentBeforeThrottle;
@property(nonatomic, assign) uint64_t completionCurrentAtCommit;
@property(nonatomic, assign) BOOL metal4ObjectsRecycled;
@property(nonatomic, assign) struct WMTCommandBufferDiagnosticInfo diagnosticInfo;
- (void)recycleMetal4ObjectsIfNeeded;
- (instancetype)initWithQueue:(DXMTMetal4CommandQueue *)queue;
- (id<MTL4CommandBuffer>)commandBuffer;
- (id<MTL4ComputeCommandEncoder>)metal4ComputeEncoder;
- (id<MTL4RenderCommandEncoder>)metal4RenderCommandEncoderWithDescriptor:(MTL4RenderPassDescriptor *)descriptor;
- (void)registerResourceForCurrentCommandBuffer:(id<MTLAllocation>)allocation;
- (void)retainTemporaryResourceForCurrentCommandBuffer:(id<MTLAllocation>)allocation;
- (void)drainPendingLifetimeOwnership;
- (id<MTL4ArgumentTable>)newArgumentTableWithLabel:(NSString *)label;
- (id<MTLBuffer>)newUploadBufferWithBytes:(const void *)bytes length:(NSUInteger)length;
- (void)commit;
- (uint64_t)commitLocked;
- (void)waitUntilCompleted;
- (enum WMTCommandBufferStatus)status;
- (NSError *)error;
- (id)logs;
- (double)kernelStartTime;
- (double)kernelEndTime;
- (double)GPUStartTime;
- (double)GPUEndTime;
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value;
- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value;
- (void)presentDrawable:(id<MTLDrawable>)drawable;
- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(double)duration;
- (void)dxmtWriteTimestampIntoHeap:(id<MTL4CounterHeap>)heap atIndex:(NSUInteger)index;
- (void)dxmtResolveCounterHeap:(id<MTL4CounterHeap>)heap
                      withRange:(NSRange)range
                     intoBuffer:(id<MTLBuffer>)buffer
                         offset:(uint64_t)offset
                         length:(uint64_t)length
                      waitFence:(id<MTLFence>)fenceToWait
                    updateFence:(id<MTLFence>)fenceToUpdate;
@end

@interface DXMTMetal4QueueEvent : NSObject
@property(nonatomic, retain) id<MTLEvent> event;
@property(nonatomic, assign) uint64_t value;
- (instancetype)initWithEvent:(id<MTLEvent>)event value:(uint64_t)value;
@end

@implementation DXMTMetal4QueueEvent
- (instancetype)initWithEvent:(id<MTLEvent>)event value:(uint64_t)value {
  self = [super init];
  if (self) {
    _event = [event retain];
    _value = value;
  }
  return self;
}

- (void)dealloc {
  [_event release];
  [super dealloc];
}
@end

static BOOL
dxmt_metal4_validate_event(id event, const char *usage, uint64_t value) {
  if (event && [event conformsToProtocol:@protocol(MTLEvent)])
    return YES;

  fprintf(stderr,
          "err:   DXMT Metal4 invalid %s event object=%p class=%s value=%" PRIu64 "\n",
          usage,
          event,
          event ? object_getClassName(event) : "<nil>",
          value);
  return NO;
}

static uint64_t
dxmt_metal4_shared_event_value(id<MTLEvent> event) {
  if (!event || ![event respondsToSelector:@selector(signaledValue)])
    return 0;
  return [(id<MTLSharedEvent>)event signaledValue];
}

#if DXMT_APITRACE_METAL
static bool dxmt_apitrace_runtime_enabled(void);
static void dxmt_apitrace_record_command_buffer_commit_state(
    obj_handle_t command_buffer,
    apitrace_metal_command_buffer_commit_phase phase,
    uint64_t wait_event_count,
    uint64_t signal_event_count,
    BOOL has_drawable,
    uint64_t completion_value,
    uint64_t d3d_sequence);
static void dxmt_apitrace_record_command_buffer_feedback(
    obj_handle_t command_buffer,
    apitrace_metal_command_buffer_feedback_status status,
    double gpu_start_time,
    double gpu_end_time,
    NSString *error);
static void dxmt_apitrace_record_command_buffer_event(
    obj_handle_t command_buffer,
    apitrace_metal_queue_event_op op,
    obj_handle_t event,
    uint64_t value,
    apitrace_metal_queue_event_phase phase);
static void dxmt_apitrace_record_counter_event(
    obj_handle_t command_buffer,
    apitrace_metal_counter_event_op op,
    obj_handle_t counter_heap,
    uint64_t start,
    uint64_t count,
    obj_handle_t destination_buffer,
    uint64_t destination_offset,
    uint64_t destination_length,
    obj_handle_t wait_fence,
    obj_handle_t update_fence);
static void dxmt_apitrace_record_buffer_binding(
    apitrace_metal_session_t *session,
    obj_handle_t encoder,
    apitrace_metal_stage_kind stage,
    obj_handle_t buffer,
    uint64_t offset,
    uint8_t index);
static void dxmt_apitrace_record_texture_binding(
    apitrace_metal_session_t *session,
    obj_handle_t encoder,
    apitrace_metal_stage_kind stage,
    obj_handle_t texture,
    uint8_t index);
static void dxmt_apitrace_record_indirect_arguments(
    apitrace_metal_session_t *session,
    obj_handle_t encoder,
    apitrace_metal_indirect_arguments_info_t *info);
static void dxmt_apitrace_record_emulated_blit(
    apitrace_metal_session_t *session,
    obj_handle_t encoder,
    apitrace_metal_emulated_blit_op op,
    obj_handle_t source_texture,
    obj_handle_t destination_buffer,
    uint64_t destination_offset,
    uint64_t bytes_per_row,
    uint64_t bytes_per_image,
    uint64_t slice,
    uint64_t level);
static void dxmt_apitrace_finalize_command_buffer(obj_handle_t command_buffer);
#else
enum {
  APITRACE_METAL_STAGE_VERTEX = 1,
  APITRACE_METAL_STAGE_FRAGMENT = 2,
  APITRACE_METAL_STAGE_COMPUTE = 3,
  APITRACE_METAL_STAGE_RENDER = 4,
  APITRACE_METAL_STAGE_BLIT = 5,
  APITRACE_METAL_STAGE_OBJECT = 6,
  APITRACE_METAL_STAGE_MESH = 7,
  APITRACE_METAL_STAGE_TILE = 8,
};

enum {
  APITRACE_METAL_COMMAND_BUFFER_COMMIT_BEGIN = 1,
  APITRACE_METAL_COMMAND_BUFFER_COMMIT_RECORDED_BEFORE_NATIVE_COMMIT = 2,
  APITRACE_METAL_COMMAND_BUFFER_COMMIT_SUBMITTED = 3,
};

enum {
  APITRACE_METAL_COMMAND_BUFFER_FEEDBACK_COMPLETED = 1,
  APITRACE_METAL_COMMAND_BUFFER_FEEDBACK_ERROR = 2,
};

enum {
  APITRACE_METAL_QUEUE_EVENT_WAIT = 1,
  APITRACE_METAL_QUEUE_EVENT_SIGNAL = 2,
};

enum {
  APITRACE_METAL_QUEUE_EVENT_RECORDED = 1,
  APITRACE_METAL_QUEUE_EVENT_ENQUEUED = 2,
};

enum {
  APITRACE_METAL_INDIRECT_DRAW = 1,
  APITRACE_METAL_INDIRECT_DRAW_INDEXED = 2,
  APITRACE_METAL_INDIRECT_DRAW_MESH_THREADGROUPS = 3,
  APITRACE_METAL_INDIRECT_DISPATCH = 4,
  APITRACE_METAL_INDIRECT_GEOMETRY_DRAW = 5,
  APITRACE_METAL_INDIRECT_GEOMETRY_DRAW_INDEXED = 6,
  APITRACE_METAL_INDIRECT_TESSELLATION_MESH_DRAW = 7,
  APITRACE_METAL_INDIRECT_TESSELLATION_MESH_DRAW_INDEXED = 8,
};

enum {
  APITRACE_METAL_COUNTER_WRITE_TIMESTAMP = 1,
  APITRACE_METAL_COUNTER_RESOLVE_HEAP = 2,
};

enum {
  APITRACE_METAL_EMULATED_BLIT_COPY_TEXTURE_TO_BUFFER = 1,
  APITRACE_METAL_EMULATED_BLIT_GENERATE_MIPMAPS = 2,
  APITRACE_METAL_EMULATED_BLIT_RESOLVE_COUNTERS = 3,
};

typedef struct apitrace_metal_indirect_arguments_info {
  uint32_t op;
  uint32_t stage;
  uint64_t indirect_buffer_id;
  uint64_t indirect_offset;
  uint64_t indirect_gpu_address;
  uint64_t indirect_buffer_length;
  uint64_t draw_arguments_buffer_id;
  uint64_t draw_arguments_offset;
  uint64_t dispatch_arguments_buffer_id;
  uint64_t dispatch_arguments_offset;
  uint64_t immediate_arguments_buffer_id;
  uint64_t index_buffer_id;
  uint64_t index_buffer_offset;
  uint32_t primitive_type;
  uint32_t index_type;
  uint32_t threadgroup_width;
  uint32_t threadgroup_height;
  uint32_t threadgroup_depth;
  uint32_t object_threadgroup_width;
  uint32_t object_threadgroup_height;
  uint32_t object_threadgroup_depth;
  uint32_t mesh_threadgroup_width;
  uint32_t mesh_threadgroup_height;
  uint32_t mesh_threadgroup_depth;
  uint32_t vertex_per_warp;
  uint32_t threads_per_patch;
  uint32_t patch_per_group;
} apitrace_metal_indirect_arguments_info_t;

static void
dxmt_apitrace_record_command_buffer_commit_state(
    obj_handle_t command_buffer,
    int phase,
    uint64_t wait_event_count,
    uint64_t signal_event_count,
    BOOL has_drawable,
    uint64_t completion_value,
    uint64_t d3d_sequence) {
  (void)command_buffer;
  (void)phase;
  (void)wait_event_count;
  (void)signal_event_count;
  (void)has_drawable;
  (void)completion_value;
  (void)d3d_sequence;
}

static void
dxmt_apitrace_record_command_buffer_feedback(
    obj_handle_t command_buffer,
    int status,
    double gpu_start_time,
    double gpu_end_time,
    NSString *error) {
  (void)command_buffer;
  (void)status;
  (void)gpu_start_time;
  (void)gpu_end_time;
  (void)error;
}

static void
dxmt_apitrace_record_command_buffer_event(
    obj_handle_t command_buffer,
    int op,
    obj_handle_t event,
    uint64_t value,
    int phase) {
  (void)command_buffer;
  (void)op;
  (void)event;
  (void)value;
  (void)phase;
}

static void
dxmt_apitrace_record_counter_event(
    obj_handle_t command_buffer,
    int op,
    obj_handle_t counter_heap,
    uint64_t start,
    uint64_t count,
    obj_handle_t destination_buffer,
    uint64_t destination_offset,
    uint64_t destination_length,
    obj_handle_t wait_fence,
    obj_handle_t update_fence) {
  (void)command_buffer;
  (void)op;
  (void)counter_heap;
  (void)start;
  (void)count;
  (void)destination_buffer;
  (void)destination_offset;
  (void)destination_length;
  (void)wait_fence;
  (void)update_fence;
}

static void
dxmt_apitrace_record_buffer_binding(
    void *session,
    obj_handle_t encoder,
    int stage,
    obj_handle_t buffer,
    uint64_t offset,
    uint8_t index) {
  (void)session;
  (void)encoder;
  (void)stage;
  (void)buffer;
  (void)offset;
  (void)index;
}

static void
dxmt_apitrace_record_texture_binding(
    void *session,
    obj_handle_t encoder,
    int stage,
    obj_handle_t texture,
    uint8_t index) {
  (void)session;
  (void)encoder;
  (void)stage;
  (void)texture;
  (void)index;
}

static void
dxmt_apitrace_record_indirect_arguments(
    void *session,
    obj_handle_t encoder,
    apitrace_metal_indirect_arguments_info_t *info) {
  (void)session;
  (void)encoder;
  (void)info;
}

static void
dxmt_apitrace_record_emulated_blit(
    void *session,
    obj_handle_t encoder,
    int op,
    obj_handle_t source_texture,
    obj_handle_t destination_buffer,
    uint64_t destination_offset,
    uint64_t bytes_per_row,
    uint64_t bytes_per_image,
    uint64_t slice,
    uint64_t level) {
  (void)session;
  (void)encoder;
  (void)op;
  (void)source_texture;
  (void)destination_buffer;
  (void)destination_offset;
  (void)bytes_per_row;
  (void)bytes_per_image;
  (void)slice;
  (void)level;
}

static void
dxmt_apitrace_finalize_command_buffer(obj_handle_t command_buffer) {
  (void)command_buffer;
}
#endif

static NSMutableDictionary *dxmt_metal4_compilers_by_device;

__attribute__((constructor)) static void
dxmt_metal4_init_locks(void) {
  dxmt_metal4_compiler_lock = [NSObject new];
}

static NSValue *
dxmt_metal4_device_key(id<MTLDevice> device) {
  return [NSValue valueWithPointer:(const void *)device];
}

static void
dxmt_metal4_register_compiler(id<MTLDevice> device, id<MTL4Compiler> compiler) {
  @synchronized(dxmt_metal4_compiler_lock) {
    if (!dxmt_metal4_compilers_by_device)
      dxmt_metal4_compilers_by_device = [[NSMutableDictionary alloc] init];
    [dxmt_metal4_compilers_by_device setObject:compiler forKey:dxmt_metal4_device_key(device)];
  }
}

static id<MTL4Compiler>
dxmt_metal4_compiler_for_device(id<MTLDevice> device) {
  @synchronized(dxmt_metal4_compiler_lock) {
    return [dxmt_metal4_compilers_by_device objectForKey:dxmt_metal4_device_key(device)];
  }
}

static NSMutableDictionary *dxmt_metal4_encoder_owners;
static NSMutableDictionary *dxmt_metal4_encoder_states;

static NSValue *
dxmt_metal4_encoder_key(obj_handle_t encoder) {
  return [NSValue valueWithPointer:(const void *)encoder];
}

static void
dxmt_metal4_register_encoder(obj_handle_t encoder, DXMTMetal4CommandBuffer *command_buffer) {
  @synchronized([DXMTMetal4CommandBuffer class]) {
    if (!dxmt_metal4_encoder_owners)
      dxmt_metal4_encoder_owners = [[NSMutableDictionary alloc] init];
    [dxmt_metal4_encoder_owners setObject:command_buffer forKey:dxmt_metal4_encoder_key(encoder)];
  }
}

static void
dxmt_metal4_register_encoder_state(obj_handle_t encoder, NSObject *state) {
  @synchronized([DXMTMetal4CommandBuffer class]) {
    if (!dxmt_metal4_encoder_states)
      dxmt_metal4_encoder_states = [[NSMutableDictionary alloc] init];
    [dxmt_metal4_encoder_states setObject:state forKey:dxmt_metal4_encoder_key(encoder)];
  }
}

static id
dxmt_metal4_encoder_state(obj_handle_t encoder) {
  @synchronized([DXMTMetal4CommandBuffer class]) {
    return [dxmt_metal4_encoder_states objectForKey:dxmt_metal4_encoder_key(encoder)];
  }
}

static DXMTMetal4CommandBuffer *
dxmt_metal4_encoder_owner(obj_handle_t encoder) {
  @synchronized([DXMTMetal4CommandBuffer class]) {
    return [dxmt_metal4_encoder_owners objectForKey:dxmt_metal4_encoder_key(encoder)];
  }
}

static void
dxmt_metal4_unregister_encoder(obj_handle_t encoder) {
  @synchronized([DXMTMetal4CommandBuffer class]) {
    [dxmt_metal4_encoder_owners removeObjectForKey:dxmt_metal4_encoder_key(encoder)];
    [dxmt_metal4_encoder_states removeObjectForKey:dxmt_metal4_encoder_key(encoder)];
  }
}

static BOOL
dxmt_metal4_is_residency_set(id object) {
  return object && [object conformsToProtocol:@protocol(MTLResidencySet)];
}

static BOOL
dxmt_metal4_is_allocation(id object) {
  return object && [object conformsToProtocol:@protocol(MTLAllocation)];
}

static BOOL
dxmt_metal4_is_resource_view_pool(id object) {
  return object && [object conformsToProtocol:@protocol(MTLResourceViewPool)];
}

static BOOL
dxmt_metal4_is_texture_view_pool(id object) {
  return object && [object conformsToProtocol:@protocol(MTLTextureViewPool)];
}

static BOOL
dxmt_metal4_is_texture(id object) {
  return object && [object conformsToProtocol:@protocol(MTLTexture)];
}

static BOOL
dxmt_metal4_is_buffer(id object) {
  return object && [object conformsToProtocol:@protocol(MTLBuffer)];
}


@interface DXMTMetal4CommandQueue : NSObject {
  struct DXMTMetal4PresentDiagnostic
      _presentDiagnostics[DXMT_METAL4_PRESENT_DIAGNOSTIC_CAPACITY];
  uint64_t _presentDiagnosticSerial;
  struct DXMTMetal4QueueMonitorDiagnostic *_monitorDiagnostic;
}
@property(nonatomic, retain) id<MTLDevice> device;
@property(nonatomic, retain) id<MTL4CommandQueue> metal4Queue;
@property(nonatomic, retain) id<MTL4Compiler> compiler;
@property(nonatomic, retain) id<MTLSharedEvent> event;
@property(nonatomic, retain) id<MTLSharedEvent> presentEvent;
@property(nonatomic, assign) uint64_t eventValue;
@property(nonatomic, assign) uint64_t presentEventValue;
@property(nonatomic, assign) uint64_t maxCommandBufferCount;
@property(nonatomic, assign) uint64_t commandBufferThrottleWaitCount;
@property(nonatomic, copy) NSString *testFeedbackErrorToken;
@property(nonatomic, retain) NSError *firstError;
@property(nonatomic, retain) id<MTLResidencySet> sparseResidencySet;
@property(nonatomic, retain) NSCountedSet *sparseResidencyAllocationRefs;
@property(nonatomic, retain) NSMutableSet *sparseTrackedTextures;
@property(nonatomic, retain) NSMutableArray *sparseResidencyRetirements;
@property(nonatomic, retain) id<MTLResidencySet> commandResidencySet;
@property(nonatomic, retain) NSCountedSet *commandResidencyAllocationRefs;
@property(nonatomic, retain) NSMutableArray *commandResidencyRetirements;
@property(nonatomic, retain) NSLock *eventLock;
@property(nonatomic, retain) NSLock *presentLock;
@property(nonatomic, retain) NSLock *throttleLock;
@property(nonatomic, retain) NSLock *submissionLock;
@property(nonatomic, retain) NSLock *errorLock;
@property(nonatomic, retain) NSLock *sparseResidencyLock;
@property(nonatomic, retain) NSMutableArray *externalLifetimeResidencySets;
/* GPTK-style MTL4 object pools (returned only after GPU completion). */
@property(nonatomic, retain) NSLock *metal4ObjectPoolLock;
@property(nonatomic, retain) NSMutableArray *metal4CommandBufferPool;
@property(nonatomic, retain) NSMutableArray *metal4AllocatorPool;
- (instancetype)initWithDevice:(id<MTLDevice>)device maxCommandBufferCount:(uint64_t)maxCommandBufferCount;
- (uint64_t)nextEventValueLocked;
- (void)waitForCommandBufferSlotLocked:(uint64_t)completionValue;
- (void)retireCompletedResidency;
- (uint64_t)prepareCommandResidencyForAllocations:(NSArray *)allocations
                                  completionValue:(uint64_t)completionValue;
- (id<MTL4CommandAllocator>)acquireCommandAllocator;
- (id<MTL4CommandBuffer>)acquireCommandBuffer;
- (void)recycleCommandBuffer:(id<MTL4CommandBuffer>)commandBuffer
                   allocator:(id<MTL4CommandAllocator>)allocator;
- (void)monitorWaitBegin:(DXMTMetal4QueueMonitorSource)source
           commandBuffer:(DXMTMetal4CommandBuffer *)commandBuffer;
- (void)monitorWaitEnd:(DXMTMetal4QueueMonitorSource)source
         commandBuffer:(DXMTMetal4CommandBuffer *)commandBuffer;
- (void)monitorAcquired:(DXMTMetal4QueueMonitorSource)source
                  phase:(DXMTMetal4QueueMonitorPhase)phase
          commandBuffer:(DXMTMetal4CommandBuffer *)commandBuffer;
- (void)monitorSetPhase:(DXMTMetal4QueueMonitorPhase)phase
          commandBuffer:(DXMTMetal4CommandBuffer *)commandBuffer;
- (void)monitorSetSubject:(uintptr_t)subject
                      aux:(uintptr_t)subjectAux
                    count:(uint64_t)subjectCount
                    flags:(uint64_t)subjectFlags;
- (void)monitorReleased;
- (const char *)monitorCurrentPhaseName;
- (uint64_t)monitorCurrentCompletionValue;
- (void)recordPresentDiagnosticForCommandBuffer:
            (DXMTMetal4CommandBuffer *)commandBuffer
                            previousValue:(uint64_t)previousValue
                             currentValue:(uint64_t)currentValue
                          waitForDrawable:(BOOL)waitForDrawable
                          presentOrdering:(BOOL)presentOrdering;
- (void)dumpPresentDiagnosticsForErrorCommandBuffer:(obj_handle_t)commandBuffer;
@end

@implementation DXMTMetal4CommandQueue
- (instancetype)initWithDevice:(id<MTLDevice>)device maxCommandBufferCount:(uint64_t)maxCommandBufferCount {
  self = [super init];
  if (!self)
    return nil;

  _device = [device retain];
  _metal4Queue = [(id<MTLDevice>)device newMTL4CommandQueue];
  NSError *compilerError = nil;
  MTL4CompilerDescriptor *compilerDesc = [[MTL4CompilerDescriptor alloc] init];
  _compiler = [(id<MTLDevice>)device newCompilerWithDescriptor:compilerDesc error:&compilerError];
  [compilerDesc release];
  _event = [(id<MTLDevice>)device newSharedEvent];
  _presentEvent = [(id<MTLDevice>)device newSharedEvent];
  _eventValue = 0;
  _presentEventValue = 0;
  _maxCommandBufferCount = maxCommandBufferCount;
  _commandBufferThrottleWaitCount = 0;
  _externalLifetimeResidencySets = [[NSMutableArray alloc] init];
  _eventLock = [[NSLock alloc] init];
  _presentLock = [[NSLock alloc] init];
  _throttleLock = [[NSLock alloc] init];
  _submissionLock = [[NSLock alloc] init];
  _errorLock = [[NSLock alloc] init];
  _sparseResidencyLock = [[NSLock alloc] init];
  _metal4ObjectPoolLock = [[NSLock alloc] init];
  _metal4CommandBufferPool = [[NSMutableArray alloc] init];
  _metal4AllocatorPool = [[NSMutableArray alloc] init];
  _monitorDiagnostic = dxmt_metal4_queue_monitor_register(
      (uintptr_t)self, (uintptr_t)_metal4Queue);
  MTLResidencySetDescriptor *sparseResidencyDescriptor =
      [[MTLResidencySetDescriptor alloc] init];
  sparseResidencyDescriptor.label = @"DXMT sparse queue residency";
  sparseResidencyDescriptor.initialCapacity = 1024;
  NSError *sparseResidencyError = nil;
  _sparseResidencySet = [device
      newResidencySetWithDescriptor:sparseResidencyDescriptor
                              error:&sparseResidencyError];
  sparseResidencyDescriptor.label = @"DXMT command resource residency";
  NSError *commandResidencyError = nil;
  _commandResidencySet = [device
      newResidencySetWithDescriptor:sparseResidencyDescriptor
                              error:&commandResidencyError];
  [sparseResidencyDescriptor release];
  _sparseResidencyAllocationRefs = [[NSCountedSet alloc] init];
  _sparseTrackedTextures = [[NSMutableSet alloc] init];
  _sparseResidencyRetirements = [[NSMutableArray alloc] init];
  _commandResidencyAllocationRefs = [[NSCountedSet alloc] init];
  _commandResidencyRetirements = [[NSMutableArray alloc] init];

  if (!_metal4Queue || !_compiler || !_event || !_presentEvent ||
      !_sparseResidencySet || !_commandResidencySet ||
      !_sparseResidencyAllocationRefs || !_eventLock || !_presentLock ||
      !_throttleLock || !_submissionLock || !_errorLock ||
      !_sparseResidencyLock || !_commandResidencyAllocationRefs ||
      !_commandResidencyRetirements || !_sparseTrackedTextures ||
      !_sparseResidencyRetirements || !_externalLifetimeResidencySets ||
      !_metal4ObjectPoolLock || !_metal4CommandBufferPool ||
      !_metal4AllocatorPool) {
    [self release];
    return nil;
  }

  [_sparseResidencySet commit];
  [_sparseResidencySet requestResidency];
  [_metal4Queue addResidencySet:_sparseResidencySet];
  [_commandResidencySet commit];
  [_commandResidencySet requestResidency];
  [_metal4Queue addResidencySet:_commandResidencySet];

  dxmt_iogpu_diagnostics_install();
  dxmt_metal4_register_compiler(device, _compiler);
  return self;
}

- (void)dealloc {
  dxmt_metal4_queue_monitor_unregister(_monitorDiagnostic);
  _monitorDiagnostic = NULL;
  if (_metal4Queue && _sparseResidencySet)
    [_metal4Queue removeResidencySet:_sparseResidencySet];
  if (_metal4Queue && _commandResidencySet)
    [_metal4Queue removeResidencySet:_commandResidencySet];
  [_sparseResidencySet endResidency];
  [_commandResidencySet endResidency];
  [_commandResidencyRetirements release];
  [_commandResidencyAllocationRefs release];
  [_commandResidencySet release];
  [_sparseTrackedTextures release];
  [_sparseResidencyRetirements release];
  [_sparseResidencyAllocationRefs release];
  [_sparseResidencySet release];
  [_sparseResidencyLock release];
  [_metal4CommandBufferPool release];
  [_metal4AllocatorPool release];
  [_metal4ObjectPoolLock release];
  [_externalLifetimeResidencySets release];
  [_errorLock release];
  [_submissionLock release];
  [_throttleLock release];
  [_presentLock release];
  [_eventLock release];
  [_device release];
  [_metal4Queue release];
  [_compiler release];
  [_event release];
  [_presentEvent release];
  [_testFeedbackErrorToken release];
  [_firstError release];
  [super dealloc];
}

enum { DXMT_METAL4_OBJECT_POOL_CAPACITY = 8 };

- (id<MTL4CommandAllocator>)acquireCommandAllocator {
  [_metal4ObjectPoolLock lock];
  id<MTL4CommandAllocator> allocator =
      (id<MTL4CommandAllocator>)[_metal4AllocatorPool lastObject];
  if (allocator) {
    [allocator retain];
    [_metal4AllocatorPool removeLastObject];
    [_metal4ObjectPoolLock unlock];
    return [allocator autorelease];
  }
  [_metal4ObjectPoolLock unlock];
  return [[_device newCommandAllocator] autorelease];
}

- (id<MTL4CommandBuffer>)acquireCommandBuffer {
  [_metal4ObjectPoolLock lock];
  id<MTL4CommandBuffer> buffer =
      (id<MTL4CommandBuffer>)[_metal4CommandBufferPool lastObject];
  if (buffer) {
    [buffer retain];
    [_metal4CommandBufferPool removeLastObject];
    [_metal4ObjectPoolLock unlock];
    return [buffer autorelease];
  }
  [_metal4ObjectPoolLock unlock];
  return [[_device newCommandBuffer] autorelease];
}

- (void)recycleCommandBuffer:(id<MTL4CommandBuffer>)commandBuffer
                   allocator:(id<MTL4CommandAllocator>)allocator {
  // Match GPTK Get/ReturnMTL4CommandBuffer: recycle only after GPU completion.
  // Drop objects when the pool is full so teardown does not accumulate.
  [_metal4ObjectPoolLock lock];
  if (commandBuffer &&
      _metal4CommandBufferPool.count < DXMT_METAL4_OBJECT_POOL_CAPACITY)
    [_metal4CommandBufferPool addObject:commandBuffer];
  if (allocator &&
      _metal4AllocatorPool.count < DXMT_METAL4_OBJECT_POOL_CAPACITY)
    [_metal4AllocatorPool addObject:allocator];
  [_metal4ObjectPoolLock unlock];
}

- (uint64_t)nextEventValueLocked {
  [_eventLock lock];
  const uint64_t value = ++_eventValue;
  [_eventLock unlock];
  return value;
}

- (void)monitorWaitBegin:(DXMTMetal4QueueMonitorSource)source
           commandBuffer:(DXMTMetal4CommandBuffer *)commandBuffer {
  if (!_monitorDiagnostic)
    return;
  const uint64_t thread_id = dxmt_metal4_current_thread_id();
  for (uint32_t waiter_index = 0;
       waiter_index < DXMT_METAL4_QUEUE_MONITOR_WAITER_CAPACITY;
       waiter_index++) {
    struct DXMTMetal4QueueMonitorWaiterDiagnostic *waiter =
        &_monitorDiagnostic->waiters[waiter_index];
    uint64_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &waiter->thread, &expected, UINT64_MAX, memory_order_acq_rel,
            memory_order_relaxed))
      continue;
    atomic_store_explicit(&waiter->source, source, memory_order_relaxed);
    atomic_store_explicit(&waiter->command_buffer,
                          (uintptr_t)commandBuffer,
                          memory_order_relaxed);
    atomic_store_explicit(&waiter->since_us, dxmt_monotonic_us(),
                          memory_order_relaxed);
    const uint32_t prior_waiters = atomic_fetch_add_explicit(
        &_monitorDiagnostic->active_waiters, 1, memory_order_relaxed);
    if (!prior_waiters && !atomic_load_explicit(
                              &_monitorDiagnostic->owner_thread,
                              memory_order_acquire)) {
      atomic_store_explicit(&_monitorDiagnostic->report_count, 0,
                            memory_order_relaxed);
      atomic_store_explicit(&_monitorDiagnostic->last_report_us, 0,
                            memory_order_relaxed);
    }
    atomic_store_explicit(&waiter->thread, thread_id,
                          memory_order_release);
    return;
  }
  atomic_fetch_add_explicit(&_monitorDiagnostic->waiter_overflow, 1,
                            memory_order_relaxed);
}

- (void)monitorWaitEnd:(DXMTMetal4QueueMonitorSource)source
         commandBuffer:(DXMTMetal4CommandBuffer *)commandBuffer {
  if (!_monitorDiagnostic)
    return;
  const uint64_t thread_id = dxmt_metal4_current_thread_id();
  for (uint32_t waiter_index = 0;
       waiter_index < DXMT_METAL4_QUEUE_MONITOR_WAITER_CAPACITY;
       waiter_index++) {
    struct DXMTMetal4QueueMonitorWaiterDiagnostic *waiter =
        &_monitorDiagnostic->waiters[waiter_index];
    if (atomic_load_explicit(&waiter->thread, memory_order_acquire) !=
            thread_id ||
        atomic_load_explicit(&waiter->source, memory_order_relaxed) !=
            source ||
        atomic_load_explicit(&waiter->command_buffer,
                             memory_order_relaxed) !=
            (uintptr_t)commandBuffer)
      continue;
    atomic_store_explicit(&waiter->since_us, 0, memory_order_relaxed);
    atomic_store_explicit(&waiter->source,
                          DXMTMetal4QueueMonitorSourceNone,
                          memory_order_relaxed);
    atomic_store_explicit(&waiter->command_buffer, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&waiter->thread, 0, memory_order_release);
    atomic_fetch_sub_explicit(&_monitorDiagnostic->active_waiters, 1,
                              memory_order_relaxed);
    return;
  }
}

- (void)monitorAcquired:(DXMTMetal4QueueMonitorSource)source
                  phase:(DXMTMetal4QueueMonitorPhase)phase
          commandBuffer:(DXMTMetal4CommandBuffer *)commandBuffer {
  if (!_monitorDiagnostic)
    return;
  struct WMTCommandBufferDiagnosticInfo info = {0};
  if (commandBuffer)
    info = commandBuffer.diagnosticInfo;
  const uint64_t now_us = dxmt_monotonic_us();
  const uint64_t thread_id = dxmt_metal4_current_thread_id();
  const uint64_t existing_owner = atomic_load_explicit(
      &_monitorDiagnostic->owner_thread, memory_order_acquire);
  if (existing_owner == thread_id &&
      atomic_load_explicit(&_monitorDiagnostic->owner_depth,
                           memory_order_relaxed)) {
    atomic_fetch_add_explicit(&_monitorDiagnostic->owner_depth, 1,
                              memory_order_relaxed);
    atomic_store_explicit(&_monitorDiagnostic->source, source,
                          memory_order_relaxed);
    atomic_store_explicit(&_monitorDiagnostic->phase, phase,
                          memory_order_relaxed);
    atomic_store_explicit(&_monitorDiagnostic->phase_us, now_us,
                          memory_order_release);
    return;
  }
  atomic_fetch_add_explicit(&_monitorDiagnostic->sequence, 1,
                            memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->source, source,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->phase, phase,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->phase_us, now_us,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->acquired_us, now_us,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->command_buffer,
                        (uintptr_t)commandBuffer, memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->metal_buffer,
                        commandBuffer
                            ? (uintptr_t)commandBuffer.metal4Buffer
                            : 0,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->completion_value,
                        commandBuffer ? commandBuffer.completionValue : 0,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->frame_id,
                        info.frame_id, memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->chunk_id,
                        info.chunk_id, memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->d3d_sequence_begin,
                        info.d3d_sequence_begin,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->d3d_sequence_end,
                        info.d3d_sequence_end,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->subject, 0,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->subject_aux, 0,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->subject_count, 0,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->subject_flags, 0,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->report_count, 0,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->last_report_us, 0,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->owner_depth, 1,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->owner_thread, thread_id,
                        memory_order_release);
}

- (void)monitorSetPhase:(DXMTMetal4QueueMonitorPhase)phase
          commandBuffer:(DXMTMetal4CommandBuffer *)commandBuffer {
  if (!_monitorDiagnostic)
    return;
  atomic_store_explicit(&_monitorDiagnostic->phase, phase,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->phase_us, dxmt_monotonic_us(),
                        memory_order_release);
  if (commandBuffer) {
    atomic_store_explicit(&_monitorDiagnostic->completion_value,
                          commandBuffer.completionValue,
                          memory_order_relaxed);
  }
}

- (void)monitorSetSubject:(uintptr_t)subject
                      aux:(uintptr_t)subjectAux
                    count:(uint64_t)subjectCount
                    flags:(uint64_t)subjectFlags {
  if (!_monitorDiagnostic)
    return;
  atomic_store_explicit(&_monitorDiagnostic->subject, subject,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->subject_aux, subjectAux,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->subject_count, subjectCount,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->subject_flags, subjectFlags,
                        memory_order_release);
}

- (void)monitorReleased {
  if (!_monitorDiagnostic)
    return;
  const uint64_t thread_id = dxmt_metal4_current_thread_id();
  if (atomic_load_explicit(&_monitorDiagnostic->owner_thread,
                           memory_order_acquire) != thread_id)
    return;
  const uint32_t depth = atomic_load_explicit(
      &_monitorDiagnostic->owner_depth, memory_order_relaxed);
  if (depth > 1) {
    atomic_fetch_sub_explicit(&_monitorDiagnostic->owner_depth, 1,
                              memory_order_relaxed);
    return;
  }
  const uint64_t now_us = dxmt_monotonic_us();
  const uint64_t acquired_us = atomic_load_explicit(
      &_monitorDiagnostic->acquired_us, memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->last_owner_thread,
                        thread_id, memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->last_source,
                        atomic_load_explicit(&_monitorDiagnostic->source,
                                             memory_order_relaxed),
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->last_phase,
                        atomic_load_explicit(&_monitorDiagnostic->phase,
                                             memory_order_relaxed),
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->last_duration_us,
                        acquired_us && now_us > acquired_us
                            ? now_us - acquired_us
                            : 0,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->last_released_us, now_us,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->owner_depth, 0,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->phase,
                        DXMTMetal4QueueMonitorPhaseIdle,
                        memory_order_relaxed);
  atomic_store_explicit(&_monitorDiagnostic->owner_thread, 0,
                        memory_order_release);
}

- (void)waitForCommandBufferSlotLocked:(uint64_t)completionValue {
  if (!_maxCommandBufferCount || completionValue <= _maxCommandBufferCount)
    return;

  const uint64_t waitValue = completionValue - _maxCommandBufferCount;
  const uint64_t currentValue = _event.signaledValue;
  if (currentValue >= waitValue)
    return;

  const uint64_t waitBeginUs = dxmt_monotonic_us();
  [_event waitUntilSignaledValue:waitValue timeoutMS:UINT64_MAX];
  const uint64_t waitElapsedUs = dxmt_monotonic_us() - waitBeginUs;
  [_throttleLock lock];
  const uint64_t waitIndex = ++_commandBufferThrottleWaitCount;
  [_throttleLock unlock];
  if (dxmt_metal4_perf_stats_enabled() && waitElapsedUs >= 1000 &&
      (waitIndex <= 5 || (waitIndex % 256) == 0)) {
    fprintf(stderr,
            "warn:  DXMT Metal4 queue depth wait: queue=%p max=%" PRIu64
            " target=%" PRIu64 " current=%" PRIu64 " elapsedMs=%.3f waitIndex=%" PRIu64 "\n",
            _metal4Queue, _maxCommandBufferCount, waitValue, currentValue,
            (double)waitElapsedUs / 1000.0, waitIndex);
    fflush(stderr);
  }
}

- (const char *)monitorCurrentPhaseName {
  if (!_monitorDiagnostic)
    return "monitor-disabled";
  const uint32_t phase = atomic_load_explicit(&_monitorDiagnostic->phase,
                                              memory_order_acquire);
  return dxmt_metal4_queue_monitor_phase_name(phase);
}

- (uint64_t)monitorCurrentCompletionValue {
  if (!_monitorDiagnostic)
    return 0;
  return atomic_load_explicit(&_monitorDiagnostic->completion_value,
                              memory_order_acquire);
}

/*
 * GPTK residency dichotomy:
 * - Lifetime (device/queue-wide) lives in externalLifetimeResidencySets /
 *   HasLifetimeResidency and is not churned per commit.
 * - Transient command residency is added on submit and retired only after
 *   the shared-event timeline passes the generation that used them.
 * Retirement is deliberately NOT under submissionLock so a Metal residency
 * set commit cannot block the whole queue serializer for hundreds of ms.
 */
- (void)retireCompletedResidency {
  // Same lock order as prepare: sparseResidencyLock → residency set ops lock.
  [_sparseResidencyLock lock];
  NSLock *opsLock = dxmt_residency_set_ops_lock_get();
  [opsLock lock];
  const uint64_t completedValue = _event.signaledValue;
  BOOL sparseChanged = NO;
  BOOL commandChanged = NO;

  NSIndexSet *completedSparseRetirements =
      [_sparseResidencyRetirements
          indexesOfObjectsPassingTest:^BOOL(NSDictionary *retirement,
                                             NSUInteger index,
                                             BOOL *stop) {
            (void)index;
            (void)stop;
            return [[retirement objectForKey:@"completion"] unsignedLongLongValue]
                   <= completedValue;
          }];
  for (NSDictionary *retirement in
           [_sparseResidencyRetirements
               objectsAtIndexes:completedSparseRetirements]) {
    for (id<MTLAllocation> allocation in
             [retirement objectForKey:@"allocations"]) {
      [_sparseResidencyAllocationRefs removeObject:allocation];
      if ([_sparseResidencyAllocationRefs countForObject:allocation] == 0) {
        [_sparseResidencySet removeAllocation:allocation];
        sparseChanged = YES;
      }
    }
  }
  [_sparseResidencyRetirements
      removeObjectsAtIndexes:completedSparseRetirements];

  NSIndexSet *completedRetirements =
      [_commandResidencyRetirements
          indexesOfObjectsPassingTest:^BOOL(NSDictionary *retirement,
                                             NSUInteger index,
                                             BOOL *stop) {
            (void)index;
            (void)stop;
            return [[retirement objectForKey:@"completion"] unsignedLongLongValue]
                   <= completedValue;
          }];
  for (NSDictionary *retirement in
           [_commandResidencyRetirements
               objectsAtIndexes:completedRetirements]) {
    for (id<MTLAllocation> allocation in
             [retirement objectForKey:@"allocations"]) {
      [_commandResidencyAllocationRefs removeObject:allocation];
      if ([_commandResidencyAllocationRefs
              countForObject:allocation] == 0) {
        [_commandResidencySet removeAllocation:allocation];
        commandChanged = YES;
      }
    }
  }
  [_commandResidencyRetirements
      removeObjectsAtIndexes:completedRetirements];

  if (sparseChanged)
    [_sparseResidencySet commit];
  if (commandChanged)
    [_commandResidencySet commit];
  [opsLock unlock];
  [_sparseResidencyLock unlock];
}

- (uint64_t)prepareCommandResidencyForAllocations:(NSArray *)allocations
                                  completionValue:(uint64_t)completionValue {
  const BOOL diag = dxmt_metal4_dense_hang_diagnostics_enabled() ||
                    dxmt_metal4_residency_diag_enabled();
  const uint64_t slow_step_us = 50000;
  const uint64_t slow_total_us = 100000;
  const uint64_t begin = dxmt_monotonic_us();
  const NSUInteger allocation_count = allocations ? allocations.count : 0;
  NSUInteger command_added = 0;

  [self monitorSetPhase:DXMTMetal4QueueMonitorPhasePrepareResidencyAdd
          commandBuffer:nil];
  [self monitorSetSubject:(uintptr_t)allocation_count
                      aux:(uintptr_t)completionValue
                    count:0
                    flags:DXMT_PREPARE_RESIDENCY_FLAG_STEP_ADD];
  // Lock order: sparseResidencyLock → residency set ops lock (never reverse).
  // Serialize Metal set mutations with RenderThread/persistent thunks.
  [_sparseResidencyLock lock];
  NSLock *opsLock = dxmt_residency_set_ops_lock_get();
  [opsLock lock];
  BOOL commandChanged = NO;
  if (allocations.count) {
    for (id<MTLAllocation> allocation in allocations) {
      if ([_commandResidencyAllocationRefs countForObject:allocation] == 0) {
        [_commandResidencySet addAllocation:allocation];
        commandChanged = YES;
        command_added++;
      }
      [_commandResidencyAllocationRefs addObject:allocation];
    }
    [_commandResidencyRetirements addObject:@{
      @"completion" : @(completionValue),
      @"allocations" : [[allocations copy] autorelease]
    }];
  }
  if (commandChanged) {
    [self monitorSetPhase:
              DXMTMetal4QueueMonitorPhasePrepareResidencyCommandCommit
            commandBuffer:nil];
    [_commandResidencySet commit];
  }
  [opsLock unlock];
  [self monitorSetPhase:DXMTMetal4QueueMonitorPhasePrepareResidencyUnlock
          commandBuffer:nil];
  [_sparseResidencyLock unlock];

  const uint64_t total_us = dxmt_monotonic_us() - begin;
  if (diag && total_us >= slow_total_us) {
    fprintf(stderr,
            "warn:  DXMT Metal4 prepare-residency diagnostic: queue=%p"
            " completionTarget=%" PRIu64 " totalUs=%" PRIu64
            " allocCount=%lu commandAdded=%lu commandChanged=%u"
            " path=add-only\n",
            _metal4Queue, completionValue, total_us,
            (unsigned long)allocation_count, (unsigned long)command_added,
            commandChanged ? 1u : 0u);
    fflush(stderr);
  }
  (void)slow_step_us;
  return total_us;
}

- (void)recordPresentDiagnosticForCommandBuffer:
            (DXMTMetal4CommandBuffer *)commandBuffer
                            previousValue:(uint64_t)previousValue
                             currentValue:(uint64_t)currentValue
                          waitForDrawable:(BOOL)waitForDrawable
                          presentOrdering:(BOOL)presentOrdering {
  id<MTLDrawable> drawable = commandBuffer.pendingDrawable;
  if (!drawable ||
      ![drawable conformsToProtocol:@protocol(CAMetalDrawable)])
    return;

  [_presentLock lock];
  id<CAMetalDrawable> metalDrawable = (id<CAMetalDrawable>)drawable;
  id<MTLTexture> texture = metalDrawable.texture;
  CAMetalLayer *layer = metalDrawable.layer;
  id<MTLResidencySet> layerSet = layer.residencySet;
  NSArray *retainedTemporaryResources =
      commandBuffer.retainedTemporaryResources;
  id<MTLAllocation> backing = texture
                                  ? dxmt_metal4_backing_allocation(
                                        (id<MTLAllocation>)texture)
                                  : nil;

  const uint64_t serial = ++_presentDiagnosticSerial;
  struct DXMTMetal4PresentDiagnostic *diag =
      &_presentDiagnostics[(serial - 1) %
                           DXMT_METAL4_PRESENT_DIAGNOSTIC_CAPACITY];
  memset(diag, 0, sizeof(*diag));
  diag->serial = serial;
  diag->capture_us = dxmt_monotonic_us();
  diag->command_buffer = (uintptr_t)commandBuffer;
  diag->metal_buffer = (uintptr_t)commandBuffer.metal4Buffer;
  diag->drawable = (uintptr_t)drawable;
  diag->texture = (uintptr_t)texture;
  diag->parent_texture = (uintptr_t)texture.parentTexture;
  diag->buffer = (uintptr_t)texture.buffer;
  diag->heap = (uintptr_t)texture.heap;
  diag->backing_allocation = (uintptr_t)backing;
  diag->layer = (uintptr_t)layer;
  diag->layer_device = (uintptr_t)layer.device;
  diag->layer_residency_set = (uintptr_t)layerSet;
  diag->texture_gpu_resource_id = texture ? texture.gpuResourceID._impl : 0;
  diag->completion_value = commandBuffer.completionValue;
  diag->previous_present_value = previousValue;
  diag->current_present_value = currentValue;
  const struct WMTCommandBufferDiagnosticInfo info =
      commandBuffer.diagnosticInfo;
  diag->frame_id = info.frame_id;
  diag->chunk_id = info.chunk_id;
  diag->d3d_sequence_begin = info.d3d_sequence_begin;
  diag->d3d_sequence_end = info.d3d_sequence_end;
  diag->width = texture.width;
  diag->height = texture.height;
  diag->depth = texture.depth;
  diag->array_length = texture.arrayLength;
  diag->mipmap_level_count = texture.mipmapLevelCount;
  diag->sample_count = texture.sampleCount;
  diag->pixel_format = texture.pixelFormat;
  diag->texture_type = texture.textureType;
  diag->usage = texture.usage;
  diag->storage_mode = texture.storageMode;
  diag->hazard_tracking_mode = texture.hazardTrackingMode;
  diag->layer_pixel_format = layer.pixelFormat;
  diag->layer_maximum_drawable_count = layer.maximumDrawableCount;
  diag->layer_allocation_count = layerSet ? layerSet.allocationCount : 0;
  diag->command_buffer_allocation_count =
      retainedTemporaryResources.count;
  const CGSize drawableSize = layer.drawableSize;
  diag->layer_width = drawableSize.width;
  diag->layer_height = drawableSize.height;
  diag->present_duration = commandBuffer.presentDuration;
  diag->texture_framebuffer_only = texture.framebufferOnly;
  diag->layer_framebuffer_only = layer.framebufferOnly;
  diag->layer_contains_texture =
      layerSet && texture
          ? [layerSet containsAllocation:(id<MTLAllocation>)texture]
          : NO;
  diag->layer_contains_backing =
      layerSet && backing ? [layerSet containsAllocation:backing] : NO;
  diag->command_buffer_contains_texture =
      texture && [retainedTemporaryResources containsObject:texture];
  diag->command_buffer_contains_backing =
      backing && [retainedTemporaryResources containsObject:backing];
  diag->wait_for_drawable = waitForDrawable;
  diag->present_ordering = presentOrdering;
  diag->has_present_duration = commandBuffer.hasPresentDuration;
  [_presentLock unlock];
}

- (void)dumpPresentDiagnosticsForErrorCommandBuffer:
    (obj_handle_t)commandBuffer {
  [_presentLock lock];
  const uint64_t newest = _presentDiagnosticSerial;
  if (!newest) {
    fprintf(stderr,
            "err:   DXMT Metal4 recent present diagnostics: errorCommandBuffer=%p count=0\n",
            (void *)(uintptr_t)commandBuffer);
    [_presentLock unlock];
    return;
  }

  const uint64_t oldest =
      newest > DXMT_METAL4_PRESENT_DIAGNOSTIC_CAPACITY
          ? newest - DXMT_METAL4_PRESENT_DIAGNOSTIC_CAPACITY + 1
          : 1;
  fprintf(stderr,
          "err:   DXMT Metal4 recent present diagnostics: errorCommandBuffer=%p"
          " oldest=%" PRIu64 " newest=%" PRIu64 " count=%" PRIu64 "\n",
          (void *)(uintptr_t)commandBuffer, oldest, newest,
          newest - oldest + 1);
  for (uint64_t serial = oldest; serial <= newest; serial++) {
    const struct DXMTMetal4PresentDiagnostic *diag =
        &_presentDiagnostics[(serial - 1) %
                             DXMT_METAL4_PRESENT_DIAGNOSTIC_CAPACITY];
    if (diag->serial != serial)
      continue;
    fprintf(stderr,
            "err:   DXMT Metal4 recent present: serial=%" PRIu64
            " ageUs=%" PRIu64 " commandBuffer=%p metalBuffer=%p"
            " completion=%" PRIu64 " presentValue=%" PRIu64 "->%" PRIu64
            " frame=%" PRIu64 " chunk=%" PRIu64 " d3dSeq=%" PRIu64 "-%" PRIu64
            " drawable=%p texture=%p gpuResource=%" PRIu64
            " parent=%p buffer=%p heap=%p backing=%p"
            " size=%" PRIu64 "x%" PRIu64 "x%" PRIu64
            " array=%" PRIu64 " mips=%" PRIu64 " samples=%" PRIu64
            " format=%" PRIu64 " type=%" PRIu64 " usage=0x%" PRIx64
            " storage=%" PRIu64 " hazard=%" PRIu64 " framebufferOnly=%u"
            " layer=%p layerDevice=%p layerSize=%.0fx%.0f"
            " layerFormat=%" PRIu64 " layerFramebufferOnly=%u maxDrawables=%" PRIu64
            " layerSet=%p layerContains=%u/%u layerAllocations=%" PRIu64
            " commandSetContains=%u/%u commandAllocations=%" PRIu64
            " waitForDrawable=%u ordering=%u duration=%u/%.6f\n",
            diag->serial, dxmt_monotonic_us() - diag->capture_us,
            (void *)diag->command_buffer, (void *)diag->metal_buffer,
            diag->completion_value, diag->previous_present_value,
            diag->current_present_value, diag->frame_id, diag->chunk_id,
            diag->d3d_sequence_begin, diag->d3d_sequence_end,
            (void *)diag->drawable, (void *)diag->texture,
            diag->texture_gpu_resource_id, (void *)diag->parent_texture,
            (void *)diag->buffer, (void *)diag->heap,
            (void *)diag->backing_allocation, diag->width, diag->height,
            diag->depth, diag->array_length, diag->mipmap_level_count,
            diag->sample_count, diag->pixel_format, diag->texture_type,
            diag->usage, diag->storage_mode, diag->hazard_tracking_mode,
            diag->texture_framebuffer_only, (void *)diag->layer,
            (void *)diag->layer_device, diag->layer_width, diag->layer_height,
            diag->layer_pixel_format, diag->layer_framebuffer_only,
            diag->layer_maximum_drawable_count,
            (void *)diag->layer_residency_set, diag->layer_contains_texture,
            diag->layer_contains_backing, diag->layer_allocation_count,
            diag->command_buffer_contains_texture,
            diag->command_buffer_contains_backing,
            diag->command_buffer_allocation_count, diag->wait_for_drawable,
            diag->present_ordering, diag->has_present_duration,
            diag->present_duration);
  }
  [_presentLock unlock];
}
@end

@implementation DXMTMetal4CommandBuffer
- (instancetype)initWithQueue:(DXMTMetal4CommandQueue *)queue {
  self = [super init];
  if (!self)
    return nil;

  _owner = [queue retain];
  // GPTK-style pool: acquire recycled MTL4 objects when available.
  _allocator = [[queue acquireCommandAllocator] retain];
  _metal4Buffer = [[queue acquireCommandBuffer] retain];
  [_metal4Buffer beginCommandBufferWithAllocator:_allocator];
  _pendingWaitEvents = [[NSMutableArray alloc] init];
  _pendingSignalEvents = [[NSMutableArray alloc] init];
  _retainedTemporaryResources = [[NSMutableArray alloc] init];
  _queueResidencyAllocations = [[NSMutableArray alloc] init];
  _queueResidencyAllocationSet = [[NSMutableSet alloc] init];
  _feedbackCondition = [[NSCondition alloc] init];
  _feedbackComplete = NO;
  _completionValue = 0;
  _metal4ObjectsRecycled = NO;
  _internalStatus = DXMTMetal4CommandBufferStateNotEnqueued;
  _feedbackGPUStartTime = 0.0;
  _feedbackGPUEndTime = 0.0;
  memset(&_diagnosticInfo, 0, sizeof(_diagnosticInfo));

  if (!_allocator || !_metal4Buffer || !_pendingWaitEvents ||
      !_pendingSignalEvents || !_retainedTemporaryResources ||
      !_queueResidencyAllocations ||
      !_queueResidencyAllocationSet || !_feedbackCondition) {
    [self release];
    return nil;
  }

  return self;
}

- (void)dealloc {
  // Safety net if waitUntilCompleted was never called after a committed buffer.
  [self drainPendingLifetimeOwnership];
  // Do not return incomplete command buffers to the pool — end/commit may not
  // have run. Only recycle after a successful wait path.
  [_pendingDrawable release];
  [_feedbackError release];
  [_pendingWaitEvents release];
  [_pendingSignalEvents release];
  [_retainedTemporaryResources release];
  [_pendingLifetimeSnapshot release];
  [_queueResidencyAllocationSet release];
  [_queueResidencyAllocations release];
  [_feedbackCondition release];
  [_metal4Buffer release];
  [_allocator release];
  [_owner release];
  [super dealloc];
}

- (void)drainPendingLifetimeOwnership {
  // Release lifetime ownership on the waiter thread (dxmt-finish / PE with TEB),
  // never on Metal's CompletionQueue. Snapshot is the exclusive temporary owner
  // for this command-buffer generation; CommandChunk still owns long-lived
  // original resources via retained_metal_resources until GPU complete.
  NSArray *snapshot = nil;
  [_feedbackCondition lock];
  snapshot = _pendingLifetimeSnapshot;
  _pendingLifetimeSnapshot = nil;
  [_feedbackCondition unlock];
  [snapshot release];
}

- (void)recycleMetal4ObjectsIfNeeded {
  // GPTK ReturnMTL4CommandBuffer: only after GPU completion / terminal latch.
  // Never recycle a buffer that was never submitted (begin without end/commit).
  if (_metal4ObjectsRecycled)
    return;
  if (!_metal4Buffer || !_allocator)
    return;
  [_feedbackCondition lock];
  const BOOL terminal =
      _internalStatus == DXMTMetal4CommandBufferStateCompleted ||
      _internalStatus == DXMTMetal4CommandBufferStateError;
  const BOOL submitted = _completionValue != 0;
  [_feedbackCondition unlock];
  if (!terminal || !submitted)
    return;
  _metal4ObjectsRecycled = YES;
  // Allocator must be reset before the next beginCommandBufferWithAllocator.
  [_allocator reset];
  [_owner recycleCommandBuffer:_metal4Buffer allocator:_allocator];
  // Pool now owns the references; drop our retains so dealloc does not free
  // objects still sitting in the pool.
  [_metal4Buffer release];
  _metal4Buffer = nil;
  [_allocator release];
  _allocator = nil;
}

- (id<MTL4CommandBuffer>)commandBuffer {
  return _metal4Buffer;
}

- (id<MTL4ComputeCommandEncoder>)metal4ComputeEncoder {
  return [_metal4Buffer computeCommandEncoder];
}

- (id<MTL4RenderCommandEncoder>)metal4RenderCommandEncoderWithDescriptor:(MTL4RenderPassDescriptor *)descriptor {
  return [_metal4Buffer renderCommandEncoderWithDescriptor:descriptor];
}

- (void)registerResourceForCurrentCommandBuffer:(id<MTLAllocation>)allocation {
  if (!allocation)
    return;

  // GPTK split (D3DMCommandQueue::RetainResourceForCurrentCommandBuffer):
  // - PE CommandChunk owns long-lived object retains until GPU complete.
  // - This method is encode-local bookkeeping for transient command-residency
  //   membership at prepare/submit only.
  // - Do NOT take residency ops lock or call containsAllocation/addAllocation:
  //   those serialized with RenderThread persistent residency and hung BMW.
  // - Lifetime resources already live in the queue-attached persistent set.
  id<MTLAllocation> backing = dxmt_metal4_backing_allocation(allocation);
  if (![_queueResidencyAllocationSet containsObject:backing]) {
    [_queueResidencyAllocationSet addObject:backing];
    [_queueResidencyAllocations addObject:backing];
  }
  if ([(id)backing conformsToProtocol:@protocol(MTLTexture)]) {
    DXMTSparseTextureResidency *sparseResidency =
        dxmt_sparse_texture_residency_for_texture((id<MTLTexture>)backing,
                                                  false);
    for (id<MTLAllocation> heap in [sparseResidency mappedHeapsSnapshot]) {
      if (![_retainedTemporaryResources containsObject:heap])
        [_retainedTemporaryResources addObject:heap];
    }
  }
}

- (void)retainTemporaryResourceForCurrentCommandBuffer:
    (id<MTLAllocation>)allocation {
  if (!allocation)
    return;
  // Sole lifetime owner for native-side temporaries (upload buffers, etc.)
  // until the generation snapshot is drained after GPU complete.
  if (![_retainedTemporaryResources containsObject:allocation])
    [_retainedTemporaryResources addObject:allocation];
  [self registerResourceForCurrentCommandBuffer:allocation];
}

- (id<MTL4ArgumentTable>)newArgumentTableWithLabel:(NSString *)label {
  MTL4ArgumentTableDescriptor *descriptor = [[MTL4ArgumentTableDescriptor alloc] init];
  descriptor.maxBufferBindCount = 31;
  descriptor.maxTextureBindCount = 128;
  descriptor.maxSamplerStateBindCount = 16;
  descriptor.initializeBindings = YES;
  descriptor.label = label;
  NSError *error = nil;
  id<MTL4ArgumentTable> table = [(id<MTLDeviceMetal4SPI>)_owner.device newArgumentTableWithDescriptor:descriptor
                                                                                                error:&error];
  if (!table) {
    fprintf(stderr, "err:   DXMT Metal4 argument table creation failed for %s: %s\n",
            label ? label.UTF8String : "<unnamed>",
            error.localizedDescription ? error.localizedDescription.UTF8String : "<no error>");
  }
  [descriptor release];
  return table;
}

- (id<MTLBuffer>)newUploadBufferWithBytes:(const void *)bytes length:(NSUInteger)length {
  if (!length)
    length = 1;
  id<MTLBuffer> buffer = [_owner.device newBufferWithLength:length options:MTLResourceStorageModeShared];
  if (!buffer)
    return nil;
  if (bytes)
    memcpy([buffer contents], bytes, length);
  [self retainTemporaryResourceForCurrentCommandBuffer:
      (id<MTLAllocation>)buffer];
  return buffer;
}

- (void)commit {
  [_owner monitorWaitBegin:DXMTMetal4QueueMonitorSourceCommandBufferCommit
             commandBuffer:self];
  [self commitLocked];
}

// D3DMetal does not serialize command-buffer commits behind a recursive critical
// section that also waits for GPU retirement. Track commit depth so nested
// commits fail fast instead of deadlocking on NSLock.
static _Thread_local int dxmt_metal4_commit_locked_depth = 0;

- (uint64_t)commitLocked {
  if (dxmt_metal4_commit_locked_depth > 0) {
    dxmt_metal4_hang_log(
        DXMT_METAL4_HANG_LOG_BOTH,
        "err:   DXMT Metal4 reentrant commit rejected: commandBuffer=%p "
        "queue=%p depth=%d parentPhase=%s parentCompletion=%" PRIu64
        " nestedStatus=%u nestedCompletion=%" PRIu64
        " frame=%" PRIu64 " chunk=%" PRIu64 " d3dSeq=%" PRIu64 "-%" PRIu64
        " reason=reentrant\n",
        self, _owner.metal4Queue, dxmt_metal4_commit_locked_depth,
        [_owner monitorCurrentPhaseName],
        [_owner monitorCurrentCompletionValue], (unsigned)_internalStatus,
        _completionValue, _diagnosticInfo.frame_id, _diagnosticInfo.chunk_id,
        _diagnosticInfo.d3d_sequence_begin, _diagnosticInfo.d3d_sequence_end);
    [_owner monitorWaitEnd:DXMTMetal4QueueMonitorSourceCommandBufferCommit
             commandBuffer:self];
    [_owner monitorWaitEnd:DXMTMetal4QueueMonitorSourceUnixCommitThunk
             commandBuffer:self];
    return 0;
  }
  dxmt_metal4_commit_locked_depth++;

  [_feedbackCondition lock];
  if (_internalStatus != DXMTMetal4CommandBufferStateNotEnqueued) {
    const unsigned already_status = (unsigned)_internalStatus;
    const uint64_t already_completion = _completionValue;
    const BOOL already_feedback = _feedbackComplete;
    [_feedbackCondition unlock];
    dxmt_metal4_hang_log(
        DXMT_METAL4_HANG_LOG_BOTH,
        "err:   DXMT Metal4 commit rejected already-enqueued: commandBuffer=%p "
        "queue=%p status=%u completion=%" PRIu64 " feedbackComplete=%u"
        " frame=%" PRIu64 " chunk=%" PRIu64 " d3dSeq=%" PRIu64 "-%" PRIu64
        " reason=already-enqueued\n",
        self, _owner.metal4Queue, already_status, already_completion,
        already_feedback ? 1u : 0u, _diagnosticInfo.frame_id,
        _diagnosticInfo.chunk_id, _diagnosticInfo.d3d_sequence_begin,
        _diagnosticInfo.d3d_sequence_end);
    [_owner monitorWaitEnd:DXMTMetal4QueueMonitorSourceCommandBufferCommit
             commandBuffer:self];
    [_owner monitorWaitEnd:DXMTMetal4QueueMonitorSourceUnixCommitThunk
             commandBuffer:self];
    dxmt_metal4_commit_locked_depth--;
    return 0;
  }
  _internalStatus = DXMTMetal4CommandBufferStateCommitted;
  [_feedbackCondition unlock];

  [_owner.errorLock lock];
  if (_owner.firstError &&
      [_owner.firstError.domain isEqualToString:@"DXMTMetal4TestErrorDomain"] &&
      !dxmt_metal4_test_feedback_error_enabled())
    _owner.firstError = nil;
  NSError *queueError = [_owner.firstError retain];
  [_owner.errorLock unlock];

  if (queueError) {
    [_feedbackCondition lock];
    [_feedbackError release];
    _feedbackError = queueError;
    _internalStatus = DXMTMetal4CommandBufferStateError;
    _feedbackComplete = YES;
    [_feedbackCondition broadcast];
    [_feedbackCondition unlock];
    [_owner monitorWaitEnd:DXMTMetal4QueueMonitorSourceCommandBufferCommit
             commandBuffer:self];
    [_owner monitorWaitEnd:DXMTMetal4QueueMonitorSourceUnixCommitThunk
             commandBuffer:self];
    const char *rejection_marker =
        getenv("DXMT_TEST_METAL4_REJECTION_MARKER");
    if (rejection_marker && *rejection_marker) {
      FILE *marker = fopen(rejection_marker, "a");
      if (marker) {
        fputs("rejected\n", marker);
        fclose(marker);
      }
    }
    dxmt_metal4_hang_log(
        DXMT_METAL4_HANG_LOG_BOTH,
        "err:   DXMT Metal4 queue rejected command buffer after first error:"
        " commandBuffer=%p queue=%p domain=%s code=%ld description=%s"
        " frame=%" PRIu64 " chunk=%" PRIu64 " reason=queue-error\n",
        self, _owner.metal4Queue,
        queueError.domain ? queueError.domain.UTF8String : "<no domain>",
        (long)queueError.code,
        queueError.localizedDescription
            ? queueError.localizedDescription.UTF8String
            : "<no description>",
        _diagnosticInfo.frame_id, _diagnosticInfo.chunk_id);
    dxmt_metal4_commit_locked_depth--;
    return 0;
  }

  dxmt_apitrace_record_command_buffer_commit_state(
      (obj_handle_t)self,
      APITRACE_METAL_COMMAND_BUFFER_COMMIT_BEGIN,
      _pendingWaitEvents.count,
      _pendingSignalEvents.count,
      _pendingDrawable != nil,
      0,
      0);

  const BOOL hasDrawable = _pendingDrawable != nil;
  const BOOL presentOrdering = hasDrawable && dxmt_metal4_present_ordering_enabled();
  const BOOL waitForDrawable = hasDrawable && dxmt_metal4_wait_for_drawable_enabled();
  uint64_t previousPresentValue = 0;
  uint64_t currentPresentValue = 0;
  uint64_t residency_submit_us = 0;
  BOOL holdingSubmissionLock = NO;
  uint64_t completionValue = 0;
  // GPTK WaitUntilSubmission closed-loop bookkeeping:
  // once nextEventValue is taken we own a timeline hole that must be closed on
  // every exit path; once Metal commit returns the feedback handler owns the
  // terminal latch.
  BOOL completionTimelineOwned = NO;
  BOOL metalSubmitted = NO;
  BOOL completionTimelineSignaled = NO;
  // Declared outside @try so @finally can reclaim them on pre-submit abort.
  MTL4CommitOptions *options = nil;
  __block NSArray *feedbackRetainedResources = nil;

  // GPTK residency dichotomy: retire completed *transient* memberships off the
  // commit serializer. Lifetime/device sets are not touched here. A residency
  // set commit under submissionLock has been observed to stall the whole queue
  // for hundreds of ms (and nest with other waiters).
  [_owner retireCompletedResidency];

  // Match D3DMetal: never park a GPU-timeline wait under the commit serializer.
  // submissionLock only covers short Metal-queue mutation; depth throttling
  // waits for older completions outside the lock so other commits / sparse
  // updates / feedback cannot deadlock behind UINT64_MAX waits.
  [_owner.submissionLock lock];
  holdingSubmissionLock = YES;
  [_owner monitorWaitEnd:DXMTMetal4QueueMonitorSourceCommandBufferCommit
           commandBuffer:self];
  [_owner monitorWaitEnd:DXMTMetal4QueueMonitorSourceUnixCommitThunk
           commandBuffer:self];
  [_owner monitorAcquired:DXMTMetal4QueueMonitorSourceCommandBufferCommit
                    phase:DXMTMetal4QueueMonitorPhaseCommitPreflight
            commandBuffer:self];
  @try {
  if (presentOrdering) {
    [_owner.presentLock lock];
    previousPresentValue = _owner.presentEventValue;
    currentPresentValue = ++_owner.presentEventValue;
    [_owner.presentLock unlock];
  }

  [_owner monitorSetPhase:DXMTMetal4QueueMonitorPhaseEnqueueWaitEvents
            commandBuffer:self];
  for (DXMTMetal4QueueEvent *wait in _pendingWaitEvents) {
    dxmt_apitrace_record_command_buffer_event(
        (obj_handle_t)self,
        APITRACE_METAL_QUEUE_EVENT_WAIT,
        (obj_handle_t)wait.event,
        wait.value,
        APITRACE_METAL_QUEUE_EVENT_ENQUEUED);
    // Schedules a GPU dependency on the Metal 4 queue; does not CPU-block.
    [_owner.metal4Queue waitForEvent:wait.event value:wait.value];
  }

  [_owner monitorSetPhase:DXMTMetal4QueueMonitorPhaseWaitForDrawable
            commandBuffer:self];
  if (waitForDrawable)
    [_owner.metal4Queue waitForDrawable:_pendingDrawable];

  [_owner monitorSetPhase:DXMTMetal4QueueMonitorPhasePresentOrderingWait
            commandBuffer:self];
  if (presentOrdering && previousPresentValue) {
#if DXMT_APITRACE_METAL
    dxmt_apitrace_record_command_buffer_event(
        (obj_handle_t)self,
        APITRACE_METAL_QUEUE_EVENT_WAIT,
        (obj_handle_t)_owner.presentEvent,
        previousPresentValue,
        APITRACE_METAL_QUEUE_EVENT_ENQUEUED);
#endif
    [_owner.metal4Queue waitForEvent:_owner.presentEvent value:previousPresentValue];
  }

  [_owner monitorSetPhase:DXMTMetal4QueueMonitorPhaseEndCommandBuffer
            commandBuffer:self];
  [_metal4Buffer endCommandBuffer];

  completionValue = [_owner nextEventValueLocked];
  completionTimelineOwned = YES;
  [_feedbackCondition lock];
  _completionValue = completionValue;
  [_feedbackCondition unlock];
  [_owner monitorSetPhase:DXMTMetal4QueueMonitorPhasePrepareResidency
            commandBuffer:self];
  residency_submit_us =
      [_owner prepareCommandResidencyForAllocations:_queueResidencyAllocations
                                   completionValue:completionValue];
  // The queue residency set and its retirement record now own the backing
  // identities. Do not keep those identities in the command-buffer lifetime
  // snapshot: only original resource owners belong there.
  [_queueResidencyAllocationSet removeAllObjects];
  [_queueResidencyAllocations removeAllObjects];
  _completionCurrentBeforeThrottle = _owner.event.signaledValue;

  // Drop the serializer before any CPU wait on the retirement timeline.
  const uint64_t throttleTarget =
      (_owner.maxCommandBufferCount &&
       completionValue > _owner.maxCommandBufferCount)
          ? (completionValue - _owner.maxCommandBufferCount)
          : 0;
  if (throttleTarget &&
      _completionCurrentBeforeThrottle < throttleTarget) {
    [_owner monitorSetPhase:DXMTMetal4QueueMonitorPhaseCommandBufferThrottle
              commandBuffer:self];
    [_owner monitorReleased];
    [_owner.submissionLock unlock];
    holdingSubmissionLock = NO;
    [_owner.event waitUntilSignaledValue:throttleTarget
                               timeoutMS:UINT64_MAX];
    [_owner.submissionLock lock];
    holdingSubmissionLock = YES;
    [_owner monitorAcquired:DXMTMetal4QueueMonitorSourceCommandBufferCommit
                      phase:DXMTMetal4QueueMonitorPhaseCommandBufferThrottle
              commandBuffer:self];
  }
  _completionCurrentAtCommit = _owner.event.signaledValue;

  const BOOL denseHangDiagnostics =
      dxmt_metal4_dense_hang_diagnostics_enabled();
  const BOOL perfFeedback =
      dxmt_metal4_perf_stats_enabled() || denseHangDiagnostics;
  const BOOL residencyDiagnostics = dxmt_metal4_residency_diag_enabled();
  const BOOL errorDiagnostics = perfFeedback || residencyDiagnostics;
  if (hasDrawable && errorDiagnostics) {
    [_owner recordPresentDiagnosticForCommandBuffer:self
                                      previousValue:previousPresentValue
                                       currentValue:currentPresentValue
                                    waitForDrawable:waitForDrawable
                                    presentOrdering:presentOrdering];
  }
  if (perfFeedback && !_metal4Buffer.label) {
    NSString *label = [[NSString alloc]
        initWithFormat:@"DXMT queue=%p depth=%" PRIu64 " completion=%" PRIu64,
                       _owner.metal4Queue, _owner.maxCommandBufferCount,
                       completionValue];
    _metal4Buffer.label = label;
    [label release];
  }

  [_owner monitorSetPhase:DXMTMetal4QueueMonitorPhaseFeedbackSetup
            commandBuffer:self];
  BOOL traceFeedback = NO;
#if DXMT_APITRACE_METAL
  traceFeedback = dxmt_apitrace_runtime_enabled() &&
                  dxmt_metal4_commit_feedback_enabled(completionValue);
#else
  (void)traceFeedback;
#endif
  // Metal 4 exposes command-buffer failures through commit feedback. Always
  // install the handler so release builds propagate GPU errors instead of
  // waiting forever for a completion event that a failed submission skipped.
  {
    options = [[MTL4CommitOptions alloc] init];
    const obj_handle_t feedbackCommandBuffer = (obj_handle_t)self;
    const obj_handle_t feedbackMetalBuffer = (obj_handle_t)_metal4Buffer;
    const obj_handle_t feedbackQueue = (obj_handle_t)_owner.metal4Queue;
    const uint64_t feedbackQueueDepth = _owner.maxCommandBufferCount;
    const uint64_t feedbackCompletionValue = completionValue;
    const uint64_t feedbackCompletionBeforeThrottle =
        _completionCurrentBeforeThrottle;
    const uint64_t feedbackCompletionAtCommit = _completionCurrentAtCommit;
    const uint64_t feedbackInflightBeforeThrottle =
        feedbackCompletionValue > feedbackCompletionBeforeThrottle
            ? feedbackCompletionValue - feedbackCompletionBeforeThrottle
            : 0;
    const uint64_t feedbackInflightAtCommit =
        feedbackCompletionValue > feedbackCompletionAtCommit
            ? feedbackCompletionValue - feedbackCompletionAtCommit
            : 0;
    const BOOL feedbackHasDrawable = _pendingDrawable != nil;
    const struct WMTCommandBufferDiagnosticInfo feedbackDiagnostic =
        _diagnosticInfo;
    // Snapshot this generation's temporary resource owners for GPU lifetime.
    // Recording state advances to a fresh container immediately. The Metal
    // completion callback only *hands off* the immutable snapshot to the
    // command buffer; the waiter (dxmt-finish-thr) is the sole releaser.
    feedbackRetainedResources = [_retainedTemporaryResources copy];
    [_retainedTemporaryResources release];
    _retainedTemporaryResources = [[NSMutableArray alloc] init];
    __block id<MTLSharedEvent> feedbackCompletionEvent = [_owner.event retain];
    __block NSArray *feedbackWaitEvents = [_pendingWaitEvents copy];
    __block NSArray *feedbackSignalEvents = [_pendingSignalEvents copy];
    __block NSString *feedbackLabel = [_metal4Buffer.label copy];
    __block DXMTMetal4CommandBuffer *feedbackOwner = [self retain];
    [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
      NSError *error = feedback.error;
      @try {
        BOOL injectTestError = NO;
        if (!error) {
          DXMTMetal4CommandQueue *feedbackQueueOwner = feedbackOwner.owner;
          [feedbackQueueOwner
              monitorWaitBegin:DXMTMetal4QueueMonitorSourceFeedbackInjection
                 commandBuffer:feedbackOwner];
          [feedbackQueueOwner
              monitorWaitEnd:DXMTMetal4QueueMonitorSourceFeedbackInjection
               commandBuffer:feedbackOwner];
          [feedbackQueueOwner.errorLock lock];
          if (dxmt_truthy_env_value(
                  getenv("DXMT_TEST_METAL4_INJECT_FEEDBACK_ERROR"))) {
            injectTestError = YES;
          } else {
            const char *token =
                getenv("DXMT_TEST_METAL4_INJECT_FEEDBACK_ERROR_ONCE");
            if (token && token[0] && strcmp(token, "0") &&
                strcmp(token, "false") && strcmp(token, "no") &&
                strcmp(token, "off")) {
              NSString *value = [NSString stringWithUTF8String:token];
              if (![feedbackQueueOwner.testFeedbackErrorToken
                      isEqualToString:value]) {
                feedbackQueueOwner.testFeedbackErrorToken = value;
                injectTestError = YES;
              }
            }
          }
          [feedbackQueueOwner.errorLock unlock];
        }
        if (injectTestError) {
          error = [NSError errorWithDomain:@"DXMTMetal4TestErrorDomain"
                                      code:1
                                  userInfo:@{
                                    NSLocalizedDescriptionKey:
                                        @"Injected Metal 4 commit feedback error"
                                  }];
        }
        BOOL firstQueueError = NO;
        if (error) {
          DXMTMetal4CommandQueue *feedbackQueueOwner = feedbackOwner.owner;
          [feedbackQueueOwner
              monitorWaitBegin:DXMTMetal4QueueMonitorSourceFeedbackErrorLatch
                 commandBuffer:feedbackOwner];
          [feedbackQueueOwner
              monitorWaitEnd:DXMTMetal4QueueMonitorSourceFeedbackErrorLatch
               commandBuffer:feedbackOwner];
          [feedbackQueueOwner.errorLock lock];
          if (!feedbackQueueOwner.firstError) {
            feedbackQueueOwner.firstError = error;
            firstQueueError = YES;
          }
          [feedbackQueueOwner.errorLock unlock];
        }
        if (error && feedbackCompletionEvent.signaledValue <
                         feedbackCompletionValue) {
          // This event is DXMT's CPU-side retirement timeline. Feedback means
          // the GPU has finished processing the submission, successfully or
          // not, so advancing it is safe and releases waiters/resource
          // retirement.
          feedbackCompletionEvent.signaledValue = feedbackCompletionValue;
        }
        if (errorDiagnostics && error) {
        fprintf(stderr,
                "err:   DXMT Metal4 commit feedback error: commandBuffer=%p metalBuffer=%p"
                " queue=%p queueDepth=%" PRIu64 " completionTarget=%" PRIu64
                " completionCurrent=%" PRIu64 " waitCount=%lu signalCount=%lu drawable=%d"
                " completionBeforeThrottle=%" PRIu64
                " completionAtCommit=%" PRIu64
                " inflightBeforeThrottle=%" PRIu64
                " inflightAtCommit=%" PRIu64
                " frame=%" PRIu64 " chunk=%" PRIu64
                " d3dSeq=%" PRIu64 "-%" PRIu64
                " encoders=%u/%u render=%u compute=%u blit=%u other=%u"
                " present=%u clear=%u resolve=%u scaler=%u"
                " signal=%u wait=%u timestamp=%u"
                " barrierOnly=%u fenceWait=%u fenceUpdate=%u initEvent=%" PRIu64
                " priorLocalWait=%u futureLocalWait=%u sameEncoderWait=%u"
                " externalWait=%u repeatedUpdate=%u"
                " renderValidCrossStage=%u renderSameStage=%u renderReverseStage=%u"
                " localFenceIds=%u boundFenceSlots=%u"
                " encodedFenceWait=%u skippedExternalFenceWait=%u"
                " fenceEdgeCount=%u fenceEdgeOverflow=%u"
                " encoderDiagnosticCount=%u encoderDiagnosticOverflow=%u"
                " sparseCalls=%u sparseOps=%u sparseMap=%u sparseUnmap=%u"
                " sparseFailures=%u sparseBarriers=%u"
                " sparseResource=%" PRIu64 " sparseTexture=0x%" PRIx64
                " sparseHeap=0x%" PRIx64 " sparseGpuResource=%" PRIu64
                " sparseGeneration=%" PRIu64 "-%" PRIu64
                " sparseAccess=%u sparseAccessFlags=0x%x"
                " sparseAccessSubresource=%u:%u"
                " sparseAccessDescriptor=0x%" PRIx64
                " sparseAccessResource=%" PRIu64
                " sparseAccessTexture=0x%" PRIx64
                " sparseAccessGpuResource=%" PRIu64
                " sparseAccessEncoder=%" PRIu64
                " label=%s gpuStart=%.9f gpuEnd=%.9f domain=%s code=%ld description=%s\n",
                (void *)(uintptr_t)feedbackCommandBuffer,
                (void *)(uintptr_t)feedbackMetalBuffer,
                (void *)(uintptr_t)feedbackQueue,
                feedbackQueueDepth,
                feedbackCompletionValue,
                feedbackCompletionEvent.signaledValue,
                (unsigned long)feedbackWaitEvents.count,
                (unsigned long)feedbackSignalEvents.count,
                feedbackHasDrawable,
                feedbackCompletionBeforeThrottle,
                feedbackCompletionAtCommit,
                feedbackInflightBeforeThrottle,
                feedbackInflightAtCommit,
                feedbackDiagnostic.frame_id,
                feedbackDiagnostic.chunk_id,
                feedbackDiagnostic.d3d_sequence_begin,
                feedbackDiagnostic.d3d_sequence_end,
                feedbackDiagnostic.input_encoder_count,
                feedbackDiagnostic.encoded_encoder_count,
                feedbackDiagnostic.render_encoder_count,
                feedbackDiagnostic.compute_encoder_count,
                feedbackDiagnostic.blit_encoder_count,
                feedbackDiagnostic.other_encoder_count,
                feedbackDiagnostic.present_encoder_count,
                feedbackDiagnostic.clear_encoder_count,
                feedbackDiagnostic.resolve_encoder_count,
                feedbackDiagnostic.scaler_encoder_count,
                feedbackDiagnostic.signal_event_count,
                feedbackDiagnostic.wait_event_count,
                feedbackDiagnostic.timestamp_encoder_count,
                feedbackDiagnostic.barrier_only_pass_count,
                feedbackDiagnostic.fence_wait_count,
                feedbackDiagnostic.fence_update_count,
                feedbackDiagnostic.resource_initializer_event_id,
                feedbackDiagnostic.prior_local_fence_wait_count,
                feedbackDiagnostic.future_local_fence_wait_count,
                feedbackDiagnostic.same_encoder_fence_wait_count,
                feedbackDiagnostic.external_fence_wait_count,
                feedbackDiagnostic.repeated_fence_update_count,
                feedbackDiagnostic.render_valid_cross_stage_count,
                feedbackDiagnostic.render_same_stage_wait_count,
                feedbackDiagnostic.render_reverse_stage_wait_count,
                feedbackDiagnostic.local_fence_id_count,
                feedbackDiagnostic.bound_fence_slot_count,
                feedbackDiagnostic.encoded_fence_wait_count,
                feedbackDiagnostic.skipped_external_fence_wait_count,
                feedbackDiagnostic.fence_edge_count,
                feedbackDiagnostic.fence_edge_overflow_count,
                feedbackDiagnostic.encoder_diagnostic_count,
                feedbackDiagnostic.encoder_diagnostic_overflow_count,
                feedbackDiagnostic.sparse_mapping_call_count,
                feedbackDiagnostic.sparse_mapping_operation_count,
                feedbackDiagnostic.sparse_mapping_map_count,
                feedbackDiagnostic.sparse_mapping_unmap_count,
                feedbackDiagnostic.sparse_mapping_failure_count,
                feedbackDiagnostic.sparse_mapping_barrier_count,
                feedbackDiagnostic.sparse_resource_identity,
                feedbackDiagnostic.sparse_texture_handle,
                feedbackDiagnostic.sparse_heap_handle,
                feedbackDiagnostic.sparse_gpu_resource_id,
                feedbackDiagnostic.sparse_mapping_generation_begin,
                feedbackDiagnostic.sparse_mapping_generation_end,
                feedbackDiagnostic.sparse_access_count,
                feedbackDiagnostic.sparse_access_flags,
                feedbackDiagnostic.sparse_access_level,
                feedbackDiagnostic.sparse_access_slice,
                feedbackDiagnostic.sparse_access_descriptor,
                feedbackDiagnostic.sparse_access_resource_identity,
                feedbackDiagnostic.sparse_access_texture_handle,
                feedbackDiagnostic.sparse_access_gpu_resource_id,
                feedbackDiagnostic.sparse_access_encoder_id,
                feedbackLabel ? feedbackLabel.UTF8String : "<unnamed>",
                feedback.GPUStartTime, feedback.GPUEndTime,
                error.domain ? error.domain.UTF8String : "<no domain>",
                (long)error.code,
                error.localizedDescription ? error.localizedDescription.UTF8String
                                           : "<no description>");

        if (firstQueueError)
          [feedbackOwner.owner
              dumpPresentDiagnosticsForErrorCommandBuffer:
                  feedbackCommandBuffer];

        if (denseHangDiagnostics) {
          for (uint32_t edge_index = 0;
               edge_index < feedbackDiagnostic.fence_edge_count &&
               edge_index < WMT_COMMAND_BUFFER_FENCE_EDGE_CAPACITY;
               edge_index++) {
            const struct WMTCommandBufferFenceEdgeDiagnostic *edge =
                &feedbackDiagnostic.fence_edges[edge_index];
            fprintf(stderr,
                    "err:   DXMT Metal4 feedback fence edge: commandBuffer=%p"
                    " edge=%u producer=%" PRIu64 " consumer=%" PRIu64
                    " producerIndex=%u consumerIndex=%u slot=%u flags=0x%x\n",
                    (void *)(uintptr_t)feedbackCommandBuffer, edge_index,
                    edge->producer_id, edge->consumer_id,
                    edge->producer_index, edge->consumer_index, edge->slot,
                    edge->flags);
          }
          for (uint32_t encoder_index = 0;
               encoder_index < feedbackDiagnostic.encoder_diagnostic_count &&
               encoder_index <
                   WMT_COMMAND_BUFFER_ENCODER_DIAGNOSTIC_CAPACITY;
               encoder_index++) {
            const struct WMTCommandBufferEncoderDiagnostic *encoder =
                &feedbackDiagnostic.encoders[encoder_index];
            fprintf(stderr,
                    "err:   DXMT Metal4 feedback encoder: commandBuffer=%p"
                    " diagnosticIndex=%u encoderIndex=%u type=%u"
                    " encoderId=%" PRIu64 " vertexEncoderId=%" PRIu64
                    " waits=%u updates=%u"
                    " waitHash=0x%" PRIx64 " updateHash=0x%" PRIx64
                    " pso=0x%" PRIx64
                    " argbuf=0x%" PRIx64 "+%" PRIu64 "/%" PRIu64
                    " primary=0x%" PRIx64 " secondary=0x%" PRIx64
                    " resourcePlan=%u/0x%" PRIx64 " flags=0x%x\n",
                    (void *)(uintptr_t)feedbackCommandBuffer, encoder_index,
                    encoder->encoder_index, encoder->type,
                    encoder->encoder_id, encoder->vertex_encoder_id,
                    encoder->wait_count, encoder->update_count,
                    encoder->wait_hash, encoder->update_hash,
                    encoder->pipeline_handle,
                    encoder->argument_buffer_handle,
                    encoder->argument_buffer_offset,
                    encoder->argument_buffer_size,
                    encoder->primary_resource,
                    encoder->secondary_resource,
                    encoder->resource_plan_count,
                    encoder->resource_plan_hash, encoder->flags);
          }
          NSString *debugDescription = error.debugDescription;
          NSString *userInfoDescription = error.userInfo.description;
          fprintf(stderr,
                  "err:   DXMT Metal4 dense feedback error: commandBuffer=%p"
                  " debug=%s userInfo=%s\n",
                  (void *)(uintptr_t)feedbackCommandBuffer,
                  debugDescription ? debugDescription.UTF8String : "<none>",
                  userInfoDescription ? userInfoDescription.UTF8String : "<none>");
        }

        NSUInteger waitIndex = 0;
        for (DXMTMetal4QueueEvent *wait in feedbackWaitEvents) {
          uint64_t current = dxmt_metal4_shared_event_value(wait.event);
          fprintf(stderr,
                  "err:   DXMT Metal4 commit feedback wait: commandBuffer=%p index=%lu"
                  " event=%p target=%" PRIu64 " current=%" PRIu64 " blocked=%d\n",
                  (void *)(uintptr_t)feedbackCommandBuffer,
                  (unsigned long)waitIndex++, wait.event,
                  wait.value, current, current < wait.value);
        }

        NSUInteger signalIndex = 0;
        for (DXMTMetal4QueueEvent *signal in feedbackSignalEvents) {
          uint64_t current = dxmt_metal4_shared_event_value(signal.event);
          fprintf(stderr,
                  "err:   DXMT Metal4 commit feedback signal: commandBuffer=%p index=%lu"
                  " event=%p target=%" PRIu64 " current=%" PRIu64 "\n",
                  (void *)(uintptr_t)feedbackCommandBuffer,
                  (unsigned long)signalIndex++, signal.event,
                  signal.value, current);
        }
        fflush(stderr);
      }
#if DXMT_APITRACE_METAL
      if (traceFeedback) {
        dxmt_apitrace_record_command_buffer_feedback(
            feedbackCommandBuffer,
            error ? APITRACE_METAL_COMMAND_BUFFER_FEEDBACK_ERROR
                  : APITRACE_METAL_COMMAND_BUFFER_FEEDBACK_COMPLETED,
            feedback.GPUStartTime,
            feedback.GPUEndTime,
            error && error.localizedDescription ? error.localizedDescription : @"");
      }
#endif
      } @finally {
        // Publish the completion latch on the Metal CompletionQueue, but do
        // NOT free temporary GPU resources here. Hand the snapshot to the
        // command buffer so dxmt-finish-thr can drain it after wait returns
        // (Wine-safe thread with a TEB). Latch first so waiters never hang if
        // a later diagnostic free faults.
        [feedbackOwner.feedbackCondition lock];
        feedbackOwner.feedbackGPUStartTime = feedback.GPUStartTime;
        feedbackOwner.feedbackGPUEndTime = feedback.GPUEndTime;
        if (error)
          feedbackOwner.feedbackError = error;
        feedbackOwner.internalStatus =
            error ? DXMTMetal4CommandBufferStateError
                  : DXMTMetal4CommandBufferStateCompleted;
        // Transfer temporary lifetime ownership. The retain property setter
        // takes ownership of the array (and releases any stale snapshot).
        if (feedbackRetainedResources) {
          feedbackOwner.pendingLifetimeSnapshot = feedbackRetainedResources;
          [feedbackRetainedResources release];
          feedbackRetainedResources = nil;
        }
        feedbackOwner.feedbackComplete = YES;
        [feedbackOwner.feedbackCondition broadcast];
        [feedbackOwner.feedbackCondition unlock];
        // Diagnostic copies are not GPU resources; safe to free here.
        [feedbackLabel release];
        [feedbackSignalEvents release];
        [feedbackWaitEvents release];
        [feedbackCompletionEvent release];
        [feedbackOwner release];
      }
    }];
  }

  id<MTL4CommandBuffer> commandBuffers[1] = {_metal4Buffer};
  [_owner monitorSetPhase:DXMTMetal4QueueMonitorPhaseMetalQueueCommit
            commandBuffer:self];
  // Match D3DMetal: a synchronous Objective-C exception from Metal commit is
  // process-fatal and is not converted into a recoverable command-buffer
  // result. Normal GPU failures arrive through feedback and always publish the
  // completion latch above. Closed-loop in @finally still closes the timeline
  // hole if we abort between nextEventValue and signalEvent.
  if (options)
    [_owner.metal4Queue commit:commandBuffers count:1 options:options];
  else
    [_owner.metal4Queue commit:commandBuffers count:1];
  metalSubmitted = YES;
  // Ownership of options (and the feedback block) transfers to Metal with the
  // successful commit; drop our retain without destroying the in-flight block.
  // Do NOT nil feedbackRetainedResources here: under MRC the __block storage is
  // shared with the handler, which must release the snapshot on completion.
  [options release];
  options = nil;
  dxmt_apitrace_record_command_buffer_commit_state(
      (obj_handle_t)self,
      APITRACE_METAL_COMMAND_BUFFER_COMMIT_SUBMITTED,
      0,
      0,
      NO,
      completionValue,
      0);

  [_owner monitorSetPhase:DXMTMetal4QueueMonitorPhasePresent
            commandBuffer:self];
  if (_pendingDrawable) {
    [_owner.metal4Queue signalEvent:_owner.event value:completionValue];
    completionTimelineSignaled = YES;
    [_owner.metal4Queue signalDrawable:_pendingDrawable];
    if (_hasPresentDuration)
      [_pendingDrawable presentAfterMinimumDuration:_presentDuration];
    else
      [_pendingDrawable present];
    if (presentOrdering) {
#if DXMT_APITRACE_METAL
      dxmt_apitrace_record_command_buffer_event(
          (obj_handle_t)self,
          APITRACE_METAL_QUEUE_EVENT_SIGNAL,
          (obj_handle_t)_owner.presentEvent,
          currentPresentValue,
          APITRACE_METAL_QUEUE_EVENT_ENQUEUED);
#endif
      [_owner.metal4Queue signalEvent:_owner.presentEvent value:currentPresentValue];
    }
  }

  [_owner monitorSetPhase:DXMTMetal4QueueMonitorPhaseSignalEvents
            commandBuffer:self];
  for (DXMTMetal4QueueEvent *signal in _pendingSignalEvents) {
    dxmt_apitrace_record_command_buffer_event(
        (obj_handle_t)self,
        APITRACE_METAL_QUEUE_EVENT_SIGNAL,
        (obj_handle_t)signal.event,
        signal.value,
        APITRACE_METAL_QUEUE_EVENT_ENQUEUED);
    [_owner.metal4Queue signalEvent:signal.event value:signal.value];
  }
  if (!_pendingDrawable) {
    [_owner.metal4Queue signalEvent:_owner.event value:completionValue];
    completionTimelineSignaled = YES;
  }
  } @finally {
    if (holdingSubmissionLock) {
      [_owner monitorReleased];
      [_owner.submissionLock unlock];
      holdingSubmissionLock = NO;
    }
    // GPTK closed-loop: after nextEventValue is taken, every exit path must
    // either leave the GPU signal path intact or force-close the timeline and
    // publish the feedback latch so waiters cannot hang forever.
    if (completionTimelineOwned && completionValue != 0) {
      if (!completionTimelineSignaled &&
          _owner.event.signaledValue < completionValue) {
        _owner.event.signaledValue = completionValue;
        completionTimelineSignaled = YES;
      }
      if (!metalSubmitted) {
        [_feedbackCondition lock];
        if (!_feedbackComplete) {
          if (!_feedbackError) {
            _feedbackError = [[NSError
                errorWithDomain:@"DXMTMetal4CommitDomain"
                           code:1
                       userInfo:@{
                         NSLocalizedDescriptionKey :
                             @"Metal4 commit aborted before queue submission"
                       }] retain];
          }
          // Reclaim the generation snapshot the uninvoked feedback handler
          // would have owned, then hand it to the waiter drain path.
          if (!_pendingLifetimeSnapshot && feedbackRetainedResources) {
            _pendingLifetimeSnapshot = feedbackRetainedResources;
            feedbackRetainedResources = nil;
          } else if (!_pendingLifetimeSnapshot &&
                     _retainedTemporaryResources.count) {
            _pendingLifetimeSnapshot =
                [_retainedTemporaryResources copy];
            [_retainedTemporaryResources release];
            _retainedTemporaryResources = [[NSMutableArray alloc] init];
          }
          _internalStatus = DXMTMetal4CommandBufferStateError;
          _feedbackComplete = YES;
          [_feedbackCondition broadcast];
        }
        [_feedbackCondition unlock];
        if (feedbackRetainedResources) {
          [feedbackRetainedResources release];
          feedbackRetainedResources = nil;
        }
        // Destroy uncommitted options / feedback block (handler never runs).
        if (options) {
          [options release];
          options = nil;
        }
        dxmt_metal4_hang_log(
            DXMT_METAL4_HANG_LOG_BOTH,
            "err:   DXMT Metal4 commit closed-loop abort: commandBuffer=%p "
            "queue=%p completion=%" PRIu64 " metalSubmitted=0"
            " timelineSignaled=%u frame=%" PRIu64 " chunk=%" PRIu64
            " reason=pre-submit-abort\n",
            self, _owner.metal4Queue, completionValue,
            completionTimelineSignaled ? 1u : 0u, _diagnosticInfo.frame_id,
            _diagnosticInfo.chunk_id);
      }
    }
    dxmt_metal4_commit_locked_depth--;
  }

  [_owner monitorSetPhase:DXMTMetal4QueueMonitorPhaseFinalize
            commandBuffer:self];
  dxmt_apitrace_finalize_command_buffer((obj_handle_t)self);
  if (dxmt_metal4_dense_hang_diagnostics_enabled() ||
      dxmt_metal4_perf_stats_enabled()) {
    /* File-only for the success path to avoid flooding wine-launch stderr. */
    dxmt_metal4_hang_log(
        DXMT_METAL4_HANG_LOG_FILE,
        "info:  DXMT Metal4 commit result: commandBuffer=%p metalBuffer=%p "
        "queue=%p status=%u completion=%" PRIu64 " residencyUs=%" PRIu64
        " drawable=%d waitCount=%lu signalCount=%lu"
        " frame=%" PRIu64 " chunk=%" PRIu64 " d3dSeq=%" PRIu64 "-%" PRIu64
        " externalWait=%u skippedExternalFenceWait=%u"
        " reason=submitted\n",
        self, _metal4Buffer, _owner.metal4Queue,
        (unsigned)DXMTMetal4CommandBufferStateCommitted, completionValue,
        residency_submit_us, _pendingDrawable != nil,
        (unsigned long)_pendingWaitEvents.count,
        (unsigned long)_pendingSignalEvents.count, _diagnosticInfo.frame_id,
        _diagnosticInfo.chunk_id, _diagnosticInfo.d3d_sequence_begin,
        _diagnosticInfo.d3d_sequence_end,
        _diagnosticInfo.external_fence_wait_count,
        _diagnosticInfo.skipped_external_fence_wait_count);
  }
  return residency_submit_us;
}

- (void)waitUntilCompleted {
  [_feedbackCondition lock];
  const unsigned entry_status = (unsigned)_internalStatus;
  const uint64_t entry_completion = _completionValue;
  const BOOL entry_feedback = _feedbackComplete;
  const BOOL needsCommit =
      _internalStatus == DXMTMetal4CommandBufferStateNotEnqueued;
  [_feedbackCondition unlock];
  const uint64_t entry_signaled =
      _owner.event ? _owner.event.signaledValue : 0;
  const BOOL hang_diag = dxmt_metal4_dense_hang_diagnostics_enabled() ||
                         dxmt_metal4_perf_stats_enabled();
  if (hang_diag) {
    dxmt_metal4_hang_log(
        DXMT_METAL4_HANG_LOG_BOTH,
        "info:  DXMT Metal4 waitUntilCompleted enter: commandBuffer=%p "
        "metalBuffer=%p queue=%p status=%u completion=%" PRIu64
        " signaled=%" PRIu64 " feedbackComplete=%u needsCommit=%u"
        " frame=%" PRIu64 " chunk=%" PRIu64 " d3dSeq=%" PRIu64 "-%" PRIu64
        " externalWait=%u skippedExternalFenceWait=%u\n",
        self, _metal4Buffer, _owner.metal4Queue, entry_status,
        entry_completion, entry_signaled, entry_feedback ? 1u : 0u,
        needsCommit ? 1u : 0u, _diagnosticInfo.frame_id,
        _diagnosticInfo.chunk_id, _diagnosticInfo.d3d_sequence_begin,
        _diagnosticInfo.d3d_sequence_end,
        _diagnosticInfo.external_fence_wait_count,
        _diagnosticInfo.skipped_external_fence_wait_count);
  }
  if (needsCommit)
    [self commit];

  [_feedbackCondition lock];
  const uint64_t completionValue = _completionValue;
  const unsigned wait_status = (unsigned)_internalStatus;
  const BOOL wait_feedback = _feedbackComplete;
  [_feedbackCondition unlock];
  if (!completionValue) {
    dxmt_metal4_hang_log(
        DXMT_METAL4_HANG_LOG_BOTH,
        "err:   DXMT Metal4 waitUntilCompleted zero-completion: "
        "commandBuffer=%p queue=%p status=%u feedbackComplete=%u "
        "entryStatus=%u entryCompletion=%" PRIu64 " needsCommit=%u"
        " frame=%" PRIu64 " chunk=%" PRIu64
        " reason=zero-completion-risks-feedback-hang\n",
        self, _owner.metal4Queue, wait_status, wait_feedback ? 1u : 0u,
        entry_status, entry_completion, needsCommit ? 1u : 0u,
        _diagnosticInfo.frame_id, _diagnosticInfo.chunk_id);
  }
  if (completionValue) {
    if (!dxmt_metal4_perf_stats_enabled() &&
        !dxmt_metal4_dense_hang_diagnostics_enabled()) {
      [_owner.event waitUntilSignaledValue:completionValue timeoutMS:UINT64_MAX];
    } else {
      uint64_t wait_begin_us = dxmt_monotonic_us();
      uint64_t timeout_count = 0;
      while (![_owner.event waitUntilSignaledValue:completionValue
                                        timeoutMS:1000]) {
        timeout_count++;
        uint64_t elapsed_ms = (dxmt_monotonic_us() - wait_begin_us) / 1000;
        if (timeout_count <= 5 || (timeout_count % 5) == 0) {
          uint64_t completion_current = _owner.event.signaledValue;
          uint64_t present_current = _owner.presentEvent.signaledValue;
          [_feedbackCondition lock];
          const unsigned stall_status = (unsigned)_internalStatus;
          const BOOL stall_feedback = _feedbackComplete;
          [_feedbackCondition unlock];
          dxmt_metal4_hang_log(
              DXMT_METAL4_HANG_LOG_BOTH,
              "warn:  DXMT Metal4 completion stall: commandBuffer=%p metalBuffer=%p queue=%p"
              " elapsedMs=%" PRIu64 " completionTarget=%" PRIu64 " completionCurrent=%" PRIu64
              " waitCount=%lu signalCount=%lu drawable=%d presentTarget=%" PRIu64
              " presentCurrent=%" PRIu64 " status=%u feedbackComplete=%u"
              " frame=%" PRIu64 " chunk=%" PRIu64 "\n",
              self, _metal4Buffer, _owner.metal4Queue, elapsed_ms,
              completionValue, completion_current,
              (unsigned long)_pendingWaitEvents.count,
              (unsigned long)_pendingSignalEvents.count,
              _pendingDrawable != nil, _owner.presentEventValue,
              present_current, stall_status, stall_feedback ? 1u : 0u,
              _diagnosticInfo.frame_id, _diagnosticInfo.chunk_id);
          const struct WMTCommandBufferDiagnosticInfo *diag = &_diagnosticInfo;
          dxmt_metal4_hang_log(
              DXMT_METAL4_HANG_LOG_BOTH,
              "warn:  DXMT Metal4 completion diagnostic: commandBuffer=%p label=%s"
              " frame=%" PRIu64 " chunk=%" PRIu64
              " d3dSequence=%" PRIu64 "..%" PRIu64
              " inputEncoders=%u encodedEncoders=%u render=%u compute=%u blit=%u other=%u"
              " present=%u clear=%u resolve=%u scaler=%u timestamps=%u"
              " barrierOnly=%u fenceWait=%u fenceUpdate=%u"
              " priorLocal=%u futureLocal=%u sameEncoder=%u external=%u"
              " repeatedUpdate=%u renderCrossStage=%u renderSameStage=%u"
              " renderReverseStage=%u localFenceIds=%u boundFenceSlots=%u"
              " encodedFenceWait=%u skippedExternalFenceWait=%u"
              " fenceEdgeCount=%u fenceEdgeOverflow=%u"
              " resourceInitializerEvent=%" PRIu64 "\n",
              self,
              _metal4Buffer.label ? _metal4Buffer.label.UTF8String : "<none>",
              diag->frame_id, diag->chunk_id, diag->d3d_sequence_begin,
              diag->d3d_sequence_end, diag->input_encoder_count,
              diag->encoded_encoder_count, diag->render_encoder_count,
              diag->compute_encoder_count, diag->blit_encoder_count,
              diag->other_encoder_count, diag->present_encoder_count,
              diag->clear_encoder_count, diag->resolve_encoder_count,
              diag->scaler_encoder_count, diag->timestamp_encoder_count,
              diag->barrier_only_pass_count, diag->fence_wait_count,
              diag->fence_update_count, diag->prior_local_fence_wait_count,
              diag->future_local_fence_wait_count,
              diag->same_encoder_fence_wait_count,
              diag->external_fence_wait_count,
              diag->repeated_fence_update_count,
              diag->render_valid_cross_stage_count,
              diag->render_same_stage_wait_count,
              diag->render_reverse_stage_wait_count,
              diag->local_fence_id_count, diag->bound_fence_slot_count,
              diag->encoded_fence_wait_count,
              diag->skipped_external_fence_wait_count,
              diag->fence_edge_count, diag->fence_edge_overflow_count,
              diag->resource_initializer_event_id);
          for (uint32_t edge_index = 0;
               edge_index < diag->fence_edge_count &&
               edge_index < WMT_COMMAND_BUFFER_FENCE_EDGE_CAPACITY;
               edge_index++) {
            const struct WMTCommandBufferFenceEdgeDiagnostic *edge =
                &diag->fence_edges[edge_index];
            dxmt_metal4_hang_log(
                DXMT_METAL4_HANG_LOG_BOTH,
                "warn:  DXMT Metal4 completion fence edge: commandBuffer=%p"
                " edge=%u producer=%" PRIu64 " consumer=%" PRIu64
                " producerIndex=%u consumerIndex=%u slot=%u flags=0x%x\n",
                self, edge_index, edge->producer_id, edge->consumer_id,
                edge->producer_index, edge->consumer_index, edge->slot,
                edge->flags);
          }
          NSUInteger wait_index = 0;
          for (DXMTMetal4QueueEvent *wait in _pendingWaitEvents) {
            uint64_t wait_current = dxmt_metal4_shared_event_value(wait.event);
            dxmt_metal4_hang_log(
                DXMT_METAL4_HANG_LOG_BOTH,
                "warn:  DXMT Metal4 completion wait: commandBuffer=%p index=%lu event=%p"
                " target=%" PRIu64 " current=%" PRIu64 " blocked=%d\n",
                self, (unsigned long)wait_index++, wait.event, wait.value,
                wait_current, wait_current < wait.value);
          }
          NSUInteger signal_index = 0;
          for (DXMTMetal4QueueEvent *signal in _pendingSignalEvents) {
            uint64_t signal_current =
                dxmt_metal4_shared_event_value(signal.event);
            dxmt_metal4_hang_log(
                DXMT_METAL4_HANG_LOG_BOTH,
                "warn:  DXMT Metal4 completion signal: commandBuffer=%p index=%lu event=%p"
                " target=%" PRIu64 " current=%" PRIu64 "\n",
                self, (unsigned long)signal_index++, signal.event,
                signal.value, signal_current);
          }
        }
      }
      if (timeout_count) {
        dxmt_metal4_hang_log(
            DXMT_METAL4_HANG_LOG_BOTH,
            "warn:  DXMT Metal4 completion recovered: commandBuffer=%p elapsedMs=%" PRIu64
            " completionTarget=%" PRIu64 " completionCurrent=%" PRIu64 "\n",
            self, (dxmt_monotonic_us() - wait_begin_us) / 1000, completionValue,
            _owner.event.signaledValue);
      }
    }
    // GPU timeline has passed this generation: drop completed *transient*
    // residency memberships without holding the commit serializer.
    [_owner retireCompletedResidency];
  }

  // The queue timeline may be signaled before Metal invokes its CPU feedback
  // handler. Feedback owns final status/timing/error publication; temporary
  // resource lifetime is handed off via pendingLifetimeSnapshot and drained here
  // on the waiter thread after the latch is visible.
  [_feedbackCondition lock];
  if (hang_diag) {
    uint64_t feedback_begin_us = dxmt_monotonic_us();
    uint64_t feedback_timeouts = 0;
    while (!_feedbackComplete) {
      NSDate *deadline =
          [NSDate dateWithTimeIntervalSinceNow:1.0];
      if (![_feedbackCondition waitUntilDate:deadline]) {
        feedback_timeouts++;
        if (feedback_timeouts <= 5 || (feedback_timeouts % 5) == 0) {
          const uint64_t elapsed_ms =
              (dxmt_monotonic_us() - feedback_begin_us) / 1000;
          const uint64_t signaled =
              _owner.event ? _owner.event.signaledValue : 0;
          dxmt_metal4_hang_log(
              DXMT_METAL4_HANG_LOG_BOTH,
              "warn:  DXMT Metal4 feedback latch stall: commandBuffer=%p "
              "queue=%p elapsedMs=%" PRIu64 " completion=%" PRIu64
              " signaled=%" PRIu64 " status=%u feedbackComplete=0"
              " frame=%" PRIu64 " chunk=%" PRIu64 "\n",
              self, _owner.metal4Queue, elapsed_ms, completionValue, signaled,
              (unsigned)_internalStatus, _diagnosticInfo.frame_id,
              _diagnosticInfo.chunk_id);
        }
      }
    }
  } else {
    while (!_feedbackComplete)
      [_feedbackCondition wait];
  }
  [_feedbackCondition unlock];
  if (hang_diag) {
    dxmt_metal4_hang_log(
        DXMT_METAL4_HANG_LOG_FILE,
        "info:  DXMT Metal4 waitUntilCompleted leave: commandBuffer=%p "
        "queue=%p status=%u completion=%" PRIu64 " signaled=%" PRIu64
        " frame=%" PRIu64 " chunk=%" PRIu64 "\n",
        self, _owner.metal4Queue, (unsigned)_internalStatus, completionValue,
        _owner.event ? _owner.event.signaledValue : 0,
        _diagnosticInfo.frame_id, _diagnosticInfo.chunk_id);
  }
  [self drainPendingLifetimeOwnership];
  // GPTK Get/ReturnMTL4CommandBuffer: return objects to the pool only after
  // GPU completion and lifetime drain (never from CompletionQueue).
  [self recycleMetal4ObjectsIfNeeded];
}

- (enum WMTCommandBufferStatus)status {
  [_feedbackCondition lock];
  const enum WMTCommandBufferStatus status =
      (enum WMTCommandBufferStatus)_internalStatus;
  [_feedbackCondition unlock];
  return status;
}

- (NSError *)error {
  [_feedbackCondition lock];
  NSError *error = [[_feedbackError retain] autorelease];
  [_feedbackCondition unlock];
  return error;
}

- (id)logs {
  return nil;
}

- (double)kernelStartTime {
  return 0.0;
}

- (double)kernelEndTime {
  return 0.0;
}

- (double)GPUStartTime {
  [_feedbackCondition lock];
  const double startTime = _feedbackGPUStartTime;
  [_feedbackCondition unlock];
  return startTime;
}

- (double)GPUEndTime {
  [_feedbackCondition lock];
  const double endTime = _feedbackGPUEndTime;
  [_feedbackCondition unlock];
  return endTime;
}

- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value {
  if (!dxmt_metal4_validate_event(event, "signal", value))
    return;
  dxmt_apitrace_record_command_buffer_event(
      (obj_handle_t)self,
      APITRACE_METAL_QUEUE_EVENT_SIGNAL,
      (obj_handle_t)event,
      value,
      APITRACE_METAL_QUEUE_EVENT_RECORDED);
  DXMTMetal4QueueEvent *op = [[DXMTMetal4QueueEvent alloc] initWithEvent:event value:value];
  [_pendingSignalEvents addObject:op];
  [op release];
}

- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value {
  if (!dxmt_metal4_validate_event(event, "wait", value))
    return;
  dxmt_apitrace_record_command_buffer_event(
      (obj_handle_t)self,
      APITRACE_METAL_QUEUE_EVENT_WAIT,
      (obj_handle_t)event,
      value,
      APITRACE_METAL_QUEUE_EVENT_RECORDED);
  DXMTMetal4QueueEvent *op = [[DXMTMetal4QueueEvent alloc] initWithEvent:event value:value];
  [_pendingWaitEvents addObject:op];
  [op release];
}

- (void)presentDrawable:(id<MTLDrawable>)drawable {
  [_pendingDrawable release];
  _pendingDrawable = [drawable retain];
  if ([drawable conformsToProtocol:@protocol(CAMetalDrawable)]) {
    id<CAMetalDrawable> metalDrawable = (id<CAMetalDrawable>)drawable;
    CAMetalLayer *layer = metalDrawable.layer;
    id<MTLResidencySet> layerSet = layer.residencySet;
    if (layerSet)
      [_metal4Buffer useResidencySet:layerSet];
    if (dxmt_metal4_residency_diag_enabled()) {
      static atomic_uint_fast64_t occurrence = 0;
      const uint64_t index = atomic_fetch_add_explicit(
                                 &occurrence, 1, memory_order_relaxed) +
                             1;
      id<MTLTexture> texture = metalDrawable.texture;
      if (texture && !texture.label) {
        texture.label = [NSString
            stringWithFormat:@"DXMT drawable texture=%p layer=%p", texture,
                             layer];
      }
      if (index <= 8 || (index & (index - 1)) == 0) {
        fprintf(stderr,
                "info:  DXMT Metal4 drawable residency: index=%" PRIu64
                " drawable=%p texture=%p layer=%p set=%p contains=%u allocations=%lu\n",
                index, drawable, texture, layer, layerSet,
                layerSet && texture
                    ? [layerSet containsAllocation:(id<MTLAllocation>)texture]
                    : 0,
                layerSet ? (unsigned long)layerSet.allocationCount : 0ul);
        fflush(stderr);
      }
    }
  }
  _hasPresentDuration = NO;
}

- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(double)duration {
  [_pendingDrawable release];
  _pendingDrawable = [drawable retain];
  if ([drawable conformsToProtocol:@protocol(CAMetalDrawable)]) {
    id<CAMetalDrawable> metalDrawable = (id<CAMetalDrawable>)drawable;
    CAMetalLayer *layer = metalDrawable.layer;
    id<MTLResidencySet> layerSet = layer.residencySet;
    if (layerSet)
      [_metal4Buffer useResidencySet:layerSet];
    if (dxmt_metal4_residency_diag_enabled()) {
      id<MTLTexture> texture = metalDrawable.texture;
      if (texture && !texture.label) {
        texture.label = [NSString
            stringWithFormat:@"DXMT drawable texture=%p layer=%p", texture,
                             layer];
      }
    }
  }
  _presentDuration = duration;
  _hasPresentDuration = YES;
}

- (void)dxmtWriteTimestampIntoHeap:(id<MTL4CounterHeap>)heap atIndex:(NSUInteger)index {
  dxmt_apitrace_record_counter_event(
      (obj_handle_t)self,
      APITRACE_METAL_COUNTER_WRITE_TIMESTAMP,
      (obj_handle_t)heap,
      index,
      0,
      0,
      0,
      0,
      0,
      0);
  [_metal4Buffer writeTimestampIntoHeap:heap atIndex:index];
}

- (void)dxmtResolveCounterHeap:(id<MTL4CounterHeap>)heap
                      withRange:(NSRange)range
                     intoBuffer:(id<MTLBuffer>)buffer
                         offset:(uint64_t)offset
                         length:(uint64_t)length
                      waitFence:(id<MTLFence>)fenceToWait
                    updateFence:(id<MTLFence>)fenceToUpdate {
  if (!heap || !buffer || !length)
    return;
  // Defined above helper section; keep a local guard (same crash class).
  if (![(id)buffer respondsToSelector:@selector(gpuAddress)] ||
      ![(id)buffer respondsToSelector:@selector(length)]) {
    fprintf(stderr,
            "err:   DXMT Metal4 resolveCounterHeap bad buffer: ptr=%p class=%s\n",
            buffer, object_getClassName((id)buffer));
    fflush(stderr);
    return;
  }

  dxmt_apitrace_record_counter_event(
      (obj_handle_t)self,
      APITRACE_METAL_COUNTER_RESOLVE_HEAP,
      (obj_handle_t)heap,
      range.location,
      range.length,
      (obj_handle_t)buffer,
      offset,
      length,
      (obj_handle_t)fenceToWait,
      (obj_handle_t)fenceToUpdate);
  [_metal4Buffer resolveCounterHeap:heap
                           withRange:range
                          intoBuffer:MTL4BufferRangeMake([buffer gpuAddress] + offset, length)
                           waitFence:fenceToWait
                         updateFence:fenceToUpdate];
}
@end

void
DXMTMetal4CommandBuffer_writeTimestampIntoHeap(obj_handle_t cmdbuf, obj_handle_t heap, uint64_t index) {
  [(DXMTMetal4CommandBuffer *)cmdbuf dxmtWriteTimestampIntoHeap:(id<MTL4CounterHeap>)heap
                                                        atIndex:(NSUInteger)index];
}

void
DXMTMetal4CommandBuffer_resolveCounterHeap(
    obj_handle_t cmdbuf, obj_handle_t heap, uint64_t start, uint64_t count,
    obj_handle_t dst_buffer, uint64_t dst_offset, uint64_t dst_length,
    obj_handle_t wait_fence, obj_handle_t update_fence) {
  [(DXMTMetal4CommandBuffer *)cmdbuf dxmtResolveCounterHeap:(id<MTL4CounterHeap>)heap
                                                  withRange:NSMakeRange((NSUInteger)start, (NSUInteger)count)
                                                 intoBuffer:(id<MTLBuffer>)dst_buffer
                                                     offset:dst_offset
                                                     length:dst_length
                                                  waitFence:(id<MTLFence>)wait_fence
                                                updateFence:(id<MTLFence>)update_fence];
}

struct dxmt_metal4_argument_binding {
  id<MTLBuffer> buffer;
  uint64_t offset;
};

struct dxmt_metal4_argument_state {
  id<MTL4ArgumentTable> table;
  DXMTMetal4CommandBuffer *owner;
  struct dxmt_metal4_argument_binding buffers[31];
  bool buffer_bound[31];
  id<MTLTexture> textures[128];
  bool texture_bound[128];
  NSMutableArray *upload_buffers;
};

static void
dxmt_metal4_argument_state_init(
    struct dxmt_metal4_argument_state *state,
    DXMTMetal4CommandBuffer *owner,
    NSString *label) {
  memset(state, 0, sizeof(*state));
  state->owner = owner;
  state->table = [owner newArgumentTableWithLabel:label];
  state->upload_buffers = [[NSMutableArray alloc] init];
}

static void
dxmt_metal4_argument_state_destroy(struct dxmt_metal4_argument_state *state) {
  [state->upload_buffers release];
  [state->table release];
}

// Guard against invalid obj_handle_t casts (UAF / wrong type). The flash-exit
// "NSConcreteValue gpuAddress" crash is this path calling Metal selectors on a
// non-buffer ObjC object. Check is intentional; log is rate-limited.
static bool
dxmt_metal4_is_mtl_buffer(id object) {
  if (!object)
    return false;
  // Prefer selector check over protocol: matches the crashing call site and
  // still rejects NSValue / other non-buffer objects.
  return [object respondsToSelector:@selector(gpuAddress)] &&
         [object respondsToSelector:@selector(length)];
}

static void
dxmt_diag_bad_buffer_handle(const char *site, id object, uint64_t offset,
                            unsigned index, DXMTMetal4CommandBuffer *owner) {
  static _Atomic uint32_t count = 0;
  const uint32_t n =
      atomic_fetch_add_explicit(&count, 1, memory_order_relaxed) + 1;
  // Keep early failures; then every 16th so a flood cannot dominate I/O.
  if (n > 48 && (n % 16) != 0)
    return;

  const char *cls = object ? object_getClassName(object) : "nil";
  fprintf(stderr,
          "err:   DXMT Metal4 bad buffer handle: site=%s ptr=%p class=%s "
          "offset=%llu index=%u owner=%p metal4Buffer=%p count=%u\n",
          site, object, cls, (unsigned long long)offset, index, owner,
          owner ? (void *)owner.metal4Buffer : NULL, n);
  if (object && [object isKindOfClass:[NSValue class]]) {
    fprintf(stderr,
            "err:   DXMT Metal4 bad buffer is NSValue pointerValue=%p "
            "objCType=%s\n",
            [(NSValue *)object pointerValue],
            [(NSValue *)object objCType] ? [(NSValue *)object objCType] : "?");
  }
  fflush(stderr);
}

static void
dxmt_metal4_argument_set_buffer(
    struct dxmt_metal4_argument_state *state,
    id<MTLBuffer> buffer,
    uint64_t offset,
    uint8_t index) {
  if (!state->table || index >= 31)
    return;
  if (buffer && !dxmt_metal4_is_mtl_buffer(buffer)) {
    dxmt_diag_bad_buffer_handle("argument_set_buffer", buffer, offset, index,
                                state->owner);
    // Do not cache a bad pointer; bind slot 0 so encode continues.
    state->buffers[index].buffer = nil;
    state->buffers[index].offset = 0;
    state->buffer_bound[index] = false;
    [state->table setAddress:0 atIndex:index];
    return;
  }
  bool same_buffer = state->buffer_bound[index] && state->buffers[index].buffer == buffer;
  if (same_buffer && state->buffers[index].offset == offset)
    return;
  state->buffers[index].buffer = buffer;
  state->buffers[index].offset = offset;
  state->buffer_bound[index] = true;
  [state->table setAddress:buffer ? [buffer gpuAddress] + offset : 0 atIndex:index];
}

static void
dxmt_metal4_argument_set_buffer_offset(
    struct dxmt_metal4_argument_state *state,
    uint64_t offset,
    uint8_t index) {
  if (index >= 31)
    return;
  dxmt_metal4_argument_set_buffer(state, state->buffers[index].buffer, offset, index);
}

static void
dxmt_metal4_argument_set_bytes(
    struct dxmt_metal4_argument_state *state,
    const void *bytes,
    uint64_t length,
    uint8_t index) {
  id<MTLBuffer> upload = [state->owner newUploadBufferWithBytes:bytes length:(NSUInteger)length];
  if (!upload)
    return;
  [state->upload_buffers addObject:upload];
  dxmt_metal4_argument_set_buffer(state, upload, 0, index);
  [upload release];
}

static void
dxmt_metal4_argument_set_texture(struct dxmt_metal4_argument_state *state, id<MTLTexture> texture, uint8_t index) {
  if (!state->table || index >= 128)
    return;
  if (texture && ![texture respondsToSelector:@selector(gpuResourceID)]) {
    dxmt_diag_bad_buffer_handle("argument_set_texture", texture, 0, index,
                                state->owner);
    state->textures[index] = nil;
    state->texture_bound[index] = false;
    [state->table setTexture:(MTLResourceID){0} atIndex:index];
    return;
  }
  if (state->texture_bound[index] && state->textures[index] == texture)
    return;
  state->textures[index] = texture;
  state->texture_bound[index] = true;
  [state->table setTexture:texture ? [texture gpuResourceID] : (MTLResourceID){0} atIndex:index];
}

static MTLStages
dxmt_metal4_render_stages(enum WMTRenderStages stages) {
  MTLStages ret = 0;
  if (stages & WMTRenderStageVertex)
    ret |= MTLStageVertex;
  if (stages & WMTRenderStageFragment)
    ret |= MTLStageFragment;
  if (stages & WMTRenderStageTile)
    ret |= MTLStageTile;
  if (stages & WMTRenderStageObject)
    ret |= MTLStageObject;
  if (stages & WMTRenderStageMesh)
    ret |= MTLStageMesh;
  return ret;
}

static MTLRenderStages
dxmt_metal4_render_argument_stages(enum WMTRenderStages stages) {
  MTLRenderStages ret = 0;
  if (stages & WMTRenderStageVertex)
    ret |= MTLRenderStageVertex;
  if (stages & WMTRenderStageFragment)
    ret |= MTLRenderStageFragment;
  return ret;
}

static NSUInteger
dxmt_metal4_index_buffer_length(id<MTLBuffer> buffer, uint64_t offset) {
  if (!buffer)
    return 0;
  if (!dxmt_metal4_is_mtl_buffer(buffer)) {
    dxmt_diag_bad_buffer_handle("index_buffer_length", buffer, offset, 0, nil);
    return 0;
  }
  NSUInteger length = [buffer length];
  if (offset >= length)
    return 0;
  return length - (NSUInteger)offset;
}

static MTLGPUAddress
dxmt_metal4_buffer_address(obj_handle_t buffer, uint64_t offset) {
  id metal_buffer = (id)buffer;
  if (!metal_buffer)
    return 0;
  if (!dxmt_metal4_is_mtl_buffer(metal_buffer)) {
    dxmt_diag_bad_buffer_handle("buffer_address", metal_buffer, offset, 0, nil);
    return 0;
  }
  return [(id<MTLBuffer>)metal_buffer gpuAddress] + offset;
}

struct dxmt_metal4_render_argument_state {
  struct dxmt_metal4_argument_state vertex;
  struct dxmt_metal4_argument_state fragment;
  struct dxmt_metal4_argument_state object;
  struct dxmt_metal4_argument_state mesh;
  struct dxmt_metal4_argument_state tile;
  bool vertex_table_bound;
  bool fragment_table_bound;
  bool object_table_bound;
  bool mesh_table_bound;
  bool tile_table_bound;
};

static void
dxmt_metal4_render_argument_state_init(
    struct dxmt_metal4_render_argument_state *state,
    DXMTMetal4CommandBuffer *owner) {
  dxmt_metal4_argument_state_init(&state->vertex, owner, @"DXMT4 Vertex Arguments");
  dxmt_metal4_argument_state_init(&state->fragment, owner, @"DXMT4 Fragment Arguments");
  dxmt_metal4_argument_state_init(&state->object, owner, @"DXMT4 Object Arguments");
  dxmt_metal4_argument_state_init(&state->mesh, owner, @"DXMT4 Mesh Arguments");
  dxmt_metal4_argument_state_init(&state->tile, owner, @"DXMT4 Tile Arguments");
  state->vertex_table_bound = false;
  state->fragment_table_bound = false;
  state->object_table_bound = false;
  state->mesh_table_bound = false;
  state->tile_table_bound = false;
}

static void
dxmt_metal4_render_argument_state_destroy(struct dxmt_metal4_render_argument_state *state) {
  dxmt_metal4_argument_state_destroy(&state->tile);
  dxmt_metal4_argument_state_destroy(&state->mesh);
  dxmt_metal4_argument_state_destroy(&state->object);
  dxmt_metal4_argument_state_destroy(&state->fragment);
  dxmt_metal4_argument_state_destroy(&state->vertex);
}

static void
dxmt_metal4_render_set_argument_tables(
    id<MTL4RenderCommandEncoder> encoder,
    struct dxmt_metal4_render_argument_state *state) {
  if (!state->vertex_table_bound) {
    [encoder setArgumentTable:state->vertex.table atStages:MTLRenderStageVertex];
    state->vertex_table_bound = true;
  }
  if (!state->fragment_table_bound) {
    [encoder setArgumentTable:state->fragment.table atStages:MTLRenderStageFragment];
    state->fragment_table_bound = true;
  }
  if (!state->object_table_bound) {
    [encoder setArgumentTable:state->object.table atStages:MTLRenderStageObject];
    state->object_table_bound = true;
  }
  if (!state->mesh_table_bound) {
    [encoder setArgumentTable:state->mesh.table atStages:MTLRenderStageMesh];
    state->mesh_table_bound = true;
  }
  if (!state->tile_table_bound) {
    [encoder setArgumentTable:state->tile.table atStages:MTLRenderStageTile];
    state->tile_table_bound = true;
  }
}

static void
dxmt_metal4_render_argument_set_buffer(
    struct dxmt_metal4_render_argument_state *state,
    id<MTLBuffer> buffer,
    uint64_t offset,
    uint8_t index,
    enum WMTRenderStages stages) {
  if (stages & WMTRenderStageVertex)
    dxmt_metal4_argument_set_buffer(&state->vertex, buffer, offset, index);
  if (stages & WMTRenderStageFragment)
    dxmt_metal4_argument_set_buffer(&state->fragment, buffer, offset, index);
  if (stages & WMTRenderStageObject)
    dxmt_metal4_argument_set_buffer(&state->object, buffer, offset, index);
  if (stages & WMTRenderStageMesh)
    dxmt_metal4_argument_set_buffer(&state->mesh, buffer, offset, index);
  if (stages & WMTRenderStageTile)
    dxmt_metal4_argument_set_buffer(&state->tile, buffer, offset, index);
}

static void
dxmt_metal4_render_argument_set_buffer_offset(
    struct dxmt_metal4_render_argument_state *state,
    uint64_t offset,
    uint8_t index,
    enum WMTRenderStages stages) {
  if (stages & WMTRenderStageVertex)
    dxmt_metal4_argument_set_buffer_offset(&state->vertex, offset, index);
  if (stages & WMTRenderStageFragment)
    dxmt_metal4_argument_set_buffer_offset(&state->fragment, offset, index);
  if (stages & WMTRenderStageObject)
    dxmt_metal4_argument_set_buffer_offset(&state->object, offset, index);
  if (stages & WMTRenderStageMesh)
    dxmt_metal4_argument_set_buffer_offset(&state->mesh, offset, index);
  if (stages & WMTRenderStageTile)
    dxmt_metal4_argument_set_buffer_offset(&state->tile, offset, index);
}

@interface DXMTMetal4ComputeEncoderState : NSObject {
@public
  struct dxmt_metal4_argument_state arguments;
  MTLSize threadgroupSize;
  bool argumentTableBound;
}
- (instancetype)initWithOwner:(DXMTMetal4CommandBuffer *)owner;
@end

@implementation DXMTMetal4ComputeEncoderState
- (instancetype)initWithOwner:(DXMTMetal4CommandBuffer *)owner {
  self = [super init];
  if (self) {
    dxmt_metal4_argument_state_init(&arguments, owner, @"DXMT4 Compute Arguments");
    threadgroupSize = MTLSizeMake(0, 0, 0);
    argumentTableBound = false;
  }
  return self;
}

- (void)dealloc {
  dxmt_metal4_argument_state_destroy(&arguments);
  [super dealloc];
}
@end

static void
dxmt_metal4_compute_set_argument_table_if_needed(
    id<MTL4ComputeCommandEncoder> encoder,
    DXMTMetal4ComputeEncoderState *state) {
  if (state->argumentTableBound)
    return;
  [encoder setArgumentTable:state->arguments.table];
  state->argumentTableBound = true;
}

@interface DXMTMetal4RenderEncoderState : NSObject {
@public
  struct dxmt_metal4_render_argument_state arguments;
}
- (instancetype)initWithOwner:(DXMTMetal4CommandBuffer *)owner;
@end

@implementation DXMTMetal4RenderEncoderState
- (instancetype)initWithOwner:(DXMTMetal4CommandBuffer *)owner {
  self = [super init];
  if (self)
    dxmt_metal4_render_argument_state_init(&arguments, owner);
  return self;
}

- (void)dealloc {
  dxmt_metal4_render_argument_state_destroy(&arguments);
  [super dealloc];
}
@end

#if DXMT_APITRACE_METAL
typedef NS_ENUM(uint32_t, DXMTApitraceEncoderKind) {
  DXMTApitraceEncoderKindRender = 1,
  DXMTApitraceEncoderKindCompute = 2,
  DXMTApitraceEncoderKindBlit = 3,
  DXMTApitraceEncoderKindMetal4Blit = 4,
};

struct dxmt_apitrace_argument_buffer_binding {
  obj_handle_t buffer;
  uint64_t offset;
  bool bound;
};

@interface DXMT4ApitraceCommandBufferState : NSObject
@property(nonatomic, assign) uint64_t beginSequence;
@property(nonatomic, assign) uint64_t d3dSequence;
@property(nonatomic, retain) NSMutableData *blitCopyTextureOps;
@property(nonatomic, retain) NSMutableData *blitWaitFences;
@property(nonatomic, retain) NSMutableData *blitUpdateFences;
@end

@implementation DXMT4ApitraceCommandBufferState
-(instancetype)init {
  self = [super init];
  if (self) {
    _blitCopyTextureOps = [[NSMutableData alloc] init];
    _blitWaitFences = [[NSMutableData alloc] init];
    _blitUpdateFences = [[NSMutableData alloc] init];
  }
  return self;
}
@end

@interface DXMT4ApitraceEncoderState : NSObject {
@public
  struct dxmt_apitrace_argument_buffer_binding renderVertexArgumentBuffers[31];
  struct dxmt_apitrace_argument_buffer_binding renderFragmentArgumentBuffers[31];
  struct dxmt_apitrace_argument_buffer_binding renderObjectArgumentBuffers[31];
  struct dxmt_apitrace_argument_buffer_binding renderMeshArgumentBuffers[31];
  struct dxmt_apitrace_argument_buffer_binding renderTileArgumentBuffers[31];
  struct dxmt_apitrace_argument_buffer_binding computeArgumentBuffers[31];
}
@property(nonatomic, assign) uint64_t beginSequence;
@property(nonatomic, assign) uint64_t commandBuffer;
@property(nonatomic, assign) uint64_t d3dSequence;
@property(nonatomic, assign) DXMTApitraceEncoderKind kind;
@property(nonatomic, assign) BOOL metalTraceBegan;
@property(nonatomic, retain) NSMutableArray *fenceOps;
@property(nonatomic, retain) NSMutableArray *blitOps;
@property(nonatomic, retain) NSMutableData *blitWaitFences;
@property(nonatomic, retain) NSMutableData *blitUpdateFences;
@end

@implementation DXMT4ApitraceEncoderState
-(instancetype)init {
  self = [super init];
  if (self) {
    _fenceOps = [[NSMutableArray alloc] init];
    _blitOps = [[NSMutableArray alloc] init];
    _blitWaitFences = [[NSMutableData alloc] init];
    _blitUpdateFences = [[NSMutableData alloc] init];
  }
  return self;
}
@end

static pthread_mutex_t dxmt_apitrace_lock = PTHREAD_MUTEX_INITIALIZER;
static apitrace_metal_session_t *dxmt_apitrace_session = NULL;
static char dxmt_apitrace_session_bundle_root[PATH_MAX] = {};
static NSMutableDictionary *dxmt_apitrace_command_buffers = nil;
static NSMutableDictionary *dxmt_apitrace_encoders = nil;
static NSMutableDictionary *dxmt_apitrace_functions = nil;
static __thread uint64_t dxmt_apitrace_current_d3d_sequence = 0;
static uint64_t dxmt_apitrace_blit_payload_count = 0;
static uint64_t dxmt_apitrace_blit_payload_ops = 0;
static uint64_t dxmt_apitrace_blit_payload_wait_fences = 0;
static uint64_t dxmt_apitrace_blit_payload_update_fences = 0;
static uint64_t dxmt_apitrace_blit_payload_bytes = 0;
static uint64_t dxmt_apitrace_blit_payload_build_ns = 0;
static uint64_t dxmt_apitrace_blit_payload_max_build_ns = 0;
static const NSUInteger dxmt_apitrace_command_buffer_blit_ops_flush_threshold = 256;

static uint32_t
dxmt_apitrace_command_buffer_copy_texture_count(DXMT4ApitraceCommandBufferState *state) {
  return state ? (uint32_t)(state.blitCopyTextureOps.length / sizeof(apitrace_metal_copy_texture_op_t)) : 0;
}

static void
dxmt_apitrace_set_sequence_locked(apitrace_metal_session_t *session, uint64_t sequence) {
  dxmt_apitrace_current_d3d_sequence = sequence;
  if (session)
    apitrace_metal_set_current_d3d_sequence(session, sequence);
}

static bool
dxmt_apitrace_truthy_env_value(const char *value) {
  return dxmt_truthy_env_value(value);
}

static bool
dxmt_apitrace_runtime_enabled(void) {
  const char *enabled = getenv("DXMT_APITRACE_ENABLED");
  return dxmt_apitrace_truthy_env_value(enabled);
}

static apitrace_metal_session_t *
dxmt_apitrace_ensure_session_locked(void);

static void
dxmt_apitrace_close_session_locked(void);

static bool
dxmt_apitrace_has_bundle_suffix(const char *path) {
  if (!path)
    return false;
  static const char suffix[] = ".apitrace";
  const size_t path_len = strlen(path);
  const size_t suffix_len = sizeof(suffix) - 1;
  return path_len >= suffix_len && !strcmp(path + path_len - suffix_len, suffix);
}

static const char *
dxmt_apitrace_bundle_root(void) {
  const char *bundle_root = getenv("APITRACE_TRACE_BUNDLE");
  if (bundle_root && bundle_root[0] && dxmt_apitrace_has_bundle_suffix(bundle_root))
    return bundle_root;

  static bool warned = false;
  if (!warned) {
    warned = true;
    fprintf(stderr,
            "warn:  DXMT apitrace: APITRACE_TRACE_BUNDLE not set to a .apitrace bundle; "
            "PE side must initialize child bundle root before opening unix session\n");
  }
  return NULL;
}

static void
dxmt_apitrace_log(const char *message, uint64_t arg0, uint64_t arg1) {
  (void)message;
  (void)arg0;
  (void)arg1;
}

static uint64_t
dxmt_apitrace_monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void
dxmt_apitrace_record_blit_payload_stats(uint64_t build_ns, NSUInteger ops_count, NSUInteger wait_count, NSUInteger update_count, NSUInteger payload_bytes) {
  ++dxmt_apitrace_blit_payload_count;
  dxmt_apitrace_blit_payload_ops += (uint64_t)ops_count;
  dxmt_apitrace_blit_payload_wait_fences += (uint64_t)wait_count;
  dxmt_apitrace_blit_payload_update_fences += (uint64_t)update_count;
  dxmt_apitrace_blit_payload_bytes += (uint64_t)payload_bytes;
  dxmt_apitrace_blit_payload_build_ns += build_ns;
  if (build_ns > dxmt_apitrace_blit_payload_max_build_ns)
    dxmt_apitrace_blit_payload_max_build_ns = build_ns;
}

static void
dxmt_apitrace_log_blit_payload_stats(void) {
  if (!dxmt_apitrace_blit_payload_count)
    return;
  fprintf(stderr,
          "info:  DXMT apitrace blit payload stats: count=%llu ops=%llu wait_fences=%llu update_fences=%llu bytes=%llu build_ns=%llu max_build_ns=%llu\n",
          (unsigned long long)dxmt_apitrace_blit_payload_count,
          (unsigned long long)dxmt_apitrace_blit_payload_ops,
          (unsigned long long)dxmt_apitrace_blit_payload_wait_fences,
          (unsigned long long)dxmt_apitrace_blit_payload_update_fences,
          (unsigned long long)dxmt_apitrace_blit_payload_bytes,
          (unsigned long long)dxmt_apitrace_blit_payload_build_ns,
          (unsigned long long)dxmt_apitrace_blit_payload_max_build_ns);
}

static uint64_t
dxmt_apitrace_texture_region_bytes_size(const struct WMTSize *size, uint64_t bytes_per_row, uint64_t bytes_per_image) {
  if (!size || !size->width || !size->height || !size->depth || !bytes_per_row)
    return 0;
  if (size->depth > 1) {
    if (!bytes_per_image)
      return 0;
    return bytes_per_image * size->depth;
  }
  return bytes_per_row * size->height;
}

static NSNumber *
dxmt_apitrace_key(obj_handle_t handle) {
  return [NSNumber numberWithUnsignedLongLong:(unsigned long long)handle];
}

static NSDictionary *
dxmt_apitrace_function_state_locked(obj_handle_t handle) {
  if (!handle || !dxmt_apitrace_functions)
    return nil;
  return [dxmt_apitrace_functions objectForKey:dxmt_apitrace_key(handle)];
}

static size_t
dxmt_apitrace_constant_data_size(enum WMTDataType type) {
  switch (type) {
  case WMTDataTypeFloat:
  case WMTDataTypeInt:
  case WMTDataTypeUInt:
  case WMTDataTypeBool:
    return 4;
  case WMTDataTypeFloat2:
  case WMTDataTypeInt2:
  case WMTDataTypeUInt2:
    return 8;
  case WMTDataTypeFloat3:
  case WMTDataTypeInt3:
  case WMTDataTypeUInt3:
    return 12;
  case WMTDataTypeFloat4:
  case WMTDataTypeInt4:
  case WMTDataTypeUInt4:
    return 16;
  default:
    return 0;
  }
}

static NSArray *
dxmt_apitrace_function_constants_json(const struct WMTFunctionConstant *constants, uint64_t num_constants, uint64_t bool_values) {
  NSMutableArray *values = [NSMutableArray arrayWithCapacity:num_constants];
  for (uint64_t i = 0; i < num_constants; ++i) {
    const struct WMTFunctionConstant *constant = &constants[i];
    NSMutableDictionary *entry = [NSMutableDictionary dictionary];
    entry[@"index"] = @(constant->index);
    entry[@"type"] = @(constant->type);
    if (constant->type == WMTDataTypeBool) {
      entry[@"bool_value"] = @((bool_values & (UINT64_C(1) << i)) != 0);
    } else {
      const size_t size = dxmt_apitrace_constant_data_size(constant->type);
      const uint8_t *bytes = constant->data.ptr;
      NSMutableArray *byte_values = [NSMutableArray arrayWithCapacity:size];
      for (size_t byte_index = 0; byte_index < size; ++byte_index) {
        [byte_values addObject:@(bytes ? bytes[byte_index] : 0)];
      }
      entry[@"bytes"] = byte_values;
    }
    [values addObject:entry];
  }
  return values;
}

static void
dxmt_apitrace_track_function_locked(obj_handle_t function, obj_handle_t library, NSString *name) {
  if (!function || !library || !name)
    return;
  if (!dxmt_apitrace_functions)
    dxmt_apitrace_functions = [[NSMutableDictionary alloc] init];
  NSDictionary *state = @{
    @"library_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)library],
    @"function_name" : name,
  };
  [dxmt_apitrace_functions setObject:state forKey:dxmt_apitrace_key(function)];
  if (dxmt_apitrace_ensure_session_locked()) {
    NSData *payload_data = [NSJSONSerialization dataWithJSONObject:state options:0 error:nil];
    NSString *payload = payload_data ? [[NSString alloc] initWithData:payload_data encoding:NSUTF8StringEncoding] : nil;
    if (payload) {
      apitrace_metal_object_metadata(dxmt_apitrace_session, function, payload.UTF8String);
      [payload release];
    }
  }
}

static void
dxmt_apitrace_track_function_with_constants_locked(
    obj_handle_t function, obj_handle_t library, NSString *name, const struct WMTFunctionConstant *constants,
    uint64_t num_constants, uint64_t bool_values) {
  if (!function || !library || !name)
    return;
  if (!dxmt_apitrace_functions)
    dxmt_apitrace_functions = [[NSMutableDictionary alloc] init];
  NSDictionary *state = @{
    @"library_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)library],
    @"function_name" : name,
    @"function_constants" : dxmt_apitrace_function_constants_json(constants, num_constants, bool_values),
  };
  [dxmt_apitrace_functions setObject:state forKey:dxmt_apitrace_key(function)];
  if (dxmt_apitrace_ensure_session_locked()) {
    NSData *payload_data = [NSJSONSerialization dataWithJSONObject:state options:0 error:nil];
    NSString *payload = payload_data ? [[NSString alloc] initWithData:payload_data encoding:NSUTF8StringEncoding] : nil;
    if (payload) {
      apitrace_metal_object_metadata(dxmt_apitrace_session, function, payload.UTF8String);
      [payload release];
    }
  }
}

static apitrace_metal_session_t *
dxmt_apitrace_ensure_session_locked(void) {
  if (!dxmt_apitrace_runtime_enabled())
    return NULL;

  const char *bundle_root = dxmt_apitrace_bundle_root();
  if (!bundle_root)
    return NULL;

  if (dxmt_apitrace_session) {
    if (!strcmp(dxmt_apitrace_session_bundle_root, bundle_root))
      return dxmt_apitrace_session;

    dxmt_apitrace_log("session bundle changed; reopening", 0, 0);
    dxmt_apitrace_close_session_locked();
  }

  dxmt_apitrace_session = apitrace_metal_session_open(bundle_root);
  if (!dxmt_apitrace_session)
    return NULL;
  snprintf(dxmt_apitrace_session_bundle_root, sizeof(dxmt_apitrace_session_bundle_root), "%s", bundle_root);

  if (!dxmt_apitrace_command_buffers)
    dxmt_apitrace_command_buffers = [[NSMutableDictionary alloc] init];
  if (!dxmt_apitrace_encoders)
    dxmt_apitrace_encoders = [[NSMutableDictionary alloc] init];
  if (!dxmt_apitrace_functions)
    dxmt_apitrace_functions = [[NSMutableDictionary alloc] init];

  dxmt_apitrace_log("session opened", 0, 0);
  return dxmt_apitrace_session;
}

static void
dxmt_apitrace_close_session_locked(void) {
  if (dxmt_apitrace_session) {
    dxmt_apitrace_log_blit_payload_stats();
    apitrace_metal_session_close(dxmt_apitrace_session);
    dxmt_apitrace_session = NULL;
  }
  dxmt_apitrace_session_bundle_root[0] = '\0';
  [dxmt_apitrace_command_buffers removeAllObjects];
  [dxmt_apitrace_encoders removeAllObjects];
  [dxmt_apitrace_functions removeAllObjects];
  dxmt_apitrace_current_d3d_sequence = 0;
}

static NSString *
dxmt_apitrace_json_string(id object) {
  if (!object)
    return @"{}";

  NSError *error = nil;
  NSData *data = [NSJSONSerialization dataWithJSONObject:object options:0 error:&error];
  if (!data || error)
    return @"{}";
  NSString *json = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
  return [json autorelease];
}

static NSNumber *
dxmt_apitrace_optional_handle(obj_handle_t handle) {
  return handle ? [NSNumber numberWithUnsignedLongLong:(unsigned long long)handle] : (NSNumber *)[NSNull null];
}

static NSString *
dxmt_apitrace_copy_texture_json(
    obj_handle_t source_texture,
    obj_handle_t destination_texture,
    const struct WMTOrigin *source_origin,
    const struct WMTSize *source_size,
    uint32_t source_slice,
    uint32_t source_level,
    const struct WMTOrigin *destination_origin,
    uint32_t destination_slice,
    uint32_t destination_level);

static void
dxmt_apitrace_append_fence_op(DXMT4ApitraceEncoderState *state, const char *op, obj_handle_t fence, uint32_t stages) {
  if (!state || !op)
    return;
  [state.fenceOps addObject:@[
    [NSString stringWithUTF8String:op],
    @(fence),
    @(stages),
  ]];
}

static void
dxmt_apitrace_flush_fence_ops(apitrace_metal_session_t *session, obj_handle_t encoder, DXMT4ApitraceEncoderState *state) {
  if (!session || !state || !state.fenceOps.count)
    return;
  NSString *payload = dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_fence_ops",
    @"ops" : state.fenceOps,
  });
  apitrace_metal_record_fence_ops(session, encoder, payload.UTF8String);
  [state.fenceOps removeAllObjects];
}

static void
dxmt_apitrace_flush_blit_ops(apitrace_metal_session_t *session, obj_handle_t encoder, DXMT4ApitraceEncoderState *state) {
  if (!session || !state ||
      (!state.blitOps.count && !state.blitWaitFences.length && !state.blitUpdateFences.length))
    return;
  const uint64_t build_start_ns = dxmt_apitrace_monotonic_ns();
  const NSUInteger ops_count = state.blitOps.count;
  const NSUInteger wait_count = state.blitWaitFences.length / sizeof(uint64_t);
  const NSUInteger update_count = state.blitUpdateFences.length / sizeof(uint64_t);
  if (!ops_count) {
    dxmt_apitrace_record_blit_payload_stats(
        dxmt_apitrace_monotonic_ns() - build_start_ns,
        ops_count,
        wait_count,
        update_count,
        0);
    apitrace_metal_blit_fence_batch(
        session,
        encoder,
        (const uint64_t *)state.blitWaitFences.bytes,
        (uint32_t)wait_count,
        (const uint64_t *)state.blitUpdateFences.bytes,
        (uint32_t)update_count);
    [state.blitWaitFences setLength:0];
    [state.blitUpdateFences setLength:0];
    return;
  }
  NSString *ops_json = state.blitOps.count ? dxmt_apitrace_json_string(state.blitOps) : @"[]";
  NSMutableString *payload = [NSMutableString stringWithFormat:@"{\"kind\":\"dxmt_blit_batch\",\"ops\":%@", ops_json];
  if (state.blitWaitFences.length || state.blitUpdateFences.length) {
    [payload appendString:@",\"fence_ops\":{\"schema\":\"blit-fence-v2\",\"stages\":0"];
    const uint64_t *wait_fences = (const uint64_t *)state.blitWaitFences.bytes;
    [payload appendString:@",\"wait_fences\":["];
    for (NSUInteger i = 0; i < wait_count; ++i) {
      if (i)
        [payload appendString:@","];
      [payload appendFormat:@"%llu", (unsigned long long)wait_fences[i]];
    }
    [payload appendString:@"]"];
    const uint64_t *update_fences = (const uint64_t *)state.blitUpdateFences.bytes;
    [payload appendString:@",\"update_fences\":["];
    for (NSUInteger i = 0; i < update_count; ++i) {
      if (i)
        [payload appendString:@","];
      [payload appendFormat:@"%llu", (unsigned long long)update_fences[i]];
    }
    [payload appendString:@"]}"];
  }
  [payload appendString:@"}"];
  dxmt_apitrace_record_blit_payload_stats(
      dxmt_apitrace_monotonic_ns() - build_start_ns,
      ops_count,
      wait_count,
      update_count,
      [payload lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
  apitrace_metal_blit_batch(session, encoder, payload.UTF8String);
  [state.blitOps removeAllObjects];
  [state.blitWaitFences setLength:0];
  [state.blitUpdateFences setLength:0];
}

static DXMT4ApitraceCommandBufferState *
dxmt_apitrace_command_buffer_state_for_id(obj_handle_t command_buffer) {
  if (!command_buffer || !dxmt_apitrace_command_buffers)
    return nil;
  return [dxmt_apitrace_command_buffers objectForKey:dxmt_apitrace_key(command_buffer)];
}

static DXMT4ApitraceCommandBufferState *
dxmt_apitrace_ensure_command_buffer_begin_locked(
    apitrace_metal_session_t *session,
    obj_handle_t command_buffer,
    uint64_t frame_id,
    uint64_t d3d_sequence) {
  if (!session || !command_buffer)
    return nil;

  NSNumber *key = dxmt_apitrace_key(command_buffer);
  DXMT4ApitraceCommandBufferState *state = [dxmt_apitrace_command_buffers objectForKey:key];
  if (state) {
    if (d3d_sequence && !state.d3dSequence)
      state.d3dSequence = d3d_sequence;
    return state;
  }

  uint64_t previous_sequence = dxmt_apitrace_current_d3d_sequence;
  dxmt_apitrace_set_sequence_locked(session, d3d_sequence);
  uint64_t begin_sequence = apitrace_metal_command_buffer_begin(session, command_buffer, frame_id, "");
  dxmt_apitrace_set_sequence_locked(session, previous_sequence);

  state = [[DXMT4ApitraceCommandBufferState alloc] init];
  state.beginSequence = begin_sequence;
  state.d3dSequence = d3d_sequence;
  [dxmt_apitrace_command_buffers setObject:state forKey:key];
  [state release];
  return state;
}

static void
dxmt_apitrace_flush_command_buffer_blit_batch(apitrace_metal_session_t *session, obj_handle_t command_buffer) {
  DXMT4ApitraceCommandBufferState *state = dxmt_apitrace_command_buffer_state_for_id(command_buffer);
  if (!session || !state || (!state.blitCopyTextureOps.length && !state.blitWaitFences.length && !state.blitUpdateFences.length))
    return;

  const uint32_t ops_count = dxmt_apitrace_command_buffer_copy_texture_count(state);
  const NSUInteger wait_count = state.blitWaitFences.length / sizeof(uint64_t);
  const NSUInteger update_count = state.blitUpdateFences.length / sizeof(uint64_t);
  apitrace_metal_blit_encoder_copy_texture_ops_batch(
      session,
      command_buffer,
      command_buffer,
      (const apitrace_metal_copy_texture_op_t *)state.blitCopyTextureOps.bytes,
      ops_count,
      (const uint64_t *)state.blitWaitFences.bytes,
      (uint32_t)wait_count,
      (const uint64_t *)state.blitUpdateFences.bytes,
      (uint32_t)update_count);
  dxmt_apitrace_record_blit_payload_stats(0, ops_count, wait_count, update_count, 0);
  [state.blitCopyTextureOps setLength:0];
  [state.blitWaitFences setLength:0];
  [state.blitUpdateFences setLength:0];
}

static void
dxmt_apitrace_accumulate_command_buffer_blit_fences(obj_handle_t command_buffer, DXMT4ApitraceEncoderState *encoder_state) {
  DXMT4ApitraceCommandBufferState *command_state = dxmt_apitrace_command_buffer_state_for_id(command_buffer);
  if (!command_state || !encoder_state)
    return;
  if (encoder_state.blitWaitFences.length)
    [command_state.blitWaitFences appendData:encoder_state.blitWaitFences];
  if (encoder_state.blitUpdateFences.length)
    [command_state.blitUpdateFences appendData:encoder_state.blitUpdateFences];
  [encoder_state.blitWaitFences setLength:0];
  [encoder_state.blitUpdateFences setLength:0];
}

static void
dxmt_apitrace_accumulate_command_buffer_blit_copy_texture(
    apitrace_metal_session_t *session,
    obj_handle_t command_buffer,
    const struct wmtcmd_blit_copy_from_texture_to_texture *cmd) {
  DXMT4ApitraceCommandBufferState *state = dxmt_apitrace_command_buffer_state_for_id(command_buffer);
  if (!state || !cmd)
    return;
  if (dxmt_apitrace_command_buffer_copy_texture_count(state) >= dxmt_apitrace_command_buffer_blit_ops_flush_threshold)
    dxmt_apitrace_flush_command_buffer_blit_batch(session, command_buffer);
  apitrace_metal_copy_texture_op_t op = {
    .source_texture_id = cmd->src,
    .destination_texture_id = cmd->dst,
    .source_origin_x = cmd->src_origin.x,
    .source_origin_y = cmd->src_origin.y,
    .source_origin_z = cmd->src_origin.z,
    .source_size_width = cmd->src_size.width,
    .source_size_height = cmd->src_size.height,
    .source_size_depth = cmd->src_size.depth,
    .source_slice = cmd->src_slice,
    .source_level = cmd->src_level,
    .destination_origin_x = cmd->dst_origin.x,
    .destination_origin_y = cmd->dst_origin.y,
    .destination_origin_z = cmd->dst_origin.z,
    .destination_slice = cmd->dst_slice,
    .destination_level = cmd->dst_level,
  };
  [state.blitCopyTextureOps appendBytes:&op length:sizeof(op)];
}

static void
dxmt_apitrace_begin_blit_encoder_if_needed(apitrace_metal_session_t *session, obj_handle_t encoder, DXMT4ApitraceEncoderState *state) {
  if (!session || !state || state.kind != DXMTApitraceEncoderKindBlit || state.metalTraceBegan)
    return;
  state.beginSequence = apitrace_metal_blit_encoder_begin(session, encoder, state.commandBuffer, "{}");
  state.metalTraceBegan = YES;
}

static void
dxmt_apitrace_emit_blit_encoder_batch(apitrace_metal_session_t *session, obj_handle_t encoder, DXMT4ApitraceEncoderState *state) {
  if (!session || !state ||
      (!state.blitOps.count && !state.blitWaitFences.length && !state.blitUpdateFences.length))
    return;
  const uint64_t build_start_ns = dxmt_apitrace_monotonic_ns();
  const NSUInteger ops_count = state.blitOps.count;
  const NSUInteger wait_count = state.blitWaitFences.length / sizeof(uint64_t);
  const NSUInteger update_count = state.blitUpdateFences.length / sizeof(uint64_t);
  if (!ops_count) {
    const uint64_t *wait_fences = (const uint64_t *)state.blitWaitFences.bytes;
    const uint64_t *update_fences = (const uint64_t *)state.blitUpdateFences.bytes;
    dxmt_apitrace_record_blit_payload_stats(
        dxmt_apitrace_monotonic_ns() - build_start_ns,
        ops_count,
        wait_count,
        update_count,
        0);
    apitrace_metal_blit_encoder_fence_batch(
        session,
        encoder,
        state.commandBuffer,
        wait_fences,
        (uint32_t)wait_count,
        update_fences,
        (uint32_t)update_count);
    [state.blitWaitFences setLength:0];
    [state.blitUpdateFences setLength:0];
    return;
  }
  NSString *ops_json = state.blitOps.count ? dxmt_apitrace_json_string(state.blitOps) : @"[]";
  NSMutableString *payload = [NSMutableString stringWithFormat:
      @"{\"command_buffer_id\":%llu,\"kind\":\"dxmt_blit_encoder_batch\",\"ops\":%@",
      (unsigned long long)state.commandBuffer,
      ops_json];
  if (state.blitWaitFences.length || state.blitUpdateFences.length) {
    [payload appendString:@",\"fence_ops\":{\"schema\":\"blit-fence-v2\",\"stages\":0"];
    const uint64_t *wait_fences = (const uint64_t *)state.blitWaitFences.bytes;
    [payload appendString:@",\"wait_fences\":["];
    for (NSUInteger i = 0; i < wait_count; ++i) {
      if (i)
        [payload appendString:@","];
      [payload appendFormat:@"%llu", (unsigned long long)wait_fences[i]];
    }
    [payload appendString:@"]"];
    const uint64_t *update_fences = (const uint64_t *)state.blitUpdateFences.bytes;
    [payload appendString:@",\"update_fences\":["];
    for (NSUInteger i = 0; i < update_count; ++i) {
      if (i)
        [payload appendString:@","];
      [payload appendFormat:@"%llu", (unsigned long long)update_fences[i]];
    }
    [payload appendString:@"]}"];
  }
  [payload appendString:@"}"];
  dxmt_apitrace_record_blit_payload_stats(
      dxmt_apitrace_monotonic_ns() - build_start_ns,
      ops_count,
      wait_count,
      update_count,
      [payload lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
  apitrace_metal_blit_encoder_batch_with_command_buffer(session, encoder, state.commandBuffer, payload.UTF8String);
  [state.blitOps removeAllObjects];
  [state.blitWaitFences setLength:0];
  [state.blitUpdateFences setLength:0];
}

static NSString *
dxmt_apitrace_texture_descriptor_json(const struct WMTTextureInfo *info) {
  NSMutableDictionary *dict = [NSMutableDictionary dictionary];
  dict[@"type"] = @(info->type);
  dict[@"pixel_format"] = @(info->pixel_format);
  dict[@"width"] = @(info->width);
  dict[@"height"] = @(info->height);
  dict[@"depth"] = @(info->depth);
  dict[@"array_length"] = @(info->array_length);
  dict[@"mipmap_level_count"] = @(info->mipmap_level_count);
  dict[@"sample_count"] = @(info->sample_count);
  dict[@"usage"] = @(info->usage);
  dict[@"options"] = @(info->options);
  dict[@"gpu_resource_id"] = @(info->gpu_resource_id);
  return dxmt_apitrace_json_string(dict);
}

static NSString *
dxmt_apitrace_render_pipeline_json(const struct WMTRenderPipelineInfo *info) {
  NSMutableDictionary *dict = [NSMutableDictionary dictionary];
  NSDictionary *vertex_state = dxmt_apitrace_function_state_locked(info->vertex_function);
  NSDictionary *fragment_state = dxmt_apitrace_function_state_locked(info->fragment_function);
  dict[@"rasterization_enabled"] = @(info->rasterization_enabled);
  dict[@"raster_sample_count"] = @(info->raster_sample_count);
  dict[@"depth_pixel_format"] = @(info->depth_pixel_format);
  dict[@"stencil_pixel_format"] = @(info->stencil_pixel_format);
  dict[@"color_pixel_format"] = @(info->colors[0].pixel_format);
  dict[@"alpha_to_coverage_enabled"] = @(info->alpha_to_coverage_enabled);
  dict[@"logic_operation_enabled"] = @(info->logic_operation_enabled);
  dict[@"logic_operation"] = @(info->logic_operation);
  dict[@"input_primitive_topology"] = @(info->input_primitive_topology);
  dict[@"immutable_vertex_buffers"] = @(info->immutable_vertex_buffers);
  dict[@"immutable_fragment_buffers"] = @(info->immutable_fragment_buffers);
  NSMutableArray *colors = [NSMutableArray arrayWithCapacity:8];
  for (unsigned i = 0; i < 8; i++) {
    const struct WMTColorAttachmentBlendInfo *color = &info->colors[i];
    [colors addObject:@{
      @"pixel_format" : @(color->pixel_format),
      @"blending_enabled" : @(color->blending_enabled),
      @"write_mask" : @(color->write_mask),
      @"rgb_blend_operation" : @(color->rgb_blend_operation),
      @"alpha_blend_operation" : @(color->alpha_blend_operation),
      @"src_rgb_blend_factor" : @(color->src_rgb_blend_factor),
      @"dst_rgb_blend_factor" : @(color->dst_rgb_blend_factor),
      @"src_alpha_blend_factor" : @(color->src_alpha_blend_factor),
      @"dst_alpha_blend_factor" : @(color->dst_alpha_blend_factor),
    }];
  }
  dict[@"colors"] = colors;
  if (vertex_state) {
    dict[@"vertex_library_id"] = vertex_state[@"library_id"];
    dict[@"vertex_function"] = vertex_state[@"function_name"];
  }
  if (fragment_state) {
    dict[@"fragment_library_id"] = fragment_state[@"library_id"];
    dict[@"fragment_function"] = fragment_state[@"function_name"];
    if (fragment_state[@"function_constants"])
      dict[@"fragment_function_constants"] = fragment_state[@"function_constants"];
  }
  return dxmt_apitrace_json_string(dict);
}

static NSString *
dxmt_apitrace_mesh_render_pipeline_json(const struct WMTMeshRenderPipelineInfo *info) {
  NSMutableDictionary *dict = [NSMutableDictionary dictionary];
  NSDictionary *object_state = dxmt_apitrace_function_state_locked(info->object_function);
  NSDictionary *mesh_state = dxmt_apitrace_function_state_locked(info->mesh_function);
  NSDictionary *fragment_state = dxmt_apitrace_function_state_locked(info->fragment_function);
  dict[@"pipeline_kind"] = @"mesh_render";
  dict[@"rasterization_enabled"] = @(info->rasterization_enabled);
  dict[@"raster_sample_count"] = @(info->raster_sample_count);
  dict[@"depth_pixel_format"] = @(info->depth_pixel_format);
  dict[@"stencil_pixel_format"] = @(info->stencil_pixel_format);
  dict[@"color_pixel_format"] = @(info->colors[0].pixel_format);
  dict[@"alpha_to_coverage_enabled"] = @(info->alpha_to_coverage_enabled);
  dict[@"logic_operation_enabled"] = @(info->logic_operation_enabled);
  dict[@"logic_operation"] = @(info->logic_operation);
  dict[@"immutable_object_buffers"] = @(info->immutable_object_buffers);
  dict[@"immutable_mesh_buffers"] = @(info->immutable_mesh_buffers);
  dict[@"immutable_fragment_buffers"] = @(info->immutable_fragment_buffers);
  dict[@"payload_memory_length"] = @(info->payload_memory_length);
  dict[@"mesh_tgsize_is_multiple_of_sgwidth"] = @(info->mesh_tgsize_is_multiple_of_sgwidth);
  dict[@"object_tgsize_is_multiple_of_sgwidth"] = @(info->object_tgsize_is_multiple_of_sgwidth);
  NSMutableArray *colors = [NSMutableArray arrayWithCapacity:8];
  for (unsigned i = 0; i < 8; i++) {
    const struct WMTColorAttachmentBlendInfo *color = &info->colors[i];
    [colors addObject:@{
      @"pixel_format" : @(color->pixel_format),
      @"blending_enabled" : @(color->blending_enabled),
      @"write_mask" : @(color->write_mask),
      @"rgb_blend_operation" : @(color->rgb_blend_operation),
      @"alpha_blend_operation" : @(color->alpha_blend_operation),
      @"src_rgb_blend_factor" : @(color->src_rgb_blend_factor),
      @"dst_rgb_blend_factor" : @(color->dst_rgb_blend_factor),
      @"src_alpha_blend_factor" : @(color->src_alpha_blend_factor),
      @"dst_alpha_blend_factor" : @(color->dst_alpha_blend_factor),
    }];
  }
  dict[@"colors"] = colors;
  if (object_state) {
    dict[@"object_library_id"] = object_state[@"library_id"];
    dict[@"object_function"] = object_state[@"function_name"];
    if (object_state[@"function_constants"])
      dict[@"object_function_constants"] = object_state[@"function_constants"];
  }
  if (mesh_state) {
    dict[@"mesh_library_id"] = mesh_state[@"library_id"];
    dict[@"mesh_function"] = mesh_state[@"function_name"];
    if (mesh_state[@"function_constants"])
      dict[@"mesh_function_constants"] = mesh_state[@"function_constants"];
  }
  if (fragment_state) {
    dict[@"fragment_library_id"] = fragment_state[@"library_id"];
    dict[@"fragment_function"] = fragment_state[@"function_name"];
    if (fragment_state[@"function_constants"])
      dict[@"fragment_function_constants"] = fragment_state[@"function_constants"];
  }
  return dxmt_apitrace_json_string(dict);
}

static NSString *
dxmt_apitrace_tile_render_pipeline_json(const struct WMTTileRenderPipelineInfo *info) {
  NSMutableDictionary *dict = [NSMutableDictionary dictionary];
  NSDictionary *tile_state = dxmt_apitrace_function_state_locked(info->tile_function);
  dict[@"pipeline_kind"] = @"tile_render";
  dict[@"raster_sample_count"] = @(info->raster_sample_count);
  dict[@"tgsize_matches_tile_size"] = @(info->tgsize_matches_tile_size);
  dict[@"immutable_tile_buffers"] = @(info->immutable_tile_buffers);
  NSMutableArray *colors = [NSMutableArray arrayWithCapacity:8];
  for (unsigned i = 0; i < 8; i++) {
    [colors addObject:@{@"pixel_format" : @(info->color_formats[i])}];
  }
  dict[@"colors"] = colors;
  if (tile_state) {
    dict[@"tile_library_id"] = tile_state[@"library_id"];
    dict[@"tile_function"] = tile_state[@"function_name"];
    if (tile_state[@"function_constants"])
      dict[@"tile_function_constants"] = tile_state[@"function_constants"];
  }
  return dxmt_apitrace_json_string(dict);
}

static NSDictionary *
dxmt_apitrace_stencil_json(const struct WMTStencilInfo *info) {
  return @{
    @"enabled" : @(info->enabled),
    @"depth_stencil_pass_op" : @(info->depth_stencil_pass_op),
    @"stencil_fail_op" : @(info->stencil_fail_op),
    @"depth_fail_op" : @(info->depth_fail_op),
    @"stencil_compare_function" : @(info->stencil_compare_function),
    @"write_mask" : @(info->write_mask),
    @"read_mask" : @(info->read_mask),
  };
}

static NSString *
dxmt_apitrace_depth_stencil_json(const struct WMTDepthStencilInfo *info, obj_handle_t dsso) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_depth_stencil_state",
    @"depth_stencil_state_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)dsso],
    @"depth_compare_function" : @(info->depth_compare_function),
    @"depth_write_enabled" : @(info->depth_write_enabled),
    @"front_stencil" : dxmt_apitrace_stencil_json(&info->front_stencil),
    @"back_stencil" : dxmt_apitrace_stencil_json(&info->back_stencil),
  });
}

static NSString *
dxmt_apitrace_set_rasterizer_state_json(const struct wmtcmd_render_setrasterizerstate *cmd) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_set_rasterizer_state",
    @"fill_mode" : @(cmd->fill_mode),
    @"cull_mode" : @(cmd->cull_mode),
    @"depth_clip_mode" : @(cmd->depth_clip_mode),
    @"winding" : @(cmd->winding),
    @"depth_bias" : @(cmd->depth_bias),
    @"slope_scale" : @(cmd->scole_scale),
    @"depth_bias_clamp" : @(cmd->depth_bias_clamp),
  });
}

static NSString *
dxmt_apitrace_set_depth_stencil_state_json(const struct wmtcmd_render_setdsso *cmd) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_set_depth_stencil_state",
    @"depth_stencil_state_id" : dxmt_apitrace_optional_handle(cmd->dsso),
    @"stencil_ref" : @(cmd->stencil_ref),
  });
}

static NSString *
dxmt_apitrace_set_blend_factor_json(const struct wmtcmd_render_setblendcolor *cmd) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_set_blend_factor",
    @"red" : @(cmd->red),
    @"green" : @(cmd->green),
    @"blue" : @(cmd->blue),
    @"alpha" : @(cmd->alpha),
    @"stencil_ref" : @(cmd->stencil_ref),
  });
}

static NSArray *
dxmt_apitrace_viewports_json(const struct WMTViewport *viewports, uint8_t count) {
  NSMutableArray *values = [NSMutableArray arrayWithCapacity:count];
  for (uint8_t i = 0; i < count; ++i) {
    const struct WMTViewport *viewport = &viewports[i];
    [values addObject:@[
      @(viewport->originX),
      @(viewport->originY),
      @(viewport->width),
      @(viewport->height),
      @(viewport->znear),
      @(viewport->zfar),
    ]];
  }
  return values;
}

static NSArray *
dxmt_apitrace_scissor_rects_json(const struct WMTScissorRect *rects, uint8_t count) {
  NSMutableArray *values = [NSMutableArray arrayWithCapacity:count];
  for (uint8_t i = 0; i < count; ++i) {
    const struct WMTScissorRect *rect = &rects[i];
    [values addObject:@[
      @(rect->x),
      @(rect->y),
      @(rect->width),
      @(rect->height),
    ]];
  }
  return values;
}

static NSString *
dxmt_apitrace_set_viewports_json(const struct WMTViewport *viewports, uint8_t count) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_set_viewports",
    @"viewports" : dxmt_apitrace_viewports_json(viewports, count),
  });
}

static NSString *
dxmt_apitrace_set_scissor_rects_json(const struct WMTScissorRect *rects, uint8_t count) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_set_scissor_rects",
    @"rects" : dxmt_apitrace_scissor_rects_json(rects, count),
  });
}

static NSString *
dxmt_apitrace_compute_pipeline_json(const struct WMTComputePipelineInfo *info) {
  NSMutableDictionary *dict = [NSMutableDictionary dictionary];
  NSDictionary *function_state = dxmt_apitrace_function_state_locked(info->compute_function);
  dict[@"immutable_buffers"] = @(info->immutable_buffers);
  dict[@"tgsize_is_multiple_of_sgwidth"] = @(info->tgsize_is_multiple_of_sgwidth);
  dict[@"fail_on_binary_archive_miss"] = @(info->fail_on_binary_archive_miss);
  dict[@"num_binary_archives_for_lookup"] = @(info->num_binary_archives_for_lookup);
  if (function_state) {
    dict[@"library_id"] = function_state[@"library_id"];
    dict[@"function"] = function_state[@"function_name"];
  }
  return dxmt_apitrace_json_string(dict);
}

static NSString *
dxmt_apitrace_render_pass_json(const struct WMTRenderPassInfo *info) {
  NSMutableDictionary *dict = [NSMutableDictionary dictionary];
  NSMutableArray *colors = [NSMutableArray array];
  for (unsigned i = 0; i < 8; i++) {
    if (!info->colors[i].texture && !info->colors[i].resolve_texture)
      continue;
    NSMutableDictionary *color = [NSMutableDictionary dictionary];
    color[@"slot"] = @(i);
    color[@"texture"] = dxmt_apitrace_optional_handle(info->colors[i].texture);
    color[@"resolve_texture"] = dxmt_apitrace_optional_handle(info->colors[i].resolve_texture);
    color[@"load_action"] = @(info->colors[i].load_action);
    color[@"store_action"] = @(info->colors[i].store_action);
    color[@"level"] = @(info->colors[i].level);
    color[@"slice"] = @(info->colors[i].slice);
    color[@"depth_plane"] = @(info->colors[i].depth_plane);
    color[@"clear_color"] = @[ @(info->colors[i].clear_color.r),
                               @(info->colors[i].clear_color.g),
                               @(info->colors[i].clear_color.b),
                               @(info->colors[i].clear_color.a) ];
    [colors addObject:color];
  }
  dict[@"colors"] = colors;
  if ([colors count] > 0) {
    NSDictionary *first_color = [colors objectAtIndex:0];
    id texture = first_color[@"texture"];
    if (texture && texture != [NSNull null])
      dict[@"color_texture_id"] = texture;
    dict[@"load_action"] = first_color[@"load_action"];
    dict[@"store_action"] = first_color[@"store_action"];
    dict[@"clear_color"] = first_color[@"clear_color"];
  }
  dict[@"default_raster_sample_count"] = @(info->default_raster_sample_count);
  dict[@"render_target_array_length"] = @(info->render_target_array_length);
  dict[@"tile_width"] = @(info->tile_width);
  dict[@"tile_height"] = @(info->tile_height);
  dict[@"render_target_width"] = @(info->render_target_width);
  dict[@"render_target_height"] = @(info->render_target_height);
  if (info->depth.texture) {
    dict[@"depth"] = @{
      @"texture" : dxmt_apitrace_optional_handle(info->depth.texture),
      @"load_action" : @(info->depth.load_action),
      @"store_action" : @(info->depth.store_action),
      @"level" : @(info->depth.level),
      @"slice" : @(info->depth.slice),
      @"depth_plane" : @(info->depth.depth_plane),
      @"clear_depth" : @(info->depth.clear_depth),
    };
  }
  if (info->stencil.texture) {
    dict[@"stencil"] = @{
      @"texture" : dxmt_apitrace_optional_handle(info->stencil.texture),
      @"load_action" : @(info->stencil.load_action),
      @"store_action" : @(info->stencil.store_action),
      @"level" : @(info->stencil.level),
      @"slice" : @(info->stencil.slice),
      @"depth_plane" : @(info->stencil.depth_plane),
      @"clear_stencil" : @(info->stencil.clear_stencil),
    };
  }
  if (info->visibility_buffer)
    dict[@"visibility_buffer"] = dxmt_apitrace_optional_handle(info->visibility_buffer);
  return dxmt_apitrace_json_string(dict);
}

static NSString *
dxmt_apitrace_copy_texture_json(
    obj_handle_t source_texture,
    obj_handle_t destination_texture,
    const struct WMTOrigin *source_origin,
    const struct WMTSize *source_size,
    uint32_t source_slice,
    uint32_t source_level,
    const struct WMTOrigin *destination_origin,
    uint32_t destination_slice,
    uint32_t destination_level) {
  NSMutableDictionary *dict = [NSMutableDictionary dictionary];
  dict[@"source_texture"] = dxmt_apitrace_optional_handle(source_texture);
  dict[@"destination_texture"] = dxmt_apitrace_optional_handle(destination_texture);
  if (source_origin) {
    dict[@"source_origin"] = @[ @(source_origin->x), @(source_origin->y), @(source_origin->z) ];
  }
  if (source_size) {
    dict[@"source_size"] = @[ @(source_size->width), @(source_size->height), @(source_size->depth) ];
  }
  dict[@"source_slice"] = @(source_slice);
  dict[@"source_level"] = @(source_level);
  if (destination_origin) {
    dict[@"destination_origin"] = @[ @(destination_origin->x), @(destination_origin->y), @(destination_origin->z) ];
  }
  dict[@"destination_slice"] = @(destination_slice);
  dict[@"destination_level"] = @(destination_level);
  return dxmt_apitrace_json_string(dict);
}

static NSString *
dxmt_apitrace_copy_buffer_to_texture_json(
    obj_handle_t source_buffer,
    uint64_t source_offset,
    uint32_t source_bytes_per_row,
    uint32_t source_bytes_per_image,
    const struct WMTSize *source_size,
    obj_handle_t destination_texture,
    uint32_t destination_slice,
    uint32_t destination_level,
    const struct WMTOrigin *destination_origin) {
  NSMutableDictionary *dict = [NSMutableDictionary dictionary];
  dict[@"kind"] = @"dxmt_copy_buffer_to_texture";
  dict[@"source_buffer"] = dxmt_apitrace_optional_handle(source_buffer);
  dict[@"source_offset"] = @(source_offset);
  dict[@"source_bytes_per_row"] = @(source_bytes_per_row);
  dict[@"source_bytes_per_image"] = @(source_bytes_per_image);
  if (source_size) {
    dict[@"source_size"] = @[ @(source_size->width), @(source_size->height), @(source_size->depth) ];
  }
  dict[@"destination_texture"] = dxmt_apitrace_optional_handle(destination_texture);
  dict[@"destination_slice"] = @(destination_slice);
  dict[@"destination_level"] = @(destination_level);
  if (destination_origin) {
    dict[@"destination_origin"] = @[ @(destination_origin->x), @(destination_origin->y), @(destination_origin->z) ];
  }
  return dxmt_apitrace_json_string(dict);
}

static NSString *
dxmt_apitrace_sampler_descriptor_json(const struct WMTSamplerInfo *info, obj_handle_t sampler) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_sampler_gpu_resource_id",
    @"sampler_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)sampler],
    @"gpu_resource_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)info->gpu_resource_id],
    @"border_color" : @(info->border_color),
    @"r_address_mode" : @(info->r_address_mode),
    @"s_address_mode" : @(info->s_address_mode),
    @"t_address_mode" : @(info->t_address_mode),
    @"mag_filter" : @(info->mag_filter),
    @"min_filter" : @(info->min_filter),
    @"mip_filter" : @(info->mip_filter),
    @"compare_function" : @(info->compare_function),
    @"lod_max_clamp" : @(info->lod_max_clamp),
    @"lod_min_clamp" : @(info->lod_min_clamp),
    @"max_anisotropy" : @(info->max_anisotroy),
    @"lod_average" : @(info->lod_average),
    @"normalized_coordinates" : @(info->normalized_coords),
    @"support_argument_buffers" : @(info->support_argument_buffers),
  });
}

static NSString *
dxmt_apitrace_texture_view_json(const struct unixcall_mtltexture_newtextureview *params) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_texture_view",
    @"texture_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->ret],
    @"source_texture_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->texture],
    @"gpu_resource_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->gpu_resource_id],
    @"pixel_format" : @(params->format),
    @"texture_type" : @(params->texture_type),
    @"level_start" : @(params->level_start),
    @"level_count" : @(params->level_count),
    @"slice_start" : @(params->slice_start),
    @"slice_count" : @(params->slice_count),
    @"swizzle" : @[ @(params->swizzle.r), @(params->swizzle.g), @(params->swizzle.b), @(params->swizzle.a) ],
  });
}

static NSString *
dxmt_apitrace_memory_barrier_json(uint32_t scope, uint32_t stages_before, uint32_t stages_after) {
  return dxmt_apitrace_json_string(@{
    @"scope" : @(scope),
    @"stages_before" : @(stages_before),
    @"stages_after" : @(stages_after),
  });
}

static NSString *
dxmt_apitrace_dispatch_threads_json(const struct WMTSize *size, const struct WMTSize *threadgroup_size) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_dispatch_threads",
    @"width" : @(size ? size->width : 1),
    @"height" : @(size ? size->height : 1),
    @"depth" : @(size ? size->depth : 1),
    @"threads_per_group_width" : @(threadgroup_size ? threadgroup_size->width : 1),
    @"threads_per_group_height" : @(threadgroup_size ? threadgroup_size->height : 1),
    @"threads_per_group_depth" : @(threadgroup_size ? threadgroup_size->depth : 1),
  });
}

static NSString *
dxmt_apitrace_bytes_json(const void *bytes, size_t length) {
  NSMutableArray *values = [NSMutableArray arrayWithCapacity:length];
  const uint8_t *raw = bytes;
  for (size_t i = 0; i < length; ++i) {
    [values addObject:@(raw ? raw[i] : 0)];
  }
  return dxmt_apitrace_json_string(@{ @"length" : @(length), @"bytes" : values });
}

static bool
dxmt_apitrace_argument_buffer_binding_update(
    struct dxmt_apitrace_argument_buffer_binding bindings[31],
    uint8_t index,
    obj_handle_t buffer,
    uint64_t offset) {
  if (index >= 31)
    return false;
  if (bindings[index].bound &&
      bindings[index].buffer == buffer &&
      bindings[index].offset == offset)
    return false;
  bindings[index].buffer = buffer;
  bindings[index].offset = offset;
  bindings[index].bound = true;
  return true;
}

static void
dxmt_apitrace_snapshot_buffer(apitrace_metal_session_t *session, obj_handle_t buffer_handle, NSMutableSet *seen) {
  if (!session || !buffer_handle)
    return;
  NSNumber *key = dxmt_apitrace_key(buffer_handle);
  if (seen && [seen containsObject:key])
    return;
  if (seen)
    [seen addObject:key];

  id<MTLBuffer> buffer = (id<MTLBuffer>)buffer_handle;
  if ([buffer storageMode] == MTLStorageModePrivate || ![buffer contents])
    return;

  apitrace_metal_buffer_gpu_address_metadata(session, buffer_handle, (uint64_t)[buffer gpuAddress]);
}

static void
dxmt_apitrace_record_buffer_binding(
    apitrace_metal_session_t *session,
    obj_handle_t encoder,
    apitrace_metal_stage_kind stage,
    obj_handle_t buffer_handle,
    uint64_t offset,
    uint8_t index) {
  uint64_t gpu_address = 0;
  uint64_t length = 0;
  if (buffer_handle) {
    id<MTLBuffer> buffer = (id<MTLBuffer>)buffer_handle;
    gpu_address = (uint64_t)[buffer gpuAddress] + offset;
    length = (uint64_t)[buffer length];
  }
  apitrace_metal_argument_table_buffer_binding(
      session, encoder, stage, index, buffer_handle, offset, gpu_address, length);
}

static void
dxmt_apitrace_record_texture_binding(
    apitrace_metal_session_t *session,
    obj_handle_t encoder,
    apitrace_metal_stage_kind stage,
    obj_handle_t texture_handle,
    uint8_t index) {
  uint64_t resource_id = 0;
  if (texture_handle)
    resource_id = [(id<MTLTexture>)texture_handle gpuResourceID]._impl;
  apitrace_metal_argument_table_texture_binding(session, encoder, stage, index, texture_handle, resource_id);
}

static void
dxmt_apitrace_record_indirect_arguments(
    apitrace_metal_session_t *session,
    obj_handle_t encoder,
    apitrace_metal_indirect_arguments_info_t *info) {
  if (!info)
    return;
  if (info->indirect_buffer_id) {
    id<MTLBuffer> buffer = (id<MTLBuffer>)info->indirect_buffer_id;
    info->indirect_gpu_address = (uint64_t)[buffer gpuAddress] + info->indirect_offset;
    info->indirect_buffer_length = (uint64_t)[buffer length];
  }
  apitrace_metal_indirect_arguments(session, encoder, info);
}

static void
dxmt_apitrace_record_emulated_blit(
    apitrace_metal_session_t *session,
    obj_handle_t encoder,
    apitrace_metal_emulated_blit_op op,
    obj_handle_t source_texture,
    obj_handle_t destination_buffer,
    uint64_t destination_offset,
    uint64_t bytes_per_row,
    uint64_t bytes_per_image,
    uint64_t slice,
    uint64_t level) {
  apitrace_metal_emulated_blit_marker(
      session, encoder, op, source_texture, destination_buffer, destination_offset,
      bytes_per_row, bytes_per_image, slice, level);
}

static void
dxmt_apitrace_finalize_command_buffer_locked(obj_handle_t command_buffer) {
  DXMT4ApitraceCommandBufferState *state =
      [dxmt_apitrace_command_buffers objectForKey:dxmt_apitrace_key(command_buffer)];
  if (!dxmt_apitrace_session || !state)
    return;

  dxmt_apitrace_flush_command_buffer_blit_batch(dxmt_apitrace_session, command_buffer);
  apitrace_metal_command_buffer_commit(dxmt_apitrace_session, command_buffer);
  [dxmt_apitrace_command_buffers removeObjectForKey:dxmt_apitrace_key(command_buffer)];
  dxmt_apitrace_set_sequence_locked(dxmt_apitrace_session, 0);
}

static void
dxmt_apitrace_finalize_command_buffer(obj_handle_t command_buffer) {
  if (!dxmt_apitrace_runtime_enabled())
    return;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  dxmt_apitrace_finalize_command_buffer_locked(command_buffer);
  pthread_mutex_unlock(&dxmt_apitrace_lock);
}

static void
dxmt_apitrace_record_command_buffer_commit_state(
    obj_handle_t command_buffer,
    apitrace_metal_command_buffer_commit_phase phase,
    uint64_t wait_event_count,
    uint64_t signal_event_count,
    BOOL has_drawable,
    uint64_t completion_value,
    uint64_t d3d_sequence) {
  if (!dxmt_apitrace_runtime_enabled())
    return;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  if (dxmt_apitrace_ensure_session_locked()) {
    dxmt_apitrace_ensure_command_buffer_begin_locked(
        dxmt_apitrace_session,
        command_buffer,
        0,
        d3d_sequence ? d3d_sequence : dxmt_apitrace_current_d3d_sequence);
    apitrace_metal_command_buffer_commit_state(
        dxmt_apitrace_session,
        command_buffer,
        phase,
        wait_event_count,
        signal_event_count,
        has_drawable,
        completion_value,
        d3d_sequence);
  }
  pthread_mutex_unlock(&dxmt_apitrace_lock);
}

static void
dxmt_apitrace_record_command_buffer_feedback(
    obj_handle_t command_buffer,
    apitrace_metal_command_buffer_feedback_status status,
    double gpu_start_time,
    double gpu_end_time,
    NSString *error) {
  if (!dxmt_apitrace_runtime_enabled())
    return;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  if (dxmt_apitrace_ensure_session_locked()) {
    apitrace_metal_command_buffer_feedback(
        dxmt_apitrace_session,
        command_buffer,
        status,
        gpu_start_time,
        gpu_end_time,
        error ? error.UTF8String : "");
  }
  pthread_mutex_unlock(&dxmt_apitrace_lock);
}

static void
dxmt_apitrace_record_command_buffer_event(
    obj_handle_t command_buffer,
    apitrace_metal_queue_event_op op,
    obj_handle_t event,
    uint64_t value,
    apitrace_metal_queue_event_phase phase) {
  if (!dxmt_apitrace_runtime_enabled())
    return;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  if (dxmt_apitrace_ensure_session_locked()) {
    dxmt_apitrace_ensure_command_buffer_begin_locked(
        dxmt_apitrace_session,
        command_buffer,
        0,
        dxmt_apitrace_current_d3d_sequence);
    apitrace_metal_queue_event(dxmt_apitrace_session, command_buffer, op, phase, event, value);
  }
  pthread_mutex_unlock(&dxmt_apitrace_lock);
}

static void
dxmt_apitrace_record_counter_event(
    obj_handle_t command_buffer,
    apitrace_metal_counter_event_op op,
    obj_handle_t counter_heap,
    uint64_t start,
    uint64_t count,
    obj_handle_t destination_buffer,
    uint64_t destination_offset,
    uint64_t destination_length,
    obj_handle_t wait_fence,
    obj_handle_t update_fence) {
  if (!dxmt_apitrace_runtime_enabled())
    return;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  if (dxmt_apitrace_ensure_session_locked()) {
    dxmt_apitrace_ensure_command_buffer_begin_locked(
        dxmt_apitrace_session,
        command_buffer,
        0,
        dxmt_apitrace_current_d3d_sequence);
    apitrace_metal_counter_event(
        dxmt_apitrace_session,
        command_buffer,
        op,
        counter_heap,
        start,
        count,
        destination_buffer,
        destination_offset,
        destination_length,
        wait_fence,
        update_fence);
  }
  pthread_mutex_unlock(&dxmt_apitrace_lock);
}

static void
dxmt_apitrace_record_render_commands(apitrace_metal_session_t *session, obj_handle_t encoder, const struct wmtcmd_base *head) {
  DXMT4ApitraceEncoderState *state = [dxmt_apitrace_encoders objectForKey:dxmt_apitrace_key(encoder)];
  if (!state)
    return;
  dxmt_apitrace_set_sequence_locked(session, state.d3dSequence);
  struct dxmt_apitrace_argument_buffer_binding *vertex_buffers = state->renderVertexArgumentBuffers;
  struct dxmt_apitrace_argument_buffer_binding *fragment_buffers = state->renderFragmentArgumentBuffers;
  struct dxmt_apitrace_argument_buffer_binding *object_buffers = state->renderObjectArgumentBuffers;
  struct dxmt_apitrace_argument_buffer_binding *mesh_buffers = state->renderMeshArgumentBuffers;
  struct dxmt_apitrace_argument_buffer_binding *tile_buffers = state->renderTileArgumentBuffers;
  for (const struct wmtcmd_base *base = head; base; base = (const struct wmtcmd_base *)base->next.ptr) {
    switch ((enum WMTRenderCommandType)base->type) {
    case WMTRenderCommandSetPSO: {
      const struct wmtcmd_render_setpso *cmd = (const struct wmtcmd_render_setpso *)base;
      apitrace_metal_set_render_pipeline_state(session, encoder, cmd->pso);
      break;
    }
    case WMTRenderCommandSetVertexBuffer: {
      const struct wmtcmd_render_setbuffer *cmd = (const struct wmtcmd_render_setbuffer *)base;
      apitrace_metal_set_vertex_buffer(session, encoder, cmd->buffer, cmd->offset, cmd->index);
      if (cmd->index < 31) {
        vertex_buffers[cmd->index].buffer = cmd->buffer;
        vertex_buffers[cmd->index].offset = cmd->offset;
      }
      dxmt_apitrace_record_buffer_binding(
          session, encoder, APITRACE_METAL_STAGE_VERTEX, cmd->buffer, cmd->offset, cmd->index);
      break;
    }
    case WMTRenderCommandSetFragmentBuffer: {
      const struct wmtcmd_render_setbuffer *cmd = (const struct wmtcmd_render_setbuffer *)base;
      apitrace_metal_set_fragment_buffer(session, encoder, cmd->buffer, cmd->offset, cmd->index);
      if (cmd->index < 31) {
        fragment_buffers[cmd->index].buffer = cmd->buffer;
        fragment_buffers[cmd->index].offset = cmd->offset;
      }
      dxmt_apitrace_record_buffer_binding(
          session, encoder, APITRACE_METAL_STAGE_FRAGMENT, cmd->buffer, cmd->offset, cmd->index);
      break;
    }
    case WMTRenderCommandSetVertexBufferOffset: {
      const struct wmtcmd_render_setbufferoffset *cmd = (const struct wmtcmd_render_setbufferoffset *)base;
      apitrace_metal_set_vertex_buffer_offset(session, encoder, cmd->offset, cmd->index);
      if (cmd->index < 31) {
        vertex_buffers[cmd->index].offset = cmd->offset;
        dxmt_apitrace_record_buffer_binding(
            session, encoder, APITRACE_METAL_STAGE_VERTEX, vertex_buffers[cmd->index].buffer, cmd->offset, cmd->index);
      }
      break;
    }
    case WMTRenderCommandSetFragmentBufferOffset: {
      const struct wmtcmd_render_setbufferoffset *cmd = (const struct wmtcmd_render_setbufferoffset *)base;
      apitrace_metal_set_fragment_buffer_offset(session, encoder, cmd->offset, cmd->index);
      if (cmd->index < 31) {
        fragment_buffers[cmd->index].offset = cmd->offset;
        dxmt_apitrace_record_buffer_binding(
            session, encoder, APITRACE_METAL_STAGE_FRAGMENT, fragment_buffers[cmd->index].buffer, cmd->offset, cmd->index);
      }
      break;
    }
    case WMTRenderCommandSetArgumentBuffer: {
      const struct wmtcmd_render_setargumentbuffer *cmd = (const struct wmtcmd_render_setargumentbuffer *)base;
      bool vertex_changed = (cmd->stages & WMTRenderStageVertex) &&
          dxmt_apitrace_argument_buffer_binding_update(vertex_buffers, cmd->index, cmd->buffer, cmd->offset);
      bool fragment_changed = (cmd->stages & WMTRenderStageFragment) &&
          dxmt_apitrace_argument_buffer_binding_update(fragment_buffers, cmd->index, cmd->buffer, cmd->offset);
      bool object_changed = (cmd->stages & WMTRenderStageObject) &&
          dxmt_apitrace_argument_buffer_binding_update(object_buffers, cmd->index, cmd->buffer, cmd->offset);
      bool mesh_changed = (cmd->stages & WMTRenderStageMesh) &&
          dxmt_apitrace_argument_buffer_binding_update(mesh_buffers, cmd->index, cmd->buffer, cmd->offset);
      bool tile_changed = (cmd->stages & WMTRenderStageTile) &&
          dxmt_apitrace_argument_buffer_binding_update(tile_buffers, cmd->index, cmd->buffer, cmd->offset);
      if (vertex_changed || fragment_changed || object_changed || mesh_changed || tile_changed) {
        apitrace_metal_set_argument_buffer(session, encoder, cmd->index, cmd->buffer, cmd->offset);
      }
      if (vertex_changed) {
        dxmt_apitrace_record_buffer_binding(
            session, encoder, APITRACE_METAL_STAGE_VERTEX, cmd->buffer, cmd->offset, cmd->index);
      }
      if (fragment_changed) {
        dxmt_apitrace_record_buffer_binding(
            session, encoder, APITRACE_METAL_STAGE_FRAGMENT, cmd->buffer, cmd->offset, cmd->index);
      }
      if (object_changed) {
        dxmt_apitrace_record_buffer_binding(
            session, encoder, APITRACE_METAL_STAGE_OBJECT, cmd->buffer, cmd->offset, cmd->index);
      }
      if (mesh_changed) {
        dxmt_apitrace_record_buffer_binding(
            session, encoder, APITRACE_METAL_STAGE_MESH, cmd->buffer, cmd->offset, cmd->index);
      }
      if (tile_changed) {
        dxmt_apitrace_record_buffer_binding(
            session, encoder, APITRACE_METAL_STAGE_TILE, cmd->buffer, cmd->offset, cmd->index);
      }
      break;
    }
    case WMTRenderCommandSetArgumentBufferOffset: {
      const struct wmtcmd_render_setargumentbufferoffset *cmd =
          (const struct wmtcmd_render_setargumentbufferoffset *)base;
      bool vertex_changed = false;
      bool fragment_changed = false;
      bool object_changed = false;
      bool mesh_changed = false;
      bool tile_changed = false;
      if (cmd->index < 31) {
        vertex_changed = (cmd->stages & WMTRenderStageVertex) &&
            dxmt_apitrace_argument_buffer_binding_update(
                vertex_buffers, cmd->index, vertex_buffers[cmd->index].buffer, cmd->offset);
        fragment_changed = (cmd->stages & WMTRenderStageFragment) &&
            dxmt_apitrace_argument_buffer_binding_update(
                fragment_buffers, cmd->index, fragment_buffers[cmd->index].buffer, cmd->offset);
        object_changed = (cmd->stages & WMTRenderStageObject) &&
            dxmt_apitrace_argument_buffer_binding_update(
                object_buffers, cmd->index, object_buffers[cmd->index].buffer, cmd->offset);
        mesh_changed = (cmd->stages & WMTRenderStageMesh) &&
            dxmt_apitrace_argument_buffer_binding_update(
                mesh_buffers, cmd->index, mesh_buffers[cmd->index].buffer, cmd->offset);
        tile_changed = (cmd->stages & WMTRenderStageTile) &&
            dxmt_apitrace_argument_buffer_binding_update(
                tile_buffers, cmd->index, tile_buffers[cmd->index].buffer, cmd->offset);
        if (vertex_changed || fragment_changed || object_changed || mesh_changed || tile_changed)
          apitrace_metal_set_argument_buffer(session, encoder, cmd->index, 0, cmd->offset);
        if (vertex_changed) {
          dxmt_apitrace_record_buffer_binding(
              session, encoder, APITRACE_METAL_STAGE_VERTEX,
              vertex_buffers[cmd->index].buffer, cmd->offset, cmd->index);
        }
        if (fragment_changed) {
          dxmt_apitrace_record_buffer_binding(
              session, encoder, APITRACE_METAL_STAGE_FRAGMENT,
              fragment_buffers[cmd->index].buffer, cmd->offset, cmd->index);
        }
        if (object_changed) {
          dxmt_apitrace_record_buffer_binding(
              session, encoder, APITRACE_METAL_STAGE_OBJECT,
              object_buffers[cmd->index].buffer, cmd->offset, cmd->index);
        }
        if (mesh_changed) {
          dxmt_apitrace_record_buffer_binding(
              session, encoder, APITRACE_METAL_STAGE_MESH,
              mesh_buffers[cmd->index].buffer, cmd->offset, cmd->index);
        }
        if (tile_changed) {
          dxmt_apitrace_record_buffer_binding(
              session, encoder, APITRACE_METAL_STAGE_TILE,
              tile_buffers[cmd->index].buffer, cmd->offset, cmd->index);
        }
      }
      break;
    }
    case WMTRenderCommandSetFragmentTexture: {
      const struct wmtcmd_render_settexture *cmd = (const struct wmtcmd_render_settexture *)base;
      apitrace_metal_set_fragment_texture(session, encoder, cmd->texture, cmd->index);
      dxmt_apitrace_record_texture_binding(
          session, encoder, APITRACE_METAL_STAGE_FRAGMENT, cmd->texture, cmd->index);
      break;
    }
    case WMTRenderCommandSetFragmentBytes: {
      const struct wmtcmd_render_setbytes *cmd = (const struct wmtcmd_render_setbytes *)base;
      NSString *payload = dxmt_apitrace_bytes_json(cmd->bytes.ptr, cmd->length);
      apitrace_metal_set_fragment_bytes(session, encoder, cmd->index, payload.UTF8String);
      break;
    }
    case WMTRenderCommandSetRasterizerState: {
      const struct wmtcmd_render_setrasterizerstate *cmd = (const struct wmtcmd_render_setrasterizerstate *)base;
      NSString *payload = dxmt_apitrace_set_rasterizer_state_json(cmd);
      apitrace_metal_encoder_state(session, encoder, payload.UTF8String);
      break;
    }
    case WMTRenderCommandSetDSSO: {
      const struct wmtcmd_render_setdsso *cmd = (const struct wmtcmd_render_setdsso *)base;
      NSString *payload = dxmt_apitrace_set_depth_stencil_state_json(cmd);
      apitrace_metal_encoder_state(session, encoder, payload.UTF8String);
      break;
    }
    case WMTRenderCommandSetBlendFactorAndStencilRef: {
      const struct wmtcmd_render_setblendcolor *cmd = (const struct wmtcmd_render_setblendcolor *)base;
      NSString *payload = dxmt_apitrace_set_blend_factor_json(cmd);
      apitrace_metal_encoder_state(session, encoder, payload.UTF8String);
      break;
    }
    case WMTRenderCommandSetViewports: {
      const struct wmtcmd_render_setviewports *cmd = (const struct wmtcmd_render_setviewports *)base;
      NSString *payload = dxmt_apitrace_set_viewports_json(cmd->viewports.ptr, cmd->viewport_count);
      apitrace_metal_encoder_state(session, encoder, payload.UTF8String);
      break;
    }
    case WMTRenderCommandSetScissorRects: {
      const struct wmtcmd_render_setscissorrects *cmd = (const struct wmtcmd_render_setscissorrects *)base;
      NSString *payload = dxmt_apitrace_set_scissor_rects_json(cmd->scissor_rects.ptr, cmd->rect_count);
      apitrace_metal_encoder_state(session, encoder, payload.UTF8String);
      break;
    }
    case WMTRenderCommandSetViewport: {
      const struct wmtcmd_render_setviewport *cmd = (const struct wmtcmd_render_setviewport *)base;
      NSString *payload = dxmt_apitrace_set_viewports_json(&cmd->viewport, 1);
      apitrace_metal_encoder_state(session, encoder, payload.UTF8String);
      break;
    }
    case WMTRenderCommandSetScissorRect: {
      const struct wmtcmd_render_setscissorrect *cmd = (const struct wmtcmd_render_setscissorrect *)base;
      NSString *payload = dxmt_apitrace_set_scissor_rects_json(&cmd->scissor_rect, 1);
      apitrace_metal_encoder_state(session, encoder, payload.UTF8String);
      break;
    }
    case WMTRenderCommandUseResource: {
      const struct wmtcmd_render_useresource *cmd = (const struct wmtcmd_render_useresource *)base;
      if ([(id)cmd->resource conformsToProtocol:@protocol(MTLBuffer)])
        dxmt_apitrace_snapshot_buffer(session, cmd->resource, NULL);
      apitrace_metal_use_resource(session, encoder, cmd->resource, cmd->usage, cmd->stages);
      break;
    }
    case WMTRenderCommandDraw: {
      const struct wmtcmd_render_draw *cmd = (const struct wmtcmd_render_draw *)base;
      apitrace_metal_draw_primitives(
          session, encoder, cmd->primitive_type, (uint32_t)cmd->vertex_start, (uint32_t)cmd->vertex_count,
          cmd->instance_count, cmd->base_instance);
      break;
    }
    case WMTRenderCommandDrawIndexed: {
      const struct wmtcmd_render_draw_indexed *cmd = (const struct wmtcmd_render_draw_indexed *)base;
      apitrace_metal_draw_indexed_primitives(
          session, encoder, cmd->primitive_type, (uint32_t)cmd->index_count, cmd->index_type,
          cmd->index_buffer, cmd->index_buffer_offset, cmd->instance_count, cmd->base_vertex, cmd->base_instance);
      break;
    }
    case WMTRenderCommandDrawIndirect: {
      const struct wmtcmd_render_draw_indirect *cmd = (const struct wmtcmd_render_draw_indirect *)base;
      apitrace_metal_draw_primitives_indirect(
          session, encoder, cmd->primitive_type, cmd->indirect_args_buffer, cmd->indirect_args_offset);
      apitrace_metal_indirect_arguments_info_t info = {
        .op = APITRACE_METAL_INDIRECT_DRAW,
        .stage = APITRACE_METAL_STAGE_RENDER,
        .indirect_buffer_id = cmd->indirect_args_buffer,
        .indirect_offset = cmd->indirect_args_offset,
        .primitive_type = cmd->primitive_type,
      };
      dxmt_apitrace_record_indirect_arguments(session, encoder, &info);
      break;
    }
    case WMTRenderCommandDrawIndexedIndirect: {
      const struct wmtcmd_render_draw_indexed_indirect *cmd = (const struct wmtcmd_render_draw_indexed_indirect *)base;
      apitrace_metal_draw_indexed_primitives_indirect(
          session, encoder, cmd->primitive_type, cmd->index_type, cmd->index_buffer, cmd->index_buffer_offset,
          cmd->indirect_args_buffer, cmd->indirect_args_offset);
      apitrace_metal_indirect_arguments_info_t info = {
        .op = APITRACE_METAL_INDIRECT_DRAW_INDEXED,
        .stage = APITRACE_METAL_STAGE_RENDER,
        .indirect_buffer_id = cmd->indirect_args_buffer,
        .indirect_offset = cmd->indirect_args_offset,
        .index_buffer_id = cmd->index_buffer,
        .index_buffer_offset = cmd->index_buffer_offset,
        .primitive_type = cmd->primitive_type,
        .index_type = cmd->index_type,
      };
      dxmt_apitrace_record_indirect_arguments(session, encoder, &info);
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroupsIndirect: {
      const struct wmtcmd_render_draw_meshthreadgroups_indirect *cmd =
          (const struct wmtcmd_render_draw_meshthreadgroups_indirect *)base;
      apitrace_metal_indirect_arguments_info_t info = {
        .op = APITRACE_METAL_INDIRECT_DRAW_MESH_THREADGROUPS,
        .stage = APITRACE_METAL_STAGE_RENDER,
        .indirect_buffer_id = cmd->indirect_args_buffer,
        .indirect_offset = cmd->indirect_args_offset,
        .object_threadgroup_width = cmd->object_threadgroup_size.width,
        .object_threadgroup_height = cmd->object_threadgroup_size.height,
        .object_threadgroup_depth = cmd->object_threadgroup_size.depth,
        .mesh_threadgroup_width = cmd->mesh_threadgroup_size.width,
        .mesh_threadgroup_height = cmd->mesh_threadgroup_size.height,
        .mesh_threadgroup_depth = cmd->mesh_threadgroup_size.depth,
      };
      dxmt_apitrace_record_indirect_arguments(session, encoder, &info);
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndirect: {
      const struct wmtcmd_render_dxmt_geometry_draw_indirect *cmd =
          (const struct wmtcmd_render_dxmt_geometry_draw_indirect *)base;
      apitrace_metal_indirect_arguments_info_t info = {
        .op = APITRACE_METAL_INDIRECT_GEOMETRY_DRAW,
        .stage = APITRACE_METAL_STAGE_RENDER,
        .indirect_buffer_id = cmd->dispatch_args_buffer,
        .indirect_offset = cmd->dispatch_args_offset,
        .draw_arguments_buffer_id = cmd->indirect_args_buffer,
        .draw_arguments_offset = cmd->indirect_args_offset,
        .dispatch_arguments_buffer_id = cmd->dispatch_args_buffer,
        .dispatch_arguments_offset = cmd->dispatch_args_offset,
        .immediate_arguments_buffer_id = cmd->imm_draw_arguments,
        .vertex_per_warp = cmd->vertex_per_warp,
      };
      dxmt_apitrace_record_indirect_arguments(session, encoder, &info);
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndexedIndirect: {
      const struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect *cmd =
          (const struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect *)base;
      apitrace_metal_indirect_arguments_info_t info = {
        .op = APITRACE_METAL_INDIRECT_GEOMETRY_DRAW_INDEXED,
        .stage = APITRACE_METAL_STAGE_RENDER,
        .indirect_buffer_id = cmd->dispatch_args_buffer,
        .indirect_offset = cmd->dispatch_args_offset,
        .draw_arguments_buffer_id = cmd->indirect_args_buffer,
        .draw_arguments_offset = cmd->indirect_args_offset,
        .dispatch_arguments_buffer_id = cmd->dispatch_args_buffer,
        .dispatch_arguments_offset = cmd->dispatch_args_offset,
        .immediate_arguments_buffer_id = cmd->imm_draw_arguments,
        .index_buffer_id = cmd->index_buffer,
        .index_buffer_offset = cmd->index_buffer_offset,
        .vertex_per_warp = cmd->vertex_per_warp,
      };
      dxmt_apitrace_record_indirect_arguments(session, encoder, &info);
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndirect: {
      const struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect *cmd =
          (const struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect *)base;
      apitrace_metal_indirect_arguments_info_t info = {
        .op = APITRACE_METAL_INDIRECT_TESSELLATION_MESH_DRAW,
        .stage = APITRACE_METAL_STAGE_RENDER,
        .indirect_buffer_id = cmd->dispatch_args_buffer,
        .indirect_offset = cmd->dispatch_args_offset,
        .draw_arguments_buffer_id = cmd->indirect_args_buffer,
        .draw_arguments_offset = cmd->indirect_args_offset,
        .dispatch_arguments_buffer_id = cmd->dispatch_args_buffer,
        .dispatch_arguments_offset = cmd->dispatch_args_offset,
        .immediate_arguments_buffer_id = cmd->imm_draw_arguments,
        .threads_per_patch = cmd->threads_per_patch,
        .patch_per_group = cmd->patch_per_group,
      };
      dxmt_apitrace_record_indirect_arguments(session, encoder, &info);
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect: {
      const struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect *cmd =
          (const struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect *)base;
      apitrace_metal_indirect_arguments_info_t info = {
        .op = APITRACE_METAL_INDIRECT_TESSELLATION_MESH_DRAW_INDEXED,
        .stage = APITRACE_METAL_STAGE_RENDER,
        .indirect_buffer_id = cmd->dispatch_args_buffer,
        .indirect_offset = cmd->dispatch_args_offset,
        .draw_arguments_buffer_id = cmd->indirect_args_buffer,
        .draw_arguments_offset = cmd->indirect_args_offset,
        .dispatch_arguments_buffer_id = cmd->dispatch_args_buffer,
        .dispatch_arguments_offset = cmd->dispatch_args_offset,
        .immediate_arguments_buffer_id = cmd->imm_draw_arguments,
        .index_buffer_id = cmd->index_buffer,
        .index_buffer_offset = cmd->index_buffer_offset,
        .threads_per_patch = cmd->threads_per_patch,
        .patch_per_group = cmd->patch_per_group,
      };
      dxmt_apitrace_record_indirect_arguments(session, encoder, &info);
      break;
    }
    case WMTRenderCommandWaitForFence:
    case WMTRenderCommandUpdateFence: {
      const struct wmtcmd_render_fence_op *cmd = (const struct wmtcmd_render_fence_op *)base;
      if (base->type == WMTRenderCommandWaitForFence)
        dxmt_apitrace_append_fence_op(state, "wait", cmd->fence, cmd->stages);
      else
        dxmt_apitrace_append_fence_op(state, "update", cmd->fence, cmd->stages);
      break;
    }
    case WMTRenderCommandMemoryBarrier: {
      const struct wmtcmd_render_memory_barrier *cmd = (const struct wmtcmd_render_memory_barrier *)base;
      NSString *payload = dxmt_apitrace_memory_barrier_json(cmd->scope, cmd->stages_before, cmd->stages_after);
      apitrace_metal_memory_barrier(session, encoder, payload.UTF8String);
      break;
    }
    default:
      break;
    }
  }
}

static void
dxmt_apitrace_record_compute_commands(apitrace_metal_session_t *session, obj_handle_t encoder, const struct wmtcmd_base *head) {
  DXMT4ApitraceEncoderState *state = [dxmt_apitrace_encoders objectForKey:dxmt_apitrace_key(encoder)];
  if (!state)
    return;
  dxmt_apitrace_set_sequence_locked(session, state.d3dSequence);
  struct WMTSize threadgroup_size = {1, 1, 1};
  struct dxmt_apitrace_argument_buffer_binding *bound_buffers = state->computeArgumentBuffers;
  for (const struct wmtcmd_base *base = head; base; base = (const struct wmtcmd_base *)base->next.ptr) {
    switch ((enum WMTComputeCommandType)base->type) {
    case WMTComputeCommandSetPSO: {
      const struct wmtcmd_compute_setpso *cmd = (const struct wmtcmd_compute_setpso *)base;
      threadgroup_size = cmd->threadgroup_size;
      apitrace_metal_set_compute_pipeline_state(session, encoder, cmd->pso);
      break;
    }
    case WMTComputeCommandSetBuffer: {
      const struct wmtcmd_compute_setbuffer *cmd = (const struct wmtcmd_compute_setbuffer *)base;
      apitrace_metal_set_compute_buffer(session, encoder, cmd->buffer, cmd->offset, cmd->index);
      if (cmd->index < 31) {
        bound_buffers[cmd->index].buffer = cmd->buffer;
        bound_buffers[cmd->index].offset = cmd->offset;
      }
      dxmt_apitrace_record_buffer_binding(
          session, encoder, APITRACE_METAL_STAGE_COMPUTE, cmd->buffer, cmd->offset, cmd->index);
      break;
    }
    case WMTComputeCommandSetBufferOffset: {
      const struct wmtcmd_compute_setbufferoffset *cmd = (const struct wmtcmd_compute_setbufferoffset *)base;
      apitrace_metal_set_compute_buffer_offset(session, encoder, cmd->offset, cmd->index);
      if (cmd->index < 31) {
        bound_buffers[cmd->index].offset = cmd->offset;
        dxmt_apitrace_record_buffer_binding(
            session, encoder, APITRACE_METAL_STAGE_COMPUTE, bound_buffers[cmd->index].buffer, cmd->offset, cmd->index);
      }
      break;
    }
    case WMTComputeCommandSetArgumentBuffer: {
      const struct wmtcmd_compute_setargumentbuffer *cmd =
          (const struct wmtcmd_compute_setargumentbuffer *)base;
      if (dxmt_apitrace_argument_buffer_binding_update(bound_buffers, cmd->index, cmd->buffer, cmd->offset)) {
        apitrace_metal_set_argument_buffer(session, encoder, cmd->index, cmd->buffer, cmd->offset);
        dxmt_apitrace_record_buffer_binding(
            session, encoder, APITRACE_METAL_STAGE_COMPUTE, cmd->buffer, cmd->offset, cmd->index);
      }
      break;
    }
    case WMTComputeCommandSetArgumentBufferOffset: {
      const struct wmtcmd_compute_setargumentbufferoffset *cmd =
          (const struct wmtcmd_compute_setargumentbufferoffset *)base;
      if (cmd->index < 31 &&
          dxmt_apitrace_argument_buffer_binding_update(
              bound_buffers, cmd->index, bound_buffers[cmd->index].buffer, cmd->offset)) {
        apitrace_metal_set_argument_buffer(session, encoder, cmd->index, 0, cmd->offset);
        dxmt_apitrace_record_buffer_binding(
            session, encoder, APITRACE_METAL_STAGE_COMPUTE,
            bound_buffers[cmd->index].buffer, cmd->offset, cmd->index);
      }
      break;
    }
    case WMTComputeCommandSetBytes: {
      const struct wmtcmd_compute_setbytes *cmd = (const struct wmtcmd_compute_setbytes *)base;
      NSMutableDictionary *dict = [NSMutableDictionary dictionary];
      dict[@"kind"] = @"dxmt_set_compute_bytes";
      dict[@"index"] = @(cmd->index);
      dict[@"length"] = @(cmd->length);
      NSMutableArray *bytes = [NSMutableArray arrayWithCapacity:cmd->length];
      const uint8_t *raw = cmd->bytes.ptr;
      for (uint64_t i = 0; i < cmd->length; ++i)
        [bytes addObject:@(raw ? raw[i] : 0)];
      dict[@"bytes"] = bytes;
      NSString *payload = dxmt_apitrace_json_string(dict);
      apitrace_metal_set_compute_bytes(session, encoder, payload.UTF8String);
      break;
    }
    case WMTComputeCommandSetTexture: {
      const struct wmtcmd_compute_settexture *cmd = (const struct wmtcmd_compute_settexture *)base;
      apitrace_metal_set_compute_texture(session, encoder, cmd->texture, cmd->index);
      dxmt_apitrace_record_texture_binding(
          session, encoder, APITRACE_METAL_STAGE_COMPUTE, cmd->texture, cmd->index);
      break;
    }
    case WMTComputeCommandUseResource: {
      const struct wmtcmd_compute_useresource *cmd = (const struct wmtcmd_compute_useresource *)base;
      if ([(id)cmd->resource conformsToProtocol:@protocol(MTLBuffer)])
        dxmt_apitrace_snapshot_buffer(session, cmd->resource, NULL);
      apitrace_metal_use_resource(session, encoder, cmd->resource, cmd->usage, 0);
      break;
    }
    case WMTComputeCommandDispatch: {
      const struct wmtcmd_compute_dispatch *cmd = (const struct wmtcmd_compute_dispatch *)base;
      apitrace_metal_dispatch_threadgroups(
          session, encoder, (uint32_t)cmd->size.width, (uint32_t)cmd->size.height, (uint32_t)cmd->size.depth,
          (uint32_t)threadgroup_size.width, (uint32_t)threadgroup_size.height, (uint32_t)threadgroup_size.depth);
      break;
    }
    case WMTComputeCommandDispatchThreads: {
      const struct wmtcmd_compute_dispatch *cmd = (const struct wmtcmd_compute_dispatch *)base;
      NSString *payload = dxmt_apitrace_dispatch_threads_json(&cmd->size, &threadgroup_size);
      apitrace_metal_dispatch_threads(session, encoder, payload.UTF8String);
      break;
    }
    case WMTComputeCommandDispatchIndirect: {
      const struct wmtcmd_compute_dispatch_indirect *cmd = (const struct wmtcmd_compute_dispatch_indirect *)base;
      apitrace_metal_dispatch_threadgroups_indirect(
          session, encoder, cmd->indirect_args_buffer, cmd->indirect_args_offset,
          (uint32_t)threadgroup_size.width, (uint32_t)threadgroup_size.height, (uint32_t)threadgroup_size.depth);
      apitrace_metal_indirect_arguments_info_t info = {
        .op = APITRACE_METAL_INDIRECT_DISPATCH,
        .stage = APITRACE_METAL_STAGE_COMPUTE,
        .indirect_buffer_id = cmd->indirect_args_buffer,
        .indirect_offset = cmd->indirect_args_offset,
        .threadgroup_width = threadgroup_size.width,
        .threadgroup_height = threadgroup_size.height,
        .threadgroup_depth = threadgroup_size.depth,
      };
      dxmt_apitrace_record_indirect_arguments(session, encoder, &info);
      break;
    }
    case WMTComputeCommandWaitForFence:
    case WMTComputeCommandUpdateFence: {
      const struct wmtcmd_compute_fence_op *cmd = (const struct wmtcmd_compute_fence_op *)base;
      if (base->type == WMTComputeCommandWaitForFence)
        dxmt_apitrace_append_fence_op(state, "wait", cmd->fence, 0);
      else
        dxmt_apitrace_append_fence_op(state, "update", cmd->fence, 0);
      break;
    }
    case WMTComputeCommandMemoryBarrier: {
      const struct wmtcmd_compute_memory_barrier *cmd = (const struct wmtcmd_compute_memory_barrier *)base;
      NSString *payload = dxmt_apitrace_memory_barrier_json(cmd->scope, 0, 0);
      apitrace_metal_memory_barrier(session, encoder, payload.UTF8String);
      break;
    }
    default:
      break;
    }
  }
}

static void
dxmt_apitrace_record_blit_commands(apitrace_metal_session_t *session, obj_handle_t encoder, const struct wmtcmd_base *head) {
  DXMT4ApitraceEncoderState *state = [dxmt_apitrace_encoders objectForKey:dxmt_apitrace_key(encoder)];
  if (!state)
    return;
  if (state)
    dxmt_apitrace_set_sequence_locked(session, state.d3dSequence);
  for (const struct wmtcmd_base *base = head; base; base = (const struct wmtcmd_base *)base->next.ptr) {
    switch ((enum WMTBlitCommandType)base->type) {
    case WMTBlitCommandCopyFromBufferToBuffer: {
      const struct wmtcmd_blit_copy_from_buffer_to_buffer *cmd = (const struct wmtcmd_blit_copy_from_buffer_to_buffer *)base;
      if (state.kind != DXMTApitraceEncoderKindMetal4Blit) {
        dxmt_apitrace_begin_blit_encoder_if_needed(session, encoder, state);
        dxmt_apitrace_flush_blit_ops(session, encoder, state);
        dxmt_apitrace_flush_command_buffer_blit_batch(session, state.commandBuffer);
      }
      apitrace_metal_copy_buffer(
          session, encoder, cmd->src, cmd->src_offset, cmd->dst, cmd->dst_offset, cmd->copy_length);
      break;
    }
    case WMTBlitCommandCopyFromBufferToTexture: {
      const struct wmtcmd_blit_copy_from_buffer_to_texture *cmd = (const struct wmtcmd_blit_copy_from_buffer_to_texture *)base;
      if (state.kind != DXMTApitraceEncoderKindMetal4Blit) {
        dxmt_apitrace_begin_blit_encoder_if_needed(session, encoder, state);
        dxmt_apitrace_flush_blit_ops(session, encoder, state);
        dxmt_apitrace_flush_command_buffer_blit_batch(session, state.commandBuffer);
      }
      NSString *payload = dxmt_apitrace_copy_buffer_to_texture_json(
          cmd->src, cmd->src_offset, cmd->bytes_per_row, cmd->bytes_per_image, &cmd->size, cmd->dst, cmd->slice,
          cmd->level, &cmd->origin);
      apitrace_metal_copy_buffer_to_texture(session, encoder, cmd->src, cmd->dst, payload.UTF8String);
      break;
    }
    case WMTBlitCommandCopyFromTextureToTexture: {
      const struct wmtcmd_blit_copy_from_texture_to_texture *cmd = (const struct wmtcmd_blit_copy_from_texture_to_texture *)base;
      if (state.kind == DXMTApitraceEncoderKindMetal4Blit) {
        NSString *payload = dxmt_apitrace_copy_texture_json(
            cmd->src, cmd->dst, &cmd->src_origin, &cmd->src_size, cmd->src_slice, cmd->src_level,
            &cmd->dst_origin, cmd->dst_slice, cmd->dst_level);
        apitrace_metal_copy_texture(session, encoder, cmd->src, cmd->dst, payload.UTF8String);
      } else if (!state.blitOps.count) {
        dxmt_apitrace_accumulate_command_buffer_blit_fences(state.commandBuffer, state);
        dxmt_apitrace_accumulate_command_buffer_blit_copy_texture(session, state.commandBuffer, cmd);
      } else {
        [state.blitOps addObject:@{
          @"op" : @"copy_texture",
          @"source_texture_id" : @(cmd->src),
          @"destination_texture_id" : @(cmd->dst),
          @"payload" : dxmt_apitrace_copy_texture_json(
              cmd->src, cmd->dst, &cmd->src_origin, &cmd->src_size, cmd->src_slice, cmd->src_level,
              &cmd->dst_origin, cmd->dst_slice, cmd->dst_level),
        }];
      }
      break;
    }
    case WMTBlitCommandFillBuffer: {
      const struct wmtcmd_blit_fillbuffer *cmd = (const struct wmtcmd_blit_fillbuffer *)base;
      if (state.kind == DXMTApitraceEncoderKindMetal4Blit) {
        apitrace_metal_blit_fill(session, encoder, cmd->buffer, cmd->offset, cmd->length, cmd->value);
      } else {
        [state.blitOps addObject:@{
          @"op" : @"fill_buffer",
          @"buffer_id" : @(cmd->buffer),
          @"range_start" : @(cmd->offset),
          @"range_length" : @(cmd->length),
          @"value" : @(cmd->value),
        }];
      }
      break;
    }
    case WMTBlitCommandWaitForFence:
    case WMTBlitCommandUpdateFence: {
      const struct wmtcmd_blit_fence_op *cmd = (const struct wmtcmd_blit_fence_op *)base;
      if (state.kind == DXMTApitraceEncoderKindMetal4Blit) {
        dxmt_apitrace_append_fence_op(
            state,
            base->type == WMTBlitCommandWaitForFence ? "wait" : "update",
            cmd->fence,
            MTLStageBlit);
      } else {
        uint64_t fence = cmd->fence;
        if (base->type == WMTBlitCommandWaitForFence)
          [state.blitWaitFences appendBytes:&fence length:sizeof(fence)];
        else
          [state.blitUpdateFences appendBytes:&fence length:sizeof(fence)];
      }
      break;
    }
    case WMTBlitCommandCopyFromTextureToBuffer: {
      const struct wmtcmd_blit_copy_from_texture_to_buffer *cmd =
          (const struct wmtcmd_blit_copy_from_texture_to_buffer *)base;
      if (state.kind == DXMTApitraceEncoderKindMetal4Blit) {
        dxmt_apitrace_record_emulated_blit(
            session,
            encoder,
            APITRACE_METAL_EMULATED_BLIT_COPY_TEXTURE_TO_BUFFER,
            cmd->src,
            cmd->dst,
            cmd->offset,
            cmd->bytes_per_row,
            cmd->bytes_per_image,
            cmd->slice,
            cmd->level);
      } else {
        dxmt_apitrace_begin_blit_encoder_if_needed(session, encoder, state);
        dxmt_apitrace_flush_blit_ops(session, encoder, state);
        dxmt_apitrace_flush_command_buffer_blit_batch(session, state.commandBuffer);
      }
      break;
    }
    case WMTBlitCommandGenerateMipmaps: {
      const struct wmtcmd_blit_generate_mipmaps *cmd = (const struct wmtcmd_blit_generate_mipmaps *)base;
      if (state.kind == DXMTApitraceEncoderKindMetal4Blit) {
        dxmt_apitrace_record_emulated_blit(
            session,
            encoder,
            APITRACE_METAL_EMULATED_BLIT_GENERATE_MIPMAPS,
            cmd->texture,
            0,
            0,
            0,
            0,
            0,
            0);
      } else {
        dxmt_apitrace_begin_blit_encoder_if_needed(session, encoder, state);
        dxmt_apitrace_flush_blit_ops(session, encoder, state);
        dxmt_apitrace_flush_command_buffer_blit_batch(session, state.commandBuffer);
      }
      break;
    }
    case WMTBlitCommandResolveCounters:
      if (state.kind == DXMTApitraceEncoderKindMetal4Blit) {
        dxmt_apitrace_record_emulated_blit(
            session,
            encoder,
            APITRACE_METAL_EMULATED_BLIT_RESOLVE_COUNTERS,
            0,
            0,
            0,
            0,
            0,
            0,
            0);
      } else {
        dxmt_apitrace_begin_blit_encoder_if_needed(session, encoder, state);
        dxmt_apitrace_flush_blit_ops(session, encoder, state);
        dxmt_apitrace_flush_command_buffer_blit_batch(session, state.commandBuffer);
      }
      break;
    case WMTBlitCommandResourceStateBarrier:
      dxmt_apitrace_begin_blit_encoder_if_needed(session, encoder, state);
      dxmt_apitrace_flush_blit_ops(session, encoder, state);
      dxmt_apitrace_flush_command_buffer_blit_batch(session, state.commandBuffer);
      break;
    default:
      break;
    }
  }
}

static void
dxmt_apitrace_begin_encoder_locked(
    apitrace_metal_session_t *session,
    DXMTApitraceEncoderKind kind,
    obj_handle_t encoder,
    obj_handle_t command_buffer,
    const struct WMTRenderPassInfo *render_pass_info) {
  if (!session)
    return;
  DXMT4ApitraceCommandBufferState *command_state =
      dxmt_apitrace_ensure_command_buffer_begin_locked(
          session,
          command_buffer,
          0,
          dxmt_apitrace_current_d3d_sequence);
  if (!command_state || !command_state.d3dSequence)
    return;

  dxmt_apitrace_set_sequence_locked(session, command_state.d3dSequence);
  uint64_t begin_sequence = 0;
  BOOL metal_trace_began = YES;
  switch (kind) {
  case DXMTApitraceEncoderKindRender: {
    dxmt_apitrace_flush_command_buffer_blit_batch(session, command_buffer);
    NSString *payload = dxmt_apitrace_render_pass_json(render_pass_info);
    begin_sequence = apitrace_metal_render_encoder_begin(session, encoder, command_buffer, payload.UTF8String);
    break;
  }
  case DXMTApitraceEncoderKindCompute:
    dxmt_apitrace_flush_command_buffer_blit_batch(session, command_buffer);
    begin_sequence = apitrace_metal_compute_encoder_begin(session, encoder, command_buffer, "{}");
    break;
  case DXMTApitraceEncoderKindBlit:
    metal_trace_began = NO;
    break;
  case DXMTApitraceEncoderKindMetal4Blit:
    dxmt_apitrace_flush_command_buffer_blit_batch(session, command_buffer);
    begin_sequence = apitrace_metal_emulated_blit_encoder_begin(session, encoder, command_buffer);
    break;
  }

  DXMT4ApitraceEncoderState *state = [[DXMT4ApitraceEncoderState alloc] init];
  state.beginSequence = begin_sequence;
  state.commandBuffer = command_buffer;
  state.d3dSequence = command_state.d3dSequence;
  state.kind = kind;
  state.metalTraceBegan = metal_trace_began;
  [dxmt_apitrace_encoders setObject:state forKey:dxmt_apitrace_key(encoder)];
  [state release];
}

static void
dxmt_apitrace_end_encoder_locked(apitrace_metal_session_t *session, obj_handle_t encoder) {
  DXMT4ApitraceEncoderState *state = [dxmt_apitrace_encoders objectForKey:dxmt_apitrace_key(encoder)];
  if (!session || !state)
    return;

  if (state.kind == DXMTApitraceEncoderKindBlit) {
    if (state.metalTraceBegan) {
      dxmt_apitrace_flush_blit_ops(session, encoder, state);
    } else {
      if (state.blitOps.count) {
        dxmt_apitrace_flush_command_buffer_blit_batch(session, state.commandBuffer);
        dxmt_apitrace_emit_blit_encoder_batch(session, encoder, state);
      } else {
        dxmt_apitrace_accumulate_command_buffer_blit_fences(state.commandBuffer, state);
      }
    }
  }
  dxmt_apitrace_flush_fence_ops(session, encoder, state);

  switch (state.kind) {
  case DXMTApitraceEncoderKindRender:
    apitrace_metal_render_encoder_end(session, encoder);
    break;
  case DXMTApitraceEncoderKindCompute:
  case DXMTApitraceEncoderKindMetal4Blit:
    apitrace_metal_compute_encoder_end(session, encoder);
    break;
  case DXMTApitraceEncoderKindBlit:
    if (state.metalTraceBegan)
      apitrace_metal_blit_encoder_end(session, encoder);
    break;
  }

  [dxmt_apitrace_encoders removeObjectForKey:dxmt_apitrace_key(encoder)];
}
#endif

static void
dxmt_set_layer_background_black(CAMetalLayer *layer) {
  CGColorRef black = CGColorGetConstantColor(kCGColorBlack);
  layer.backgroundColor = black;
  layer.opaque = YES;

  NSView *view = [layer.delegate isKindOfClass:NSView.class] ? (NSView *)layer.delegate : nil;
  for (NSView *ancestor = view; ancestor; ancestor = ancestor.superview) {
    CALayer *ancestor_layer = ancestor.layer;
    if (ancestor_layer) {
      ancestor_layer.backgroundColor = black;
      ancestor_layer.opaque = YES;
    }
  }

  NSWindow *window = view.window;
  if (window) {
    window.backgroundColor = NSColor.blackColor;
    window.opaque = YES;

    NSView *content_view = window.contentView;
    CALayer *content_layer = content_view.layer;
    if (content_layer) {
      content_layer.backgroundColor = black;
      content_layer.opaque = YES;
    }
  }
}

void
execute_on_main(dispatch_block_t block) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_sync(dispatch_get_main_queue(), block);
  }
}

static NTSTATUS
_NSObject_retain(NSObject **obj) {
  [*obj retain];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSObject_release(NSObject **obj) {
  [*obj release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSArray_object(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(NSArray *)params->handle objectAtIndex:params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSArray_count(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(NSArray *)params->handle count];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCopyAllDevices(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = (obj_handle_t)MTLCopyAllDevices();
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_recommendedMaxWorkingSetSize(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle recommendedMaxWorkingSetSize];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_currentAllocatedSize(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle currentAllocatedSize];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_queryTimestampFrequency(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->handle;
  params->ret = 0;
  if (!device)
    return STATUS_SUCCESS;

  if (@available(macOS 26.0, *))
    params->ret = [device queryTimestampFrequency];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_sampleTimestamps(void *obj) {
  struct unixcall_mtldevice_sampletimestamps *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  params->cpu_timestamp = 0;
  params->gpu_timestamp = 0;
  params->ret_success = 0;
  if (!device)
    return STATUS_SUCCESS;

  if (@available(macOS 10.15, *)) {
    MTLTimestamp cpu_timestamp = 0;
    MTLTimestamp gpu_timestamp = 0;
    [device sampleTimestamps:&cpu_timestamp gpuTimestamp:&gpu_timestamp];
    params->cpu_timestamp = cpu_timestamp;
    params->gpu_timestamp = gpu_timestamp;
    params->ret_success = 1;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_name(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle name];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_getCString(void *obj) {
  struct unixcall_nsstring_getcstring *params = obj;
  params->ret = (uint32_t)[(NSString *)params->str getCString:(char *)params->buffer_ptr
                                                    maxLength:params->max_length
                                                     encoding:params->encoding];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newCommandQueue(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)[[DXMTMetal4CommandQueue alloc]
      initWithDevice:(id<MTLDevice>)params->handle
      maxCommandBufferCount:params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newResidencySet(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  MTLResidencySetDescriptor *descriptor = [[MTLResidencySetDescriptor alloc] init];
  descriptor.initialCapacity = params->arg;
  NSError *error = nil;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newResidencySetWithDescriptor:descriptor
                                                                                     error:&error];
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newTextureViewPool(void *obj) {
  struct unixcall_mtldevice_newtextureviewpool *params = obj;
  const struct WMTTextureViewPoolInfo *info = params->info.ptr;
  params->ret_pool = 0;
  params->ret_error = 0;
  if (!params->device || !info)
    return STATUS_SUCCESS;

  if (@available(macOS 26.0, *)) {
    MTLResourceViewPoolDescriptor *descriptor = [[MTLResourceViewPoolDescriptor alloc] init];
    descriptor.resourceViewCount = info->initial_count;
    NSError *error = nil;
    params->ret_pool = (obj_handle_t)[(id<MTLDevice>)params->device newTextureViewPoolWithDescriptor:descriptor
                                                                                               error:&error];
    params->ret_error = (obj_handle_t)error;
    [descriptor release];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSAutoreleasePool_alloc_init(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = (obj_handle_t)[[NSAutoreleasePool alloc] init];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandQueue_commandBuffer(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[[[DXMTMetal4CommandBuffer alloc]
      initWithQueue:(DXMTMetal4CommandQueue *)params->handle] autorelease];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandQueue_addResidencySet(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  DXMTMetal4CommandQueue *queue = (DXMTMetal4CommandQueue *)params->handle;
  id set = (id)params->arg;
  if (!queue || !dxmt_metal4_is_residency_set(set))
    return STATUS_SUCCESS;
  NSLock *membershipLock = dxmt_residency_membership_lock_get();
  [membershipLock lock];
  if (![queue.externalLifetimeResidencySets containsObject:set])
    [queue.externalLifetimeResidencySets addObject:set];
  [queue.metal4Queue addResidencySet:(id<MTLResidencySet>)set];
  [membershipLock unlock];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandQueue_removeResidencySet(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  DXMTMetal4CommandQueue *queue = (DXMTMetal4CommandQueue *)params->handle;
  id set = (id)params->arg;
  if (!queue || !dxmt_metal4_is_residency_set(set))
    return STATUS_SUCCESS;
  NSLock *membershipLock = dxmt_residency_membership_lock_get();
  [membershipLock lock];
  [queue.metal4Queue removeResidencySet:(id<MTLResidencySet>)set];
  [queue.externalLifetimeResidencySets removeObject:set];
  [membershipLock unlock];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_commit(void *obj) {
  struct unixcall_mtlcommandbuffer_commit_stats *params = obj;
  DXMTMetal4CommandBuffer *cmdbuf = (DXMTMetal4CommandBuffer *)params->handle;
  DXMTMetal4CommandQueue *owner = cmdbuf.owner;
  params->ret_residency_submit_us = 0;
  [owner monitorWaitBegin:DXMTMetal4QueueMonitorSourceUnixCommitThunk
            commandBuffer:cmdbuf];
  params->ret_residency_submit_us = [cmdbuf commitLocked];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_registerResource(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(DXMTMetal4CommandBuffer *)params->handle
      registerResourceForCurrentCommandBuffer:(id<MTLAllocation>)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_setDiagnosticInfo(void *obj) {
  struct unixcall_mtlcommandbuffer_set_diagnostic_info *params = obj;
  DXMTMetal4CommandBuffer *cmdbuf =
      (DXMTMetal4CommandBuffer *)params->handle;
  cmdbuf.diagnosticInfo = params->info;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_waitUntilCompleted(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(DXMTMetal4CommandBuffer *)params->handle waitUntilCompleted];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_status(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(DXMTMetal4CommandBuffer *)params->handle status];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSharedEvent(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newSharedEvent];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_signaledValue(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLSharedEvent>)params->handle signaledValue];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeSignalEvent(void *obj) {
  struct unixcall_generic_obj_obj_uint64_noret *params = obj;
  [(DXMTMetal4CommandBuffer *)params->handle encodeSignalEvent:(id<MTLEvent>)params->arg0 value:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newBuffer(void *obj) {
  struct unixcall_mtldevice_newbuffer *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTBufferInfo *info = params->info.ptr;
  id<MTLBuffer> buffer;
  const void *initial_bytes = info->memory.ptr;
  if (info->memory.ptr) {
    buffer = [device newBufferWithBytesNoCopy:info->memory.ptr
                                       length:info->length
                                      options:(enum MTLResourceOptions)info->options
                                  deallocator:NULL];
  } else {
    buffer = [device newBufferWithLength:info->length options:(enum MTLResourceOptions)info->options];
    info->memory.ptr = [buffer storageMode] == MTLStorageModePrivate ? NULL : [buffer contents];
  }
  params->ret = (obj_handle_t)buffer;
  info->gpu_address = [buffer gpuAddress];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      apitrace_metal_buffer_gpu_address_metadata(dxmt_apitrace_session, params->ret, [buffer gpuAddress]);
      apitrace_metal_register_buffer(
          dxmt_apitrace_session,
          params->ret,
          info->length,
          (uint32_t)[buffer storageMode],
          initial_bytes,
          initial_bytes ? info->length : 0);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLHeap_newBuffer(void *obj) {
  struct unixcall_mtlheap_newbuffer *params = obj;
  id<MTLHeap> heap = (id<MTLHeap>)params->heap;
  struct WMTBufferInfo *info = params->info.ptr;
  params->ret = 0;
  if (!heap || !info || info->memory.ptr)
    return STATUS_SUCCESS;

  id<MTLBuffer> buffer = nil;
  if (@available(macOS 10.15, *)) {
    const MTLResourceOptions options = (MTLResourceOptions)info->options;
    const MTLSizeAndAlign placement =
        [heap.device heapBufferSizeAndAlignWithLength:info->length
                                              options:options];
    const uint64_t heap_size = heap.size;
    if (placement.align && !(params->offset % placement.align) &&
        params->offset <= heap_size && placement.size <= heap_size - params->offset) {
      // Diagnose before Metal: Private subresource on Shared heap aborts the process.
      dxmt_diag_heap_placement("buffer", heap, params->offset, info->length,
                               options, NULL, YES);
      buffer = [heap newBufferWithLength:info->length
                                 options:options
                                  offset:params->offset];
      if (!buffer) {
        dxmt_diag_heap_placement("buffer-nil", heap, params->offset, info->length,
                                 options, NULL, NO);
      }
    } else if (dxmt_metal4_heap_storage_diag_verbose()) {
      fprintf(stderr,
              "warn:  DXMT Metal4 heap placement rejected: kind=buffer heap=%p "
              "offset=%llu length=%llu placementSize=%llu placementAlign=%llu "
              "heapSize=%llu\n",
              heap, (unsigned long long)params->offset,
              (unsigned long long)info->length,
              (unsigned long long)placement.size,
              (unsigned long long)placement.align,
              (unsigned long long)heap_size);
      fflush(stderr);
    }
  }
  params->ret = (obj_handle_t)buffer;
  info->memory.ptr = buffer && [buffer storageMode] != MTLStorageModePrivate
                         ? [buffer contents]
                         : NULL;
  info->gpu_address = buffer ? [buffer gpuAddress] : 0;
#if DXMT_APITRACE_METAL
  if (buffer && dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      apitrace_metal_buffer_gpu_address_metadata(
          dxmt_apitrace_session, params->ret, [buffer gpuAddress]);
      apitrace_metal_register_buffer(
          dxmt_apitrace_session, params->ret, info->length,
          (uint32_t)[buffer storageMode], NULL, 0);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newArgumentTable(void *obj) {
  struct unixcall_mtldevice_newargumenttable *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTArgumentTableInfo *info = params->info.ptr;
  if (!device || !info) {
    params->ret = 0;
    return STATUS_SUCCESS;
  }

  MTL4ArgumentTableDescriptor *desc = [[MTL4ArgumentTableDescriptor alloc] init];
  desc.maxBufferBindCount = info->max_buffer_bind_count;
  desc.maxTextureBindCount = info->max_texture_bind_count;
  desc.maxSamplerStateBindCount = info->max_sampler_state_bind_count;
  desc.initializeBindings = info->initialize_bindings;
  NSError *error = nil;
  id<MTL4ArgumentTable> table =
      [(id<MTLDeviceMetal4SPI>)device newArgumentTableWithDescriptor:desc
                                                               error:&error];
  if (!table) {
    fprintf(stderr, "err:   DXMT Metal4 argument table creation failed: %s\n",
            error.localizedDescription ? error.localizedDescription.UTF8String : "<no error>");
  }
  [desc release];
  params->ret = (obj_handle_t)table;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4ArgumentTable_setAddress(void *obj) {
  struct unixcall_mtl4argumenttable_setentry *params = obj;
  id<MTL4ArgumentTable> table = (id<MTL4ArgumentTable>)params->table;
  if (table)
    [table setAddress:(MTLGPUAddress)params->payload atIndex:params->index];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4ArgumentTable_setTexture(void *obj) {
  struct unixcall_mtl4argumenttable_setentry *params = obj;
  id<MTL4ArgumentTable> table = (id<MTL4ArgumentTable>)params->table;
  if (table) {
    MTLResourceID resource_id = { ._impl = params->payload };
    [table setTexture:resource_id atIndex:params->index];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4ArgumentTable_setSamplerState(void *obj) {
  struct unixcall_mtl4argumenttable_setentry *params = obj;
  id<MTL4ArgumentTable> table = (id<MTL4ArgumentTable>)params->table;
  if (table) {
    MTLResourceID resource_id = { ._impl = params->payload };
    [table setSamplerState:resource_id atIndex:params->index];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSamplerState(void *obj) {
  struct unixcall_mtldevice_newsamplerstate *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTSamplerInfo *info = params->info.ptr;

  MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
  sampler_desc.borderColor = (MTLSamplerBorderColor)info->border_color;
  sampler_desc.rAddressMode = (MTLSamplerAddressMode)info->r_address_mode;
  sampler_desc.sAddressMode = (MTLSamplerAddressMode)info->s_address_mode;
  sampler_desc.tAddressMode = (MTLSamplerAddressMode)info->t_address_mode;
  sampler_desc.magFilter = (MTLSamplerMinMagFilter)info->mag_filter;
  sampler_desc.minFilter = (MTLSamplerMinMagFilter)info->min_filter;
  sampler_desc.mipFilter = (MTLSamplerMipFilter)info->mip_filter;
  sampler_desc.compareFunction = (MTLCompareFunction)info->compare_function;
  sampler_desc.lodMaxClamp = info->lod_max_clamp;
  sampler_desc.lodMinClamp = info->lod_min_clamp;
  sampler_desc.maxAnisotropy = info->max_anisotroy;
  sampler_desc.lodAverage = info->lod_average;
  sampler_desc.normalizedCoordinates = info->normalized_coords;
  sampler_desc.supportArgumentBuffers = info->support_argument_buffers;

  id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sampler_desc];
  info->gpu_resource_id = info->support_argument_buffers ? [sampler gpuResourceID]._impl : 0;
  params->ret = (obj_handle_t)sampler;
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && info->gpu_resource_id) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *payload = dxmt_apitrace_sampler_descriptor_json(info, params->ret);
      apitrace_metal_object_metadata(dxmt_apitrace_session, params->ret, payload.UTF8String);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  [sampler_desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newDepthStencilState(void *obj) {
  struct unixcall_mtldevice_newdepthstencilstate *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTDepthStencilInfo *info = params->info.ptr;

  MTLDepthStencilDescriptor *desc = [[MTLDepthStencilDescriptor alloc] init];
  desc.depthCompareFunction = (MTLCompareFunction)info->depth_compare_function;
  desc.depthWriteEnabled = info->depth_write_enabled;

  if (info->front_stencil.enabled) {
    desc.frontFaceStencil.depthStencilPassOperation = (MTLStencilOperation)info->front_stencil.depth_stencil_pass_op;
    desc.frontFaceStencil.depthFailureOperation = (MTLStencilOperation)info->front_stencil.depth_fail_op;
    desc.frontFaceStencil.stencilFailureOperation = (MTLStencilOperation)info->front_stencil.stencil_fail_op;
    desc.frontFaceStencil.stencilCompareFunction = (MTLCompareFunction)info->front_stencil.stencil_compare_function;
    desc.frontFaceStencil.writeMask = info->front_stencil.write_mask;
    desc.frontFaceStencil.readMask = info->front_stencil.read_mask;
  }

  if (info->back_stencil.enabled) {
    desc.backFaceStencil.depthStencilPassOperation = (MTLStencilOperation)info->back_stencil.depth_stencil_pass_op;
    desc.backFaceStencil.depthFailureOperation = (MTLStencilOperation)info->back_stencil.depth_fail_op;
    desc.backFaceStencil.stencilFailureOperation = (MTLStencilOperation)info->back_stencil.stencil_fail_op;
    desc.backFaceStencil.stencilCompareFunction = (MTLCompareFunction)info->back_stencil.stencil_compare_function;
    desc.backFaceStencil.writeMask = info->back_stencil.write_mask;
    desc.backFaceStencil.readMask = info->back_stencil.read_mask;
  }

  params->ret = (obj_handle_t)[device newDepthStencilStateWithDescriptor:desc];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && params->ret) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *payload = dxmt_apitrace_depth_stencil_json(info, params->ret);
      apitrace_metal_object_metadata(dxmt_apitrace_session, params->ret, payload.UTF8String);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  [desc release];
  return STATUS_SUCCESS;
}

MTLPixelFormat to_metal_pixel_format(enum WMTPixelFormat format) {
  return (MTLPixelFormat)ORIGINAL_FORMAT(format);
}

void
fill_texture_descriptor(MTLTextureDescriptor *desc, struct WMTTextureInfo *info) {
  desc.textureType = (MTLTextureType)info->type;
  desc.pixelFormat = to_metal_pixel_format(info->pixel_format);
  desc.width = info->width;
  desc.height = info->height;
  desc.depth = info->depth;
  desc.arrayLength = info->array_length;
  desc.mipmapLevelCount = info->mipmap_level_count;
  desc.sampleCount = info->sample_count;
  desc.usage = (MTLTextureUsage)info->usage;
  desc.resourceOptions = (MTLResourceOptions)info->options;
  if (info->reserved & WMTTextureInfoFlagPlacementSparse) {
    if (@available(macOS 26.0, *)) {
      desc.placementSparsePageSize = MTLSparsePageSize64;
    }
  }
};

void
extract_texture_descriptor(id<MTLTexture> desc, struct WMTTextureInfo *info) {
  info->type = desc.textureType;
  info->pixel_format = desc.pixelFormat;
  info->width = desc.width;
  info->height = desc.height;
  info->depth = desc.depth;
  info->array_length = desc.arrayLength;
  info->mipmap_level_count = desc.mipmapLevelCount;
  info->sample_count = desc.sampleCount;
  info->usage = desc.usage;
  info->options = (enum WMTResourceOptions)desc.resourceOptions;
  info->reserved = 0;
};

static NTSTATUS
_MTLDevice_newTexture(void *obj) {
  struct unixcall_mtldevice_newtexture *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTTextureInfo *info = params->info.ptr;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);

  id<MTLTexture> ret = [device newTextureWithDescriptor:desc];
  params->ret = (obj_handle_t)ret;
  info->gpu_resource_id = ret ? [ret gpuResourceID]._impl : 0;
  info->mach_port = 0;
#if DXMT_APITRACE_METAL
  if (ret && dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *resource_payload = dxmt_apitrace_json_string(@{
        @"kind" : @"dxmt_texture_gpu_resource_id",
        @"texture_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->ret],
        @"gpu_resource_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)info->gpu_resource_id],
      });
      apitrace_metal_object_metadata(dxmt_apitrace_session, params->ret, resource_payload.UTF8String);
      NSString *descriptor_json = dxmt_apitrace_texture_descriptor_json(info);
      apitrace_metal_register_texture(dxmt_apitrace_session, params->ret, descriptor_json.UTF8String);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLHeap_newTexture(void *obj) {
  struct unixcall_mtlheap_newtexture *params = obj;
  id<MTLHeap> heap = (id<MTLHeap>)params->heap;
  struct WMTTextureInfo *info = params->info.ptr;
  params->ret = 0;
  if (!heap || !info)
    return STATUS_SUCCESS;

  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);
  id<MTLTexture> texture = nil;
  if (@available(macOS 10.15, *)) {
    const MTLResourceOptions options = (MTLResourceOptions)info->options;
    const MTLSizeAndAlign placement =
        [heap.device heapTextureSizeAndAlignWithDescriptor:desc];
    const uint64_t heap_size = heap.size;
    if (placement.align && !(params->offset % placement.align) &&
        params->offset <= heap_size && placement.size <= heap_size - params->offset) {
      dxmt_diag_heap_placement("texture", heap, params->offset, placement.size,
                               options, info, YES);
      texture = [heap newTextureWithDescriptor:desc offset:params->offset];
      if (!texture) {
        dxmt_diag_heap_placement("texture-nil", heap, params->offset,
                                 placement.size, options, info, NO);
      }
    } else if (dxmt_metal4_heap_storage_diag_verbose()) {
      fprintf(stderr,
              "warn:  DXMT Metal4 heap placement rejected: kind=texture heap=%p "
              "offset=%llu placementSize=%llu placementAlign=%llu heapSize=%llu "
              "tex=%ux%ux%u format=%u\n",
              heap, (unsigned long long)params->offset,
              (unsigned long long)placement.size,
              (unsigned long long)placement.align,
              (unsigned long long)heap_size, (unsigned)info->width,
              (unsigned)info->height, (unsigned)info->depth,
              (unsigned)info->pixel_format);
      fflush(stderr);
    }
  }
  params->ret = (obj_handle_t)texture;
  info->gpu_resource_id = texture ? [texture gpuResourceID]._impl : 0;
  info->mach_port = 0;
#if DXMT_APITRACE_METAL
  if (texture && dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *resource_payload = dxmt_apitrace_json_string(@{
        @"kind" : @"dxmt_texture_gpu_resource_id",
        @"texture_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->ret],
        @"gpu_resource_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)info->gpu_resource_id],
      });
      apitrace_metal_object_metadata(
          dxmt_apitrace_session, params->ret, resource_payload.UTF8String);
      NSString *descriptor_json = dxmt_apitrace_texture_descriptor_json(info);
      apitrace_metal_register_texture(
          dxmt_apitrace_session, params->ret, descriptor_json.UTF8String);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsPlacementSparse(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->handle;
  params->ret = 0;
  if (@available(macOS 26.4, *)) {
    params->ret = device.supportsPlacementSparse ? 1 : 0;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_sparseTileSize(void *obj) {
  struct unixcall_mtldevice_sparsetilesize *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTTextureInfo *info = params->info.ptr;
  struct WMTSparseTileSize *tile_size = params->tile_size.ptr;
  params->ret = 0;
  if (!info || !tile_size)
    return STATUS_SUCCESS;

  if (@available(macOS 26.0, *)) {
    const MTLPixelFormat format = to_metal_pixel_format(info->pixel_format);
    MTLSize size = [device sparseTileSizeWithTextureType:(MTLTextureType)info->type
                                             pixelFormat:format
                                             sampleCount:info->sample_count
                                          sparsePageSize:MTLSparsePageSize64];
    tile_size->width = size.width;
    tile_size->height = size.height;
    tile_size->depth = size.depth;
    tile_size->bytes = [device sparseTileSizeInBytesForSparsePageSize:MTLSparsePageSize64];
    params->ret = tile_size->width && tile_size->height && tile_size->depth && tile_size->bytes;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_heapTextureSizeAndAlign(void *obj) {
  struct unixcall_mtldevice_heaptexturesizeandalign *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTTextureInfo *info = params->info.ptr;
  struct WMTSizeAndAlign *size_and_align = params->size_and_align.ptr;
  params->ret = 0;
  if (!device || !info || !size_and_align)
    return STATUS_SUCCESS;

  size_and_align->size = 0;
  size_and_align->alignment = 0;
  if (@available(macOS 10.13, *)) {
    struct WMTTextureInfo descriptor_info = *info;
    MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
    fill_texture_descriptor(desc, &descriptor_info);
    const MTLSizeAndAlign placement =
        [device heapTextureSizeAndAlignWithDescriptor:desc];
    size_and_align->size = placement.size;
    size_and_align->alignment = placement.align;
    params->ret = placement.size && placement.align;
    [desc release];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newPlacementHeap(void *obj) {
  struct unixcall_mtldevice_newplacementheap *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTPlacementHeapInfo *info = params->info.ptr;
  params->ret = 0;
  if (!info || !info->size)
    return STATUS_SUCCESS;

  if (@available(macOS 10.15, *)) {
    MTLHeapDescriptor *desc = [[MTLHeapDescriptor alloc] init];
    desc.type = MTLHeapTypePlacement;
    desc.size = info->size;
    desc.resourceOptions = (MTLResourceOptions)info->options;
    if (@available(macOS 26.0, *))
      desc.maxCompatiblePlacementSparsePageSize = MTLSparsePageSize64;
    id<MTLHeap> heap = [device newHeapWithDescriptor:desc];
    params->ret = (obj_handle_t)heap;
    if (heap && (dxmt_metal4_heap_storage_diag_verbose() ||
                 dxmt_storage_mode_from_resource_options(
                     (MTLResourceOptions)info->options) != heap.storageMode)) {
      static _Atomic uint32_t heap_create_count = 0;
      const uint32_t n =
          atomic_fetch_add_explicit(&heap_create_count, 1,
                                    memory_order_relaxed) +
          1;
      if (n <= 16 || (n % 64) == 0) {
        fprintf(stderr,
                "info:  DXMT Metal4 placement heap created: heap=%p size=%llu "
                "reqOptions=0x%llx reqStorage=%s heapStorage=%s type=%lu "
                "hazard=%lu count=%u\n",
                heap, (unsigned long long)info->size,
                (unsigned long long)info->options,
                dxmt_storage_mode_name(dxmt_storage_mode_from_resource_options(
                    (MTLResourceOptions)info->options)),
                dxmt_storage_mode_name(heap.storageMode),
                (unsigned long)heap.type,
                (unsigned long)heap.hazardTrackingMode, n);
        fflush(stderr);
      }
    }
    [desc release];
  }
  return STATUS_SUCCESS;
}

static MTLSparseTextureMappingMode
to_metal_sparse_texture_mapping_mode(enum WMTSparseTextureMappingMode mode) {
  switch (mode) {
  case WMTSparseTextureMappingModeUnmap:
    return MTLSparseTextureMappingModeUnmap;
  case WMTSparseTextureMappingModeMap:
  default:
    return MTLSparseTextureMappingModeMap;
  }
}

static NTSTATUS
_MTLDevice_updateSparseTextureMappings(void *obj) {
  struct unixcall_mtldevice_updatesparsetexturemappings *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  id<MTLTexture> texture = (id<MTLTexture>)params->texture;
  id<MTLHeap> heap = (id<MTLHeap>)params->heap;
  const struct WMTSparseTextureMappingOperation *operations = params->operations.ptr;
  params->ret = 0;
  if (!texture || !operations || !params->operation_count)
    return STATUS_SUCCESS;

  if (@available(macOS 26.0, *)) {
    id<MTL4CommandQueue> queue = [device newMTL4CommandQueue];
    id<MTLSharedEvent> event = [device newSharedEvent];
    MTLResidencySetDescriptor *residencyDescriptor =
        [[MTLResidencySetDescriptor alloc] init];
    residencyDescriptor.initialCapacity = 2;
    NSError *residencyError = nil;
    id<MTLResidencySet> residencySet =
        [device newResidencySetWithDescriptor:residencyDescriptor
                                        error:&residencyError];
    [residencyDescriptor release];
    if (!queue || !event || !residencySet) {
      [queue release];
      [event release];
      [residencySet release];
      return STATUS_SUCCESS;
    }

    [residencySet addAllocation:(id<MTLAllocation>)texture];
    DXMTSparseTextureResidency *residency =
        dxmt_sparse_texture_residency_for_texture(texture, true);
    [residency addMappedHeapsToResidencySet:residencySet];
    if (heap)
      [residencySet addAllocation:(id<MTLAllocation>)heap];
    [residencySet commit];
    [residencySet requestResidency];
    [queue addResidencySet:residencySet];

    MTL4UpdateSparseTextureMappingOperation *mtl_ops =
        calloc((size_t)params->operation_count, sizeof(*mtl_ops));
    if (!mtl_ops) {
      [queue removeResidencySet:residencySet];
      [residencySet endResidency];
      [residencySet release];
      [queue release];
      [event release];
      return STATUS_SUCCESS;
    }

    for (uint64_t i = 0; i < params->operation_count; ++i) {
      const struct WMTSparseTextureMappingOperation *src = &operations[i];
      mtl_ops[i].mode = to_metal_sparse_texture_mapping_mode(src->mode);
      mtl_ops[i].textureRegion = MTLRegionMake3D(src->x, src->y, src->z,
                                                 src->width, src->height,
                                                 src->depth);
      mtl_ops[i].textureLevel = src->level;
      mtl_ops[i].textureSlice = src->slice;
      mtl_ops[i].heapOffset =
          src->heap_offset / WMT_SPARSE_TILE_SIZE_IN_BYTES;
    }

    [queue updateTextureMappings:texture
                            heap:heap
                      operations:mtl_ops
                           count:(NSUInteger)params->operation_count];
    [queue signalEvent:event value:1];
    params->ret = [event waitUntilSignaledValue:1 timeoutMS:UINT64_MAX] ? 1 : 0;

    if (params->ret) {
      [residency applyOperations:operations
                           count:params->operation_count
                            heap:heap];
    }

    [queue removeResidencySet:residencySet];
    [residencySet endResidency];
    [residencySet release];
    free(mtl_ops);
    [queue release];
    [event release];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandQueue_updateSparseTextureMappings(void *obj) {
  struct unixcall_mtldevice_updatesparsetexturemappings *params = obj;
  DXMTMetal4CommandQueue *queue =
      (DXMTMetal4CommandQueue *)params->device;
  id<MTLTexture> texture = (id<MTLTexture>)params->texture;
  id<MTLHeap> heap = (id<MTLHeap>)params->heap;
  const struct WMTSparseTextureMappingOperation *operations =
      params->operations.ptr;
  params->ret = 0;
  if (!queue || !texture || !operations || !params->operation_count)
    return STATUS_SUCCESS;

  if (@available(macOS 26.0, *)) {
    MTL4UpdateSparseTextureMappingOperation *mtl_ops =
        calloc((size_t)params->operation_count, sizeof(*mtl_ops));
    if (!mtl_ops)
      return STATUS_SUCCESS;

    for (uint64_t i = 0; i < params->operation_count; ++i) {
      const struct WMTSparseTextureMappingOperation *src = &operations[i];
      mtl_ops[i].mode = to_metal_sparse_texture_mapping_mode(src->mode);
      mtl_ops[i].textureRegion =
          MTLRegionMake3D(src->x, src->y, src->z, src->width, src->height,
                          src->depth);
      mtl_ops[i].textureLevel = src->level;
      mtl_ops[i].textureSlice = src->slice;
      mtl_ops[i].heapOffset =
          src->heap_offset / WMT_SPARSE_TILE_SIZE_IN_BYTES;
    }

    uint64_t map_count = 0;
    uint64_t unmap_count = 0;
    for (uint64_t i = 0; i < params->operation_count; ++i) {
      if (operations[i].mode == WMTSparseTextureMappingModeMap)
        map_count++;
      else
        unmap_count++;
    }

    DXMTSparseTextureResidency *residency =
        dxmt_sparse_texture_residency_for_texture(texture, true);
    NSSet *oldMappedHeaps =
        [NSSet setWithArray:[residency mappedHeapsSnapshot]];

    [queue monitorWaitBegin:DXMTMetal4QueueMonitorSourceSparseTextureMappings
              commandBuffer:nil];
    [queue.submissionLock lock];
    [queue monitorWaitEnd:DXMTMetal4QueueMonitorSourceSparseTextureMappings
            commandBuffer:nil];
    [queue monitorAcquired:DXMTMetal4QueueMonitorSourceSparseTextureMappings
                     phase:DXMTMetal4QueueMonitorPhaseSparseMappingPreflight
             commandBuffer:nil];
    @try {
    [queue monitorSetSubject:(uintptr_t)texture
                         aux:(uintptr_t)heap
                       count:params->operation_count
                       flags:(map_count & 0xffffffffull) |
                             (unmap_count << 32)];
    [queue.errorLock lock];
    const BOOL queueFailed = queue.firstError != nil;
    [queue.errorLock unlock];
    if (!queueFailed) {
      [queue.eventLock lock];
      const uint64_t waitValue = queue.eventValue;
      const uint64_t mappingCompletionValue = ++queue.eventValue;
      [queue.eventLock unlock];

      [queue monitorSetPhase:
                 DXMTMetal4QueueMonitorPhaseSparseMappingResidencyRefs
               commandBuffer:nil];
      [queue.sparseResidencyLock lock];
      const uint64_t completedValue = queue.event.signaledValue;
      NSIndexSet *completedRetirements =
          [queue.sparseResidencyRetirements
              indexesOfObjectsPassingTest:^BOOL(NSDictionary *retirement,
                                                 NSUInteger index,
                                                 BOOL *stop) {
                (void)index;
                (void)stop;
                return [[retirement objectForKey:@"completion"]
                           unsignedLongLongValue] <= completedValue;
              }];
      for (NSDictionary *retirement in
               [queue.sparseResidencyRetirements
                   objectsAtIndexes:completedRetirements]) {
        for (id<MTLAllocation> allocation in
                 [retirement objectForKey:@"allocations"]) {
          [queue.sparseResidencyAllocationRefs removeObject:allocation];
          if ([queue.sparseResidencyAllocationRefs
                  countForObject:allocation] == 0)
            [queue.sparseResidencySet removeAllocation:allocation];
        }
      }
      [queue.sparseResidencyRetirements
          removeObjectsAtIndexes:completedRetirements];
      if (![queue.sparseTrackedTextures containsObject:texture]) {
        [queue.sparseTrackedTextures addObject:texture];
        [queue.sparseResidencyAllocationRefs
            addObject:(id<MTLAllocation>)texture];
        [queue.sparseResidencySet
            addAllocation:(id<MTLAllocation>)texture];
      }
      BOOL heapPrecounted = NO;
      if (heap) {
        if ([queue.sparseResidencyAllocationRefs
                countForObject:(id<MTLAllocation>)heap] == 0)
          [queue.sparseResidencySet
              addAllocation:(id<MTLAllocation>)heap];
        [queue.sparseResidencyAllocationRefs
            addObject:(id<MTLAllocation>)heap];
        heapPrecounted = YES;
      }
      [queue monitorSetPhase:
                 DXMTMetal4QueueMonitorPhaseSparseMappingResidencyCommit
               commandBuffer:nil];
      [queue.sparseResidencySet commit];
      [queue.sparseResidencyLock unlock];

      if (waitValue)
        [queue.metal4Queue waitForEvent:queue.event value:waitValue];
      [queue monitorSetPhase:
                 DXMTMetal4QueueMonitorPhaseSparseMappingMetalUpdate
               commandBuffer:nil];
      [queue.metal4Queue
          updateTextureMappings:texture
                           heap:heap
                     operations:mtl_ops
                          count:(NSUInteger)params->operation_count];
      [queue.metal4Queue signalEvent:queue.event
                              value:mappingCompletionValue];
      [queue monitorSetPhase:DXMTMetal4QueueMonitorPhaseSparseMappingApply
               commandBuffer:nil];
      [residency applyOperations:operations
                           count:params->operation_count
                            heap:heap];

      NSSet *newMappedHeaps =
          [NSSet setWithArray:[residency mappedHeapsSnapshot]];
      NSMutableSet *addedHeaps = [newMappedHeaps mutableCopy];
      [addedHeaps minusSet:oldMappedHeaps];
      NSMutableSet *removedHeaps = [oldMappedHeaps mutableCopy];
      [removedHeaps minusSet:newMappedHeaps];
      NSMutableArray *retiredAllocations = [NSMutableArray array];

      [queue.sparseResidencyLock lock];
      for (id<MTLAllocation> addedHeap in addedHeaps) {
        if (heapPrecounted && addedHeap == (id<MTLAllocation>)heap) {
          heapPrecounted = NO;
          continue;
        }
        if ([queue.sparseResidencyAllocationRefs
                countForObject:addedHeap] == 0)
          [queue.sparseResidencySet addAllocation:addedHeap];
        [queue.sparseResidencyAllocationRefs addObject:addedHeap];
      }
      [retiredAllocations addObjectsFromArray:[removedHeaps allObjects]];
      if (heapPrecounted)
        [retiredAllocations addObject:(id<MTLAllocation>)heap];
      if (!newMappedHeaps.count &&
          [queue.sparseTrackedTextures containsObject:texture]) {
        /*
         * Change logical membership immediately, but keep the allocation in
         * the residency set until this ordered mapping update has completed.
         * A remap submitted before completion re-adds a reference, so retiring
         * the old mapping cannot evict the newly mapped texture.
         */
        [queue.sparseTrackedTextures removeObject:texture];
        [retiredAllocations addObject:(id<MTLAllocation>)texture];
      }
      if (retiredAllocations.count) {
        [queue.sparseResidencyRetirements addObject:@{
          @"completion" : @(mappingCompletionValue),
          @"allocations" : [[retiredAllocations copy] autorelease]
        }];
      }
      [queue.sparseResidencySet commit];
      [queue.sparseResidencyLock unlock];
      [removedHeaps release];
      [addedHeaps release];
      params->ret = 1;
    }
    } @finally {
      [queue monitorReleased];
      [queue.submissionLock unlock];
    }

    free(mtl_ops);
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_newTexture(void *obj) {
  struct unixcall_mtlbuffer_newtexture *params = obj;
  id<MTLBuffer> buffer = (id<MTLBuffer>)params->buffer;
  struct WMTTextureInfo *info = params->info.ptr;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);

  id<MTLTexture> ret = [buffer newTextureWithDescriptor:desc offset:params->offset bytesPerRow:params->bytes_per_row];
  params->ret = (obj_handle_t)ret;
  info->gpu_resource_id = ret ? [ret gpuResourceID]._impl : 0;
  info->mach_port = 0;
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *resource_payload = dxmt_apitrace_json_string(@{
        @"kind" : @"dxmt_texture_gpu_resource_id",
        @"texture_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->ret],
        @"gpu_resource_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)info->gpu_resource_id],
      });
      apitrace_metal_object_metadata(dxmt_apitrace_session, params->ret, resource_payload.UTF8String);
      NSString *descriptor_json = dxmt_apitrace_texture_descriptor_json(info);
      apitrace_metal_register_texture(dxmt_apitrace_session, params->ret, descriptor_json.UTF8String);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif

  [desc release];
  return STATUS_SUCCESS;
}

static inline MTLTextureSwizzleChannels
to_metal_swizzle(struct WMTTextureSwizzleChannels swizzle, enum WMTPixelFormat format) {
  if (format & WMTPixelFormatRGB1Swizzle) {
    return MTLTextureSwizzleChannelsMake(
        (MTLTextureSwizzle)swizzle.r, (MTLTextureSwizzle)swizzle.g, (MTLTextureSwizzle)swizzle.b, MTLTextureSwizzleOne
    );
  }
  if (format & WMTPixelFormatR001Swizzle) {
    return MTLTextureSwizzleChannelsMake(
        (MTLTextureSwizzle)swizzle.r, MTLTextureSwizzleZero, MTLTextureSwizzleZero, MTLTextureSwizzleOne
    );
  }
  if (format & WMTPixelFormat0R01Swizzle) {
    return MTLTextureSwizzleChannelsMake(
        MTLTextureSwizzleOne, (MTLTextureSwizzle)swizzle.r, MTLTextureSwizzleOne, MTLTextureSwizzleOne
    );
  }
  if (format & WMTPixelFormatGBARSwizzle) {
    return MTLTextureSwizzleChannelsMake(
        (MTLTextureSwizzle)swizzle.g, (MTLTextureSwizzle)swizzle.b, (MTLTextureSwizzle)swizzle.a,
        (MTLTextureSwizzle)swizzle.r
    );
  }
  return MTLTextureSwizzleChannelsMake(
      (MTLTextureSwizzle)swizzle.r, (MTLTextureSwizzle)swizzle.g, (MTLTextureSwizzle)swizzle.b,
      (MTLTextureSwizzle)swizzle.a
  );
}

static inline void
fill_texture_view_descriptor(MTLTextureViewDescriptor *dst, const struct WMTTextureViewDescriptor *src) {
  dst.pixelFormat = to_metal_pixel_format(src->pixel_format);
  dst.textureType = (MTLTextureType)src->texture_type;
  dst.levelRange = NSMakeRange(src->level_start, src->level_count);
  dst.sliceRange = NSMakeRange(src->slice_start, src->slice_count);
  dst.swizzle = to_metal_swizzle(src->swizzle, src->pixel_format);
}

static NTSTATUS
_MTLTexture_newTextureView(void *obj) {
  struct unixcall_mtltexture_newtextureview *params = obj;
  id<MTLTexture> texture = (id<MTLTexture>)params->texture;
  params->ret = 0;
  params->gpu_resource_id = 0;
  if (!dxmt_metal4_is_texture((id)texture))
    return STATUS_SUCCESS;

  id<MTLTexture> ret = [texture
      newTextureViewWithPixelFormat:to_metal_pixel_format(params->format)
                        textureType:(MTLTextureType)params->texture_type
                             levels:NSMakeRange(params->level_start, params->level_count)
                             slices:NSMakeRange(params->slice_start, params->slice_count)
                            swizzle:to_metal_swizzle(params->swizzle, params->format)];
  params->ret = (obj_handle_t)ret;
  params->gpu_resource_id = ret ? [ret gpuResourceID]._impl : 0;
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && params->ret) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *resource_payload = dxmt_apitrace_json_string(@{
        @"kind" : @"dxmt_texture_gpu_resource_id",
        @"texture_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->ret],
        @"gpu_resource_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->gpu_resource_id],
      });
      apitrace_metal_object_metadata(dxmt_apitrace_session, params->ret, resource_payload.UTF8String);
      NSString *view_payload = dxmt_apitrace_texture_view_json(params);
      apitrace_metal_object_metadata(dxmt_apitrace_session, params->ret, view_payload.UTF8String);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTextureViewPool_setTextureView(void *obj) {
  struct unixcall_mtltextureviewpool_set_texture *params = obj;
  id pool = (id)params->pool;
  id texture = (id)params->texture;
  params->ret_gpu_resource_id = 0;
  if (!dxmt_metal4_is_texture_view_pool(pool) || !dxmt_metal4_is_texture(texture))
    return STATUS_SUCCESS;

  if (@available(macOS 26.0, *)) {
    params->ret_gpu_resource_id = [(id<MTLTextureViewPool>)pool setTextureView:(id<MTLTexture>)texture
                                                                       atIndex:(NSUInteger)params->index]._impl;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTextureViewPool_setTextureViewWithDescriptor(void *obj) {
  struct unixcall_mtltextureviewpool_set_texture_descriptor *params = obj;
  id pool = (id)params->pool;
  id texture = (id)params->texture;
  const struct WMTTextureViewDescriptor *info = params->descriptor.ptr;
  params->ret_gpu_resource_id = 0;
  if (!dxmt_metal4_is_texture_view_pool(pool) || !dxmt_metal4_is_texture(texture) || !info)
    return STATUS_SUCCESS;

  if (@available(macOS 26.0, *)) {
    MTLTextureViewDescriptor *descriptor = [[MTLTextureViewDescriptor alloc] init];
    fill_texture_view_descriptor(descriptor, info);
    params->ret_gpu_resource_id = [(id<MTLTextureViewPool>)pool setTextureView:(id<MTLTexture>)texture
                                                                    descriptor:descriptor
                                                                       atIndex:(NSUInteger)params->index]._impl;
    [descriptor release];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTextureViewPool_setTextureViewFromBuffer(void *obj) {
  struct unixcall_mtltextureviewpool_set_buffer_descriptor *params = obj;
  id pool = (id)params->pool;
  id buffer = (id)params->buffer;
  const struct WMTTextureBufferViewDescriptor *info = params->descriptor.ptr;
  params->ret_gpu_resource_id = 0;
  if (!dxmt_metal4_is_texture_view_pool(pool) || !dxmt_metal4_is_buffer(buffer) || !info)
    return STATUS_SUCCESS;

  if (@available(macOS 26.0, *)) {
    MTLTextureDescriptor *descriptor = [[MTLTextureDescriptor alloc] init];
    struct WMTTextureInfo texture_info = info->texture;
    fill_texture_descriptor(descriptor, &texture_info);
    params->ret_gpu_resource_id = [(id<MTLTextureViewPool>)pool setTextureViewFromBuffer:(id<MTLBuffer>)buffer
                                                                              descriptor:descriptor
                                                                                  offset:(NSUInteger)params->offset
                                                                             bytesPerRow:(NSUInteger)params->bytes_per_row
                                                                                 atIndex:(NSUInteger)params->index]._impl;
    [descriptor release];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResourceViewPool_baseResourceID(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  id pool = (id)params->handle;
  params->ret = 0;
  if (!dxmt_metal4_is_resource_view_pool(pool))
    return STATUS_SUCCESS;

  if (@available(macOS 26.0, *)) {
    params->ret = [(id<MTLResourceViewPool>)pool baseResourceID]._impl;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResourceViewPool_copyResourceViews(void *obj) {
  struct unixcall_mtlresourceviewpool_copy *params = obj;
  id destination_pool = (id)params->destination_pool;
  id source_pool = (id)params->source_pool;
  params->ret_gpu_resource_id = 0;
  if (!dxmt_metal4_is_resource_view_pool(destination_pool) ||
      !dxmt_metal4_is_resource_view_pool(source_pool) || !params->count)
    return STATUS_SUCCESS;

  if (@available(macOS 26.0, *)) {
    params->ret_gpu_resource_id = [(id<MTLResourceViewPool>)destination_pool
        copyResourceViewsFromPool:(id<MTLResourceViewPool>)source_pool
                      sourceRange:NSMakeRange((NSUInteger)params->source_index, (NSUInteger)params->count)
                 destinationIndex:(NSUInteger)params->destination_index]._impl;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_minimumLinearTextureAlignmentForPixelFormat(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle minimumLinearTextureAlignmentForPixelFormat:to_metal_pixel_format(params->arg)];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newLibrary(void *obj) {
  struct unixcall_mtldevice_newlibrary *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  NSError *err = NULL;
  params->ret_library = (obj_handle_t)[device newLibraryWithData:(dispatch_data_t)params->data error:&err];
  params->ret_error = (obj_handle_t)err;
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && !err && params->ret_library) {
    const void *bytes = NULL;
    size_t size = 0;
    dispatch_data_t mapped = dispatch_data_create_map((dispatch_data_t)params->data, &bytes, &size);
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      apitrace_metal_register_library(dxmt_apitrace_session, params->ret_library, bytes, size);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
    (void)mapped;
  }
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLLibrary_newFunction(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  id<MTLLibrary> library = (id<MTLLibrary>)params->handle;
  NSString *name = [[NSString alloc] initWithCString:(char *)params->arg encoding:NSUTF8StringEncoding];
  params->ret = (obj_handle_t)[library newFunctionWithName:name];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && params->ret) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    dxmt_apitrace_track_function_locked(params->ret, params->handle, name);
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  [name release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_lengthOfBytesUsingEncoding(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = (uint64_t)[(NSString *)params->handle lengthOfBytesUsingEncoding:(NSStringEncoding)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSObject_description(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(NSObject *)params->handle description];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newComputePipelineState(void *obj) {
  struct unixcall_mtldevice_newcomputepso *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTComputePipelineInfo *info = params->info.ptr;
  MTLComputePipelineDescriptor *descriptor = [[MTLComputePipelineDescriptor alloc] init];
  NSError *err = NULL;
  descriptor.computeFunction = (id<MTLFunction>)info->compute_function;
  descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth = info->tgsize_is_multiple_of_sgwidth;
  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_buffers & (1 << i))
      descriptor.buffers[i].mutability = MTLMutabilityImmutable;
  }
  if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
    descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                    count:info->num_binary_archives_for_lookup];
  MTLPipelineOption options =
      info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  uint64_t compile_wait_begin = dxmt_monotonic_us();
  params->ret_pso =
      (obj_handle_t)[device newComputePipelineStateWithDescriptor:descriptor options:options reflection:nil error:&err];
  uint64_t compile_wait_end = dxmt_monotonic_us();
  params->ret_compile_wait_us =
      compile_wait_end >= compile_wait_begin ? compile_wait_end - compile_wait_begin : 0;
  params->ret_error = (obj_handle_t)err;
  if (!err && info->binary_archive_for_serialization) {
    [(id<MTLBinaryArchive>)info->binary_archive_for_serialization addComputePipelineFunctionsWithDescriptor:descriptor
                                                                                                      error:&err];
  }
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && params->ret_pso) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *descriptor_json = dxmt_apitrace_compute_pipeline_json(info);
      apitrace_metal_register_compute_pipeline(
          dxmt_apitrace_session,
          params->ret_pso,
          descriptor_json.UTF8String,
          info->compute_function);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_blitCommandEncoder(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(DXMTMetal4CommandBuffer *)params->handle metal4ComputeEncoder];
  dxmt_metal4_register_encoder(params->ret, (DXMTMetal4CommandBuffer *)params->handle);
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    dxmt_apitrace_begin_encoder_locked(
        dxmt_apitrace_ensure_session_locked(),
        DXMTApitraceEncoderKindMetal4Blit,
        params->ret,
        params->handle,
        NULL);
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_computeCommandEncoder(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  (void)params->arg;
  DXMTMetal4CommandBuffer *owner = (DXMTMetal4CommandBuffer *)params->handle;
  params->ret = (obj_handle_t)[owner metal4ComputeEncoder];
  dxmt_metal4_register_encoder(params->ret, owner);
  DXMTMetal4ComputeEncoderState *state = [[DXMTMetal4ComputeEncoderState alloc] initWithOwner:owner];
  dxmt_metal4_register_encoder_state(params->ret, state);
  [state release];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    dxmt_apitrace_begin_encoder_locked(
        dxmt_apitrace_ensure_session_locked(),
        DXMTApitraceEncoderKindCompute,
        params->ret,
        params->handle,
        NULL);
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  return STATUS_SUCCESS;
}

static void
dxmt_metal4_use_render_attachment(DXMTMetal4CommandBuffer *owner,
                                  id<MTLTexture> texture,
                                  const char *role,
                                  unsigned attachment_index) {
  if (!texture)
    return;

  if (!dxmt_metal4_residency_diag_enabled())
    return;

  const BOOL was_unlabelled = texture.label == nil;
  if (was_unlabelled) {
    texture.label = [NSString
        stringWithFormat:@"DXMT attachment %s[%u] texture=%p", role,
                         attachment_index, texture];
  }
  id<MTLAllocation> backing = dxmt_metal4_backing_allocation(
      (id<MTLAllocation>)texture);
  if (backing != (id<MTLAllocation>)texture &&
      [(id)backing conformsToProtocol:@protocol(MTLResource)] &&
      ![(id<MTLResource>)backing label]) {
    [(id<MTLResource>)backing setLabel:[NSString
        stringWithFormat:@"DXMT backing for %s[%u] allocation=%p", role,
                         attachment_index, backing]];
  }

  static atomic_uint_fast64_t occurrence = 0;
  const uint64_t index = atomic_fetch_add_explicit(
                             &occurrence, 1, memory_order_relaxed) +
                         1;
  if (was_unlabelled && (index <= 32 || (index & (index - 1)) == 0)) {
    fprintf(stderr,
            "info:  DXMT Metal4 render attachment: index=%" PRIu64
            " role=%s slot=%u texture=%p class=%s label=%s parent=%p buffer=%p"
            " heap=%p storage=%lu type=%lu format=%lu size=%lux%lu samples=%lu"
            " framebufferOnly=%u backing=%p setContainsView=%u"
            " setContainsBacking=%u setAllocations=%lu\n",
            index, role, attachment_index, texture,
            class_getName(object_getClass(texture)), texture.label.UTF8String,
            texture.parentTexture, texture.buffer, texture.heap,
            (unsigned long)texture.storageMode,
            (unsigned long)texture.textureType,
            (unsigned long)texture.pixelFormat, (unsigned long)texture.width,
            (unsigned long)texture.height, (unsigned long)texture.sampleCount,
            texture.framebufferOnly, backing,
            [owner.retainedTemporaryResources containsObject:texture],
            [owner.retainedTemporaryResources containsObject:backing],
            (unsigned long)owner.retainedTemporaryResources.count);
    fflush(stderr);
  }
}

static NTSTATUS
_MTLCommandBuffer_renderCommandEncoder(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  struct WMTRenderPassInfo *info = (struct WMTRenderPassInfo *)params->arg;
  DXMTMetal4CommandBuffer *owner = (DXMTMetal4CommandBuffer *)params->handle;
  MTL4RenderPassDescriptor *descriptor = [[MTL4RenderPassDescriptor alloc] init];
  for (unsigned i = 0; i < 8; i++) {
    dxmt_metal4_use_render_attachment(
        owner, (id<MTLTexture>)info->colors[i].texture, "color", i);
    dxmt_metal4_use_render_attachment(
        owner, (id<MTLTexture>)info->colors[i].resolve_texture, "resolve", i);
    descriptor.colorAttachments[i].clearColor = MTLClearColorMake(
        info->colors[i].clear_color.r, info->colors[i].clear_color.g, info->colors[i].clear_color.b,
        info->colors[i].clear_color.a
    );
    descriptor.colorAttachments[i].level = info->colors[i].level;
    descriptor.colorAttachments[i].slice = info->colors[i].slice;
    descriptor.colorAttachments[i].depthPlane = info->colors[i].depth_plane;
    descriptor.colorAttachments[i].texture = (id<MTLTexture>)info->colors[i].texture;
    descriptor.colorAttachments[i].loadAction = (MTLLoadAction)info->colors[i].load_action;
    descriptor.colorAttachments[i].storeAction = (MTLStoreAction)info->colors[i].store_action;
    descriptor.colorAttachments[i].resolveTexture = (id<MTLTexture>)info->colors[i].resolve_texture;
    descriptor.colorAttachments[i].resolveLevel = info->colors[i].resolve_level;
    descriptor.colorAttachments[i].resolveSlice = info->colors[i].resolve_slice;
    descriptor.colorAttachments[i].resolveDepthPlane = info->colors[i].resolve_depth_plane;
  }

  if (info->depth.texture) {
    dxmt_metal4_use_render_attachment(
        owner, (id<MTLTexture>)info->depth.texture, "depth", 0);
    descriptor.depthAttachment.clearDepth = info->depth.clear_depth;
    descriptor.depthAttachment.depthPlane = info->depth.depth_plane;
    descriptor.depthAttachment.level = info->depth.level;
    descriptor.depthAttachment.slice = info->depth.slice;
    descriptor.depthAttachment.texture = (id<MTLTexture>)info->depth.texture;
    descriptor.depthAttachment.loadAction = (MTLLoadAction)info->depth.load_action;
    descriptor.depthAttachment.storeAction = (MTLStoreAction)info->depth.store_action;
  }

  if (info->stencil.texture) {
    dxmt_metal4_use_render_attachment(
        owner, (id<MTLTexture>)info->stencil.texture, "stencil", 0);
    descriptor.stencilAttachment.clearStencil = info->stencil.clear_stencil;
    descriptor.stencilAttachment.depthPlane = info->stencil.depth_plane;
    descriptor.stencilAttachment.level = info->stencil.level;
    descriptor.stencilAttachment.slice = info->stencil.slice;
    descriptor.stencilAttachment.texture = (id<MTLTexture>)info->stencil.texture;
    descriptor.stencilAttachment.loadAction = (MTLLoadAction)info->stencil.load_action;
    descriptor.stencilAttachment.storeAction = (MTLStoreAction)info->stencil.store_action;
  }

  descriptor.defaultRasterSampleCount = info->default_raster_sample_count;
  descriptor.renderTargetArrayLength = info->render_target_array_length;
  descriptor.renderTargetHeight = info->render_target_height;
  descriptor.renderTargetWidth = info->render_target_width;
  descriptor.visibilityResultBuffer = (id<MTLBuffer>)info->visibility_buffer;

  if (info->tile_height && info->tile_width) {
    descriptor.tileWidth = info->tile_width;
    descriptor.tileHeight = info->tile_height;
  }

  params->ret = (obj_handle_t)[owner metal4RenderCommandEncoderWithDescriptor:descriptor];
  dxmt_metal4_register_encoder(params->ret, owner);
  DXMTMetal4RenderEncoderState *state = [[DXMTMetal4RenderEncoderState alloc] initWithOwner:owner];
  dxmt_metal4_register_encoder_state(params->ret, state);
  [state release];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    dxmt_apitrace_begin_encoder_locked(
        dxmt_apitrace_ensure_session_locked(),
        DXMTApitraceEncoderKindRender,
        params->ret,
        params->handle,
        info);
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif

  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandEncoder_endEncoding(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(id)params->handle endEncoding];
  dxmt_metal4_unregister_encoder(params->handle);
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    dxmt_apitrace_end_encoder_locked(dxmt_apitrace_session, params->handle);
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  return STATUS_SUCCESS;
}

#ifndef DXMT_NO_PRIVATE_API

typedef NS_ENUM(NSUInteger, MTLLogicOperation) {
  MTLLogicOperationClear,
  MTLLogicOperationSet,
  MTLLogicOperationCopy,
  MTLLogicOperationCopyInverted,
  MTLLogicOperationNoop,
  MTLLogicOperationInvert,
  MTLLogicOperationAnd,
  MTLLogicOperationNand,
  MTLLogicOperationOr,
  MTLLogicOperationNor,
  MTLLogicOperationXor,
  MTLLogicOperationEquivalence,
  MTLLogicOperationAndReverse,
  MTLLogicOperationAndInverted,
  MTLLogicOperationOrReverse,
  MTLLogicOperationOrInverted,
};

@interface
MTLRenderPipelineDescriptor ()

- (void)setLogicOperationEnabled:(BOOL)enable;
- (void)setLogicOperation:(MTLLogicOperation)op;

@end

@interface
MTLMeshRenderPipelineDescriptor ()

- (void)setLogicOperationEnabled:(BOOL)enable;
- (void)setLogicOperation:(MTLLogicOperation)op;

@end

#endif

static NTSTATUS
_MTLDevice_newRenderPipelineState(void *obj) {
  struct unixcall_mtldevice_newrenderpso *params = obj;
  const struct WMTRenderPipelineInfo *info = params->info.ptr;
  MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->colors[i].pixel_format);
    descriptor.colorAttachments[i].blendingEnabled = info->colors[i].blending_enabled;
    descriptor.colorAttachments[i].writeMask = (MTLColorWriteMask)info->colors[i].write_mask;

    descriptor.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)info->colors[i].alpha_blend_operation;
    descriptor.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)info->colors[i].rgb_blend_operation;

    descriptor.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)info->colors[i].src_rgb_blend_factor;
    descriptor.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)info->colors[i].src_alpha_blend_factor;
    descriptor.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)info->colors[i].dst_rgb_blend_factor;
    descriptor.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)info->colors[i].dst_alpha_blend_factor;
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_fragment_buffers & (1 << i))
      descriptor.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_vertex_buffers & (1 << i))
      descriptor.vertexBuffers[i].mutability = MTLMutabilityImmutable;
  }

#ifndef DXMT_NO_PRIVATE_API
  [descriptor setLogicOperationEnabled:info->logic_operation_enabled];
  [descriptor setLogicOperation:(MTLLogicOperation)info->logic_operation];
#endif
  descriptor.depthAttachmentPixelFormat = to_metal_pixel_format(info->depth_pixel_format);
  descriptor.stencilAttachmentPixelFormat = to_metal_pixel_format(info->stencil_pixel_format);
  descriptor.alphaToCoverageEnabled = info->alpha_to_coverage_enabled;
  descriptor.rasterizationEnabled = info->rasterization_enabled;
  descriptor.rasterSampleCount = info->raster_sample_count;
  descriptor.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)info->input_primitive_topology;
  descriptor.tessellationPartitionMode = (MTLTessellationPartitionMode)info->tessellation_partition_mode;
  descriptor.tessellationFactorStepFunction = (MTLTessellationFactorStepFunction)info->tessellation_factor_step;
  descriptor.tessellationOutputWindingOrder = (MTLWinding)info->tessellation_output_winding_order;
  descriptor.maxTessellationFactor = info->max_tessellation_factor;

  descriptor.vertexFunction = (id<MTLFunction>)info->vertex_function;
  descriptor.fragmentFunction = (id<MTLFunction>)info->fragment_function;
  if (dxmt_metal4_pso_labels_enabled() && info->debug_label[0])
    descriptor.label = [NSString stringWithUTF8String:info->debug_label];

  if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
    descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                    count:info->num_binary_archives_for_lookup];
  NSError *err = NULL;
  MTLPipelineOption options =
      info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  uint64_t compile_wait_begin = dxmt_monotonic_us();
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithDescriptor:descriptor
                                                                                              options:options
                                                                                           reflection:nil
                                                                                                error:&err];
  uint64_t compile_wait_end = dxmt_monotonic_us();
  params->ret_compile_wait_us =
      compile_wait_end >= compile_wait_begin ? compile_wait_end - compile_wait_begin : 0;
  params->ret_error = (obj_handle_t)err;
  const BOOL pso_created = params->ret_pso != 0;
  if (pso_created && info->binary_archive_for_serialization) {
    [(id<MTLBinaryArchive>)info->binary_archive_for_serialization addRenderPipelineFunctionsWithDescriptor:descriptor
                                                                                                     error:&err];
  }
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && pso_created) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *descriptor_json = dxmt_apitrace_render_pipeline_json(info);
      apitrace_metal_register_render_pipeline(
          dxmt_apitrace_session,
          params->ret_pso,
          descriptor_json.UTF8String,
          info->vertex_function,
          info->fragment_function);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  [descriptor release];
  return STATUS_SUCCESS;
}

static id<MTLFunction>
DXMTGetNullFragmentFunction(id<MTLDevice> device) {
  static NSMutableDictionary *functions_by_device = nil;

  @synchronized([MTLMeshRenderPipelineDescriptor class]) {
    if (!functions_by_device)
      functions_by_device = [[NSMutableDictionary alloc] init];

    NSValue *key = [[NSValue alloc] initWithBytes:&device objCType:@encode(void *)];
    id<MTLFunction> function = [functions_by_device objectForKey:key];
    if (function) {
      [key release];
      return function;
    }

    NSString *source = @"using namespace metal;\n"
                       "fragment void dxmt_null_fragment() {}\n";
    NSError *err = NULL;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&err];
    if (!library) {
      fprintf(stderr,
              "err:   DXMT failed to create null fragment library: %s\n",
              [[err localizedDescription] UTF8String]);
      [key release];
      return nil;
    }

    function = [library newFunctionWithName:@"dxmt_null_fragment"];
    if (!function) {
      fprintf(stderr,
              "err:   DXMT failed to create null fragment function\n");
      [library release];
      [key release];
      return nil;
    }

    [functions_by_device setObject:function forKey:key];
    [function release];
    [library release];
    function = [functions_by_device objectForKey:key];
    [key release];
    return function;
  }
}

static NTSTATUS
_MTLDevice_newMeshRenderPipelineState(void *obj) {
  struct unixcall_mtldevice_newmeshrenderpso *params = obj;
  const struct WMTMeshRenderPipelineInfo *info = params->info.ptr;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  MTLMeshRenderPipelineDescriptor *descriptor = [[MTLMeshRenderPipelineDescriptor alloc] init];

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->colors[i].pixel_format);
    descriptor.colorAttachments[i].blendingEnabled = info->colors[i].blending_enabled;
    descriptor.colorAttachments[i].writeMask = (MTLColorWriteMask)info->colors[i].write_mask;

    descriptor.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)info->colors[i].alpha_blend_operation;
    descriptor.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)info->colors[i].rgb_blend_operation;

    descriptor.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)info->colors[i].src_rgb_blend_factor;
    descriptor.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)info->colors[i].src_alpha_blend_factor;
    descriptor.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)info->colors[i].dst_rgb_blend_factor;
    descriptor.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)info->colors[i].dst_alpha_blend_factor;
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_fragment_buffers & (1 << i))
      descriptor.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_mesh_buffers & (1 << i))
      descriptor.meshBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_object_buffers & (1 << i))
      descriptor.objectBuffers[i].mutability = MTLMutabilityImmutable;
  }

#ifndef DXMT_NO_PRIVATE_API
  [descriptor setLogicOperationEnabled:info->logic_operation_enabled];
  [descriptor setLogicOperation:(MTLLogicOperation)info->logic_operation];
#endif
  descriptor.depthAttachmentPixelFormat = to_metal_pixel_format(info->depth_pixel_format);
  descriptor.stencilAttachmentPixelFormat = to_metal_pixel_format(info->stencil_pixel_format);
  descriptor.alphaToCoverageEnabled = info->alpha_to_coverage_enabled;
  descriptor.rasterizationEnabled = info->rasterization_enabled;
  descriptor.rasterSampleCount = info->raster_sample_count;

  descriptor.objectFunction = (id<MTLFunction>)info->object_function;
  descriptor.meshFunction = (id<MTLFunction>)info->mesh_function;
  id<MTLFunction> fragment_function = (id<MTLFunction>)info->fragment_function;
  if (info->rasterization_enabled && !fragment_function)
    fragment_function = DXMTGetNullFragmentFunction(device);
  descriptor.fragmentFunction = fragment_function;
  if (dxmt_metal4_pso_labels_enabled() && info->debug_label[0])
    descriptor.label = [NSString stringWithUTF8String:info->debug_label];
  descriptor.payloadMemoryLength = info->payload_memory_length;

  if (info->rasterization_enabled && !info->fragment_function) {
    fprintf(stderr,
            "warn:  DXMT diagnostic: Metal mesh render descriptor has rasterization enabled but no D3D pixel shader; "
            "using_null_fragment=%u; "
            "object=0x%llx mesh=0x%llx depth=%u stencil=%u samples=%u payload=%u rt=[%u,%u,%u,%u,%u,%u,%u,%u]\n",
            fragment_function ? 1 : 0,
            (unsigned long long)info->object_function,
            (unsigned long long)info->mesh_function,
            info->depth_pixel_format,
            info->stencil_pixel_format,
            info->raster_sample_count,
            info->payload_memory_length,
            info->colors[0].pixel_format,
            info->colors[1].pixel_format,
            info->colors[2].pixel_format,
            info->colors[3].pixel_format,
            info->colors[4].pixel_format,
            info->colors[5].pixel_format,
            info->colors[6].pixel_format,
            info->colors[7].pixel_format);
  }

  descriptor.meshThreadgroupSizeIsMultipleOfThreadExecutionWidth = info->mesh_tgsize_is_multiple_of_sgwidth;
  descriptor.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth = info->object_tgsize_is_multiple_of_sgwidth;

  MTLPipelineOption options = MTLPipelineOptionNone;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
      descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                      count:info->num_binary_archives_for_lookup];
    options = info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  }
#endif
  NSError *err = NULL;
  uint64_t compile_wait_begin = dxmt_monotonic_us();
  params->ret_pso = (obj_handle_t)[device newRenderPipelineStateWithMeshDescriptor:descriptor
                                                                           options:options
                                                                        reflection:nil
                                                                             error:&err];
  uint64_t compile_wait_end = dxmt_monotonic_us();
  params->ret_compile_wait_us =
      compile_wait_end >= compile_wait_begin ? compile_wait_end - compile_wait_begin : 0;
  params->ret_error = (obj_handle_t)err;
  const BOOL pso_created = params->ret_pso != 0;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (pso_created && info->binary_archive_for_serialization) {
      [(id<MTLBinaryArchive>)info->binary_archive_for_serialization
          addMeshRenderPipelineFunctionsWithDescriptor:descriptor
                                                 error:&err];
    }
  }
#endif
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && pso_created) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *descriptor_json = dxmt_apitrace_mesh_render_pipeline_json(info);
      apitrace_metal_register_mesh_render_pipeline(
          dxmt_apitrace_session,
          params->ret_pso,
          descriptor_json.UTF8String,
          info->object_function,
          info->mesh_function,
          info->fragment_function);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBlitCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  id<MTL4ComputeCommandEncoder> encoder = (id<MTL4ComputeCommandEncoder>)params->encoder;
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    DXMT4ApitraceEncoderState *state = [dxmt_apitrace_encoders objectForKey:dxmt_apitrace_key(params->encoder)];
    if (dxmt_apitrace_session && state) {
      dxmt_apitrace_record_blit_commands(dxmt_apitrace_session, params->encoder, next);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  while (next) {
    switch ((enum WMTBlitCommandType)next->type) {
    default:
      assert(!next->type && "unhandled blit command type");
      break;
    case WMTBlitCommandCopyFromBufferToBuffer: {
      struct wmtcmd_blit_copy_from_buffer_to_buffer *body = (struct wmtcmd_blit_copy_from_buffer_to_buffer *)next;
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
                     toBuffer:(id<MTLBuffer>)body->dst
            destinationOffset:body->dst_offset
                         size:body->copy_length];
      break;
    }
    case WMTBlitCommandCopyFromBufferToTexture: {
      struct wmtcmd_blit_copy_from_buffer_to_texture *body = (struct wmtcmd_blit_copy_from_buffer_to_texture *)next;
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
            sourceBytesPerRow:body->bytes_per_row
          sourceBytesPerImage:body->bytes_per_image
                   sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                     toTexture:(id<MTLTexture>)body->dst
              destinationSlice:body->slice
              destinationLevel:body->level
             destinationOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)];
      break;
    }
    case WMTBlitCommandCopyFromTextureToBuffer: {
      struct wmtcmd_blit_copy_from_texture_to_buffer *body = (struct wmtcmd_blit_copy_from_texture_to_buffer *)next;
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                       sourceSlice:body->slice
                       sourceLevel:body->level
                      sourceOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
                        sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                          toBuffer:(id<MTLBuffer>)body->dst
                 destinationOffset:body->offset
            destinationBytesPerRow:body->bytes_per_row
          destinationBytesPerImage:body->bytes_per_image];
      break;
    }
    case WMTBlitCommandCopyFromTextureToTexture: {
      struct wmtcmd_blit_copy_from_texture_to_texture *body = (struct wmtcmd_blit_copy_from_texture_to_texture *)next;
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                   sourceSlice:body->src_slice
                   sourceLevel:body->src_level
                  sourceOrigin:MTLOriginMake(body->src_origin.x, body->src_origin.y, body->src_origin.z)
                    sourceSize:MTLSizeMake(body->src_size.width, body->src_size.height, body->src_size.depth)
                     toTexture:(id<MTLTexture>)body->dst
              destinationSlice:body->dst_slice
              destinationLevel:body->dst_level
             destinationOrigin:MTLOriginMake(body->dst_origin.x, body->dst_origin.y, body->dst_origin.z)];
      break;
    }
    case WMTBlitCommandGenerateMipmaps: {
      struct wmtcmd_blit_generate_mipmaps *body = (struct wmtcmd_blit_generate_mipmaps *)next;
      [encoder generateMipmapsForTexture:(id<MTLTexture>)body->texture];
      break;
    }
    case WMTBlitCommandUpdateFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [(id)encoder updateFence:(id<MTLFence>)body->fence afterEncoderStages:MTLStageBlit];
      break;
    }
    case WMTBlitCommandWaitForFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [(id)encoder waitForFence:(id<MTLFence>)body->fence beforeEncoderStages:MTLStageBlit];
      break;
    }
    case WMTBlitCommandFillBuffer: {
      struct wmtcmd_blit_fillbuffer *body = (struct wmtcmd_blit_fillbuffer *)next;
      [encoder fillBuffer:(id<MTLBuffer>)body->buffer range:NSMakeRange(body->offset, body->length) value:body->value];
      break;
    }
    case WMTBlitCommandResolveCounters: {
      assert(!"MTLCounterSampleBuffer resolve is not supported by winemetal4 blit emulation");
      break;
    }
    case WMTBlitCommandResourceStateBarrier: {
      [(id)encoder barrierAfterQueueStages:MTLStageResourceState
                              beforeStages:(MTLStageVertex | MTLStageFragment |
                                            MTLStageTile | MTLStageObject |
                                            MTLStageMesh | MTLStageDispatch |
                                            MTLStageBlit)
                         visibilityOptions:MTL4VisibilityOptionDevice];
      break;
    }
    }

    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLComputeCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  id<MTL4ComputeCommandEncoder> encoder = (id<MTL4ComputeCommandEncoder>)params->encoder;
  DXMTMetal4CommandBuffer *owner = dxmt_metal4_encoder_owner(params->encoder);
  DXMTMetal4ComputeEncoderState *state = dxmt_metal4_encoder_state(params->encoder);
  if (!state) {
    state = [[[DXMTMetal4ComputeEncoderState alloc] initWithOwner:owner] autorelease];
    dxmt_metal4_register_encoder_state(params->encoder, state);
  }
  struct dxmt_metal4_argument_state *args = &state->arguments;
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    DXMT4ApitraceEncoderState *state = [dxmt_apitrace_encoders objectForKey:dxmt_apitrace_key(params->encoder)];
    if (dxmt_apitrace_session && state) {
      dxmt_apitrace_record_compute_commands(dxmt_apitrace_session, params->encoder, next);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  while (next) {
    switch ((enum WMTComputeCommandType)next->type) {
    default:
      assert(!next->type && "unhandled compute command type");
      break;
    case WMTComputeCommandDispatch: {
      struct wmtcmd_compute_dispatch *body = (struct wmtcmd_compute_dispatch *)next;
      dxmt_metal4_compute_set_argument_table_if_needed(encoder, state);
      [encoder dispatchThreadgroups:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
              threadsPerThreadgroup:state->threadgroupSize];
      break;
    }
    case WMTComputeCommandDispatchThreads: {
      struct wmtcmd_compute_dispatch *body = (struct wmtcmd_compute_dispatch *)next;
      dxmt_metal4_compute_set_argument_table_if_needed(encoder, state);
      [encoder dispatchThreads:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
          threadsPerThreadgroup:state->threadgroupSize];
      break;
    }
    case WMTComputeCommandDispatchIndirect: {
      struct wmtcmd_compute_dispatch_indirect *body = (struct wmtcmd_compute_dispatch_indirect *)next;
      dxmt_metal4_compute_set_argument_table_if_needed(encoder, state);
      [encoder dispatchThreadgroupsWithIndirectBuffer:dxmt_metal4_buffer_address(body->indirect_args_buffer, body->indirect_args_offset)
                                threadsPerThreadgroup:state->threadgroupSize];
      break;
    }
    case WMTComputeCommandSetPSO: {
      struct wmtcmd_compute_setpso *body = (struct wmtcmd_compute_setpso *)next;
      [encoder setComputePipelineState:(id<MTLComputePipelineState>)body->pso];
      state->threadgroupSize = MTLSizeMake(
          body->threadgroup_size.width,
          body->threadgroup_size.height,
          body->threadgroup_size.depth);
      break;
    }
    case WMTComputeCommandSetBuffer: {
      struct wmtcmd_compute_setbuffer *body = (struct wmtcmd_compute_setbuffer *)next;
      dxmt_metal4_argument_set_buffer(args, (id<MTLBuffer>)body->buffer, body->offset, body->index);
      break;
    }
    case WMTComputeCommandSetBufferOffset: {
      struct wmtcmd_compute_setbufferoffset *body = (struct wmtcmd_compute_setbufferoffset *)next;
      dxmt_metal4_argument_set_buffer_offset(args, body->offset, body->index);
      break;
    }
    case WMTComputeCommandSetArgumentBuffer: {
      struct wmtcmd_compute_setargumentbuffer *body = (struct wmtcmd_compute_setargumentbuffer *)next;
      dxmt_metal4_argument_set_buffer(args, (id<MTLBuffer>)body->buffer, body->offset, body->index);
      break;
    }
    case WMTComputeCommandSetArgumentBufferOffset: {
      struct wmtcmd_compute_setargumentbufferoffset *body =
          (struct wmtcmd_compute_setargumentbufferoffset *)next;
      dxmt_metal4_argument_set_buffer_offset(args, body->offset, body->index);
      break;
    }
    case WMTComputeCommandSetArgumentTable: {
      struct wmtcmd_compute_setargumenttable *body =
          (struct wmtcmd_compute_setargumenttable *)next;
      [encoder setArgumentTable:(id<MTL4ArgumentTable>)body->table];
      state->argumentTableBound = true;
      break;
    }
    case WMTComputeCommandUseResource: {
      struct wmtcmd_compute_useresource *body = (struct wmtcmd_compute_useresource *)next;
      (void)body->usage;
      break;
    }
    case WMTComputeCommandSetBytes: {
      struct wmtcmd_compute_setbytes *body = (struct wmtcmd_compute_setbytes *)next;
      dxmt_metal4_argument_set_bytes(args, body->bytes.ptr, body->length, body->index);
      break;
    }
    case WMTComputeCommandSetTexture: {
      struct wmtcmd_compute_settexture *body = (struct wmtcmd_compute_settexture *)next;
      dxmt_metal4_argument_set_texture(args, (id<MTLTexture>)body->texture, body->index);
      break;
    }
    case WMTComputeCommandUpdateFence: {
      struct wmtcmd_compute_fence_op *body = (struct wmtcmd_compute_fence_op *)next;
      [(id)encoder updateFence:(id<MTLFence>)body->fence afterEncoderStages:MTLStageDispatch];
      break;
    }
    case WMTComputeCommandWaitForFence: {
      struct wmtcmd_compute_fence_op *body = (struct wmtcmd_compute_fence_op *)next;
      [(id)encoder waitForFence:(id<MTLFence>)body->fence beforeEncoderStages:MTLStageDispatch];
      break;
    }
    case WMTComputeCommandMemoryBarrier: {
      struct wmtcmd_compute_memory_barrier *body = (struct wmtcmd_compute_memory_barrier *)next;
      (void)body->scope;
      [(id)encoder barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
      break;
    }
    }

    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLRenderCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  id<MTL4RenderCommandEncoder> encoder = (id<MTL4RenderCommandEncoder>)params->encoder;
  DXMTMetal4CommandBuffer *owner = dxmt_metal4_encoder_owner(params->encoder);
  DXMTMetal4RenderEncoderState *state = dxmt_metal4_encoder_state(params->encoder);
  if (!state) {
    state = [[[DXMTMetal4RenderEncoderState alloc] initWithOwner:owner] autorelease];
    dxmt_metal4_register_encoder_state(params->encoder, state);
  }
  struct dxmt_metal4_render_argument_state *args = &state->arguments;
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    DXMT4ApitraceEncoderState *state = [dxmt_apitrace_encoders objectForKey:dxmt_apitrace_key(params->encoder)];
    if (dxmt_apitrace_session && state) {
      dxmt_apitrace_record_render_commands(dxmt_apitrace_session, params->encoder, next);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  while (next) {
    switch ((enum WMTRenderCommandType)next->type) {
    default:
      assert(!next->type && "unhandled render command type");
      break;
    case WMTRenderCommandNop:
      break;
    case WMTRenderCommandUseResource: {
      struct wmtcmd_render_useresource *body = (struct wmtcmd_render_useresource *)next;
      (void)body->usage;
      (void)body->stages;
      break;
    }
    case WMTRenderCommandSetVertexBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      dxmt_metal4_argument_set_buffer(&args->vertex, (id<MTLBuffer>)body->buffer, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetVertexBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      dxmt_metal4_argument_set_buffer_offset(&args->vertex, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetFragmentBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      dxmt_metal4_argument_set_buffer(&args->fragment, (id<MTLBuffer>)body->buffer, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetFragmentBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      dxmt_metal4_argument_set_buffer_offset(&args->fragment, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetArgumentBuffer: {
      struct wmtcmd_render_setargumentbuffer *body = (struct wmtcmd_render_setargumentbuffer *)next;
      dxmt_metal4_render_argument_set_buffer(
          args, (id<MTLBuffer>)body->buffer, body->offset, body->index, body->stages);
      break;
    }
    case WMTRenderCommandSetArgumentBufferOffset: {
      struct wmtcmd_render_setargumentbufferoffset *body =
          (struct wmtcmd_render_setargumentbufferoffset *)next;
      dxmt_metal4_render_argument_set_buffer_offset(
          args, body->offset, body->index, body->stages);
      break;
    }
    case WMTRenderCommandSetArgumentTable: {
      struct wmtcmd_render_setargumenttable *body =
          (struct wmtcmd_render_setargumenttable *)next;
      [encoder setArgumentTable:(id<MTL4ArgumentTable>)body->table
                       atStages:dxmt_metal4_render_argument_stages(body->stages)];
      if (body->stages & WMTRenderStageVertex)
        args->vertex_table_bound = true;
      if (body->stages & WMTRenderStageFragment)
        args->fragment_table_bound = true;
      if (body->stages & WMTRenderStageObject)
        args->object_table_bound = true;
      if (body->stages & WMTRenderStageMesh)
        args->mesh_table_bound = true;
      if (body->stages & WMTRenderStageTile)
        args->tile_table_bound = true;
      break;
    }
    case WMTRenderCommandSetMeshBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      dxmt_metal4_argument_set_buffer(&args->mesh, (id<MTLBuffer>)body->buffer, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetMeshBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      dxmt_metal4_argument_set_buffer_offset(&args->mesh, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetObjectBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      dxmt_metal4_argument_set_buffer(&args->object, (id<MTLBuffer>)body->buffer, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetObjectBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      dxmt_metal4_argument_set_buffer_offset(&args->object, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetFragmentBytes: {
      struct wmtcmd_render_setbytes *body = (struct wmtcmd_render_setbytes *)next;
      dxmt_metal4_argument_set_bytes(&args->fragment, body->bytes.ptr, body->length, body->index);
      break;
    }
    case WMTRenderCommandSetFragmentTexture: {
      struct wmtcmd_render_settexture *body = (struct wmtcmd_render_settexture *)next;
      dxmt_metal4_argument_set_texture(&args->fragment, (id<MTLTexture>)body->texture, body->index);
      break;
    }
    case WMTRenderCommandSetRasterizerState: {
      struct wmtcmd_render_setrasterizerstate *body = (struct wmtcmd_render_setrasterizerstate *)next;
      [encoder setTriangleFillMode:(MTLTriangleFillMode)body->fill_mode];
      [encoder setCullMode:(MTLCullMode)body->cull_mode];
      [encoder setDepthClipMode:(MTLDepthClipMode)body->depth_clip_mode];
      [encoder setDepthBias:body->depth_bias slopeScale:body->scole_scale clamp:body->depth_bias_clamp];
      [encoder setFrontFacingWinding:(MTLWinding)body->winding];
      break;
    }
    case WMTRenderCommandSetViewports: {
      struct wmtcmd_render_setviewports *body = (struct wmtcmd_render_setviewports *)next;
      [encoder setViewports:(const MTLViewport *)body->viewports.ptr count:body->viewport_count];
      break;
    }
    case WMTRenderCommandSetScissorRects: {
      struct wmtcmd_render_setscissorrects *body = (struct wmtcmd_render_setscissorrects *)next;
      [encoder setScissorRects:(const MTLScissorRect *)body->scissor_rects.ptr count:body->rect_count];
      break;
    }
    case WMTRenderCommandSetPSO: {
      struct wmtcmd_render_setpso *body = (struct wmtcmd_render_setpso *)next;
      [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)body->pso];
      break;
    }
    case WMTRenderCommandSetDSSO: {
      struct wmtcmd_render_setdsso *body = (struct wmtcmd_render_setdsso *)next;
      [encoder setDepthStencilState:(id<MTLDepthStencilState>)body->dsso];
      [encoder setStencilReferenceValue:body->stencil_ref];
      break;
    }
    case WMTRenderCommandSetBlendFactorAndStencilRef: {
      struct wmtcmd_render_setblendcolor *body = (struct wmtcmd_render_setblendcolor *)next;
      [encoder setBlendColorRed:body->red green:body->green blue:body->blue alpha:body->alpha];
      [encoder setStencilReferenceValue:body->stencil_ref];
      break;
    }
    case WMTRenderCommandSetVisibilityMode: {
      struct wmtcmd_render_setvisibilitymode *body = (struct wmtcmd_render_setvisibilitymode *)next;
      [encoder setVisibilityResultMode:(MTLVisibilityResultMode)body->mode offset:body->offset];
      break;
    }
    case WMTRenderCommandDraw: {
      struct wmtcmd_render_draw *body = (struct wmtcmd_render_draw *)next;
      dxmt_metal4_render_set_argument_tables(encoder, args);
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
                  vertexStart:body->vertex_start
                  vertexCount:body->vertex_count
                instanceCount:body->instance_count
                 baseInstance:body->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndexed: {
      struct wmtcmd_render_draw_indexed *body = (struct wmtcmd_render_draw_indexed *)next;
      id<MTLBuffer> index_buffer = (id<MTLBuffer>)body->index_buffer;
      dxmt_metal4_render_set_argument_tables(encoder, args);
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                          indexCount:body->index_count
                           indexType:(MTLIndexType)body->index_type
                         indexBuffer:dxmt_metal4_buffer_address(body->index_buffer, body->index_buffer_offset)
                   indexBufferLength:dxmt_metal4_index_buffer_length(index_buffer, body->index_buffer_offset)
                       instanceCount:body->instance_count
                          baseVertex:body->base_vertex
                        baseInstance:body->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndirect: {
      struct wmtcmd_render_draw_indirect *body = (struct wmtcmd_render_draw_indirect *)next;
      dxmt_metal4_render_set_argument_tables(encoder, args);
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
                indirectBuffer:dxmt_metal4_buffer_address(body->indirect_args_buffer, body->indirect_args_offset)];
      break;
    }
    case WMTRenderCommandDrawIndexedIndirect: {
      struct wmtcmd_render_draw_indexed_indirect *body = (struct wmtcmd_render_draw_indexed_indirect *)next;
      id<MTLBuffer> index_buffer = (id<MTLBuffer>)body->index_buffer;
      dxmt_metal4_render_set_argument_tables(encoder, args);
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                           indexType:(MTLIndexType)body->index_type
                         indexBuffer:dxmt_metal4_buffer_address(body->index_buffer, body->index_buffer_offset)
                   indexBufferLength:dxmt_metal4_index_buffer_length(index_buffer, body->index_buffer_offset)
                      indirectBuffer:dxmt_metal4_buffer_address(body->indirect_args_buffer, body->indirect_args_offset)];
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroups: {
      struct wmtcmd_render_draw_meshthreadgroups *body = (struct wmtcmd_render_draw_meshthreadgroups *)next;
      dxmt_metal4_render_set_argument_tables(encoder, args);
      [encoder drawMeshThreadgroups:MTLSizeMake(
                                        body->threadgroup_per_grid.width, body->threadgroup_per_grid.height,
                                        body->threadgroup_per_grid.depth
                                    )
          threadsPerObjectThreadgroup:MTLSizeMake(
                                          body->object_threadgroup_size.width, body->object_threadgroup_size.height,
                                          body->object_threadgroup_size.depth
                                      )
            threadsPerMeshThreadgroup:MTLSizeMake(
                                          body->mesh_threadgroup_size.width, body->mesh_threadgroup_size.height,
                                          body->mesh_threadgroup_size.depth
                                      )];
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroupsIndirect: {
      struct wmtcmd_render_draw_meshthreadgroups_indirect *body =
          (struct wmtcmd_render_draw_meshthreadgroups_indirect *)next;
      dxmt_metal4_render_set_argument_tables(encoder, args);
      [encoder drawMeshThreadgroupsWithIndirectBuffer:dxmt_metal4_buffer_address(body->indirect_args_buffer, body->indirect_args_offset)
                          threadsPerObjectThreadgroup:MTLSizeMake(
                                                          body->object_threadgroup_size.width,
                                                          body->object_threadgroup_size.height,
                                                          body->object_threadgroup_size.depth
                                                      )
                            threadsPerMeshThreadgroup:MTLSizeMake(
                                                          body->mesh_threadgroup_size.width,
                                                          body->mesh_threadgroup_size.height,
                                                          body->mesh_threadgroup_size.depth
                                                      )];
      break;
    }
    case WMTRenderCommandMemoryBarrier: {
      struct wmtcmd_render_memory_barrier *body = (struct wmtcmd_render_memory_barrier *)next;
      MTLStages before = dxmt_metal4_render_stages(body->stages_before);
      MTLStages after = dxmt_metal4_render_stages(body->stages_after);
      MTL4VisibilityOptions visibility = body->scope ? MTL4VisibilityOptionDevice : MTL4VisibilityOptionNone;
      [(id)encoder barrierAfterEncoderStages:after beforeEncoderStages:before visibilityOptions:visibility];
      break;
    }
    case WMTRenderCommandDXMTGeometryDraw: {
      struct wmtcmd_render_dxmt_geometry_draw *body = (struct wmtcmd_render_dxmt_geometry_draw *)next;
      dxmt_metal4_argument_set_buffer_offset(&args->object, body->draw_arguments_offset, 21);
      dxmt_metal4_render_set_argument_tables(encoder, args);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->warp_count, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndexed: {
      struct wmtcmd_render_dxmt_geometry_draw_indexed *body = (struct wmtcmd_render_dxmt_geometry_draw_indexed *)next;
      dxmt_metal4_argument_set_buffer(&args->object, (id<MTLBuffer>)body->index_buffer, body->index_buffer_offset, 20);
      dxmt_metal4_argument_set_buffer_offset(&args->object, body->draw_arguments_offset, 21);
      dxmt_metal4_render_set_argument_tables(encoder, args);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->warp_count, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndirect: {
      struct wmtcmd_render_dxmt_geometry_draw_indirect *body = (struct wmtcmd_render_dxmt_geometry_draw_indirect *)next;
      dxmt_metal4_argument_set_buffer(&args->object, (id<MTLBuffer>)body->indirect_args_buffer, body->indirect_args_offset, 21);
      dxmt_metal4_render_set_argument_tables(encoder, args);
      [encoder drawMeshThreadgroupsWithIndirectBuffer:dxmt_metal4_buffer_address(body->dispatch_args_buffer, body->dispatch_args_offset)
                          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      dxmt_metal4_argument_set_buffer(&args->object, (id<MTLBuffer>)body->imm_draw_arguments, 0, 21);
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndexedIndirect: {
      struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect *body =
          (struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect *)next;
      dxmt_metal4_argument_set_buffer(&args->object, (id<MTLBuffer>)body->index_buffer, body->index_buffer_offset, 20);
      dxmt_metal4_argument_set_buffer(&args->object, (id<MTLBuffer>)body->indirect_args_buffer, body->indirect_args_offset, 21);
      dxmt_metal4_render_set_argument_tables(encoder, args);
      [encoder drawMeshThreadgroupsWithIndirectBuffer:dxmt_metal4_buffer_address(body->dispatch_args_buffer, body->dispatch_args_offset)
                          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      dxmt_metal4_argument_set_buffer(&args->object, (id<MTLBuffer>)body->imm_draw_arguments, 0, 21);
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDraw: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw *body = (struct wmtcmd_render_dxmt_tessellation_mesh_draw *)next;
      dxmt_metal4_argument_set_buffer_offset(&args->object, body->draw_arguments_offset, 21);
      dxmt_metal4_render_set_argument_tables(encoder, args);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndexed: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed *body = (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed *)next;
      dxmt_metal4_argument_set_buffer(&args->object, (id<MTLBuffer>)body->index_buffer, body->index_buffer_offset, 20);
      dxmt_metal4_argument_set_buffer_offset(&args->object, body->draw_arguments_offset, 21);
      dxmt_metal4_render_set_argument_tables(encoder, args);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }

    case WMTRenderCommandDXMTTessellationMeshDrawIndirect: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect *body = (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect *)next;
      dxmt_metal4_argument_set_buffer(&args->object, (id<MTLBuffer>)body->indirect_args_buffer, body->indirect_args_offset, 21);
      dxmt_metal4_render_set_argument_tables(encoder, args);
      [encoder drawMeshThreadgroupsWithIndirectBuffer:dxmt_metal4_buffer_address(body->dispatch_args_buffer, body->dispatch_args_offset)
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      dxmt_metal4_argument_set_buffer(&args->object, (id<MTLBuffer>)body->imm_draw_arguments, 0, 21);
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect *body =
          (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect *)next;
      dxmt_metal4_argument_set_buffer(&args->object, (id<MTLBuffer>)body->index_buffer, body->index_buffer_offset, 20);
      dxmt_metal4_argument_set_buffer(&args->object, (id<MTLBuffer>)body->indirect_args_buffer, body->indirect_args_offset, 21);
      dxmt_metal4_render_set_argument_tables(encoder, args);
      [encoder drawMeshThreadgroupsWithIndirectBuffer:dxmt_metal4_buffer_address(body->dispatch_args_buffer, body->dispatch_args_offset)
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      dxmt_metal4_argument_set_buffer(&args->object, (id<MTLBuffer>)body->imm_draw_arguments, 0, 21);
      break;
    }
    case WMTRenderCommandUpdateFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      MTLStages stages = dxmt_metal4_render_stages(body->stages);
      [(id)encoder updateFence:(id<MTLFence>)body->fence afterEncoderStages:stages];
      break;
    }
    case WMTRenderCommandWaitForFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      MTLStages stages = dxmt_metal4_render_stages(body->stages);
      [(id)encoder waitForFence:(id<MTLFence>)body->fence beforeEncoderStages:stages];
      break;
    }
    case WMTRenderCommandSetViewport: {
      struct wmtcmd_render_setviewport *body = (struct wmtcmd_render_setviewport *)next;
      union {
        struct WMTViewport src;
        MTLViewport dst;
      } u = {.src = body->viewport};
      [encoder setViewport:u.dst];
      break;
    }
    case WMTRenderCommandSetScissorRect: {
      struct wmtcmd_render_setscissorrect *body = (struct wmtcmd_render_setscissorrect *)next;
      union {
        struct WMTScissorRect src;
        MTLScissorRect dst;
      } u = {.src = body->scissor_rect};
      [encoder setScissorRect:u.dst];
      break;
    }
    case WMTRenderCommandDispatchThreadsPerTile: {
      struct wmtcmd_render_dispatch_threads_per_tile *body = (struct wmtcmd_render_dispatch_threads_per_tile *)next;
      [encoder dispatchThreadsPerTile:MTLSizeMake(body->width, body->height, 1)];
      break;
    }
    }
    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_pixelFormat(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle pixelFormat];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_usage(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle usage];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_width(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle width];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_height(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle height];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_depth(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle depth];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_arrayLength(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle arrayLength];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_mipmapLevelCount(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle mipmapLevelCount];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_replaceRegion(void *obj) {
  struct unixcall_mtltexture_replaceregion *params = obj;
  [(id<MTLTexture>)params->texture replaceRegion:MTLRegionMake3D(
                                                     params->origin.x, params->origin.y, params->origin.z,
                                                     params->size.width, params->size.height, params->size.depth
                                                 )
                                     mipmapLevel:params->level
                                           slice:params->slice
                                       withBytes:params->data.ptr
                                     bytesPerRow:params->bytes_per_row
                                   bytesPerImage:params->bytes_per_image];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && params->data.ptr) {
    const uint64_t bytes_size =
        dxmt_apitrace_texture_region_bytes_size(&params->size, params->bytes_per_row, params->bytes_per_image);
    if (bytes_size) {
      pthread_mutex_lock(&dxmt_apitrace_lock);
      if (dxmt_apitrace_ensure_session_locked()) {
        apitrace_metal_replace_texture_region(
            dxmt_apitrace_session,
            params->texture,
            params->origin.x,
            params->origin.y,
            params->origin.z,
            params->size.width,
            params->size.height,
            params->size.depth,
            (uint32_t)params->level,
            (uint32_t)params->slice,
            params->bytes_per_row,
            params->bytes_per_image,
            params->data.ptr,
            bytes_size);
      }
      pthread_mutex_unlock(&dxmt_apitrace_lock);
    }
  }
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_didModifyRange(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  [(id<MTLBuffer>)params->handle didModifyRange:NSMakeRange(params->arg, params->ret)];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && [(id<MTLBuffer>)params->handle storageMode] != MTLStorageModePrivate) {
    const uint64_t offset = params->arg;
    const uint64_t length = params->ret;
    const uint64_t buffer_length = [(id<MTLBuffer>)params->handle length];
    const void *contents = [(id<MTLBuffer>)params->handle contents];
    if (contents && length && offset <= buffer_length && length <= buffer_length - offset) {
      pthread_mutex_lock(&dxmt_apitrace_lock);
      if (dxmt_apitrace_ensure_session_locked()) {
        apitrace_metal_update_buffer_contents(
            dxmt_apitrace_session,
            params->handle,
            offset,
            length,
            (uint32_t)[(id<MTLBuffer>)params->handle storageMode],
            (const uint8_t *)contents + offset,
            length);
      }
      pthread_mutex_unlock(&dxmt_apitrace_lock);
    }
  }
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_presentDrawable(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(DXMTMetal4CommandBuffer *)params->handle presentDrawable:(id<MTLDrawable>)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_presentDrawableAfterMinimumDuration(void *obj) {
  struct unixcall_generic_obj_obj_double_noret *params = obj;
  [(DXMTMetal4CommandBuffer *)params->handle presentDrawable:(id<MTLDrawable>)params->arg0
                                        afterMinimumDuration:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsFamily(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle supportsFamily:(MTLGPUFamily)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsBCTextureCompression(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle supportsBCTextureCompression];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsTextureSampleCount(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle supportsTextureSampleCount:params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_hasUnifiedMemory(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle hasUnifiedMemory];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_sharedCaptureManager(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = (obj_handle_t)[MTLCaptureManager sharedCaptureManager];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_startCapture(void *obj) {
  struct unixcall_mtlcapturemanager_startcapture *params = obj;
  MTLCaptureDescriptor *desc = [[MTLCaptureDescriptor alloc] init];
  const struct WMTCaptureInfo *info = params->info.ptr;
  desc.destination = (MTLCaptureDestination)info->destination;
  desc.captureObject = (id)info->capture_object;
  NSString *path_str = [[NSString alloc] initWithCString:info->output_url.ptr encoding:NSUTF8StringEncoding];
  NSURL *url = [[NSURL alloc] initFileURLWithPath:path_str];
  desc.outputURL = url;
  [(MTLCaptureManager *)params->capture_manager startCaptureWithDescriptor:desc error:nil];
  [url release];
  [path_str release];
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_stopCapture(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(MTLCaptureManager *)params->handle stopCapture];
  return STATUS_SUCCESS;
}

#include <signal.h>

void
temp_handler(int signum) {
  fprintf(stderr, "received signal %d in temp_handler(), and it may cause problem!\n", signum);
}

static const int SIGNALS[] = {
    SIGHUP,
    SIGINT,
    SIGTERM,
    SIGUSR2,
    SIGILL,
    SIGTRAP,
    SIGABRT,
    SIGFPE,
    SIGBUS,
    SIGSEGV,
    SIGQUIT
#ifdef SIGSYS
    ,
    SIGSYS
#endif
#ifdef SIGXCPU
    ,
    SIGXCPU
#endif
#ifdef SIGXFSZ
    ,
    SIGXFSZ
#endif
#ifdef SIGEMT
    ,
    SIGEMT
#endif
    ,
    SIGUSR1
#ifdef SIGINFO
    ,
    SIGINFO
#endif
};

static NTSTATUS
_MTLDevice_newTemporalScaler(void *obj) {
  struct unixcall_mtldevice_newfxtemporalscaler *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  id<MTL4Compiler> compiler = dxmt_metal4_compiler_for_device(device);
  if (!compiler) {
    fprintf(stderr, "err:   DXMT Metal4 scaler unavailable: missing MTL4Compiler\n");
    params->ret = 0;
    return STATUS_SUCCESS;
  }
  MTLFXTemporalScalerDescriptor *desc = [[MTLFXTemporalScalerDescriptor alloc] init];
  const struct WMTFXTemporalScalerInfo *info = params->info.ptr;
  desc.colorTextureFormat = to_metal_pixel_format(info->color_format);
  desc.outputTextureFormat = to_metal_pixel_format(info->output_format);
  desc.depthTextureFormat = to_metal_pixel_format(info->depth_format);
  desc.motionTextureFormat = to_metal_pixel_format(info->motion_format);
  desc.inputWidth = info->input_width;
  desc.inputHeight = info->input_height;
  desc.outputWidth = info->output_width;
  desc.outputHeight = info->output_height;
  desc.inputContentMaxScale = info->input_content_max_scale;
  desc.inputContentMinScale = info->input_content_min_scale;
  desc.inputContentPropertiesEnabled = info->input_content_properties_enabled;
  #if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    desc.requiresSynchronousInitialization = info->requires_synchronous_initialization;
  }
  #endif
  desc.autoExposureEnabled = info->auto_exposure;

  struct sigaction old_action[sizeof(SIGNALS) / sizeof(int)], new_action;
  if (@available(macOS 16, *)) {} else {
    new_action.sa_handler = temp_handler;
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;
    for (unsigned int i = 0; i < sizeof(SIGNALS) / sizeof(int); i++)
      sigaction(SIGNALS[i], &new_action, &old_action[i]);
  }

  params->ret = (obj_handle_t)[desc newTemporalScalerWithDevice:device compiler:compiler];
  if (!params->ret)
    fprintf(stderr, "err:   DXMT Metal4 scaler unavailable: temporal scaler creation failed\n");

  if (@available(macOS 16, *)) {} else {
    for (unsigned int i = 0; i < sizeof(SIGNALS) / sizeof(int); i++)
      sigaction(SIGNALS[i], &old_action[i], NULL);
  }

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSpatialScaler(void *obj) {
  struct unixcall_mtldevice_newfxspatialscaler *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  id<MTL4Compiler> compiler = dxmt_metal4_compiler_for_device(device);
  if (!compiler) {
    fprintf(stderr, "err:   DXMT Metal4 scaler unavailable: missing MTL4Compiler\n");
    params->ret = 0;
    return STATUS_SUCCESS;
  }
  MTLFXSpatialScalerDescriptor *desc = [[MTLFXSpatialScalerDescriptor alloc] init];
  const struct WMTFXSpatialScalerInfo *info = params->info.ptr;
  desc.colorTextureFormat = to_metal_pixel_format(info->color_format);
  desc.outputTextureFormat = to_metal_pixel_format(info->output_format);
  desc.inputWidth = info->input_width;
  desc.inputHeight = info->input_height;
  desc.outputWidth = info->output_width;
  desc.outputHeight = info->output_height;
  params->ret = (obj_handle_t)[desc newSpatialScalerWithDevice:device compiler:compiler];
  if (!params->ret)
    fprintf(stderr, "err:   DXMT Metal4 scaler unavailable: spatial scaler creation failed\n");
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeTemporalScale(void *obj) {
  struct unixcall_mtlcommandbuffer_temporal_scale *params = obj;
  DXMTMetal4CommandBuffer *wrapper = (DXMTMetal4CommandBuffer *)params->cmdbuf;
  id<MTL4FXTemporalScaler> scaler = (id<MTL4FXTemporalScaler>)params->scaler;
  scaler.colorTexture = (id<MTLTexture>)params->color;
  scaler.outputTexture = (id<MTLTexture>)params->output;
  scaler.depthTexture = (id<MTLTexture>)params->depth;
  scaler.motionTexture = (id<MTLTexture>)params->motion;
  scaler.exposureTexture = (id<MTLTexture>)params->exposure;
  scaler.fence = (id<MTLFence>)params->fence;
  const struct WMTFXTemporalScalerProps *props = params->props.ptr;
  scaler.inputContentWidth = props->input_content_width;
  scaler.inputContentHeight = props->input_content_height;
  scaler.reset = props->reset;
  scaler.depthReversed = props->depth_reversed;
  scaler.motionVectorScaleX = props->motion_vector_scale_x;
  scaler.motionVectorScaleY = props->motion_vector_scale_y;
  scaler.jitterOffsetX = props->jitter_offset_x;
  scaler.jitterOffsetY = props->jitter_offset_y;
  scaler.preExposure = props->pre_exposure;
  [scaler encodeToCommandBuffer:[wrapper commandBuffer]];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeSpatialScale(void *obj) {
  struct unixcall_mtlcommandbuffer_spatial_scale *params = obj;
  DXMTMetal4CommandBuffer *wrapper = (DXMTMetal4CommandBuffer *)params->cmdbuf;
  id<MTL4FXSpatialScaler> scaler = (id<MTL4FXSpatialScaler>)params->scaler;
  scaler.colorTexture = (id<MTLTexture>)params->color;
  scaler.outputTexture = (id<MTLTexture>)params->output;
  scaler.fence = (id<MTLFence>)params->fence;
  [scaler encodeToCommandBuffer:[wrapper commandBuffer]];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_string(void *obj) {
  struct unixcall_nsstring_string *params = obj;
  NSString *str = [NSString stringWithCString:params->buffer_ptr.ptr encoding:(NSStringEncoding)params->encoding];
  params->ret = (obj_handle_t)str;
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_alloc_init(void *obj) {
  struct unixcall_nsstring_string *params = obj;
  NSString *str = [[NSString alloc] initWithCString:params->buffer_ptr.ptr encoding:(NSStringEncoding)params->encoding];
  params->ret = (obj_handle_t)str;
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_instance(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret =
      (obj_handle_t)((id(*)(id, SEL))objc_msgSend)(objc_lookUpClass("_CADeveloperHUDProperties"), @selector(instance));
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_addLabel(void *obj) {
  struct unixcall_generic_obj_obj_obj_uint64_ret *params = obj;
  params->ret = ((bool (*)(id, SEL, id, id)
  )objc_msgSend)((id)params->handle, @selector(addLabel:after:), (id)params->arg0, (id)params->arg1);
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_updateLabel(void *obj) {
  struct unixcall_generic_obj_obj_obj_noret *params = obj;
  ((void (*)(id, SEL, id, id)
  )objc_msgSend)((id)params->handle, @selector(updateLabel:value:), (id)params->arg0, (id)params->arg1);
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_remove(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  ((void (*)(id, SEL, id))objc_msgSend)((id)params->handle, @selector(remove:), (id)params->arg);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalDrawable_texture(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  id<MTLTexture> texture = [(id<CAMetalDrawable>)params->handle texture];
  params->ret = (obj_handle_t)texture;
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && texture) {
    struct WMTTextureInfo info;
    extract_texture_descriptor(texture, &info);
    info.gpu_resource_id = [texture gpuResourceID]._impl;
    info.mach_port = 0;
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *resource_payload = dxmt_apitrace_json_string(@{
        @"kind" : @"dxmt_texture_gpu_resource_id",
        @"texture_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->ret],
        @"gpu_resource_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)info.gpu_resource_id],
      });
      apitrace_metal_object_metadata(dxmt_apitrace_session, params->ret, resource_payload.UTF8String);
      NSString *descriptor_json = dxmt_apitrace_texture_descriptor_json(&info);
      apitrace_metal_register_drawable_texture(
          dxmt_apitrace_session,
          params->ret,
          params->handle,
          descriptor_json.UTF8String);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_nextDrawable(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(CAMetalLayer *)params->handle nextDrawable];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsFXSpatialScaler(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [MTLFXSpatialScalerDescriptor supportsDevice:(id<MTLDevice>)params->handle];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsFXTemporalScaler(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [MTLFXTemporalScalerDescriptor supportsDevice:(id<MTLDevice>)params->handle];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_setProps(void *obj) {
  struct unixcall_generic_obj_constptr_noret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  const struct WMTLayerProps *props = params->arg.ptr;
  execute_on_main(^{
    layer.device = (id<MTLDevice>)props->device;
    layer.opaque = props->opaque;
    dxmt_set_layer_background_black(layer);
    layer.framebufferOnly = props->framebuffer_only;
    layer.contentsScale = props->contents_scale;
    layer.displaySyncEnabled = props->display_sync_enabled;
    layer.drawableSize = CGSizeMake(props->drawable_width, props->drawable_height);
    layer.pixelFormat = to_metal_pixel_format(props->pixel_format);
  });
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_getProps(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  struct WMTLayerProps *props = params->arg.ptr;
  props->device = (obj_handle_t)layer.device;
  props->opaque = layer.opaque;
  props->framebuffer_only = layer.framebufferOnly;
  props->contents_scale = layer.contentsScale;
  props->display_sync_enabled = layer.displaySyncEnabled;
  props->drawable_height = layer.drawableSize.height;
  props->drawable_width = layer.drawableSize.width;
  props->pixel_format = layer.pixelFormat;
  return STATUS_SUCCESS;
}

typedef struct macdrv_opaque_metal_device *macdrv_metal_device;
typedef struct macdrv_opaque_metal_view *macdrv_metal_view;
typedef struct macdrv_opaque_metal_layer *macdrv_metal_layer;
typedef struct macdrv_opaque_view *macdrv_view;
typedef struct macdrv_opaque_window *macdrv_window;
typedef struct macdrv_opaque_window_data *macdrv_window_data;
typedef struct opaque_window_surface *window_surface;
typedef struct opaque_HWND *HWND;
struct macdrv_win_data {
  HWND hwnd; /* hwnd that this private data belongs to */
  macdrv_window cocoa_window;
  macdrv_view cocoa_view;
  macdrv_view client_cocoa_view;
};

struct macdrv_functions_t {
  void (*macdrv_init_display_devices)(BOOL);
  struct macdrv_win_data *(*get_win_data)(HWND hwnd);
  void (*release_win_data)(struct macdrv_win_data *data);
  macdrv_window (*macdrv_get_cocoa_window)(HWND hwnd, BOOL require_on_screen);
  macdrv_metal_device (*macdrv_create_metal_device)(void);
  void (*macdrv_release_metal_device)(macdrv_metal_device d);
  macdrv_metal_view (*macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d);
  macdrv_metal_layer (*macdrv_view_get_metal_layer)(macdrv_metal_view v);
  void (*macdrv_view_release_metal_view)(macdrv_metal_view v);
  void (*on_main_thread)(dispatch_block_t b);
};

static NTSTATUS
_CreateMetalViewFromHWND(void *obj) {
  struct unixcall_create_metal_view_from_hwnd *params = obj;

  struct macdrv_win_data *(*pfn_get_win_data)(HWND hwnd) = NULL;
  void (*pfn_release_win_data)(struct macdrv_win_data *data) = NULL;
  macdrv_metal_view (*pfn_macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d) = NULL;
  macdrv_metal_layer (*pfn_macdrv_view_get_metal_layer)(macdrv_metal_view v) = NULL;

  struct macdrv_functions_t *macdrv_functions;
  if ((macdrv_functions = dlsym(RTLD_DEFAULT, "macdrv_functions"))) {
    pfn_get_win_data = macdrv_functions->get_win_data;
    pfn_release_win_data = macdrv_functions->release_win_data;
    pfn_macdrv_view_create_metal_view = macdrv_functions->macdrv_view_create_metal_view;
    pfn_macdrv_view_get_metal_layer = macdrv_functions->macdrv_view_get_metal_layer;
  } else {
    pfn_get_win_data = dlsym(RTLD_DEFAULT, "get_win_data");
    pfn_release_win_data = dlsym(RTLD_DEFAULT, "release_win_data");
    pfn_macdrv_view_create_metal_view = dlsym(RTLD_DEFAULT, "macdrv_view_create_metal_view");
    pfn_macdrv_view_get_metal_layer = dlsym(RTLD_DEFAULT, "macdrv_view_get_metal_layer");
  }

  if (pfn_get_win_data && pfn_release_win_data && pfn_macdrv_view_create_metal_view &&
      pfn_macdrv_view_get_metal_layer) {
    struct macdrv_win_data *win_data = pfn_get_win_data((HWND)params->hwnd);
    macdrv_metal_view view =
        pfn_macdrv_view_create_metal_view(win_data->client_cocoa_view, (macdrv_metal_device)params->device);
    params->ret_view = (obj_handle_t)view;
    if (view) {
      params->ret_layer = (obj_handle_t)pfn_macdrv_view_get_metal_layer(view);
      execute_on_main(^{
        dxmt_set_layer_background_black((CAMetalLayer *)params->ret_layer);
      });
    }
    pfn_release_win_data(win_data);
  }

  return STATUS_SUCCESS;
}

static NTSTATUS
_ReleaseMetalView(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;

  void (*pfn_macdrv_view_release_metal_view)(macdrv_metal_view v) = NULL;

  struct macdrv_functions_t *macdrv_functions;
  if ((macdrv_functions = dlsym(RTLD_DEFAULT, "macdrv_functions"))) {
    pfn_macdrv_view_release_metal_view = macdrv_functions->macdrv_view_release_metal_view;
  } else {
    pfn_macdrv_view_release_metal_view = dlsym(RTLD_DEFAULT, "macdrv_view_release_metal_view");
  }

  if (pfn_macdrv_view_release_metal_view)
    pfn_macdrv_view_release_metal_view((macdrv_metal_view)params->handle);

  return STATUS_SUCCESS;
}

static void
dxmt_airconv_store_compile_outputs(sm50_bitcode_t *bitcode_slot,
                                   sm50_bitcode_t bitcode,
                                   sm50_error_t *error_slot,
                                   sm50_error_t error) {
  if (bitcode_slot)
    *bitcode_slot = bitcode;
  if (error_slot)
    *error_slot = error;
}

static NTSTATUS
thunk_SM50Initialize(void *args) {
  struct sm50_initialize_params *params = args;

  params->ret = SM50Initialize(params->bytecode, params->bytecode_size,
                               params->shader, params->reflection,
                               params->error);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50Destroy(void *args) {
  struct sm50_destroy_params *params = args;

  SM50Destroy(params->shader);

  return STATUS_SUCCESS;
}

static struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *
sm50_compilation_argument_copy(const struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *src) {
  if (!src)
    return NULL;

  switch (src->type) {
  case SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT: {
    const struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA *data = (const void *)src;
    struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA *copy = calloc(1, sizeof(*copy));
    if (!copy)
      return NULL;
    *copy = *data;
    copy->next = sm50_compilation_argument_copy(data->next);
    return (void *)copy;
  }
  case SM50_SHADER_COMMON: {
    const struct SM50_SHADER_COMMON_DATA *data = (const void *)src;
    struct SM50_SHADER_COMMON_DATA *copy = calloc(1, sizeof(*copy));
    if (!copy)
      return NULL;
    *copy = *data;
    copy->next = sm50_compilation_argument_copy(data->next);
    return (void *)copy;
  }
  case SM50_SHADER_DIAG_FORCE_FULLSCREEN_POSITION: {
    const struct SM50_SHADER_DIAG_FORCE_FULLSCREEN_POSITION_DATA *data = (const void *)src;
    struct SM50_SHADER_DIAG_FORCE_FULLSCREEN_POSITION_DATA *copy = calloc(1, sizeof(*copy));
    if (!copy)
      return NULL;
    *copy = *data;
    copy->next = sm50_compilation_argument_copy(data->next);
    return (void *)copy;
  }
  case SM50_SHADER_PSO_PIXEL_SHADER: {
    const struct SM50_SHADER_PSO_PIXEL_SHADER_DATA *data = (const void *)src;
    struct SM50_SHADER_PSO_PIXEL_SHADER_DATA *copy = calloc(1, sizeof(*copy));
    if (!copy)
      return NULL;
    *copy = *data;
    copy->next = sm50_compilation_argument_copy(data->next);
    return (void *)copy;
  }
  case SM50_SHADER_IA_INPUT_LAYOUT: {
    const struct SM50_SHADER_IA_INPUT_LAYOUT_DATA *data = (const void *)src;
    struct SM50_SHADER_IA_INPUT_LAYOUT_DATA *copy = calloc(1, sizeof(*copy));
    if (!copy)
      return NULL;
    *copy = *data;
    copy->next = sm50_compilation_argument_copy(data->next);
    return (void *)copy;
  }
  case SM50_SHADER_GS_PASS_THROUGH: {
    const struct SM50_SHADER_GS_PASS_THROUGH_DATA *data = (const void *)src;
    struct SM50_SHADER_GS_PASS_THROUGH_DATA *copy = calloc(1, sizeof(*copy));
    if (!copy)
      return NULL;
    *copy = *data;
    copy->next = sm50_compilation_argument_copy(data->next);
    return (void *)copy;
  }
  case SM50_SHADER_PSO_GEOMETRY_SHADER: {
    const struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA *data = (const void *)src;
    struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA *copy = calloc(1, sizeof(*copy));
    if (!copy)
      return NULL;
    *copy = *data;
    copy->next = sm50_compilation_argument_copy(data->next);
    return (void *)copy;
  }
  case SM50_SHADER_PSO_TESSELLATOR: {
    const struct SM50_SHADER_PSO_TESSELLATOR_DATA *data = (const void *)src;
    struct SM50_SHADER_PSO_TESSELLATOR_DATA *copy = calloc(1, sizeof(*copy));
    if (!copy)
      return NULL;
    *copy = *data;
    copy->next = sm50_compilation_argument_copy(data->next);
    return (void *)copy;
  }
  case SM50_SHADER_BINDLESS_MIRROR: {
    const struct SM50_SHADER_BINDLESS_MIRROR_DATA *data = (const void *)src;
    struct SM50_SHADER_BINDLESS_MIRROR_DATA *copy = calloc(1, sizeof(*copy));
    if (!copy)
      return NULL;
    *copy = *data;
    copy->next = sm50_compilation_argument_copy(data->next);
    return (void *)copy;
  }
  case SM50_SHADER_DXMT12_NATIVE_DESCRIPTOR_ABI: {
    const struct DXMT12_MTL4_NATIVE_DESCRIPTOR_ABI_DATA *data = (const void *)src;
    struct DXMT12_MTL4_NATIVE_DESCRIPTOR_ABI_DATA *copy = calloc(1, sizeof(*copy));
    if (!copy)
      return NULL;
    *copy = *data;
    copy->next = sm50_compilation_argument_copy(data->next);
    return (void *)copy;
  }
  case SM50_SHADER_ARGUMENT_TYPE_MAX:
    break;
  }

  return NULL;
}

static void
sm50_compilation_argument_free(struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *arg) {
  while (arg) {
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *next = arg->next;
    free(arg);
    arg = next;
  }
}

static NTSTATUS
thunk_SM50Compile(void *args) {
  struct sm50_compile_params *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *native_args =
      sm50_compilation_argument_copy(params->args);

  sm50_bitcode_t native_bitcode = NULL;
  sm50_error_t native_error = NULL;
  params->ret = SM50Compile(params->shader, native_args, params->func_name,
                            &native_bitcode, &native_error);
  dxmt_airconv_store_compile_outputs(params->bitcode, native_bitcode,
                                     params->error, native_error);

  sm50_compilation_argument_free(native_args);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetCompiledBitcode(void *args) {
  struct sm50_get_compiled_bitcode_params *params = args;

  SM50GetCompiledBitcode(params->bitcode, params->data_out);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50DestroyBitcode(void *args) {
  struct sm50_destroy_bitcode_params *params = args;

  SM50DestroyBitcode(params->bitcode);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetErrorMessage(void *args) {
  struct sm50_get_error_message_params *params = args;

  params->ret_size = SM50GetErrorMessage(params->error, params->buffer, params->buffer_size);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50FreeError(void *args) {
  struct sm50_free_error_params *params = args;

  SM50FreeError(params->error);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileTessellationPipelineHull(void *args) {
  struct sm50_compile_tessellation_pipeline_hull_params *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *native_args =
      sm50_compilation_argument_copy(params->hull_args);

  sm50_bitcode_t native_bitcode = NULL;
  sm50_error_t native_error = NULL;
  params->ret = SM50CompileTessellationPipelineHull(
      params->vertex, params->hull, native_args, params->func_name,
      &native_bitcode, &native_error
  );
  dxmt_airconv_store_compile_outputs(params->bitcode, native_bitcode,
                                     params->error, native_error);

  sm50_compilation_argument_free(native_args);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileTessellationPipelineDomain(void *args) {
  struct sm50_compile_tessellation_pipeline_domain_params *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *native_args =
      sm50_compilation_argument_copy(params->domain_args);

  sm50_bitcode_t native_bitcode = NULL;
  sm50_error_t native_error = NULL;
  params->ret = SM50CompileTessellationPipelineDomain(
      params->hull, params->domain, native_args, params->func_name,
      &native_bitcode, &native_error
  );
  dxmt_airconv_store_compile_outputs(params->bitcode, native_bitcode,
                                     params->error, native_error);

  sm50_compilation_argument_free(native_args);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileGeometryPipelineVertex(void *args) {
  struct sm50_compile_geometry_pipeline_vertex_params *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *native_args =
      sm50_compilation_argument_copy(params->vertex_args);

  sm50_bitcode_t native_bitcode = NULL;
  sm50_error_t native_error = NULL;
  params->ret = SM50CompileGeometryPipelineVertex(
      params->vertex, params->geometry, native_args, params->func_name,
      &native_bitcode, &native_error
  );
  dxmt_airconv_store_compile_outputs(params->bitcode, native_bitcode,
                                     params->error, native_error);

  sm50_compilation_argument_free(native_args);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileGeometryPipelineGeometry(void *args) {
  struct sm50_compile_geometry_pipeline_geometry_params *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *native_args =
      sm50_compilation_argument_copy(params->geometry_args);

  sm50_bitcode_t native_bitcode = NULL;
  sm50_error_t native_error = NULL;
  params->ret = SM50CompileGeometryPipelineGeometry(
      params->vertex, params->geometry, native_args, params->func_name,
      &native_bitcode, &native_error
  );
  dxmt_airconv_store_compile_outputs(params->bitcode, native_bitcode,
                                     params->error, native_error);

  sm50_compilation_argument_free(native_args);

  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandEncoder_setLabel(void *args) {
  struct unixcall_generic_obj_obj_noret *params = args;
  [(id<MTLCommandEncoder>)params->handle setLabel:(NSString *)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_setShouldMaximizeConcurrentCompilation(void *args) {
  struct unixcall_generic_obj_uint64_noret *params = args;
  [(id<MTLDevice>)params->handle setShouldMaximizeConcurrentCompilation:(BOOL)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetArgumentsInfo(void *args) {
  struct sm50_get_arguments_info_params *params = args;
  SM50GetArgumentsInfo(params->shader, params->constant_buffers, params->arguments);
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_DXILInitialize(void *args) {
  struct sm50_initialize_params *params = args;

  params->ret = DXMT12DXILInitialize(params->bytecode, params->bytecode_size,
                               params->shader, params->reflection,
                               params->error);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_DXILDestroy(void *args) {
  struct sm50_destroy_params *params = args;

  DXMT12DXILDestroy(params->shader);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_DXILCompile(void *args) {
  struct sm50_compile_params *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *native_args =
      sm50_compilation_argument_copy(params->args);

  sm50_bitcode_t native_bitcode = NULL;
  sm50_error_t native_error = NULL;
  params->ret = DXMT12DXILCompile(params->shader, native_args, params->func_name,
                            &native_bitcode, &native_error);
  dxmt_airconv_store_compile_outputs(params->bitcode, native_bitcode,
                                     params->error, native_error);

  sm50_compilation_argument_free(native_args);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_DXILGetArgumentsInfo(void *args) {
  struct sm50_get_arguments_info_params *params = args;
  DXMT12DXILGetArgumentsInfo((dxmt12_airconv_shader_t)params->shader, params->constant_buffers, params->arguments);
  return STATUS_SUCCESS;
}

static inline void *
UInt32ToPtr(uint32_t v) {
  return (void *)(uint64_t)v;
}

#ifndef DXMT_NATIVE

static NTSTATUS
thunk32_SM50Initialize(void *args) {
  struct sm50_initialize_params32 *params = args;

  params->ret = SM50Initialize(
      UInt32ToPtr(params->bytecode), params->bytecode_size, UInt32ToPtr(params->shader),
      UInt32ToPtr(params->reflection), UInt32ToPtr(params->error)
  );

  return STATUS_SUCCESS;
}

struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t num_output_slots;
  uint32_t num_elements;
  uint32_t strides[4];
  uint32_t elements;
};

struct SM50_SHADER_COMMON_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_SHADER_METAL_VERSION metal_version;
  enum SM50_SHADER_FLAG flag;
};

struct SM50_SHADER_DIAG_FORCE_FULLSCREEN_POSITION_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  bool enabled;
};

struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

struct SM50_SHADER_IA_INPUT_LAYOUT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_INDEX_BUFFER_FORMAT index_buffer_format;
  uint32_t slot_mask;
  uint32_t num_elements;
  uint32_t elements;
};

struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t sample_mask;
  bool dual_source_blending;
  bool disable_depth_output;
  uint32_t unorm_output_reg_mask;
  uint64_t demote_msaa_srv_mask_lo;
  uint64_t demote_msaa_srv_mask_hi;
};

_Static_assert(sizeof(struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32) == 40, "SM50 pixel PSO args32 size mismatch");
_Static_assert(offsetof(struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32, demote_msaa_srv_mask_lo) == 24,
               "SM50 pixel PSO args32 demote mask lo offset mismatch");
_Static_assert(offsetof(struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32, demote_msaa_srv_mask_hi) == 32,
               "SM50 pixel PSO args32 demote mask hi offset mismatch");

struct SM50_SHADER_GS_PASS_THROUGH_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  union {
    struct MTL_GEOMETRY_SHADER_PASS_THROUGH Data;
    uint32_t DataEncoded;
  };
  bool RasterizationDisabled;
};

struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  bool strip_topology;
};

struct SM50_SHADER_PSO_TESSELLATOR_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t max_potential_tess_factor;
};

struct SM50_SHADER_BINDLESS_MIRROR_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  bool enabled;
};

struct DXMT12_MTL4_NATIVE_DESCRIPTOR_ABI_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  DXMT12_MTL4_SHADER_ABI_VERSION version;
  bool enabled;
};

void
sm50_compilation_argument32_convert(
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *first_arg, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32
) {
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *last_arg = first_arg;

  first_arg->type = SM50_SHADER_ARGUMENT_TYPE_MAX;
  first_arg->next = NULL;

  while (args32) {
    switch (args32->type) {
    case SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT: {
      struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA32 *src = (void *)args32;
      struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA *data =
          malloc(sizeof(struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->num_output_slots = src->num_output_slots;
      data->num_elements = src->num_elements;
      data->strides[0] = src->strides[0];
      data->strides[1] = src->strides[1];
      data->strides[2] = src->strides[2];
      data->strides[3] = src->strides[3];
      data->elements = UInt32ToPtr(src->elements);
      break;
    }
    case SM50_SHADER_COMMON: {
      struct SM50_SHADER_COMMON_DATA32 *src = (void *)args32;
      struct SM50_SHADER_COMMON_DATA *data = malloc(sizeof(struct SM50_SHADER_COMMON_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->metal_version = src->metal_version;
      data->flags = src->flag;
      break;
    }
    case SM50_SHADER_DIAG_FORCE_FULLSCREEN_POSITION: {
      struct SM50_SHADER_DIAG_FORCE_FULLSCREEN_POSITION_DATA32 *src = (void *)args32;
      struct SM50_SHADER_DIAG_FORCE_FULLSCREEN_POSITION_DATA *data =
          malloc(sizeof(struct SM50_SHADER_DIAG_FORCE_FULLSCREEN_POSITION_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->enabled = src->enabled;
      break;
    }
    case SM50_SHADER_PSO_PIXEL_SHADER: {
      struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_PIXEL_SHADER_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_PIXEL_SHADER_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->unorm_output_reg_mask = src->unorm_output_reg_mask;
      data->disable_depth_output = src->disable_depth_output;
      data->sample_mask = src->sample_mask;
      data->dual_source_blending = src->dual_source_blending;
      data->demote_msaa_srv_mask_lo = src->demote_msaa_srv_mask_lo;
      data->demote_msaa_srv_mask_hi = src->demote_msaa_srv_mask_hi;
      break;
    }
    case SM50_SHADER_IA_INPUT_LAYOUT: {
      struct SM50_SHADER_IA_INPUT_LAYOUT_DATA32 *src = (void *)args32;
      struct SM50_SHADER_IA_INPUT_LAYOUT_DATA *data = malloc(sizeof(struct SM50_SHADER_IA_INPUT_LAYOUT_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->slot_mask = src->slot_mask;
      data->index_buffer_format = src->index_buffer_format;
      data->num_elements = src->num_elements;
      data->elements = UInt32ToPtr(src->elements);
      break;
    }
    case SM50_SHADER_GS_PASS_THROUGH: {
      struct SM50_SHADER_GS_PASS_THROUGH_DATA32 *src = (void *)args32;
      struct SM50_SHADER_GS_PASS_THROUGH_DATA *data = malloc(sizeof(struct SM50_SHADER_GS_PASS_THROUGH_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->Data = src->Data;
      data->RasterizationDisabled = src->RasterizationDisabled;
      break;
    }
    case SM50_SHADER_PSO_GEOMETRY_SHADER: {
      struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->strip_topology = src->strip_topology;
      break;
    }
    case SM50_SHADER_PSO_TESSELLATOR: {
      struct SM50_SHADER_PSO_TESSELLATOR_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_TESSELLATOR_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_TESSELLATOR_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->max_potential_tess_factor = src->max_potential_tess_factor;
      break;
    }
    case SM50_SHADER_BINDLESS_MIRROR: {
      struct SM50_SHADER_BINDLESS_MIRROR_DATA32 *src = (void *)args32;
      struct SM50_SHADER_BINDLESS_MIRROR_DATA *data = malloc(sizeof(struct SM50_SHADER_BINDLESS_MIRROR_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->enabled = src->enabled;
      break;
    }
    case SM50_SHADER_DXMT12_NATIVE_DESCRIPTOR_ABI: {
      struct DXMT12_MTL4_NATIVE_DESCRIPTOR_ABI_DATA32 *src = (void *)args32;
      struct DXMT12_MTL4_NATIVE_DESCRIPTOR_ABI_DATA *data =
          malloc(sizeof(struct DXMT12_MTL4_NATIVE_DESCRIPTOR_ABI_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->version = src->version;
      data->enabled = src->enabled;
      break;
    }
    case SM50_SHADER_ARGUMENT_TYPE_MAX:
      break;
    }
    args32 = UInt32ToPtr(args32->next);
  }
}

void
sm50_compilation_argument32_free(struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *first_arg) {
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *arg = first_arg->next;

  while (arg) {
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *next = arg->next;
    free(arg);
    arg = next;
  }
}

static NTSTATUS
thunk32_SM50Compile(void *args) {
  struct sm50_compile_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50Compile(
      params->shader, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetCompiledBitcode(void *args) {
  struct sm50_get_compiled_bitcode_params32 *params = args;

  SM50GetCompiledBitcode(params->bitcode, UInt32ToPtr(params->data_out));

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetErrorMessage(void *args) {
  struct sm50_get_error_message_params32 *params = args;

  params->ret_size = SM50GetErrorMessage(params->error, UInt32ToPtr(params->buffer), params->buffer_size);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileTessellationPipelineHull(void *args) {
  struct sm50_compile_tessellation_pipeline_hull_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->hull_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileTessellationPipelineHull(
      params->vertex, params->hull, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileTessellationPipelineDomain(void *args) {
  struct sm50_compile_tessellation_pipeline_domain_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->domain_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileTessellationPipelineDomain(
      params->hull, params->domain, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileGeometryPipelineVertex(void *args) {
  struct sm50_compile_geometry_pipeline_vertex_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->vertex_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileGeometryPipelineVertex(
      params->vertex, params->geometry, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileGeometryPipelineGeometry(void *args) {
  struct sm50_compile_geometry_pipeline_geometry_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->geometry_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileGeometryPipelineGeometry(
      params->vertex, params->geometry, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetArgumentsInfo(void *args) {
  struct sm50_get_arguments_info_params32 *params = args;

  SM50GetArgumentsInfo(params->shader, UInt32ToPtr(params->constant_buffers), UInt32ToPtr(params->arguments));

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_DXILInitialize(void *args) {
  struct sm50_initialize_params32 *params = args;

  params->ret = DXMT12DXILInitialize(
      UInt32ToPtr(params->bytecode), params->bytecode_size, UInt32ToPtr(params->shader),
      UInt32ToPtr(params->reflection), UInt32ToPtr(params->error)
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_DXILCompile(void *args) {
  struct sm50_compile_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = DXMT12DXILCompile(
      params->shader, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_DXILGetArgumentsInfo(void *args) {
  struct sm50_get_arguments_info_params32 *params = args;
  DXMT12DXILGetArgumentsInfo(
      (dxmt12_airconv_shader_t)params->shader, UInt32ToPtr(params->constant_buffers),
      UInt32ToPtr(params->arguments)
  );
  return STATUS_SUCCESS;
}
#endif /* DXMT_NATIVE */

static NTSTATUS
_MTLCommandBuffer_error(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(DXMTMetal4CommandBuffer *)params->handle error];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_logs(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(DXMTMetal4CommandBuffer *)params->handle logs];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLLogContainer_enumerate(void *obj) {
  struct unixcall_enumerate *params = obj;
  uint64_t count = 0;
  uint64_t read = 0;
  id *buffer = params->buffer.ptr;
  for (id _ in (id<MTLLogContainer>)params->enumerable) {
    if (count >= params->start) {
      if (count < params->start + params->buffer_size) {
        buffer[count - params->start] = _;
        read++;
      } else {
        break;
      }
    }
    count++;
  }
  params->ret_read = read;
  return STATUS_SUCCESS;
}

CFStringRef
GetColorSpaceName(enum WMTColorSpace colorspace) {
  switch (colorspace) {
  case WMTColorSpaceSRGB:
    return kCGColorSpaceSRGB;
  case WMTColorSpaceSRGBLinear:
  case WMTColorSpaceHDR_scRGB:
    return kCGColorSpaceExtendedLinearSRGB;
  case WMTColorSpaceBT2020:
    return kCGColorSpaceITUR_2020_sRGBGamma;
  case WMTColorSpaceHDR_PQ:
    return kCGColorSpaceITUR_2100_PQ;
  default:
    return nil;
  }
}

static NTSTATUS
_CGColorSpace_checkColorSpaceSupported(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = false;
  CFStringRef name = GetColorSpaceName((enum WMTColorSpace)params->handle);
  if (!name)
    return STATUS_SUCCESS;
  CGColorSpaceRef ref = CGColorSpaceCreateWithName(name);
  if (!ref)
    return STATUS_SUCCESS;
  CGColorSpaceRelease(ref);
  params->ret = true;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_setColorSpace(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  enum WMTColorSpace colorspace = params->arg;
  CFStringRef name = GetColorSpaceName(colorspace);
  params->ret = false;
  if (!name)
    return STATUS_SUCCESS;
  CGColorSpaceRef ref = CGColorSpaceCreateWithName(name);
  if (!ref)
    return STATUS_SUCCESS;
  execute_on_main(^{
    layer.colorspace = ref;
    layer.wantsExtendedDynamicRangeContent = WMT_COLORSPACE_IS_HDR(colorspace);
    CGColorSpaceRelease(ref);
  });
  params->ret = true;
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetPrimaryDisplayId(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = CGMainDisplayID();
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetSecondaryDisplayId(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = kCGNullDirectDisplay;

  uint32_t count = 0;
  CGGetOnlineDisplayList(0, NULL, &count);

  if (count == 0)
    return STATUS_SUCCESS;

  CGDirectDisplayID main_display = CGMainDisplayID();
  CGDirectDisplayID displays[count];
  CGGetOnlineDisplayList(count, displays, &count);

  for (uint32_t i = 0; i < count; i++) {
    CGDirectDisplayID id = displays[i];
    if (id == main_display)
      continue;
    if (CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay)
      continue;
    params->ret = id;
    break;
  }

  return STATUS_SUCCESS;
}

typedef struct icc_XYZ_t {
  uint32_t sig;      // 0x205a5958
  uint32_t reserved; // 0
  int32_t x;
  int32_t y;
  int32_t z;
} icc_XYZ_t;

bool
GetChromaticity_xy(ColorSyncProfileRef profile, CFStringRef tag, float *out_x, float *out_y) {
  CFDataRef tag_data = ColorSyncProfileCopyTag(profile, tag);
  if (!tag_data)
    return false;
  if (CFDataGetLength(tag_data) != sizeof(icc_XYZ_t))
    return false;
  icc_XYZ_t *data = (icc_XYZ_t *)CFDataGetBytePtr(tag_data);
  if (data->sig != 0x205a5958)
    return false;
  double X = (int32_t)__builtin_bswap32(data->x) / 65536.0;
  double Y = (int32_t)__builtin_bswap32(data->y) / 65536.0;
  double Z = (int32_t)__builtin_bswap32(data->z) / 65536.0;
  *out_x = X / (X + Y + Z);
  *out_y = Y / (X + Y + Z);
  return true;
}

bool
GetDisplayColorGamut(ColorSyncProfileRef profile, struct WMTDisplayDescription *desc_out) {
  return GetChromaticity_xy(
             profile, kColorSyncSigMediaWhitePointTag, &desc_out->white_points[0], &desc_out->white_points[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigRedColorantTag, &desc_out->red_primaries[0], &desc_out->red_primaries[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigGreenColorantTag, &desc_out->green_primaries[0], &desc_out->green_primaries[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigBlueColorantTag, &desc_out->blue_primaries[0], &desc_out->blue_primaries[1]
         );
}

NSScreen *
GetNSScreenForDisplayID(CGDirectDisplayID display_id) {
  for (NSScreen *screen in [NSScreen screens]) {
    CGDirectDisplayID id = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
    if (id == display_id) {
      return screen;
    }
  }
  return nil;
}

static NTSTATUS
_WMTGetDisplayDescription(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  CGDirectDisplayID display_id = params->handle;
  struct WMTDisplayDescription *desc_out = params->arg.ptr;
  ColorSyncProfileRef profile = ColorSyncProfileCreateWithDisplayID(display_id);
  if (!profile || !GetDisplayColorGamut(profile, desc_out))
    GetDisplayColorGamut(ColorSyncProfileCreateWithName(kColorSyncGenericRGBProfile), desc_out);
  NSScreen *screen = GetNSScreenForDisplayID(display_id);
  if (screen) {
    desc_out->maximum_edr_color_component_value = [screen maximumExtendedDynamicRangeColorComponentValue];
    desc_out->maximum_reference_edr_color_component_value =
        [screen maximumReferenceExtendedDynamicRangeColorComponentValue];
    desc_out->maximum_potential_edr_color_component_value =
        [screen maximumPotentialExtendedDynamicRangeColorComponentValue];
  } else {
    desc_out->maximum_edr_color_component_value = 1.0;
    desc_out->maximum_reference_edr_color_component_value = 0.0;
    desc_out->maximum_potential_edr_color_component_value = 1.0;
  }
  return STATUS_SUCCESS;
}

struct DisplaySetting {
  uint64_t version;
  enum WMTColorSpace colorspace;
  struct WMTHDRMetadata hdr_metadata;
};

struct DisplaySetting g_display_settings[2] = {{0, 0, {}}, {0, 0, {}}};

static NTSTATUS
_MetalLayer_getEDRValue(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  struct WMTEDRValue *value = params->arg.ptr;
  value->maximum_edr_color_component_value = 1.0;
  value->maximum_potential_edr_color_component_value = 1.0;

  if (!layer)
    return STATUS_SUCCESS;

  execute_on_main(^{
    if (![layer isKindOfClass:CAMetalLayer.class] ||
        ![layer.delegate isKindOfClass:NSView.class])
      return;

    NSView *view = (NSView *)layer.delegate;
    if (!view.window || !view.window.screen)
      return;

    NSScreen *screen = view.window.screen;
    value->maximum_edr_color_component_value =
        layer.wantsExtendedDynamicRangeContent ? screen.maximumExtendedDynamicRangeColorComponentValue : 1.0;
    value->maximum_potential_edr_color_component_value =
        screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
  });

  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLLibrary_newFunctionWithConstants(void *obj) {
  struct unixcall_mtllibrary_newfunction_with_constants *params = obj;
  id<MTLLibrary> library = (id<MTLLibrary>)params->library;
  NSString *name = [[NSString alloc] initWithCString:(char *)params->name.ptr encoding:NSUTF8StringEncoding];
  NSError *err = NULL;
  MTLFunctionConstantValues *values = [[MTLFunctionConstantValues alloc] init];
  for (uint64_t i = 0; i < params->num_constants; i++) {
    const struct WMTFunctionConstant *constant = &params->constants[i];
    if (constant->type == WMTDataTypeBool) {
      bool value = (params->bool_values & (UINT64_C(1) << i)) != 0;
      [values setConstantValue:&value type:(MTLDataType)constant->type atIndex:constant->index];
    } else {
      [values setConstantValue:constant->data.ptr type:(MTLDataType)constant->type atIndex:constant->index];
    }
  }

  params->ret = (obj_handle_t)[library newFunctionWithName:name constantValues:values error:&err];
  params->ret_error = (obj_handle_t)err;
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && params->ret && !err) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    dxmt_apitrace_track_function_with_constants_locked(
        params->ret, params->library, name, params->constants, params->num_constants, params->bool_values);
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  [name release];
  [values release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTQueryDisplaySetting(void *obj) {
  struct unixcall_query_display_setting *params = obj;
  CGDirectDisplayID display_id = params->display_id;
  struct WMTHDRMetadata *value = params->hdr_metadata.ptr;
  params->ret = false;
  struct DisplaySetting *setting = &g_display_settings[display_id == CGMainDisplayID()];
  if (setting->version) {
    *value = setting->hdr_metadata;
    params->colorspace = setting->colorspace;
    params->ret = true;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTUpdateDisplaySetting(void *obj) {
  struct unixcall_update_display_setting *params = obj;
  CGDirectDisplayID display_id = params->display_id;
  const struct WMTHDRMetadata *value = params->hdr_metadata.ptr;
  struct DisplaySetting *setting = &g_display_settings[display_id == CGMainDisplayID()];
  if (value) {
    setting->hdr_metadata = *value;
    setting->colorspace = params->colorspace;
    setting->version++;
  } else {
    setting->version = 0;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTQueryDisplaySettingForLayer(void *obj) {
  struct unixcall_query_display_setting_for_layer *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->layer;
  struct WMTHDRMetadata *hdr_metadata_out = params->hdr_metadata.ptr;

  params->version = 0;
  params->edr_value.maximum_edr_color_component_value = 1.0;
  params->edr_value.maximum_potential_edr_color_component_value = 1.0;
  if (!layer)
    return STATUS_SUCCESS;

  execute_on_main(^{
    if (![layer isKindOfClass:CAMetalLayer.class] ||
        ![layer.delegate isKindOfClass:NSView.class])
      return;

    NSView *view = (NSView *)layer.delegate;
    if (!view.window || !view.window.screen)
      return;

    NSScreen *screen = view.window.screen;
    CGDirectDisplayID id = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];

    struct DisplaySetting *setting = &g_display_settings[id == CGMainDisplayID()];
    *hdr_metadata_out = setting->hdr_metadata;
    params->version = setting->version;
    params->colorspace = setting->colorspace;
    params->edr_value.maximum_edr_color_component_value =
        layer.wantsExtendedDynamicRangeContent ? screen.maximumExtendedDynamicRangeColorComponentValue : 1.0;
    params->edr_value.maximum_potential_edr_color_component_value =
        screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
  });
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeWaitForEvent(void *obj) {
  struct unixcall_generic_obj_obj_uint64_noret *params = obj;
  [(DXMTMetal4CommandBuffer *)params->handle encodeWaitForEvent:(id<MTLEvent>)params->arg0 value:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_signalValue(void *obj) {
  struct unixcall_generic_obj_uint64_noret *params = obj;
  [(id<MTLSharedEvent>)params->handle setSignaledValue:params->arg];
  return STATUS_SUCCESS;
}

#ifndef DXMT_NATIVE

typedef struct {
  _Atomic(CFRunLoopRef) runloop_ref;
  MTLSharedEventListener *shared_listener;
} *shared_event_listener_t;

extern NTSTATUS NtSetEvent(void *handle, void *prev_state);

static NTSTATUS
_MTLSharedEvent_setWin32EventAtValue(void *obj) {
  struct unixcall_mtlsharedevent_setevent *params = obj;
  void *nt_event_handle = (shared_event_listener_t)params->event_handle;
  shared_event_listener_t q = (shared_event_listener_t)params->shared_event_listener;
  [(id<MTLSharedEvent>)params->shared_event
      notifyListener:q->shared_listener
             atValue:params->value
               block:^(id<MTLSharedEvent> _e, uint64_t _v) {
                 // NOTE: must ensure no more notification comes after listener been destroyed.
                 while (!atomic_load_explicit(&q->runloop_ref, memory_order_acquire)) {
#if defined(__x86_64__)
                   _mm_pause();
#elif defined(__aarch64__)
          __asm__ __volatile__("yield");
#endif
                 }
                 CFRunLoopPerformBlock(q->runloop_ref, kCFRunLoopCommonModes, ^{
                   NtSetEvent(nt_event_handle, NULL);
                 });
                 CFRunLoopWakeUp(q->runloop_ref);
               }];
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_start(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  shared_event_listener_t q = (shared_event_listener_t)params->handle;
  CFRunLoopRef uninited = NULL;
  if (q && atomic_compare_exchange_strong(&q->runloop_ref, &uninited, CFRunLoopGetCurrent())) {
    /* Add a dummy source so the runloop stays running */
    CFRunLoopSourceContext source_context = {0};
    CFRunLoopSourceRef source = CFRunLoopSourceCreate(NULL, 0, &source_context);
    CFRunLoopAddSource(q->runloop_ref, source, kCFRunLoopCommonModes);
    CFRunLoopRun();
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_create(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  shared_event_listener_t q = malloc(sizeof(*q));
  if (q) {
    q->runloop_ref = NULL;
    q->shared_listener = [[MTLSharedEventListener alloc] init];
  }
  params->ret = (obj_handle_t)q;
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_destroy(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  shared_event_listener_t q = (shared_event_listener_t)params->handle;
  if (q && q->runloop_ref) {
    CFRunLoopStop(q->runloop_ref);
    q->runloop_ref = NULL;
    [q->shared_listener release];
    q->shared_listener = nil;
    free(q);
  }
  return STATUS_SUCCESS;
}

#else
static NTSTATUS
_MTLSharedEvent_setWin32EventAtValue(void *obj) {
  // nop
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_start(void *obj) {
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_create(void *obj) {
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_destroy(void *obj) {
  return STATUS_SUCCESS;
}

#endif

static NTSTATUS
_MTLDevice_newFence(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newFence];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newEvent(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newEvent];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_updateContents(void *obj) {
  struct unixcall_mtlbuffer_updatecontents *params = obj;
  memcpy((void *)((char *)[(id<MTLBuffer>)params->buffer contents] + params->offset), params->data.ptr, params->length);
  if ([(id<MTLBuffer>)params->buffer storageMode] == MTLStorageModeManaged)
    [(id<MTLBuffer>)params->buffer didModifyRange:NSMakeRange(params->offset, params->length)];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && [(id<MTLBuffer>)params->buffer storageMode] != MTLStorageModePrivate) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      apitrace_metal_update_buffer_contents(
          dxmt_apitrace_session,
          params->buffer,
          params->offset,
          params->length,
          (uint32_t)[(id<MTLBuffer>)params->buffer storageMode],
          (const uint8_t *)[(id<MTLBuffer>)params->buffer contents] + params->offset,
          params->length);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetOSVersion(void *obj) {
  struct unixcall_get_os_version *params = obj;
  NSOperatingSystemVersion version = [NSProcessInfo processInfo].operatingSystemVersion;
  params->ret_major = version.majorVersion;
  params->ret_minor = version.minorVersion;
  params->ret_patch = version.patchVersion;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newBinaryArchive(void *obj) {
  struct unixcall_mtldevice_newbinaryarchive *params = obj;
  NSString *path_str = NULL;
  NSURL *url = NULL;
  MTLBinaryArchiveDescriptor *desc = [[MTLBinaryArchiveDescriptor alloc] init];
  if (params->url.ptr != NULL) {
    path_str = [[NSString alloc] initWithCString:params->url.ptr encoding:NSUTF8StringEncoding];
    url = [[NSURL alloc] initFileURLWithPath:path_str];
    desc.url = url;
  }
  NSError *err = NULL;
  params->ret_archive = (obj_handle_t)[(id<MTLDevice>)params->device newBinaryArchiveWithDescriptor:desc error:&err];
  params->ret_error = (obj_handle_t)err;
  [desc release];
  if (url)
    [url release];
  if (path_str)
    [path_str release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBinaryArchive_serialize(void *obj) {
  struct unixcall_mtlbinaryarchive_serialize *params = obj;
  NSString *path_str = [[NSString alloc] initWithCString:params->url.ptr encoding:NSUTF8StringEncoding];
  NSURL *url = [[NSURL alloc] initFileURLWithPath:path_str];
  NSError *err = NULL;
  [(id<MTLBinaryArchive>)params->archive serializeToURL:url error:&err];
  params->ret_error = (obj_handle_t)err;
  [url release];
  [path_str release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_DispatchData_alloc_init(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)dispatch_data_create((void *)params->handle, params->arg, NULL, NULL);
  return STATUS_SUCCESS;
}

static NTSTATUS
_DispatchData_copy(void *obj) {
  struct unixcall_dispatch_data_copy *params = obj;
  const void *bytes = NULL;
  size_t length = 0;
  dispatch_data_t mapped = dispatch_data_create_map((dispatch_data_t)params->data, &bytes, &length);
  params->ret_length = length;
  void *dst = params->bytes.ptr;
  if (dst && params->capacity >= length && length)
    memcpy(dst, bytes, length);
  dispatch_release(mapped);
  return STATUS_SUCCESS;
}

@interface MTLSharedTextureHandle ()

- (MTLSharedTextureHandle *)initWithMachPort:(mach_port_t)port;
- (mach_port_t)createMachPort;

@end

static NTSTATUS
_MTLDevice_newSharedTexture(void *obj) {
  struct unixcall_mtldevice_newtexture *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTTextureInfo *info = params->info.ptr;

  if (info->mach_port) {
    MTLSharedTextureHandle *handle = [[MTLSharedTextureHandle alloc] initWithMachPort:info->mach_port];
    id<MTLTexture> ret = [device newSharedTextureWithHandle:handle];
    extract_texture_descriptor(ret, info);
    params->ret = (obj_handle_t)ret;
    info->gpu_resource_id = [ret gpuResourceID]._impl;
    [handle release];
  } else {
    MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
    fill_texture_descriptor(desc, info);
    id<MTLTexture> ret = [device newSharedTextureWithDescriptor:desc];
    MTLSharedTextureHandle *handle = [ret newSharedTextureHandle];
    params->ret = (obj_handle_t)ret;
    info->gpu_resource_id = [ret gpuResourceID]._impl;
    info->mach_port = [handle createMachPort]; // implicitly add ref to underlying IOSurface
    [handle release];
    [desc release];
  }

  return STATUS_SUCCESS;
}

/* Private API to register a mach port with the bootstrap server */
extern kern_return_t bootstrap_register2(mach_port_t bp, name_t service_name, mach_port_t sp, int flags);

static NTSTATUS
_WMT4BootstrapRegister(void *obj) {
  struct unixcall_bootstrap *params = obj;
  mach_port_t rp = params->mach_port;
  mach_port_t bp;

  if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS)
    return STATUS_UNSUCCESSFUL;
  NTSTATUS ret = bootstrap_register2(bp, params->name, rp, 0) != KERN_SUCCESS ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
  mach_port_deallocate(mach_task_self(), bp);
  return ret;
}

static NTSTATUS
_WMT4BootstrapLookUp(void *obj) {
  struct unixcall_bootstrap *params = obj;
  mach_port_t rp = 0;
  mach_port_t bp;

  if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS)
    return STATUS_UNSUCCESSFUL;
  NTSTATUS ret = bootstrap_look_up(bp, params->name, &rp) != KERN_SUCCESS ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
  mach_port_deallocate(mach_task_self(), bp);
  params->mach_port = rp;
  return ret;
}

@protocol MTLDeviceSPI <MTLDevice>

- (id<MTLSharedEvent>)newSharedEventWithMachPort:(mach_port_t)machPort;

@end

@interface MTLSharedEventHandle ()

- (mach_port_t)eventPort;

@end

static NTSTATUS
_MTLSharedEvent_createMachPort(void *obj) {
  struct unixcall_mtlsharedevent_createmachport *params = obj;
  id<MTLSharedEvent> event = (id<MTLSharedEvent>)params->event;
  MTLSharedEventHandle *handle = [event newSharedEventHandle];
  mach_port_t port = [handle eventPort];

  // The eventPort method returns a send right that's owned by the handle.
  // We need to add our own send right since we're keeping the port but releasing the handle.
  // This increments the send right count so the port remains valid.
  mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1);

  params->ret_mach_port = port;
  [handle release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSharedEventWithMachPort(void *obj) {
  struct unixcall_mtldevice_newsharedeventwithmachport *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  id<MTLDeviceSPI> deviceSPI = (id<MTLDeviceSPI>)device;
  params->ret_event = (obj_handle_t)[deviceSPI newSharedEventWithMachPort:params->mach_port];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_registryID(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle registryID];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_waitUntilSignaledValue(void *obj) {
  struct unixcall_mtlsharedevent_waituntilsignaledvalue *params = obj;
  bool timeout = [(id<MTLSharedEvent>)params->event waitUntilSignaledValue:params->value timeoutMS:params->timeout_ms];
  params->ret_timeout = timeout;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCounterSampleBuffer_newTimestampBuffer(void *obj) {
  struct unixcall_mtlcountersamplebuffer_newtimestampbuffer *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;

  MTLCounterSampleBufferDescriptor *desc = [[MTLCounterSampleBufferDescriptor alloc] init];
  NSArray<id<MTLCounterSet>> *counter_sets = [device counterSets];
  id<MTLCounterSet> timestamp_counter_set = nil;
  for (id<MTLCounterSet> counterSet in counter_sets) {
    if ([counterSet.name isEqualToString:MTLCommonCounterSetTimestamp]) {
      timestamp_counter_set = counterSet;
      break;
    }
  }
  desc.counterSet = timestamp_counter_set;
  desc.sampleCount = params->sample_count;
  desc.storageMode = params->shared ? MTLStorageModeShared : MTLStorageModePrivate;

  NSError *error = nil;
  params->ret = (obj_handle_t)[device newCounterSampleBufferWithDescriptor:desc error:&error];

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCounterSampleBuffer_resolveCounterRange(void *obj) {
  struct unixcall_mtlcountersamplebuffer_resolvecounterrange *params = obj;
  id<MTLCounterSampleBuffer> sample_buffer = (id<MTLCounterSampleBuffer>)params->sample_buffer;

  NSData *data = [sample_buffer resolveCounterRange:NSMakeRange(params->start, params->len)];
  if (data && params->data_out.ptr) {
    [data getBytes:params->data_out.ptr length:params->data_length];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers(void *obj) {
  struct unixcall_mtlcommandbuffer_blitcommandencoderwithsamplebuffers *params = obj;
  fprintf(stderr, "err:   DXMT Metal4 timestamp sample-buffer blit path is unavailable\n");
  params->ret = 0;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_property(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  double ns = 1000000000;
  switch (params->arg) {
  case WMTCommandBufferPropertyKernelStartTime:
    params->ret = (uint64_t)([(DXMTMetal4CommandBuffer *)params->handle kernelStartTime] * ns);
    break;
  case WMTCommandBufferPropertyKernelEndTime:
    params->ret = (uint64_t)([(DXMTMetal4CommandBuffer *)params->handle kernelEndTime] * ns);
    break;
  case WMTCommandBufferPropertyGPUStartTime:
    params->ret = (uint64_t)([(DXMTMetal4CommandBuffer *)params->handle GPUStartTime] * ns);
    break;
  case WMTCommandBufferPropertyGPUEndTime:
    params->ret = (uint64_t)([(DXMTMetal4CommandBuffer *)params->handle GPUEndTime] * ns);
    break;
  default:
    params->ret = 0;
    break;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_addAllocation(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  id set = (id)params->handle;
  id allocation = (id)params->arg;
  if (!dxmt_metal4_is_residency_set(set) || !dxmt_metal4_is_allocation(allocation))
    return STATUS_SUCCESS;
  NSLock *membershipLock = dxmt_residency_membership_lock_get();
  [membershipLock lock];
  [(id<MTLResidencySet>)set addAllocation:(id<MTLAllocation>)allocation];
  [membershipLock unlock];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_removeAllocation(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  id set = (id)params->handle;
  id allocation = (id)params->arg;
  if (!dxmt_metal4_is_residency_set(set) || !dxmt_metal4_is_allocation(allocation))
    return STATUS_SUCCESS;
  NSLock *membershipLock = dxmt_residency_membership_lock_get();
  [membershipLock lock];
  [(id<MTLResidencySet>)set removeAllocation:(id<MTLAllocation>)allocation];
  [membershipLock unlock];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_commit(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  id set = (id)params->handle;
  if (!dxmt_metal4_is_residency_set(set))
    return STATUS_SUCCESS;
  NSLock *membershipLock = dxmt_residency_membership_lock_get();
  [membershipLock lock];
  [(id<MTLResidencySet>)set commit];
  [membershipLock unlock];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_requestResidency(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  id set = (id)params->handle;
  if (!dxmt_metal4_is_residency_set(set))
    return STATUS_SUCCESS;
  [(id<MTLResidencySet>)set requestResidency];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newTileRenderPipelineState(void *obj) {
  struct unixcall_mtldevice_newrenderpso *params = obj;
  const struct WMTTileRenderPipelineInfo *info = params->info.ptr;
  MTLTileRenderPipelineDescriptor *descriptor = [[MTLTileRenderPipelineDescriptor alloc] init];

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->color_formats[i]);
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_tile_buffers & (1 << i))
      descriptor.tileBuffers[i].mutability = MTLMutabilityImmutable;
  }

  descriptor.rasterSampleCount = info->raster_sample_count;
  descriptor.threadgroupSizeMatchesTileSize = info->tgsize_matches_tile_size;


  descriptor.tileFunction = (id<MTLFunction>)info->tile_function;

  MTLPipelineOption options = MTLPipelineOptionNone;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
      descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                      count:info->num_binary_archives_for_lookup];
    options = info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  }
#endif
  NSError *err = NULL;
  uint64_t compile_wait_begin = dxmt_monotonic_us();
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithTileDescriptor:descriptor
                                                                                                  options:options
                                                                                               reflection:nil
                                                                                                    error:&err];
  uint64_t compile_wait_end = dxmt_monotonic_us();
  params->ret_compile_wait_us =
      compile_wait_end >= compile_wait_begin ? compile_wait_end - compile_wait_begin : 0;
  params->ret_error = (obj_handle_t)err;
  const BOOL pso_created = params->ret_pso != 0;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (pso_created && info->binary_archive_for_serialization) {
      [(id<MTLBinaryArchive>)info->binary_archive_for_serialization
          addTileRenderPipelineFunctionsWithDescriptor:descriptor
                                                 error:&err];
    }
  }
#endif
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && pso_created) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *descriptor_json = dxmt_apitrace_tile_render_pipeline_json(info);
      apitrace_metal_register_tile_render_pipeline(
          dxmt_apitrace_session,
          params->ret_pso,
          descriptor_json.UTF8String,
          info->tile_function);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  [descriptor release];
  return STATUS_SUCCESS;
}

#if DXMT_APITRACE_METAL
static NTSTATUS
_WMT4ApitraceSessionEnsureOpen(void *obj) {
  (void)obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  (void)dxmt_apitrace_ensure_session_locked();
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMT4ApitraceSessionClose(void *obj) {
  (void)obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  dxmt_apitrace_close_session_locked();
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMT4ApitraceSessionFlush(void *obj) {
  (void)obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  if (dxmt_apitrace_session)
    apitrace_metal_session_flush(dxmt_apitrace_session);
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMT4ApitraceSessionSealCheckpoint(void *obj) {
  (void)obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  if (dxmt_apitrace_session)
    apitrace_metal_session_seal_checkpoint(dxmt_apitrace_session);
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMT4ApitraceSetCurrentD3DSequence(void *obj) {
  struct unixcall_generic_obj_uint64_noret *params = obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  dxmt_apitrace_set_sequence_locked(dxmt_apitrace_ensure_session_locked(), params->arg);
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMT4ApitraceCommandBufferBegin(void *obj) {
  struct unixcall_apitrace_command_buffer_begin *params = obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  if (dxmt_apitrace_ensure_session_locked()) {
    dxmt_apitrace_set_sequence_locked(dxmt_apitrace_session, dxmt_apitrace_current_d3d_sequence);
    dxmt_apitrace_ensure_command_buffer_begin_locked(
        dxmt_apitrace_session,
        params->command_buffer,
        params->frame_id,
        dxmt_apitrace_current_d3d_sequence);
  }
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMT4ApitraceCommandBufferCommit(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  DXMT4ApitraceCommandBufferState *state = nil;
  if (dxmt_apitrace_ensure_session_locked()) {
    state = dxmt_apitrace_ensure_command_buffer_begin_locked(
        dxmt_apitrace_session,
        params->handle,
        0,
        dxmt_apitrace_current_d3d_sequence);
  }
  if (state) {
    apitrace_metal_command_buffer_commit_state(
        dxmt_apitrace_session,
        params->handle,
        APITRACE_METAL_COMMAND_BUFFER_COMMIT_RECORDED_BEFORE_NATIVE_COMMIT,
        0,
        0,
        0,
        0,
        state.d3dSequence);
  }
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMT4ApitracePresentDrawable(void *obj) {
  struct unixcall_apitrace_present *params = obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  if (dxmt_apitrace_ensure_session_locked()) {
    dxmt_apitrace_ensure_command_buffer_begin_locked(
        dxmt_apitrace_session,
        params->command_buffer,
        params->frame_index,
        dxmt_apitrace_current_d3d_sequence);
    id<MTLDrawableWithTexture> drawable = (id<MTLDrawableWithTexture>)params->drawable;
    id<MTLTexture> texture = [drawable respondsToSelector:@selector(texture)] ? [drawable texture] : nil;
    uint32_t width = texture ? (uint32_t)texture.width : 0;
    uint32_t height = texture ? (uint32_t)texture.height : 0;
    apitrace_metal_present_drawable(
        dxmt_apitrace_session,
        params->command_buffer,
        params->drawable,
        (uint64_t)texture,
        params->frame_index,
        width,
        height,
        params->sync_interval,
        params->flags);
  }
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}
#else
static NTSTATUS _WMT4ApitraceSessionEnsureOpen(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMT4ApitraceSessionClose(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMT4ApitraceSessionFlush(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMT4ApitraceSessionSealCheckpoint(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMT4ApitraceSetCurrentD3DSequence(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMT4ApitraceCommandBufferBegin(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMT4ApitraceCommandBufferCommit(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMT4ApitracePresentDrawable(void *obj) { (void)obj; return STATUS_SUCCESS; }
#endif

/*
 * Definition from cache.c
 */

NTSTATUS _WMT4CacheReader_alloc_init(void *obj);
NTSTATUS _WMT4CacheReader_get(void *obj);
NTSTATUS _WMT4CacheWriter_alloc_init(void *obj);
NTSTATUS _WMT4CacheWriter_set(void *obj);
NTSTATUS _WMT4SetMetalShaderCachePath(void *obj);
NTSTATUS _MTL4CounterHeap_newTimestampHeap(void *obj);
NTSTATUS _MTL4CounterHeap_resolveCounterRange(void *obj);
NTSTATUS _MTL4CommandBuffer_resolveCounterHeap(void *obj);
NTSTATUS _MTL4TimestampContext_create(void *obj);
NTSTATUS _MTL4TimestampContext_destroy(void *obj);
NTSTATUS _MTL4TimestampContext_writeTimestamp(void *obj);
NTSTATUS _MTLDevice_sizeOfTimestampHeapEntry(void *obj);
static NTSTATUS _DispatchData_copy(void *obj);

const void *__wine_unix_call_funcs[] = {
    &_NSObject_retain,
    &_NSObject_release,
    &_NSArray_object,
    &_NSArray_count,
    &_MTLCopyAllDevices,
    &_MTLDevice_recommendedMaxWorkingSetSize,
    &_MTLDevice_currentAllocatedSize,
    &_MTLDevice_name,
    &_NSString_getCString,
    &_MTLDevice_newCommandQueue,
    &_NSAutoreleasePool_alloc_init,
    &_MTLCommandQueue_commandBuffer,
    &_MTLCommandBuffer_commit,
    &_MTLCommandBuffer_waitUntilCompleted,
    &_MTLCommandBuffer_status,
    &_MTLDevice_newSharedEvent,
    &_MTLSharedEvent_signaledValue,
    &_MTLCommandBuffer_encodeSignalEvent,
    &_MTLDevice_newBuffer,
    &_MTLDevice_newSamplerState,
    &_MTLDevice_newDepthStencilState,
    &_MTLDevice_newTexture,
    &_MTLBuffer_newTexture,
    &_MTLTexture_newTextureView,
    &_MTLDevice_minimumLinearTextureAlignmentForPixelFormat,
    &_MTLDevice_newLibrary,
    &_MTLLibrary_newFunction,
    &_NSString_lengthOfBytesUsingEncoding,
    &_NSObject_description,
    &_MTLDevice_newComputePipelineState,
    &_MTLCommandBuffer_blitCommandEncoder,
    &_MTLCommandBuffer_computeCommandEncoder,
    &_MTLCommandBuffer_renderCommandEncoder,
    &_MTLCommandEncoder_endEncoding,
    &_MTLDevice_newRenderPipelineState,
    &_MTLDevice_newMeshRenderPipelineState,
    &_MTLBlitCommandEncoder_encodeCommands,
    &_MTLComputeCommandEncoder_encodeCommands,
    &_MTLRenderCommandEncoder_encodeCommands,
    &_MTLTexture_pixelFormat,
    &_MTLTexture_width,
    &_MTLTexture_height,
    &_MTLTexture_depth,
    &_MTLTexture_arrayLength,
    &_MTLTexture_mipmapLevelCount,
    &_MTLTexture_replaceRegion,
    &_MTLBuffer_didModifyRange,
    &_MTLCommandBuffer_presentDrawable,
    &_MTLCommandBuffer_presentDrawableAfterMinimumDuration,
    &_MTLDevice_supportsFamily,
    &_MTLDevice_supportsBCTextureCompression,
    &_MTLDevice_supportsTextureSampleCount,
    &_MTLDevice_hasUnifiedMemory,
    &_MTLCaptureManager_sharedCaptureManager,
    &_MTLCaptureManager_startCapture,
    &_MTLCaptureManager_stopCapture,
    &_MTLDevice_newTemporalScaler,
    &_MTLDevice_newSpatialScaler,
    &_MTLCommandBuffer_encodeTemporalScale,
    &_MTLCommandBuffer_encodeSpatialScale,
    &_NSString_string,
    &_NSString_alloc_init,
    &_DeveloperHUDProperties_instance,
    &_DeveloperHUDProperties_addLabel,
    &_DeveloperHUDProperties_updateLabel,
    &_DeveloperHUDProperties_remove,
    &_MetalDrawable_texture,
    &_MetalLayer_nextDrawable,
    &_MTLDevice_supportsFXSpatialScaler,
    &_MTLDevice_supportsFXTemporalScaler,
    &_MetalLayer_setProps,
    &_MetalLayer_getProps,
    &_CreateMetalViewFromHWND,
    &_ReleaseMetalView,
    &thunk_SM50Initialize,
    &thunk_SM50Destroy,
    &thunk_SM50Compile,
    &thunk_SM50GetCompiledBitcode,
    &thunk_SM50DestroyBitcode,
    &thunk_SM50GetErrorMessage,
    &thunk_SM50FreeError,
    &thunk_SM50CompileGeometryPipelineVertex,
    &thunk_SM50CompileGeometryPipelineGeometry,
    &_MTLTexture_usage,
    &thunk_SM50CompileTessellationPipelineHull,
    &thunk_SM50CompileTessellationPipelineDomain,
    &_MTLCommandEncoder_setLabel,
    &_MTLDevice_setShouldMaximizeConcurrentCompilation,
    &thunk_SM50GetArgumentsInfo,
    &thunk_DXILInitialize,
    &thunk_DXILDestroy,
    &thunk_DXILCompile,
    &thunk_DXILGetArgumentsInfo,
    &_MTLCommandBuffer_error,
    &_MTLCommandBuffer_logs,
    &_MTLLogContainer_enumerate,
    &_CGColorSpace_checkColorSpaceSupported,
    &_MetalLayer_setColorSpace,
    &_WMTGetPrimaryDisplayId,
    &_WMTGetSecondaryDisplayId,
    &_WMTGetDisplayDescription,
    &_MetalLayer_getEDRValue,
    &_MTLLibrary_newFunctionWithConstants,
    &_WMTQueryDisplaySetting,
    &_WMTUpdateDisplaySetting,
    &_WMTQueryDisplaySettingForLayer,
    &_MTLCommandBuffer_encodeWaitForEvent,
    &_MTLSharedEvent_signalValue,
    &_MTLSharedEvent_setWin32EventAtValue,
    &_MTLDevice_newFence,
    &_MTLDevice_newEvent,
    &_MTLBuffer_updateContents,
    &_SharedEventListener_create,
    &_SharedEventListener_start,
    &_SharedEventListener_destroy,
    &_WMTGetOSVersion,
    &_MTLDevice_newBinaryArchive,
    &_MTLBinaryArchive_serialize,
    &_DispatchData_alloc_init,
    &_WMT4CacheReader_alloc_init,
    &_WMT4CacheReader_get,
    &_WMT4CacheWriter_alloc_init,
    &_WMT4CacheWriter_set,
    &_WMT4SetMetalShaderCachePath,
    &_MTLDevice_newSharedTexture,
    &_WMT4BootstrapRegister,
    &_WMT4BootstrapLookUp,
    &_MTLSharedEvent_createMachPort,
    &_MTLDevice_newSharedEventWithMachPort,
    &_MTLDevice_registryID,
    &_MTLSharedEvent_waitUntilSignaledValue,
    &_MTLCounterSampleBuffer_newTimestampBuffer,
    &_MTLCounterSampleBuffer_resolveCounterRange,
    &_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers,
    &_MTLCommandBuffer_property,
    &_MTLDevice_newTileRenderPipelineState,
    &_WMT4ApitraceSessionEnsureOpen,
    &_WMT4ApitraceSessionClose,
    &_WMT4ApitraceSetCurrentD3DSequence,
    &_WMT4ApitraceCommandBufferBegin,
    &_WMT4ApitraceCommandBufferCommit,
    &_WMT4ApitracePresentDrawable,
    &_WMT4ApitraceSessionSealCheckpoint,
    &_MTLDevice_supportsPlacementSparse,
    &_MTLDevice_sparseTileSize,
    &_MTLDevice_newPlacementHeap,
    &_MTLDevice_updateSparseTextureMappings,
    &_WMT4ApitraceSessionFlush,
    &_MTL4CounterHeap_newTimestampHeap,
    &_MTL4CounterHeap_resolveCounterRange,
    &_MTL4TimestampContext_create,
    &_MTL4TimestampContext_destroy,
    &_MTL4TimestampContext_writeTimestamp,
    &_MTLDevice_sizeOfTimestampHeapEntry,
    &_MTL4CommandBuffer_resolveCounterHeap,
    &_DispatchData_copy,
    &_MTLDevice_newArgumentTable,
    &_MTL4ArgumentTable_setAddress,
    &_MTL4ArgumentTable_setTexture,
    &_MTL4ArgumentTable_setSamplerState,
    &_MTLDevice_newResidencySet,
    &_MTLCommandQueue_addResidencySet,
    &_MTLCommandQueue_removeResidencySet,
    &_MTLResidencySet_addAllocation,
    &_MTLResidencySet_removeAllocation,
    &_MTLResidencySet_commit,
    &_MTLResidencySet_requestResidency,
    &_MTLDevice_newTextureViewPool,
    &_MTLResourceViewPool_baseResourceID,
    &_MTLResourceViewPool_copyResourceViews,
    &_MTLTextureViewPool_setTextureView,
    &_MTLTextureViewPool_setTextureViewWithDescriptor,
    &_MTLTextureViewPool_setTextureViewFromBuffer,
    &_MTLHeap_newBuffer,
    &_MTLHeap_newTexture,
    &_MTLDevice_heapTextureSizeAndAlign,
    &_MTLCommandBuffer_setDiagnosticInfo,
    &_MTLCommandQueue_updateSparseTextureMappings,
    &_MTLDevice_queryTimestampFrequency,
    &_MTLDevice_sampleTimestamps,
    &_MTLCommandBuffer_registerResource,
};

#ifndef DXMT_NATIVE
const void *__wine_unix_call_wow64_funcs[] = {
    &_NSObject_retain,
    &_NSObject_release,
    &_NSArray_object,
    &_NSArray_count,
    &_MTLCopyAllDevices,
    &_MTLDevice_recommendedMaxWorkingSetSize,
    &_MTLDevice_currentAllocatedSize,
    &_MTLDevice_name,
    &_NSString_getCString,
    &_MTLDevice_newCommandQueue,
    &_NSAutoreleasePool_alloc_init,
    &_MTLCommandQueue_commandBuffer,
    &_MTLCommandBuffer_commit,
    &_MTLCommandBuffer_waitUntilCompleted,
    &_MTLCommandBuffer_status,
    &_MTLDevice_newSharedEvent,
    &_MTLSharedEvent_signaledValue,
    &_MTLCommandBuffer_encodeSignalEvent,
    &_MTLDevice_newBuffer,
    &_MTLDevice_newSamplerState,
    &_MTLDevice_newDepthStencilState,
    &_MTLDevice_newTexture,
    &_MTLBuffer_newTexture,
    &_MTLTexture_newTextureView,
    &_MTLDevice_minimumLinearTextureAlignmentForPixelFormat,
    &_MTLDevice_newLibrary,
    &_MTLLibrary_newFunction,
    &_NSString_lengthOfBytesUsingEncoding,
    &_NSObject_description,
    &_MTLDevice_newComputePipelineState,
    &_MTLCommandBuffer_blitCommandEncoder,
    &_MTLCommandBuffer_computeCommandEncoder,
    &_MTLCommandBuffer_renderCommandEncoder,
    &_MTLCommandEncoder_endEncoding,
    &_MTLDevice_newRenderPipelineState,
    &_MTLDevice_newMeshRenderPipelineState,
    &_MTLBlitCommandEncoder_encodeCommands,
    &_MTLComputeCommandEncoder_encodeCommands,
    &_MTLRenderCommandEncoder_encodeCommands,
    &_MTLTexture_pixelFormat,
    &_MTLTexture_width,
    &_MTLTexture_height,
    &_MTLTexture_depth,
    &_MTLTexture_arrayLength,
    &_MTLTexture_mipmapLevelCount,
    &_MTLTexture_replaceRegion,
    &_MTLBuffer_didModifyRange,
    &_MTLCommandBuffer_presentDrawable,
    &_MTLCommandBuffer_presentDrawableAfterMinimumDuration,
    &_MTLDevice_supportsFamily,
    &_MTLDevice_supportsBCTextureCompression,
    &_MTLDevice_supportsTextureSampleCount,
    &_MTLDevice_hasUnifiedMemory,
    &_MTLCaptureManager_sharedCaptureManager,
    &_MTLCaptureManager_startCapture,
    &_MTLCaptureManager_stopCapture,
    &_MTLDevice_newTemporalScaler,
    &_MTLDevice_newSpatialScaler,
    &_MTLCommandBuffer_encodeTemporalScale,
    &_MTLCommandBuffer_encodeSpatialScale,
    &_NSString_string,
    &_NSString_alloc_init,
    &_DeveloperHUDProperties_instance,
    &_DeveloperHUDProperties_addLabel,
    &_DeveloperHUDProperties_updateLabel,
    &_DeveloperHUDProperties_remove,
    &_MetalDrawable_texture,
    &_MetalLayer_nextDrawable,
    &_MTLDevice_supportsFXSpatialScaler,
    &_MTLDevice_supportsFXTemporalScaler,
    &_MetalLayer_setProps,
    &_MetalLayer_getProps,
    &_CreateMetalViewFromHWND,
    &_ReleaseMetalView,
    &thunk32_SM50Initialize,
    &thunk_SM50Destroy,
    &thunk32_SM50Compile,
    &thunk32_SM50GetCompiledBitcode,
    &thunk_SM50DestroyBitcode,
    &thunk32_SM50GetErrorMessage,
    &thunk_SM50FreeError,
    &thunk32_SM50CompileGeometryPipelineVertex,
    &thunk32_SM50CompileGeometryPipelineGeometry,
    &_MTLTexture_usage,
    &thunk32_SM50CompileTessellationPipelineHull,
    &thunk32_SM50CompileTessellationPipelineDomain,
    &_MTLCommandEncoder_setLabel,
    &_MTLDevice_setShouldMaximizeConcurrentCompilation,
    &thunk32_SM50GetArgumentsInfo,
    &thunk32_DXILInitialize,
    &thunk_DXILDestroy,
    &thunk32_DXILCompile,
    &thunk32_DXILGetArgumentsInfo,
    &_MTLCommandBuffer_error,
    &_MTLCommandBuffer_logs,
    &_MTLLogContainer_enumerate,
    &_CGColorSpace_checkColorSpaceSupported,
    &_MetalLayer_setColorSpace,
    &_WMTGetPrimaryDisplayId,
    &_WMTGetSecondaryDisplayId,
    &_WMTGetDisplayDescription,
    &_MetalLayer_getEDRValue,
    &_MTLLibrary_newFunctionWithConstants,
    &_WMTQueryDisplaySetting,
    &_WMTUpdateDisplaySetting,
    &_WMTQueryDisplaySettingForLayer,
    &_MTLCommandBuffer_encodeWaitForEvent,
    &_MTLSharedEvent_signalValue,
    &_MTLSharedEvent_setWin32EventAtValue,
    &_MTLDevice_newFence,
    &_MTLDevice_newEvent,
    &_MTLBuffer_updateContents,
    &_SharedEventListener_create,
    &_SharedEventListener_start,
    &_SharedEventListener_destroy,
    &_WMTGetOSVersion,
    &_MTLDevice_newBinaryArchive,
    &_MTLBinaryArchive_serialize,
    &_DispatchData_alloc_init,
    &_WMT4CacheReader_alloc_init,
    &_WMT4CacheReader_get,
    &_WMT4CacheWriter_alloc_init,
    &_WMT4CacheWriter_set,
    &_WMT4SetMetalShaderCachePath,
    &_MTLDevice_newSharedTexture,
    &_WMT4BootstrapRegister,
    &_WMT4BootstrapLookUp,
    &_MTLSharedEvent_createMachPort,
    &_MTLDevice_newSharedEventWithMachPort,
    &_MTLDevice_registryID,
    &_MTLSharedEvent_waitUntilSignaledValue,
    &_MTLCounterSampleBuffer_newTimestampBuffer,
    &_MTLCounterSampleBuffer_resolveCounterRange,
    &_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers,
    &_MTLCommandBuffer_property,
    &_MTLDevice_newTileRenderPipelineState,
    &_WMT4ApitraceSessionEnsureOpen,
    &_WMT4ApitraceSessionClose,
    &_WMT4ApitraceSetCurrentD3DSequence,
    &_WMT4ApitraceCommandBufferBegin,
    &_WMT4ApitraceCommandBufferCommit,
    &_WMT4ApitracePresentDrawable,
    &_WMT4ApitraceSessionSealCheckpoint,
    &_MTLDevice_supportsPlacementSparse,
    &_MTLDevice_sparseTileSize,
    &_MTLDevice_newPlacementHeap,
    &_MTLDevice_updateSparseTextureMappings,
    &_WMT4ApitraceSessionFlush,
    &_MTL4CounterHeap_newTimestampHeap,
    &_MTL4CounterHeap_resolveCounterRange,
    &_MTL4TimestampContext_create,
    &_MTL4TimestampContext_destroy,
    &_MTL4TimestampContext_writeTimestamp,
    &_MTLDevice_sizeOfTimestampHeapEntry,
    &_MTL4CommandBuffer_resolveCounterHeap,
    &_DispatchData_copy,
    &_MTLDevice_newArgumentTable,
    &_MTL4ArgumentTable_setAddress,
    &_MTL4ArgumentTable_setTexture,
    &_MTL4ArgumentTable_setSamplerState,
    &_MTLDevice_newResidencySet,
    &_MTLCommandQueue_addResidencySet,
    &_MTLCommandQueue_removeResidencySet,
    &_MTLResidencySet_addAllocation,
    &_MTLResidencySet_removeAllocation,
    &_MTLResidencySet_commit,
    &_MTLResidencySet_requestResidency,
    &_MTLDevice_newTextureViewPool,
    &_MTLResourceViewPool_baseResourceID,
    &_MTLResourceViewPool_copyResourceViews,
    &_MTLTextureViewPool_setTextureView,
    &_MTLTextureViewPool_setTextureViewWithDescriptor,
    &_MTLTextureViewPool_setTextureViewFromBuffer,
    &_MTLHeap_newBuffer,
    &_MTLHeap_newTexture,
    &_MTLDevice_heapTextureSizeAndAlign,
    &_MTLCommandBuffer_setDiagnosticInfo,
    &_MTLCommandQueue_updateSparseTextureMappings,
    &_MTLDevice_queryTimestampFrequency,
    &_MTLDevice_sampleTimestamps,
    &_MTLCommandBuffer_registerResource,
};
#endif
