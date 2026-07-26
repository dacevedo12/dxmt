// Host-side Metal capability probes for the bindless descriptor-heap
// migration.
//
// This spec is *not* a D3D behaviour test.  It runs natively on macOS, talks to
// Metal directly, and answers a much narrower question: does this machine's
// Metal stack actually support the descriptor-heap shape DXMT wants to move
// onto?  Concretely it checks that
//
//   * an unbounded `device const DescriptorEntry *` array of 24-byte entries
//     whose middle qword is an MTLResourceID can be indexed by a value only
//     known at runtime, across slots far past the 128 and 65536 boundaries,
//   * the same works for a sampler heap, including the `supportArgumentBuffers`
//     precondition, with a directly-bound control group next to it,
//   * a large number of *distinct* MTLSamplerState objects can be created, and
//   * the `as_type<texture2d<float>>(uint64_t)` handle bit-cast used by other
//     translation layers is rejected by the MSL front end.
//
// Every conclusion drawn from this spec is only valid for the GPU family and OS
// version it printed, so the environment block is part of the output, not
// decoration.
//
// Scale is controlled by DXMT_METAL_PROBE_SCALE:
//   (unset) / "default"  CI-sized run, a couple of seconds
//   "extended"           the long form (1M texture slots, 600K sampler slots,
//                        200K distinct sampler objects)

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include <sys/sysctl.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "metal_descriptor_heap_probe.h"
#include "metal_handle_reinterpret_accepted.h"
#include "metal_handle_reinterpret_rejected.h"

namespace {

// Host mirror of the MSL `DescriptorEntry` / `SamplerEntry`.  The MSL side
// static-asserts its own layout; this mirror plus the runtime scalar readback
// below is what ties the two together.
struct HeapEntry {
  uint64_t first;
  uint64_t handle;
  uint64_t last;
};
static_assert(sizeof(HeapEntry) == 24, "host mirror must match the MSL entry");
static_assert(alignof(HeapEntry) == 8, "host mirror must stay 8-byte aligned");
static_assert(offsetof(HeapEntry, handle) == 8, "handle must be the second qword");

constexpr uint64_t kGpuAddressTag = 0xFEEDull << 48;
constexpr uint64_t kMetadataTag = 0xCAFEull << 48;

int g_failures = 0;
int g_checks = 0;

void Check(bool passed, const std::string &message) {
  ++g_checks;
  if (!passed)
    ++g_failures;
  std::printf("    [%s] %s\n", passed ? "ok" : "FAILED", message.c_str());
}

void Note(const std::string &message) {
  std::printf("    (note) %s\n", message.c_str());
}

void Section(const char *title) { std::printf("\n== %s\n", title); }

std::string Format(const char *format, ...) __attribute__((format(printf, 1, 2)));

std::string Format(const char *format, ...) {
  char buffer[512];
  va_list arguments;
  va_start(arguments, format);
  std::vsnprintf(buffer, sizeof(buffer), format, arguments);
  va_end(arguments);
  return std::string(buffer);
}

struct ProbeScale {
  bool extended = false;
  uint32_t texture_heap_slots = 70000;
  uint32_t sampler_heap_slots = 100000;
  uint32_t distinct_samplers = 4096;
};

ProbeScale ResolveScale() {
  ProbeScale scale;
  const char *requested = std::getenv("DXMT_METAL_PROBE_SCALE");
  if (requested != nullptr && std::strcmp(requested, "extended") == 0) {
    scale.extended = true;
    scale.texture_heap_slots = 1048576;
    scale.sampler_heap_slots = 600000;
    scale.distinct_samplers = 200000;
  }
  return scale;
}

// Slots around every interesting binding-model boundary that still fit in the
// heap, plus the very last slot.  Slot 0 is included on purpose: its expected
// texel is deliberately non-zero so "read back zeros" can never be confused
// with "the entry was never written".
std::vector<uint32_t> ProbeSlots(uint32_t slot_count) {
  static const uint32_t kCandidates[] = {
      0,     1,     2,     127,   128,    129,    255,    256,
      511,   512,   1023,  1024,  4095,   4096,   65535,  65536,
      65537, 131071, 131072, 262144, 499999, 500000, 500001};
  std::vector<uint32_t> slots;
  for (uint32_t candidate : kCandidates) {
    if (candidate < slot_count)
      slots.push_back(candidate);
  }
  slots.push_back(slot_count - 1);
  std::sort(slots.begin(), slots.end());
  slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
  return slots;
}

// Every slot gets a distinct, non-zero expected texel derived from its index.
void ExpectedTexel(uint32_t slot, float out[4]) {
  const float base = static_cast<float>(slot) + 1.0f;
  out[0] = base;
  out[1] = base * 2.0f;
  out[2] = base * 3.0f;
  out[3] = 1.0f;
}

bool ProcessIsTranslated() {
  int translated = 0;
  size_t size = sizeof(translated);
  if (sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr, 0) != 0)
    return false;
  return translated != 0;
}

void ReportEnvironment(id<MTLDevice> device, const ProbeScale &scale) {
  Section("environment");
  const NSOperatingSystemVersion version =
      NSProcessInfo.processInfo.operatingSystemVersion;
  std::printf("    macOS %ld.%ld.%ld (%s)\n",
              static_cast<long>(version.majorVersion),
              static_cast<long>(version.minorVersion),
              static_cast<long>(version.patchVersion),
              NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String);
#if defined(__aarch64__)
  const char *architecture = "arm64";
#elif defined(__x86_64__)
  const char *architecture = "x86_64";
#else
  const char *architecture = "unknown";
#endif
  std::printf("    process architecture: %s%s\n", architecture,
              ProcessIsTranslated() ? " (running under Rosetta translation)" : "");
  std::printf("    device: %s (registryID 0x%llx, unified memory %s)\n",
              device.name.UTF8String,
              static_cast<unsigned long long>(device.registryID),
              device.hasUnifiedMemory ? "yes" : "no");

  struct FamilyEntry {
    const char *name;
    MTLGPUFamily family;
  };
  // MTLGPUFamilyMac2 is deprecated in favour of MTLGPUFamilyApple7 and is
  // deliberately left out so this file stays warning-free.
  static const FamilyEntry kFamilies[] = {
      {"Apple6", MTLGPUFamilyApple6}, {"Apple7", MTLGPUFamilyApple7},
      {"Apple8", MTLGPUFamilyApple8}, {"Apple9", MTLGPUFamilyApple9},
      {"Metal3", MTLGPUFamilyMetal3},
  };
  std::string families;
  for (const FamilyEntry &entry : kFamilies) {
    if ([device supportsFamily:entry.family]) {
      if (!families.empty())
        families += ", ";
      families += entry.name;
    }
  }
  std::printf("    GPU families: %s\n",
              families.empty() ? "(none reported)" : families.c_str());
  std::printf("    argument buffers tier: %ld\n",
              static_cast<long>(device.argumentBuffersSupport) + 1);
  std::printf("    maxBufferLength: %llu bytes\n",
              static_cast<unsigned long long>(device.maxBufferLength));
  std::printf("    scale: %s (texture slots %u, sampler slots %u, distinct "
              "samplers %u)\n",
              scale.extended ? "extended" : "default", scale.texture_heap_slots,
              scale.sampler_heap_slots, scale.distinct_samplers);
}

id<MTLTexture> MakeUnitTexture(id<MTLDevice> device, const float texel[4]) {
  MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                   width:1
                                  height:1
                               mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
  [texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
             mipmapLevel:0
               withBytes:texel
             bytesPerRow:16];
  return texture;
}

id<MTLSamplerState> MakeSampler(id<MTLDevice> device, MTLSamplerAddressMode mode,
                                bool argument_buffers) {
  MTLSamplerDescriptor *descriptor = [[MTLSamplerDescriptor alloc] init];
  descriptor.sAddressMode = mode;
  descriptor.tAddressMode = mode;
  descriptor.rAddressMode = mode;
  descriptor.minFilter = MTLSamplerMinMagFilterNearest;
  descriptor.magFilter = MTLSamplerMinMagFilterNearest;
  descriptor.supportArgumentBuffers = argument_buffers ? YES : NO;
  return [device newSamplerStateWithDescriptor:descriptor];
}

id<MTLComputePipelineState> MakePipeline(id<MTLDevice> device,
                                         id<MTLLibrary> library,
                                         const char *name) {
  NSError *error = nil;
  id<MTLFunction> function =
      [library newFunctionWithName:[NSString stringWithUTF8String:name]];
  if (function == nil) {
    Check(false, Format("kernel %s is present in the fixture library", name));
    return nil;
  }
  id<MTLComputePipelineState> pipeline =
      [device newComputePipelineStateWithFunction:function error:&error];
  if (pipeline == nil)
    Check(false, Format("pipeline %s builds: %s", name,
                        error.localizedDescription.UTF8String));
  return pipeline;
}

void Dispatch(id<MTLCommandQueue> queue, id<MTLComputePipelineState> pipeline,
              uint32_t threads,
              const std::function<void(id<MTLComputeCommandEncoder>)> &encode) {
  id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  [encoder setComputePipelineState:pipeline];
  encode(encoder);
  const NSUInteger group =
      std::min<NSUInteger>(threads, pipeline.maxTotalThreadsPerThreadgroup);
  [encoder dispatchThreads:MTLSizeMake(threads, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.error != nil)
    Check(false, Format("command buffer completed without error: %s",
                        command_buffer.error.localizedDescription.UTF8String));
}

// ---------------------------------------------------------------------------

void TestTextureDescriptorHeap(id<MTLDevice> device, id<MTLCommandQueue> queue,
                               id<MTLLibrary> library, const ProbeScale &scale) {
  Section("unbounded texture descriptor heap (AoS, 24-byte entries)");
  id<MTLComputePipelineState> pipeline =
      MakePipeline(device, library, "probe_texture_heap");
  if (pipeline == nil)
    return;

  const uint32_t slot_count = scale.texture_heap_slots;
  const std::vector<uint32_t> slots = ProbeSlots(slot_count);
  const uint32_t probe_count = static_cast<uint32_t>(slots.size());

  id<MTLBuffer> heap =
      [device newBufferWithLength:sizeof(HeapEntry) * slot_count
                          options:MTLResourceStorageModeShared];
  if (heap == nil) {
    Check(false, Format("allocate a %u-entry descriptor heap", slot_count));
    return;
  }
  HeapEntry *entries = static_cast<HeapEntry *>(heap.contents);
  std::memset(entries, 0, sizeof(HeapEntry) * slot_count);

  NSMutableArray<id<MTLTexture>> *textures = [NSMutableArray array];
  for (uint32_t slot : slots) {
    float texel[4];
    ExpectedTexel(slot, texel);
    id<MTLTexture> texture = MakeUnitTexture(device, texel);
    [textures addObject:texture];
    entries[slot].first = kGpuAddressTag | (slot + 1u);
    entries[slot].handle = texture.gpuResourceID._impl;
    entries[slot].last = kMetadataTag | (0xFFFFFFFFu - slot);
  }

  id<MTLBuffer> slot_buffer =
      [device newBufferWithBytes:slots.data()
                          length:sizeof(uint32_t) * probe_count
                         options:MTLResourceStorageModeShared];
  id<MTLBuffer> texels =
      [device newBufferWithLength:sizeof(float) * 4 * probe_count
                          options:MTLResourceStorageModeShared];
  id<MTLBuffer> scalars =
      [device newBufferWithLength:sizeof(uint32_t) * 2 * probe_count
                          options:MTLResourceStorageModeShared];

  Dispatch(queue, pipeline, probe_count, [&](id<MTLComputeCommandEncoder> encoder) {
    [encoder setBuffer:heap offset:0 atIndex:0];
    [encoder setBuffer:slot_buffer offset:0 atIndex:1];
    [encoder setBuffer:texels offset:0 atIndex:2];
    [encoder setBuffer:scalars offset:0 atIndex:3];
    for (id<MTLTexture> texture in textures)
      [encoder useResource:texture usage:MTLResourceUsageRead];
  });

  const float *read_texels = static_cast<const float *>(texels.contents);
  const uint32_t *read_scalars = static_cast<const uint32_t *>(scalars.contents);
  uint32_t wrong_texels = 0;
  uint32_t wrong_scalars = 0;
  for (uint32_t index = 0; index < probe_count; ++index) {
    const uint32_t slot = slots[index];
    float expected[4];
    ExpectedTexel(slot, expected);
    const bool texel_ok = read_texels[index * 4 + 0] == expected[0] &&
                          read_texels[index * 4 + 1] == expected[1] &&
                          read_texels[index * 4 + 2] == expected[2];
    const bool scalar_ok = read_scalars[index * 2 + 0] == slot + 1u &&
                           read_scalars[index * 2 + 1] == 0xFFFFFFFFu - slot;
    if (!texel_ok) {
      ++wrong_texels;
      Note(Format("slot %u read (%g, %g, %g), expected (%g, %g, %g)", slot,
                  static_cast<double>(read_texels[index * 4 + 0]),
                  static_cast<double>(read_texels[index * 4 + 1]),
                  static_cast<double>(read_texels[index * 4 + 2]),
                  static_cast<double>(expected[0]),
                  static_cast<double>(expected[1]),
                  static_cast<double>(expected[2])));
    }
    if (!scalar_ok) {
      ++wrong_scalars;
      Note(Format("slot %u scalar fields read (0x%x, 0x%x), expected "
                  "(0x%x, 0x%x)",
                  slot, read_scalars[index * 2 + 0], read_scalars[index * 2 + 1],
                  slot + 1u, 0xFFFFFFFFu - slot));
    }
  }

  Check(slots.front() == 0 && slots.back() == slot_count - 1,
        Format("probe set spans slot 0 to slot %u (%u probes)", slot_count - 1,
               probe_count));
  Check(std::find(slots.begin(), slots.end(), 128u) != slots.end() &&
            std::find(slots.begin(), slots.end(), 65536u) != slots.end(),
        "probe set crosses the 128 and 65536 slot boundaries");
  Check(wrong_texels == 0,
        Format("every probed slot resolves its own MTLResourceID (%u probes)",
               probe_count));
  Check(wrong_scalars == 0,
        "the 24-byte AoS field offsets hold at runtime, not only in static_assert");
}

// ---------------------------------------------------------------------------

void TestSamplerDescriptorHeap(id<MTLDevice> device, id<MTLCommandQueue> queue,
                               id<MTLLibrary> library, const ProbeScale &scale) {
  Section("unbounded sampler descriptor heap");
  id<MTLComputePipelineState> pipeline =
      MakePipeline(device, library, "probe_sampler_heap");
  if (pipeline == nil)
    return;

  static const float kTexel[4] = {7.0f, 8.0f, 9.0f, 1.0f};
  id<MTLTexture> texture = MakeUnitTexture(device, kTexel);

  // Pitfall guard: MTLSamplerDescriptor.supportArgumentBuffers must be YES or
  // the sampler's gpuResourceID is not a usable argument-buffer handle and the
  // heap entry silently has no effect.  The observed value for a
  // supportArgumentBuffers=NO sampler is *not* asserted here: it is outside the
  // documented contract, and on this machine it is small but non-zero, so a
  // "== 0" assertion would encode an implementation detail rather than a rule.
  // Feeding such an id to the GPU is deliberately not attempted.
  id<MTLSamplerState> plain_edge =
      MakeSampler(device, MTLSamplerAddressModeClampToEdge, false);
  id<MTLSamplerState> edge =
      MakeSampler(device, MTLSamplerAddressModeClampToEdge, true);
  id<MTLSamplerState> zero =
      MakeSampler(device, MTLSamplerAddressModeClampToZero, true);
  Check(edge != nil && zero != nil, "argument-buffer samplers are created");
  if (edge == nil || zero == nil)
    return;
  Check(edge.gpuResourceID._impl != 0 && zero.gpuResourceID._impl != 0,
        "supportArgumentBuffers=YES samplers expose a non-zero gpuResourceID");
  Note(Format("supportArgumentBuffers=NO sampler reports gpuResourceID 0x%llx "
              "(observed, not asserted: the value is undefined by contract)",
              static_cast<unsigned long long>(plain_edge.gpuResourceID._impl)));

  id<MTLBuffer> texture_heap =
      [device newBufferWithLength:sizeof(HeapEntry) * 4
                          options:MTLResourceStorageModeShared];
  HeapEntry *texture_entries = static_cast<HeapEntry *>(texture_heap.contents);
  std::memset(texture_entries, 0, sizeof(HeapEntry) * 4);
  texture_entries[0].handle = texture.gpuResourceID._impl;

  const uint32_t slot_count = scale.sampler_heap_slots;
  id<MTLBuffer> sampler_heap =
      [device newBufferWithLength:sizeof(HeapEntry) * slot_count
                          options:MTLResourceStorageModeShared];
  if (sampler_heap == nil) {
    Check(false, Format("allocate a %u-entry sampler heap", slot_count));
    return;
  }
  HeapEntry *sampler_entries = static_cast<HeapEntry *>(sampler_heap.contents);
  std::memset(sampler_entries, 0, sizeof(HeapEntry) * slot_count);
  const uint64_t edge_id = edge.gpuResourceID._impl;
  const uint64_t zero_id = zero.gpuResourceID._impl;
  for (uint32_t slot = 0; slot < slot_count; ++slot)
    sampler_entries[slot].first = (slot & 1u) != 0 ? zero_id : edge_id;

  const std::vector<uint32_t> slots = ProbeSlots(slot_count);
  const uint32_t probe_count = static_cast<uint32_t>(slots.size());
  std::vector<uint32_t> texture_slots(probe_count, 0);

  id<MTLBuffer> sampler_slot_buffer =
      [device newBufferWithBytes:slots.data()
                          length:sizeof(uint32_t) * probe_count
                         options:MTLResourceStorageModeShared];
  id<MTLBuffer> texture_slot_buffer =
      [device newBufferWithBytes:texture_slots.data()
                          length:sizeof(uint32_t) * probe_count
                         options:MTLResourceStorageModeShared];
  id<MTLBuffer> texels =
      [device newBufferWithLength:sizeof(float) * 4 * probe_count
                          options:MTLResourceStorageModeShared];

  auto run = [&](float u, float v) {
    const float coordinate[2] = {u, v};
    std::memset(texels.contents, 0xCD, sizeof(float) * 4 * probe_count);
    Dispatch(queue, pipeline, probe_count, [&](id<MTLComputeCommandEncoder> encoder) {
      [encoder setBuffer:texture_heap offset:0 atIndex:0];
      [encoder setBuffer:sampler_heap offset:0 atIndex:1];
      [encoder setBuffer:texture_slot_buffer offset:0 atIndex:2];
      [encoder setBuffer:sampler_slot_buffer offset:0 atIndex:3];
      [encoder setBuffer:texels offset:0 atIndex:4];
      [encoder setBytes:coordinate length:sizeof(coordinate) atIndex:5];
      [encoder useResource:texture usage:MTLResourceUsageRead];
    });
    const float *read = static_cast<const float *>(texels.contents);
    return std::vector<float>(read, read + 4 * probe_count);
  };

  // Out-of-bounds coordinate: this is the only coordinate that can tell the two
  // address modes apart on a 1x1 texture.
  const std::vector<float> outside = run(1.5f, 1.5f);
  uint32_t wrong = 0;
  for (uint32_t index = 0; index < probe_count; ++index) {
    const uint32_t slot = slots[index];
    const bool clamp_to_zero = (slot & 1u) != 0;
    const float *rgb = outside.data() + index * 4;
    const bool ok = clamp_to_zero
                        ? (rgb[0] == 0.0f && rgb[1] == 0.0f && rgb[2] == 0.0f)
                        : (rgb[0] == kTexel[0] && rgb[1] == kTexel[1] &&
                           rgb[2] == kTexel[2]);
    if (!ok) {
      ++wrong;
      Note(Format("sampler slot %u (%s) read (%g, %g, %g)", slot,
                  clamp_to_zero ? "ClampToZero" : "ClampToEdge",
                  static_cast<double>(rgb[0]), static_cast<double>(rgb[1]),
                  static_cast<double>(rgb[2])));
    }
  }
  Check(wrong == 0,
        Format("every probed sampler slot applies its own address mode "
               "(%u probes over %u slots)",
               probe_count, slot_count));

  // Pitfall guard: an in-bounds coordinate hits the same texel for both address
  // modes, so a run at (0.25, 0.25) proves nothing about the sampler.
  const std::vector<float> inside = run(0.25f, 0.25f);
  bool inside_uniform = true;
  for (uint32_t index = 0; index < probe_count; ++index) {
    const float *rgb = inside.data() + index * 4;
    inside_uniform = inside_uniform && rgb[0] == kTexel[0] &&
                     rgb[1] == kTexel[1] && rgb[2] == kTexel[2];
  }
  Check(inside_uniform,
        "an in-bounds coordinate cannot discriminate address modes "
        "(guards against reading (0.25,0.25) as 'the sampler did nothing')");
}

void TestDirectBindingControl(id<MTLDevice> device, id<MTLCommandQueue> queue,
                              id<MTLLibrary> library) {
  Section("control group: directly bound texture and sampler");
  id<MTLComputePipelineState> pipeline =
      MakePipeline(device, library, "probe_direct_binding");
  if (pipeline == nil)
    return;

  static const float kTexel[4] = {7.0f, 8.0f, 9.0f, 1.0f};
  id<MTLTexture> texture = MakeUnitTexture(device, kTexel);
  id<MTLBuffer> texels =
      [device newBufferWithLength:sizeof(float) * 4
                          options:MTLResourceStorageModeShared];

  auto run = [&](MTLSamplerAddressMode mode, float u, float v) {
    id<MTLSamplerState> sampler = MakeSampler(device, mode, false);
    const float coordinate[2] = {u, v};
    std::memset(texels.contents, 0xCD, sizeof(float) * 4);
    Dispatch(queue, pipeline, 1, [&](id<MTLComputeCommandEncoder> encoder) {
      [encoder setTexture:texture atIndex:0];
      [encoder setSamplerState:sampler atIndex:0];
      [encoder setBuffer:texels offset:0 atIndex:0];
      [encoder setBytes:coordinate length:sizeof(coordinate) atIndex:1];
    });
    const float *read = static_cast<const float *>(texels.contents);
    return std::vector<float>(read, read + 4);
  };

  const std::vector<float> edge_outside =
      run(MTLSamplerAddressModeClampToEdge, 1.5f, 1.5f);
  const std::vector<float> zero_outside =
      run(MTLSamplerAddressModeClampToZero, 1.5f, 1.5f);
  const std::vector<float> edge_inside =
      run(MTLSamplerAddressModeClampToEdge, 0.25f, 0.25f);
  const std::vector<float> zero_inside =
      run(MTLSamplerAddressModeClampToZero, 0.25f, 0.25f);

  Check(edge_outside[0] == kTexel[0] && edge_outside[1] == kTexel[1] &&
            edge_outside[2] == kTexel[2],
        Format("direct ClampToEdge at (1.5,1.5) reads (%g, %g, %g)",
               static_cast<double>(edge_outside[0]),
               static_cast<double>(edge_outside[1]),
               static_cast<double>(edge_outside[2])));
  Check(zero_outside[0] == 0.0f && zero_outside[1] == 0.0f &&
            zero_outside[2] == 0.0f,
        Format("direct ClampToZero at (1.5,1.5) reads (%g, %g, %g)",
               static_cast<double>(zero_outside[0]),
               static_cast<double>(zero_outside[1]),
               static_cast<double>(zero_outside[2])));
  Check(edge_inside[0] == kTexel[0] && zero_inside[0] == kTexel[0] &&
            edge_inside[1] == zero_inside[1] && edge_inside[2] == zero_inside[2],
        "direct binding at (0.25,0.25) is identical for both address modes");
}

// ---------------------------------------------------------------------------

void TestDistinctSamplerObjects(id<MTLDevice> device, const ProbeScale &scale) {
  Section("distinct MTLSamplerState objects");
  const uint32_t target = scale.distinct_samplers;
  NSMutableArray<id<MTLSamplerState>> *keep =
      [NSMutableArray arrayWithCapacity:target];
  std::unordered_set<uint64_t> identifiers;
  identifiers.reserve(target);
  uint64_t lowest = UINT64_MAX;
  uint64_t highest = 0;
  uint32_t created = 0;
  for (uint32_t index = 0; index < target; ++index) {
    MTLSamplerDescriptor *descriptor = [[MTLSamplerDescriptor alloc] init];
    descriptor.supportArgumentBuffers = YES;
    descriptor.lodMinClamp = static_cast<float>(index) * 1e-4f;
    descriptor.lodMaxClamp = descriptor.lodMinClamp + 1.0f;
    id<MTLSamplerState> sampler =
        [device newSamplerStateWithDescriptor:descriptor];
    if (sampler == nil) {
      Note(Format("newSamplerStateWithDescriptor returned nil at object %u",
                  index));
      break;
    }
    [keep addObject:sampler];
    const uint64_t identifier = sampler.gpuResourceID._impl;
    identifiers.insert(identifier);
    lowest = std::min(lowest, identifier);
    highest = std::max(highest, identifier);
    ++created;
  }

  Check(created == target,
        Format("%u distinct sampler objects stay alive simultaneously "
               "(created %u)",
               target, created));
  if (created == 0)
    return;
  std::printf("    gpuResourceID range [0x%llx, 0x%llx], %zu distinct ids\n",
              static_cast<unsigned long long>(lowest),
              static_cast<unsigned long long>(highest), identifiers.size());
  Check(identifiers.size() == created,
        Format("every live sampler object has its own gpuResourceID "
               "(%zu distinct out of %u)",
               identifiers.size(), created));
}

// ---------------------------------------------------------------------------

NSString *FixtureSource(const unsigned char *bytes, unsigned int length) {
  return [[NSString alloc] initWithBytes:bytes
                                  length:length
                                encoding:NSUTF8StringEncoding];
}

void TestHandleReinterpretIsRejected(id<MTLDevice> device) {
  Section("negative case: as_type<texture2d<float>>(uint64_t)");
  MTLCompileOptions *options = [[MTLCompileOptions alloc] init];
  options.languageVersion = MTLLanguageVersion3_1;

  NSError *accepted_error = nil;
  id<MTLLibrary> accepted = [device
      newLibraryWithSource:FixtureSource(metal_handle_reinterpret_accepted,
                                         metal_handle_reinterpret_accepted_len)
                   options:options
                     error:&accepted_error];
  Check(accepted != nil,
        Format("control: the typed descriptor-struct form compiles at runtime "
               "(%s)",
               accepted != nil
                   ? "ok"
                   : accepted_error.localizedDescription.UTF8String));

  NSError *rejected_error = nil;
  id<MTLLibrary> rejected = [device
      newLibraryWithSource:FixtureSource(metal_handle_reinterpret_rejected,
                                         metal_handle_reinterpret_rejected_len)
                   options:options
                     error:&rejected_error];
  Check(rejected == nil,
        "the raw-uint64 handle bit-cast is rejected by the MSL front end");
  if (rejected == nil && rejected_error != nil) {
    NSString *description = rejected_error.localizedDescription;
    NSString *first =
        [description componentsSeparatedByString:@"\n"].firstObject;
    if (first == nil)
      first = @"(no diagnostic text)";
    Note(Format("front-end diagnostic: %s", first.UTF8String));
  }
}

} // namespace

int main() {
  @autoreleasepool {
    const ProbeScale scale = ResolveScale();
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      std::printf("no Metal device available; skipping the capability probe\n");
      return 77; // Meson SKIP
    }
    ReportEnvironment(device, scale);

    NSError *error = nil;
    dispatch_data_t data = dispatch_data_create(
        metal_descriptor_heap_probe, metal_descriptor_heap_probe_len,
        dispatch_get_main_queue(), DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
    if (library == nil) {
      std::printf("failed to load the offline-compiled fixture library: %s\n",
                  error.localizedDescription.UTF8String);
      return 1;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];

    TestTextureDescriptorHeap(device, queue, library, scale);
    TestSamplerDescriptorHeap(device, queue, library, scale);
    TestDirectBindingControl(device, queue, library);
    TestDistinctSamplerObjects(device, scale);
    TestHandleReinterpretIsRejected(device);

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
  }
}
