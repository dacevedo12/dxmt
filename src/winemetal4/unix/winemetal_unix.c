#include <stdatomic.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
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
#import <MetalFX/MetalFX.h>
#import <QuartzCore/QuartzCore.h>
#include "objc/objc-runtime.h"
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

typedef NS_ENUM(uint64_t, DXMTMetal4CommandBufferState) {
  DXMTMetal4CommandBufferStateNotEnqueued = 0,
  DXMTMetal4CommandBufferStateCommitted = 2,
  DXMTMetal4CommandBufferStateCompleted = 4,
  DXMTMetal4CommandBufferStateError = 5,
};

@interface DXMTMetal4CommandBuffer : NSObject
@property(nonatomic, retain) DXMTMetal4CommandQueue *owner;
@property(nonatomic, retain) id<MTL4CommandAllocator> allocator;
@property(nonatomic, retain) id<MTL4CommandBuffer> metal4Buffer;
@property(nonatomic, retain) id<MTLResidencySet> residencySet;
@property(nonatomic, retain) NSMutableArray *pendingWaitEvents;
@property(nonatomic, retain) NSMutableArray *pendingSignalEvents;
@property(nonatomic, retain) id<MTLDrawable> pendingDrawable;
@property(nonatomic, assign) BOOL hasPresentDuration;
@property(nonatomic, assign) double presentDuration;
@property(nonatomic, assign) uint64_t completionValue;
@property(nonatomic, assign) DXMTMetal4CommandBufferState internalStatus;
@property(nonatomic, retain) NSError *feedbackError;
@property(nonatomic, assign) double feedbackGPUStartTime;
@property(nonatomic, assign) double feedbackGPUEndTime;
- (instancetype)initWithQueue:(DXMTMetal4CommandQueue *)queue;
- (id<MTL4CommandBuffer>)commandBuffer;
- (id<MTL4ComputeCommandEncoder>)metal4ComputeEncoder;
- (id<MTL4RenderCommandEncoder>)metal4RenderCommandEncoderWithDescriptor:(MTL4RenderPassDescriptor *)descriptor;
- (void)useResidencyAllocation:(id<MTLAllocation>)allocation;
- (id<MTL4ArgumentTable>)newArgumentTableWithLabel:(NSString *)label;
- (id<MTLBuffer>)newUploadBufferWithBytes:(const void *)bytes length:(NSUInteger)length;
- (void)prepareResidencyForCommit;
- (void)commit;
- (void)waitUntilCompleted;
- (enum WMTCommandBufferStatus)status;
- (NSError *)error;
- (id)logs;
- (double)kernelStartTime;
- (double)kernelEndTime;
- (double)GPUStartTime;
- (double)GPUEndTime;
- (void)encodeSignalEvent:(id<MTLSharedEvent>)event value:(uint64_t)value;
- (void)encodeWaitForEvent:(id<MTLSharedEvent>)event value:(uint64_t)value;
- (void)presentDrawable:(id<MTLDrawable>)drawable;
- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(double)duration;
- (void)dxmtWriteTimestampIntoHeap:(id<MTL4CounterHeap>)heap atIndex:(NSUInteger)index;
@end

@interface DXMTMetal4QueueEvent : NSObject
@property(nonatomic, retain) id<MTLSharedEvent> event;
@property(nonatomic, assign) uint64_t value;
- (instancetype)initWithEvent:(id<MTLSharedEvent>)event value:(uint64_t)value;
@end

@implementation DXMTMetal4QueueEvent
- (instancetype)initWithEvent:(id<MTLSharedEvent>)event value:(uint64_t)value {
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


@interface DXMTMetal4CommandQueue : NSObject
@property(nonatomic, retain) id<MTLDevice> device;
@property(nonatomic, retain) id<MTL4CommandQueue> metal4Queue;
@property(nonatomic, retain) id<MTL4Compiler> compiler;
@property(nonatomic, retain) id<MTLSharedEvent> event;
@property(nonatomic, assign) uint64_t eventValue;
- (instancetype)initWithDevice:(id<MTLDevice>)device maxCommandBufferCount:(uint64_t)maxCommandBufferCount;
- (uint64_t)nextEventValue;
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
  _eventValue = 0;

  if (!_metal4Queue || !_compiler || !_event) {
    [self release];
    return nil;
  }

  dxmt_metal4_register_compiler(device, _compiler);
  return self;
}

- (void)dealloc {
  [_device release];
  [_metal4Queue release];
  [_compiler release];
  [_event release];
  [super dealloc];
}

- (uint64_t)nextEventValue {
  @synchronized(self) {
    return ++_eventValue;
  }
}
@end

@implementation DXMTMetal4CommandBuffer
- (instancetype)initWithQueue:(DXMTMetal4CommandQueue *)queue {
  self = [super init];
  if (!self)
    return nil;

  _owner = [queue retain];
  _allocator = [queue.device newCommandAllocator];
  _metal4Buffer = [queue.device newCommandBuffer];
  [_metal4Buffer beginCommandBufferWithAllocator:_allocator];
  _pendingWaitEvents = [[NSMutableArray alloc] init];
  _pendingSignalEvents = [[NSMutableArray alloc] init];
  MTLResidencySetDescriptor *residencyDesc = [[MTLResidencySetDescriptor alloc] init];
  residencyDesc.initialCapacity = 1024;
  NSError *residencyError = nil;
  _residencySet = [queue.device newResidencySetWithDescriptor:residencyDesc error:&residencyError];
  [residencyDesc release];
  _completionValue = 0;
  _internalStatus = DXMTMetal4CommandBufferStateNotEnqueued;
  _feedbackGPUStartTime = 0.0;
  _feedbackGPUEndTime = 0.0;

  if (!_allocator || !_metal4Buffer || !_pendingWaitEvents || !_pendingSignalEvents || !_residencySet) {
    [self release];
    return nil;
  }

  return self;
}

- (void)dealloc {
  [_pendingDrawable release];
  [_feedbackError release];
  [_pendingWaitEvents release];
  [_pendingSignalEvents release];
  [_residencySet release];
  [_metal4Buffer release];
  [_allocator release];
  [_owner release];
  [super dealloc];
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

- (void)useResidencyAllocation:(id<MTLAllocation>)allocation {
  if (!allocation)
    return;
  [_residencySet addAllocation:allocation];
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
  [self useResidencyAllocation:(id<MTLAllocation>)buffer];
  return buffer;
}

- (void)prepareResidencyForCommit {
  [_residencySet commit];
  [_residencySet requestResidency];
  [_metal4Buffer useResidencySet:_residencySet];
}

- (void)commit {
  if (_internalStatus != DXMTMetal4CommandBufferStateNotEnqueued)
    return;

  for (DXMTMetal4QueueEvent *wait in _pendingWaitEvents)
    [_owner.metal4Queue waitForEvent:wait.event value:wait.value];

  [self prepareResidencyForCommit];
  [_metal4Buffer endCommandBuffer];

  _completionValue = [_owner nextEventValue];
  MTL4CommitOptions *options = [[MTL4CommitOptions alloc] init];

  id<MTL4CommandBuffer> commandBuffers[1] = {_metal4Buffer};
  _internalStatus = DXMTMetal4CommandBufferStateCommitted;
  [_owner.metal4Queue commit:commandBuffers count:1 options:options];
  [options release];

  if (_pendingDrawable) {
    [_owner.metal4Queue signalDrawable:_pendingDrawable];
    if (_hasPresentDuration)
      [_pendingDrawable presentAfterMinimumDuration:_presentDuration];
    else
      [_pendingDrawable present];
  }

  for (DXMTMetal4QueueEvent *signal in _pendingSignalEvents)
    [_owner.metal4Queue signalEvent:signal.event value:signal.value];
  [_owner.metal4Queue signalEvent:_owner.event value:_completionValue];

}

- (void)waitUntilCompleted {
  if (_internalStatus == DXMTMetal4CommandBufferStateNotEnqueued)
    [self commit];
  if (_completionValue) {
    [_owner.event waitUntilSignaledValue:_completionValue timeoutMS:UINT64_MAX];
    if (_internalStatus == DXMTMetal4CommandBufferStateCommitted)
      _internalStatus = DXMTMetal4CommandBufferStateCompleted;
  }
}

- (enum WMTCommandBufferStatus)status {
  return (enum WMTCommandBufferStatus)_internalStatus;
}

- (NSError *)error {
  return _feedbackError;
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
  return _feedbackGPUStartTime;
}

- (double)GPUEndTime {
  return _feedbackGPUEndTime;
}

- (void)encodeSignalEvent:(id<MTLSharedEvent>)event value:(uint64_t)value {
  DXMTMetal4QueueEvent *op = [[DXMTMetal4QueueEvent alloc] initWithEvent:event value:value];
  [_pendingSignalEvents addObject:op];
  [op release];
}

- (void)encodeWaitForEvent:(id<MTLSharedEvent>)event value:(uint64_t)value {
  DXMTMetal4QueueEvent *op = [[DXMTMetal4QueueEvent alloc] initWithEvent:event value:value];
  [_pendingWaitEvents addObject:op];
  [op release];
}

- (void)presentDrawable:(id<MTLDrawable>)drawable {
  [_pendingDrawable release];
  _pendingDrawable = [drawable retain];
  _hasPresentDuration = NO;
}

- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(double)duration {
  [_pendingDrawable release];
  _pendingDrawable = [drawable retain];
  _presentDuration = duration;
  _hasPresentDuration = YES;
}

- (void)dxmtWriteTimestampIntoHeap:(id<MTL4CounterHeap>)heap atIndex:(NSUInteger)index {
  [_metal4Buffer writeTimestampIntoHeap:heap atIndex:index];
}
@end

void
DXMTMetal4CommandBuffer_writeTimestampIntoHeap(obj_handle_t cmdbuf, obj_handle_t heap, uint64_t index) {
  [(DXMTMetal4CommandBuffer *)cmdbuf dxmtWriteTimestampIntoHeap:(id<MTL4CounterHeap>)heap
                                                        atIndex:(NSUInteger)index];
}

struct dxmt_metal4_argument_binding {
  id<MTLBuffer> buffer;
  uint64_t offset;
};

struct dxmt_metal4_argument_state {
  id<MTL4ArgumentTable> table;
  DXMTMetal4CommandBuffer *owner;
  struct dxmt_metal4_argument_binding buffers[31];
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

static void
dxmt_metal4_argument_set_buffer(
    struct dxmt_metal4_argument_state *state,
    id<MTLBuffer> buffer,
    uint64_t offset,
    uint8_t index) {
  if (!state->table || index >= 31)
    return;
  state->buffers[index].buffer = buffer;
  state->buffers[index].offset = offset;
  [state->owner useResidencyAllocation:(id<MTLAllocation>)buffer];
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
  [state->owner useResidencyAllocation:(id<MTLAllocation>)texture];
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

static NSUInteger
dxmt_metal4_index_buffer_length(id<MTLBuffer> buffer, uint64_t offset) {
  if (!buffer)
    return 0;
  NSUInteger length = [buffer length];
  if (offset >= length)
    return 0;
  return length - (NSUInteger)offset;
}

static MTLGPUAddress
dxmt_metal4_buffer_address(obj_handle_t buffer, uint64_t offset) {
  id<MTLBuffer> metal_buffer = (id<MTLBuffer>)buffer;
  return metal_buffer ? [metal_buffer gpuAddress] + offset : 0;
}

struct dxmt_metal4_render_argument_state {
  struct dxmt_metal4_argument_state vertex;
  struct dxmt_metal4_argument_state fragment;
  struct dxmt_metal4_argument_state object;
  struct dxmt_metal4_argument_state mesh;
  struct dxmt_metal4_argument_state tile;
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
  [encoder setArgumentTable:state->vertex.table atStages:MTLRenderStageVertex];
  [encoder setArgumentTable:state->fragment.table atStages:MTLRenderStageFragment];
  [encoder setArgumentTable:state->object.table atStages:MTLRenderStageObject];
  [encoder setArgumentTable:state->mesh.table atStages:MTLRenderStageMesh];
  [encoder setArgumentTable:state->tile.table atStages:MTLRenderStageTile];
}

@interface DXMTMetal4ComputeEncoderState : NSObject {
@public
  struct dxmt_metal4_argument_state arguments;
  MTLSize threadgroupSize;
}
- (instancetype)initWithOwner:(DXMTMetal4CommandBuffer *)owner;
@end

@implementation DXMTMetal4ComputeEncoderState
- (instancetype)initWithOwner:(DXMTMetal4CommandBuffer *)owner {
  self = [super init];
  if (self) {
    dxmt_metal4_argument_state_init(&arguments, owner, @"DXMT4 Compute Arguments");
    threadgroupSize = MTLSizeMake(0, 0, 0);
  }
  return self;
}

- (void)dealloc {
  dxmt_metal4_argument_state_destroy(&arguments);
  [super dealloc];
}
@end

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
};

@interface DXMT4ApitraceCommandBufferState : NSObject
@property(nonatomic, assign) uint64_t beginSequence;
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

@interface DXMT4ApitraceEncoderState : NSObject
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

static bool
dxmt_apitrace_truthy_env_value(const char *value) {
  if (!value || !value[0])
    return false;
  return !strcmp(value, "1") || !strcmp(value, "true") || !strcmp(value, "yes") || !strcmp(value, "trace");
}

static bool
dxmt_apitrace_runtime_enabled(void) {
  const char *enabled = getenv("DXMT_APITRACE_ENBALED");
  if (!enabled || !enabled[0])
    enabled = getenv("DXMT_APITRACE_ENABLED");
  return dxmt_apitrace_truthy_env_value(enabled);
}

static apitrace_metal_session_t *
dxmt_apitrace_ensure_session_locked(void);

static const char *
dxmt_apitrace_bundle_root(void) {
  const char *bundle_root = getenv("APITRACE_TRACE_BUNDLE");
  if (bundle_root && bundle_root[0])
    return bundle_root;

  static bool warned = false;
  if (!warned) {
    warned = true;
    fprintf(stderr,
            "warn:  DXMT apitrace: APITRACE_TRACE_BUNDLE not set; PE side must "
            "initialize bundle root before opening unix session\n");
  }
  return NULL;
}

static bool
dxmt_apitrace_verbose_enabled(void) {
  static bool initialized = false;
  static bool enabled = false;
  if (!initialized) {
    const char *value = getenv("APITRACE_METAL_VERBOSE");
    enabled = dxmt_apitrace_truthy_env_value(value);
    initialized = true;
  }
  return enabled;
}

static void
dxmt_apitrace_log(const char *message, uint64_t arg0, uint64_t arg1) {
  if (!dxmt_apitrace_verbose_enabled())
    return;
  fprintf(stderr, "info:  DXMT apitrace: %s arg0=%llu arg1=%llu\n",
          message,
          (unsigned long long)arg0,
          (unsigned long long)arg1);
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
  if (dxmt_apitrace_session)
    return dxmt_apitrace_session;

  if (!dxmt_apitrace_runtime_enabled())
    return NULL;

  const char *bundle_root = dxmt_apitrace_bundle_root();
  if (!bundle_root)
    return NULL;
  dxmt_apitrace_session = apitrace_metal_session_open(bundle_root);
  if (!dxmt_apitrace_session)
    return NULL;

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

  NSString *address_payload = dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_buffer_gpu_address",
    @"buffer_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)buffer_handle],
    @"gpu_address" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)[buffer gpuAddress]],
  });
  apitrace_metal_object_metadata(session, buffer_handle, address_payload.UTF8String);
}

static void
dxmt_apitrace_record_render_commands(apitrace_metal_session_t *session, obj_handle_t encoder, const struct wmtcmd_base *head) {
  DXMT4ApitraceEncoderState *state = [dxmt_apitrace_encoders objectForKey:dxmt_apitrace_key(encoder)];
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
      break;
    }
    case WMTRenderCommandSetFragmentBuffer: {
      const struct wmtcmd_render_setbuffer *cmd = (const struct wmtcmd_render_setbuffer *)base;
      apitrace_metal_set_fragment_buffer(session, encoder, cmd->buffer, cmd->offset, cmd->index);
      break;
    }
    case WMTRenderCommandSetVertexBufferOffset: {
      const struct wmtcmd_render_setbufferoffset *cmd = (const struct wmtcmd_render_setbufferoffset *)base;
      apitrace_metal_set_vertex_buffer_offset(session, encoder, cmd->offset, cmd->index);
      break;
    }
    case WMTRenderCommandSetFragmentBufferOffset: {
      const struct wmtcmd_render_setbufferoffset *cmd = (const struct wmtcmd_render_setbufferoffset *)base;
      apitrace_metal_set_fragment_buffer_offset(session, encoder, cmd->offset, cmd->index);
      break;
    }
    case WMTRenderCommandSetFragmentTexture: {
      const struct wmtcmd_render_settexture *cmd = (const struct wmtcmd_render_settexture *)base;
      apitrace_metal_set_fragment_texture(session, encoder, cmd->texture, cmd->index);
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
      break;
    }
    case WMTRenderCommandDrawIndexedIndirect: {
      const struct wmtcmd_render_draw_indexed_indirect *cmd = (const struct wmtcmd_render_draw_indexed_indirect *)base;
      apitrace_metal_draw_indexed_primitives_indirect(
          session, encoder, cmd->primitive_type, cmd->index_type, cmd->index_buffer, cmd->index_buffer_offset,
          cmd->indirect_args_buffer, cmd->indirect_args_offset);
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
  struct WMTSize threadgroup_size = {1, 1, 1};
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
      break;
    }
    case WMTComputeCommandSetBufferOffset: {
      const struct wmtcmd_compute_setbufferoffset *cmd = (const struct wmtcmd_compute_setbufferoffset *)base;
      apitrace_metal_set_compute_buffer_offset(session, encoder, cmd->offset, cmd->index);
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
  for (const struct wmtcmd_base *base = head; base; base = (const struct wmtcmd_base *)base->next.ptr) {
    switch ((enum WMTBlitCommandType)base->type) {
    case WMTBlitCommandCopyFromBufferToBuffer: {
      dxmt_apitrace_begin_blit_encoder_if_needed(session, encoder, state);
      dxmt_apitrace_flush_blit_ops(session, encoder, state);
      dxmt_apitrace_flush_command_buffer_blit_batch(session, state.commandBuffer);
      const struct wmtcmd_blit_copy_from_buffer_to_buffer *cmd = (const struct wmtcmd_blit_copy_from_buffer_to_buffer *)base;
      apitrace_metal_copy_buffer(
          session, encoder, cmd->src, cmd->src_offset, cmd->dst, cmd->dst_offset, cmd->copy_length);
      break;
    }
    case WMTBlitCommandCopyFromBufferToTexture: {
      dxmt_apitrace_begin_blit_encoder_if_needed(session, encoder, state);
      dxmt_apitrace_flush_blit_ops(session, encoder, state);
      dxmt_apitrace_flush_command_buffer_blit_batch(session, state.commandBuffer);
      const struct wmtcmd_blit_copy_from_buffer_to_texture *cmd = (const struct wmtcmd_blit_copy_from_buffer_to_texture *)base;
      NSString *payload = dxmt_apitrace_copy_buffer_to_texture_json(
          cmd->src, cmd->src_offset, cmd->bytes_per_row, cmd->bytes_per_image, &cmd->size, cmd->dst, cmd->slice,
          cmd->level, &cmd->origin);
      apitrace_metal_copy_buffer_to_texture(session, encoder, cmd->src, cmd->dst, payload.UTF8String);
      break;
    }
    case WMTBlitCommandCopyFromTextureToTexture: {
      const struct wmtcmd_blit_copy_from_texture_to_texture *cmd = (const struct wmtcmd_blit_copy_from_texture_to_texture *)base;
      if (!state.blitOps.count) {
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
      [state.blitOps addObject:@{
        @"op" : @"fill_buffer",
        @"buffer_id" : @(cmd->buffer),
        @"range_start" : @(cmd->offset),
        @"range_length" : @(cmd->length),
        @"value" : @(cmd->value),
      }];
      break;
    }
    case WMTBlitCommandWaitForFence:
    case WMTBlitCommandUpdateFence: {
      const struct wmtcmd_blit_fence_op *cmd = (const struct wmtcmd_blit_fence_op *)base;
      uint64_t fence = cmd->fence;
      if (base->type == WMTBlitCommandWaitForFence)
        [state.blitWaitFences appendBytes:&fence length:sizeof(fence)];
      else
        [state.blitUpdateFences appendBytes:&fence length:sizeof(fence)];
      break;
    }
    case WMTBlitCommandCopyFromTextureToBuffer:
    case WMTBlitCommandGenerateMipmaps:
    case WMTBlitCommandResolveCounters:
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
  if (!session || !dxmt_apitrace_current_d3d_sequence)
    return;
  if (![dxmt_apitrace_command_buffers objectForKey:dxmt_apitrace_key(command_buffer)])
    return;

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
  }

  DXMT4ApitraceEncoderState *state = [[DXMT4ApitraceEncoderState alloc] init];
  state.beginSequence = begin_sequence;
  state.commandBuffer = command_buffer;
  state.d3dSequence = dxmt_apitrace_current_d3d_sequence;
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
_MTLCommandBuffer_commit(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(DXMTMetal4CommandBuffer *)params->handle commit];
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
  [(DXMTMetal4CommandBuffer *)params->handle encodeSignalEvent:(id<MTLSharedEvent>)params->arg0 value:params->arg1];
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
      NSString *address_payload = dxmt_apitrace_json_string(@{
        @"kind" : @"dxmt_buffer_gpu_address",
        @"buffer_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->ret],
        @"gpu_address" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)[buffer gpuAddress]],
      });
      apitrace_metal_object_metadata(dxmt_apitrace_session, params->ret, address_payload.UTF8String);
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
_MTLDevice_newPlacementHeap(void *obj) {
  struct unixcall_mtldevice_newplacementheap *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTPlacementHeapInfo *info = params->info.ptr;
  params->ret = 0;
  if (!info || !info->size)
    return STATUS_SUCCESS;

  if (@available(macOS 26.0, *)) {
    MTLHeapDescriptor *desc = [[MTLHeapDescriptor alloc] init];
    desc.type = MTLHeapTypePlacement;
    desc.size = info->size;
    desc.resourceOptions = (MTLResourceOptions)info->options;
    desc.maxCompatiblePlacementSparsePageSize = MTLSparsePageSize64;
    params->ret = (obj_handle_t)[device newHeapWithDescriptor:desc];
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
    if (!queue || !event) {
      [queue release];
      [event release];
      return STATUS_SUCCESS;
    }

    MTL4UpdateSparseTextureMappingOperation *mtl_ops =
        calloc((size_t)params->operation_count, sizeof(*mtl_ops));
    if (!mtl_ops) {
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
      mtl_ops[i].heapOffset = src->heap_offset;
    }

    [queue updateTextureMappings:texture
                            heap:heap
                      operations:mtl_ops
                           count:(NSUInteger)params->operation_count];
    [queue signalEvent:event value:1];
    params->ret = [event waitUntilSignaledValue:1 timeoutMS:UINT64_MAX] ? 1 : 0;

    free(mtl_ops);
    [queue release];
    [event release];
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
  info->gpu_resource_id = [ret gpuResourceID]._impl;
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

static NTSTATUS
_MTLTexture_newTextureView(void *obj) {
  struct unixcall_mtltexture_newtextureview *params = obj;
  id<MTLTexture> texture = (id<MTLTexture>)params->texture;

  id<MTLTexture> ret = [texture
      newTextureViewWithPixelFormat:to_metal_pixel_format(params->format)
                        textureType:(MTLTextureType)params->texture_type
                             levels:NSMakeRange(params->level_start, params->level_count)
                             slices:NSMakeRange(params->slice_start, params->slice_count)
                            swizzle:to_metal_swizzle(params->swizzle, params->format)];
  params->ret = (obj_handle_t)ret;
  params->gpu_resource_id = [ret gpuResourceID]._impl;
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
  params->ret_pso =
      (obj_handle_t)[device newComputePipelineStateWithDescriptor:descriptor options:options reflection:nil error:&err];
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
        DXMTApitraceEncoderKindBlit,
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

static NTSTATUS
_MTLCommandBuffer_renderCommandEncoder(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  struct WMTRenderPassInfo *info = (struct WMTRenderPassInfo *)params->arg;
  MTL4RenderPassDescriptor *descriptor = [[MTL4RenderPassDescriptor alloc] init];
  for (unsigned i = 0; i < 8; i++) {
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
    descriptor.depthAttachment.clearDepth = info->depth.clear_depth;
    descriptor.depthAttachment.depthPlane = info->depth.depth_plane;
    descriptor.depthAttachment.level = info->depth.level;
    descriptor.depthAttachment.slice = info->depth.slice;
    descriptor.depthAttachment.texture = (id<MTLTexture>)info->depth.texture;
    descriptor.depthAttachment.loadAction = (MTLLoadAction)info->depth.load_action;
    descriptor.depthAttachment.storeAction = (MTLStoreAction)info->depth.store_action;
  }

  if (info->stencil.texture) {
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

  params->ret = (obj_handle_t)[(DXMTMetal4CommandBuffer *)params->handle metal4RenderCommandEncoderWithDescriptor:descriptor];
  DXMTMetal4CommandBuffer *owner = (DXMTMetal4CommandBuffer *)params->handle;
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

  if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
    descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                    count:info->num_binary_archives_for_lookup];
  NSError *err = NULL;
  MTLPipelineOption options =
      info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithDescriptor:descriptor
                                                                                              options:options
                                                                                           reflection:nil
                                                                                                error:&err];
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
  params->ret_pso = (obj_handle_t)[device newRenderPipelineStateWithMeshDescriptor:descriptor
                                                                           options:options
                                                                        reflection:nil
                                                                             error:&err];
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
  DXMTMetal4CommandBuffer *owner = dxmt_metal4_encoder_owner(params->encoder);
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
  bool has_prior_blit_work = false;
  while (next) {
    switch ((enum WMTBlitCommandType)next->type) {
    default:
      assert(!next->type && "unhandled blit command type");
      break;
    case WMTBlitCommandCopyFromBufferToBuffer: {
      struct wmtcmd_blit_copy_from_buffer_to_buffer *body = (struct wmtcmd_blit_copy_from_buffer_to_buffer *)next;
      [owner useResidencyAllocation:(id<MTLAllocation>)body->src];
      [owner useResidencyAllocation:(id<MTLAllocation>)body->dst];
      if (has_prior_blit_work)
        [encoder barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
	      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
	                 sourceOffset:body->src_offset
	                     toBuffer:(id<MTLBuffer>)body->dst
	            destinationOffset:body->dst_offset
	                         size:body->copy_length];
      has_prior_blit_work = true;
      break;
    }
    case WMTBlitCommandCopyFromBufferToTexture: {
      struct wmtcmd_blit_copy_from_buffer_to_texture *body = (struct wmtcmd_blit_copy_from_buffer_to_texture *)next;
      [owner useResidencyAllocation:(id<MTLAllocation>)body->src];
      [owner useResidencyAllocation:(id<MTLAllocation>)body->dst];
      if (has_prior_blit_work)
        [encoder barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
            sourceBytesPerRow:body->bytes_per_row
          sourceBytesPerImage:body->bytes_per_image
	                   sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
	                    toTexture:(id<MTLTexture>)body->dst
	             destinationSlice:body->slice
	             destinationLevel:body->level
	            destinationOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)];
      has_prior_blit_work = true;
      break;
    }
    case WMTBlitCommandCopyFromTextureToBuffer: {
      struct wmtcmd_blit_copy_from_texture_to_buffer *body = (struct wmtcmd_blit_copy_from_texture_to_buffer *)next;
      [owner useResidencyAllocation:(id<MTLAllocation>)body->src];
      [owner useResidencyAllocation:(id<MTLAllocation>)body->dst];
      if (has_prior_blit_work)
        [encoder barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                       sourceSlice:body->slice
                       sourceLevel:body->level
                      sourceOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
	                        sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
	                          toBuffer:(id<MTLBuffer>)body->dst
	                 destinationOffset:body->offset
	            destinationBytesPerRow:body->bytes_per_row
	          destinationBytesPerImage:body->bytes_per_image];
      has_prior_blit_work = true;
      break;
    }
    case WMTBlitCommandCopyFromTextureToTexture: {
      struct wmtcmd_blit_copy_from_texture_to_texture *body = (struct wmtcmd_blit_copy_from_texture_to_texture *)next;
      [owner useResidencyAllocation:(id<MTLAllocation>)body->src];
      [owner useResidencyAllocation:(id<MTLAllocation>)body->dst];
      if (has_prior_blit_work)
        [encoder barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                   sourceSlice:body->src_slice
                   sourceLevel:body->src_level
                  sourceOrigin:MTLOriginMake(body->src_origin.x, body->src_origin.y, body->src_origin.z)
	                    sourceSize:MTLSizeMake(body->src_size.width, body->src_size.height, body->src_size.depth)
	                     toTexture:(id<MTLTexture>)body->dst
	              destinationSlice:body->dst_slice
	              destinationLevel:body->dst_level
	             destinationOrigin:MTLOriginMake(body->dst_origin.x, body->dst_origin.y, body->dst_origin.z)];
      has_prior_blit_work = true;
      break;
    }
    case WMTBlitCommandGenerateMipmaps: {
      struct wmtcmd_blit_generate_mipmaps *body = (struct wmtcmd_blit_generate_mipmaps *)next;
      [owner useResidencyAllocation:(id<MTLAllocation>)body->texture];
      if (has_prior_blit_work)
        [encoder barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
      [encoder generateMipmapsForTexture:(id<MTLTexture>)body->texture];
      has_prior_blit_work = true;
      break;
    }
    case WMTBlitCommandUpdateFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [(id)encoder barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
      [(id)encoder updateFence:(id<MTLFence>)body->fence afterEncoderStages:MTLStageBlit];
      break;
    }
    case WMTBlitCommandWaitForFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [(id)encoder waitForFence:(id<MTLFence>)body->fence beforeEncoderStages:MTLStageBlit];
      [(id)encoder barrierAfterQueueStages:MTLStageBlit beforeStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
      break;
    }
    case WMTBlitCommandFillBuffer: {
      struct wmtcmd_blit_fillbuffer *body = (struct wmtcmd_blit_fillbuffer *)next;
      [owner useResidencyAllocation:(id<MTLAllocation>)body->buffer];
      if (has_prior_blit_work)
        [encoder barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
      [encoder fillBuffer:(id<MTLBuffer>)body->buffer range:NSMakeRange(body->offset, body->length) value:body->value];
      has_prior_blit_work = true;
      break;
    }
    case WMTBlitCommandResolveCounters: {
      assert(!"MTLCounterSampleBuffer resolve is not supported by winemetal4 blit emulation");
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
      [encoder setArgumentTable:args->table];
      [encoder dispatchThreadgroups:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
              threadsPerThreadgroup:state->threadgroupSize];
      break;
    }
    case WMTComputeCommandDispatchThreads: {
      struct wmtcmd_compute_dispatch *body = (struct wmtcmd_compute_dispatch *)next;
      [encoder setArgumentTable:args->table];
      [encoder dispatchThreads:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
          threadsPerThreadgroup:state->threadgroupSize];
      break;
    }
    case WMTComputeCommandDispatchIndirect: {
      struct wmtcmd_compute_dispatch_indirect *body = (struct wmtcmd_compute_dispatch_indirect *)next;
      [owner useResidencyAllocation:(id<MTLAllocation>)body->indirect_args_buffer];
      [encoder setArgumentTable:args->table];
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
    case WMTComputeCommandUseResource: {
      struct wmtcmd_compute_useresource *body = (struct wmtcmd_compute_useresource *)next;
      (void)body->usage;
      [owner useResidencyAllocation:(id<MTLAllocation>)body->resource];
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
      [(id)encoder barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
      [(id)encoder updateFence:(id<MTLFence>)body->fence afterEncoderStages:MTLStageDispatch];
      break;
    }
    case WMTComputeCommandWaitForFence: {
      struct wmtcmd_compute_fence_op *body = (struct wmtcmd_compute_fence_op *)next;
      [(id)encoder waitForFence:(id<MTLFence>)body->fence beforeEncoderStages:MTLStageDispatch];
      [(id)encoder barrierAfterQueueStages:MTLStageDispatch beforeStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
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
      [owner useResidencyAllocation:(id<MTLAllocation>)body->resource];
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
      [owner useResidencyAllocation:(id<MTLAllocation>)index_buffer];
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
      [owner useResidencyAllocation:(id<MTLAllocation>)body->indirect_args_buffer];
      dxmt_metal4_render_set_argument_tables(encoder, args);
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
                indirectBuffer:dxmt_metal4_buffer_address(body->indirect_args_buffer, body->indirect_args_offset)];
      break;
    }
    case WMTRenderCommandDrawIndexedIndirect: {
      struct wmtcmd_render_draw_indexed_indirect *body = (struct wmtcmd_render_draw_indexed_indirect *)next;
      id<MTLBuffer> index_buffer = (id<MTLBuffer>)body->index_buffer;
      [owner useResidencyAllocation:(id<MTLAllocation>)index_buffer];
      [owner useResidencyAllocation:(id<MTLAllocation>)body->indirect_args_buffer];
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
      [owner useResidencyAllocation:(id<MTLAllocation>)body->indirect_args_buffer];
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
      [owner useResidencyAllocation:(id<MTLAllocation>)body->dispatch_args_buffer];
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
      [owner useResidencyAllocation:(id<MTLAllocation>)body->dispatch_args_buffer];
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
      [owner useResidencyAllocation:(id<MTLAllocation>)body->dispatch_args_buffer];
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
      [owner useResidencyAllocation:(id<MTLAllocation>)body->dispatch_args_buffer];
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
      [(id)encoder barrierAfterStages:stages beforeQueueStages:stages visibilityOptions:MTL4VisibilityOptionDevice];
      [(id)encoder updateFence:(id<MTLFence>)body->fence afterEncoderStages:stages];
      break;
    }
    case WMTRenderCommandWaitForFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      MTLStages stages = dxmt_metal4_render_stages(body->stages);
      [(id)encoder waitForFence:(id<MTLFence>)body->fence beforeEncoderStages:stages];
      [(id)encoder barrierAfterQueueStages:stages beforeStages:stages visibilityOptions:MTL4VisibilityOptionDevice];
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
  [wrapper useResidencyAllocation:(id<MTLAllocation>)params->color];
  [wrapper useResidencyAllocation:(id<MTLAllocation>)params->output];
  [wrapper useResidencyAllocation:(id<MTLAllocation>)params->depth];
  [wrapper useResidencyAllocation:(id<MTLAllocation>)params->motion];
  [wrapper useResidencyAllocation:(id<MTLAllocation>)params->exposure];
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
  [wrapper useResidencyAllocation:(id<MTLAllocation>)params->color];
  [wrapper useResidencyAllocation:(id<MTLAllocation>)params->output];
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
  [(DXMTMetal4CommandBuffer *)params->handle encodeWaitForEvent:(id<MTLSharedEvent>)params->arg0 value:params->arg1];
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
_WMTBootstrapRegister(void *obj) {
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
_WMTBootstrapLookUp(void *obj) {
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
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithTileDescriptor:descriptor
                                                                                                  options:options
                                                                                               reflection:nil
                                                                                                    error:&err];
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
_WMTApitraceSessionEnsureOpen(void *obj) {
  (void)obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  (void)dxmt_apitrace_ensure_session_locked();
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTApitraceSessionClose(void *obj) {
  (void)obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  dxmt_apitrace_close_session_locked();
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTApitraceSessionFlush(void *obj) {
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
_WMTApitraceSessionSealCheckpoint(void *obj) {
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
_WMTApitraceSetCurrentD3DSequence(void *obj) {
  struct unixcall_generic_obj_uint64_noret *params = obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  dxmt_apitrace_current_d3d_sequence = params->arg;
  if (dxmt_apitrace_ensure_session_locked()) {
    apitrace_metal_set_current_d3d_sequence(dxmt_apitrace_session, params->arg);
  }
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTApitraceCommandBufferBegin(void *obj) {
  struct unixcall_apitrace_command_buffer_begin *params = obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  if (dxmt_apitrace_ensure_session_locked()) {
    uint64_t begin_sequence =
        apitrace_metal_command_buffer_begin(dxmt_apitrace_session, params->command_buffer, params->frame_id, "");
    DXMT4ApitraceCommandBufferState *state = [[DXMT4ApitraceCommandBufferState alloc] init];
    state.beginSequence = begin_sequence;
    [dxmt_apitrace_command_buffers setObject:state forKey:dxmt_apitrace_key(params->command_buffer)];
    [state release];
  }
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTApitraceCommandBufferCommit(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  DXMT4ApitraceCommandBufferState *state = [dxmt_apitrace_command_buffers objectForKey:dxmt_apitrace_key(params->handle)];
  if (dxmt_apitrace_session && state) {
    dxmt_apitrace_flush_command_buffer_blit_batch(dxmt_apitrace_session, params->handle);
    apitrace_metal_command_buffer_commit(dxmt_apitrace_session, params->handle);
    [dxmt_apitrace_command_buffers removeObjectForKey:dxmt_apitrace_key(params->handle)];
    apitrace_metal_set_current_d3d_sequence(dxmt_apitrace_session, 0);
    dxmt_apitrace_current_d3d_sequence = 0;
  }
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTApitracePresentDrawable(void *obj) {
  struct unixcall_apitrace_present *params = obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  if (dxmt_apitrace_ensure_session_locked()) {
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
static NTSTATUS _WMTApitraceSessionEnsureOpen(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMTApitraceSessionClose(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMTApitraceSessionFlush(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMTApitraceSessionSealCheckpoint(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMTApitraceSetCurrentD3DSequence(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMTApitraceCommandBufferBegin(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMTApitraceCommandBufferCommit(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMTApitracePresentDrawable(void *obj) { (void)obj; return STATUS_SUCCESS; }
#endif

/*
 * Definition from cache.c
 */

NTSTATUS _CacheReader_alloc_init(void *obj);
NTSTATUS _CacheReader_get(void *obj);
NTSTATUS _CacheWriter_alloc_init(void *obj);
NTSTATUS _CacheWriter_set(void *obj);
NTSTATUS _WMTSetMetalShaderCachePath(void *obj);
NTSTATUS _MTL4CounterHeap_newTimestampHeap(void *obj);
NTSTATUS _MTL4CounterHeap_resolveCounterRange(void *obj);
NTSTATUS _MTL4TimestampContext_create(void *obj);
NTSTATUS _MTL4TimestampContext_destroy(void *obj);
NTSTATUS _MTL4TimestampContext_writeTimestamp(void *obj);
NTSTATUS _MTLDevice_sizeOfTimestampHeapEntry(void *obj);

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
    &_CacheReader_alloc_init,
    &_CacheReader_get,
    &_CacheWriter_alloc_init,
    &_CacheWriter_set,
    &_WMTSetMetalShaderCachePath,
    &_MTLDevice_newSharedTexture,
    &_WMTBootstrapRegister,
    &_WMTBootstrapLookUp,
    &_MTLSharedEvent_createMachPort,
    &_MTLDevice_newSharedEventWithMachPort,
    &_MTLDevice_registryID,
    &_MTLSharedEvent_waitUntilSignaledValue,
    &_MTLCounterSampleBuffer_newTimestampBuffer,
    &_MTLCounterSampleBuffer_resolveCounterRange,
    &_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers,
    &_MTLCommandBuffer_property,
    &_MTLDevice_newTileRenderPipelineState,
    &_WMTApitraceSessionEnsureOpen,
    &_WMTApitraceSessionClose,
    &_WMTApitraceSetCurrentD3DSequence,
    &_WMTApitraceCommandBufferBegin,
    &_WMTApitraceCommandBufferCommit,
    &_WMTApitracePresentDrawable,
    &_WMTApitraceSessionSealCheckpoint,
    &_MTLDevice_supportsPlacementSparse,
    &_MTLDevice_sparseTileSize,
    &_MTLDevice_newPlacementHeap,
    &_MTLDevice_updateSparseTextureMappings,
    &_WMTApitraceSessionFlush,
    &_MTL4CounterHeap_newTimestampHeap,
    &_MTL4CounterHeap_resolveCounterRange,
    &_MTL4TimestampContext_create,
    &_MTL4TimestampContext_destroy,
    &_MTL4TimestampContext_writeTimestamp,
    &_MTLDevice_sizeOfTimestampHeapEntry,
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
    &_CacheReader_alloc_init,
    &_CacheReader_get,
    &_CacheWriter_alloc_init,
    &_CacheWriter_set,
    &_WMTSetMetalShaderCachePath,
    &_MTLDevice_newSharedTexture,
    &_WMTBootstrapRegister,
    &_WMTBootstrapLookUp,
    &_MTLSharedEvent_createMachPort,
    &_MTLDevice_newSharedEventWithMachPort,
    &_MTLDevice_registryID,
    &_MTLSharedEvent_waitUntilSignaledValue,
    &_MTLCounterSampleBuffer_newTimestampBuffer,
    &_MTLCounterSampleBuffer_resolveCounterRange,
    &_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers,
    &_MTLCommandBuffer_property,
    &_MTLDevice_newTileRenderPipelineState,
    &_WMTApitraceSessionEnsureOpen,
    &_WMTApitraceSessionClose,
    &_WMTApitraceSetCurrentD3DSequence,
    &_WMTApitraceCommandBufferBegin,
    &_WMTApitraceCommandBufferCommit,
    &_WMTApitracePresentDrawable,
    &_WMTApitraceSessionSealCheckpoint,
    &_MTLDevice_supportsPlacementSparse,
    &_MTLDevice_sparseTileSize,
    &_MTLDevice_newPlacementHeap,
    &_MTLDevice_updateSparseTextureMappings,
    &_WMTApitraceSessionFlush,
    &_MTL4CounterHeap_newTimestampHeap,
    &_MTL4CounterHeap_resolveCounterRange,
    &_MTL4TimestampContext_create,
    &_MTL4TimestampContext_destroy,
    &_MTL4TimestampContext_writeTimestamp,
    &_MTLDevice_sizeOfTimestampHeapEntry,
};
#endif
