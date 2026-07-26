#include "builder.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include <sys/wait.h>
#include <unistd.h>

namespace {

void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::string ReadFile(const std::filesystem::path &path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

} // namespace

int main() {
  try {
    using namespace dxmt::builder;
    Check(Sha256("") ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "empty SHA-256 vector failed");
    Check(Sha256("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "abc SHA-256 vector failed");
    Check(Profiles().size() == 4, "unexpected profile count");
    Check(FindProfile("gcc-x64-release-full") != nullptr,
          "default profile missing");
    Check(FindProfile("apple-clang-x86_64-release") != nullptr,
          "native x86_64 profile missing");
    Check(FindProfile("gcc-x86-release") == nullptr,
          "native i386 DXMT profile must not be registered");
    for (const auto &profile : Profiles())
      Check(profile.target_arch == "x64" || profile.target_arch == "x86_64",
            "only x64 PE / native x86_64 profiles are supported");
    Check(FindProfile("not-a-profile") == nullptr,
          "unknown profile was accepted");
    Check(JsonEscape("a\n\"b") == "a\\n\\\"b", "JSON escaping failed");
    Check(IsPathWithin("/tmp/root/child", "/tmp/root"),
          "managed child was rejected");
    Check(!IsPathWithin("/tmp/root-other", "/tmp/root"),
          "outside path was accepted");
    const auto properties = testing::ParseProperties(
        "# metadata\nschema=1\nprofile=gcc-x64-release-full\n");
    Check(properties.at("schema") == "1" &&
              properties.at("profile") == "gcc-x64-release-full",
          "metadata parsing failed");
    const auto config = testing::ParseJsonStringObject(
        "{\n  \"cache_root\": \"../cache\",\n"
        "  \"profile_namespace\": \"feature\\/cache\"\n}\n");
    Check(config.at("cache_root") == "../cache" &&
              config.at("profile_namespace") == "feature/cache",
          "builder config JSON parsing failed");
    Check(testing::CcacheRoot("/tmp/cache", "test-dev") ==
              "/tmp/cache/ccache/test-dev",
          "namespaced ccache root is incorrect");
    Check(testing::CcacheRoot("/tmp/cache", "feature/cache") ==
              "/tmp/cache/ccache/feature/cache",
          "nested branch ccache root is incorrect");
    Check(testing::CcacheRoot("/tmp/cache", "") == "/tmp/cache/ccache",
          "unnamespaced ccache root is incorrect");
    bool rejected_non_string = false;
    try {
      static_cast<void>(testing::ParseJsonStringObject(
          "{\"cache_root\": 1}"));
    } catch (const std::runtime_error &) {
      rejected_non_string = true;
    }
    Check(rejected_non_string, "non-string builder config value was accepted");

    const auto temp = std::filesystem::temp_directory_path() /
                      ("dxmt-builder-tests-" + std::to_string(getpid()));
    std::filesystem::remove_all(temp);
    const auto repo = temp / "parent/worktree";
    std::filesystem::create_directories(repo / ".dxmt-builder");
    testing::WriteFileAtomic(temp / "parent/.dxmt-builder/config.json", "{}");
    Check(!testing::DiscoverConfigPath(repo),
          "parent worktree config leaked into the current checkout");
    const auto local_config = repo / ".dxmt-builder/config.json";
    testing::WriteFileAtomic(local_config, "{}");
    Check(testing::DiscoverConfigPath(repo) ==
              std::filesystem::absolute(local_config).lexically_normal(),
          "checkout-local builder config was not discovered");
    const auto explicit_config = temp / "explicit-config.json";
    testing::WriteFileAtomic(explicit_config, "{}");
    Check(testing::DiscoverConfigPath(repo, explicit_config) ==
              std::filesystem::absolute(explicit_config).lexically_normal(),
          "explicit builder config did not override the checkout default");
    const auto stage = temp / "runtime-stage";
    Check(testing::StagedInstallRoot(stage, "/usr/local") ==
              stage / "usr/local",
          "absolute Meson prefix escaped the runtime stage");
    Check(testing::StagedInstallRoot(stage, "custom/prefix") ==
              stage / "custom/prefix",
          "relative Meson prefix was not preserved under the runtime stage");

    const auto atomic_file = temp / "atomic.txt";
    testing::WriteFileAtomic(atomic_file, "complete");
    Check(ReadFile(atomic_file) == "complete", "atomic publication failed");

    const auto audit_repo = temp / "audit-repo";
    std::filesystem::create_directories(
        audit_repo / "src/winemetal4/unix");
    std::filesystem::create_directories(audit_repo / "src/d3d12");
    std::filesystem::create_directories(audit_repo / "src/dxmt");
    std::filesystem::create_directories(audit_repo / "include");
    std::filesystem::create_directories(audit_repo / "tests");
    std::filesystem::create_directories(audit_repo / "tools/audit");
    testing::WriteFileAtomic(
        audit_repo / "include/dxmt_d3d12_submission_model.hpp",
        "#include <variant>\n"
        "struct ExecutePacket {};\n"
        "struct StopPacket {};\n"
        "using SubmissionPayload = std::variant<ExecutePacket, StopPacket>;\n"
        "class D3D12SubmissionPacket final {\n"
        "  SubmissionPayload payload_;\n"
        "};\n"
        "struct ResourceRetirement {};\n"
        "using RetirementPayload = std::variant<ResourceRetirement>;\n"
        "template <typename T> concept GpuRetirementPayload = true;\n"
        "class GpuRetirementRecord final {\n"
        "  RetirementPayload payload_;\n"
        "};\n"
        "class SubmissionSequencer final {};\n"
        "class RetirementQueue final {};\n"
        "class BackendThreadCapability final {};\n"
        "class ReplayBackend {};\n"
        "class SubmissionExecutor final {};\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_submission_model.cpp",
        "#include \"dxmt_d3d12_submission_model.hpp\"\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_submission_model_header.cpp",
        "#include \"dxmt_d3d12_submission_model.hpp\"\n");
    const std::string valid_cpu_query_target =
        "class CpuQueryResolveTarget {};\n"
        "void AddPendingCpuQueryResolve(\n"
        "    std::unique_ptr<CpuQueryResolveTarget> target);\n";
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_resource.hpp",
        valid_cpu_query_target);
    const std::string valid_production_submission_service =
        "class QueueSubmissionEndpoint final {};\n"
        "void RegisterProductionQueue() {\n"
        "  device_queue_state_->SubmissionService()->RegisterQueue();\n"
        "  device_queue_state_->SubmissionService()->UnregisterQueue();\n"
        "}\n"
        "bool ProcessReadySubmission() noexcept { return true; }\n"
        "class RetirementRecord final : public GpuCompletionTarget {};\n"
        "class DeferredCpuQueryResolveTarget final : public "
        "CpuQueryResolveTarget {};\n"
        "class FencePrivateReference {};\n"
        "struct ExecuteSubmission final {};\n"
        "struct QueueWorkSubmission final {};\n"
        "struct FenceSignalSubmission final {\n"
        "  FencePrivateReference fence;\n"
        "};\n"
        "struct FenceWaitSubmission final {\n"
        "  FencePrivateReference fence;\n"
        "};\n"
        "struct StopSubmission final {};\n"
        "using PendingOperationPayload =\n"
        "    std::variant<ExecuteSubmission, QueueWorkSubmission,\n"
        "                 FenceSignalSubmission, FenceWaitSubmission,\n"
        "                 StopSubmission>;\n"
        "struct PendingOperation final {\n"
        "  PendingOperationPayload payload;\n"
        "};\n"
        "std::deque<PendingOperation> pending_operations_ "
        "DXMT_GUARDED_BY(mutex_);\n"
        "bool submission_worker_active_ DXMT_GUARDED_BY(mutex_);\n"
        "bool submission_worker_waiting_for_wait_ "
        "DXMT_GUARDED_BY(mutex_);\n"
        "bool submission_worker_stopping_ DXMT_GUARDED_BY(mutex_);\n"
        "bool submission_service_stopped_ DXMT_GUARDED_BY(mutex_);\n"
        "bool SubmitDxmtQueueWork() { return true; }\n"
        "struct SwapChainResizeQueueWork final {};\n"
        "class ReplayPassEncodeCommand {};\n"
        "template <typename T, typename U>\n"
        "class ReplayCompiledEncodeCommand final {};\n"
        "std::shared_ptr<ReplayPassEncodeCommand> encoder;\n"
        "class ReplayBlitEncodeCommand {};\n"
        "std::shared_ptr<ReplayBlitEncodeCommand> encoder;\n"
        "using ReplayEncoderArena = int;\n"
        "template <typename T> struct ReplayCommandStorage final {\n"
        "  std::shared_ptr<ReplayEncoderArena> arena;\n"
        "};\n"
        "void RegisterRetirement(CommandChunk *chunk) {\n"
        "  chunk->addCompletionTarget();\n"
        "}\n"
        "const char *worker_name = \"dxmt-d3d12-sequencer\";\n";
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_command_queue.cpp",
        valid_production_submission_service);
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_submission_service.cpp",
        "class DeviceQueueWorkRequest final {};\n"
        "class DeviceSubmissionService::Impl final {\n"
        "  std::vector<std::shared_ptr<SubmissionQueueEndpoint>> queues_ "
        "DXMT_GUARDED_BY(mutex_);\n"
        "  std::deque<std::shared_ptr<DeviceQueueWorkRequest>> device_work_ "
        "DXMT_GUARDED_BY(mutex_);\n"
        "};\n"
        "const char *worker_name = \"dxmt-d3d12-sequencer\";\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_swapchain.hpp",
        "class D3D12SwapChainHost {};\n"
        "struct D3D12PresentSubmission final {\n"
        "  D3D12PresentSubmission(const D3D12PresentSubmission &) = delete;\n"
        "};\n"
        "static_assert(!std::is_copy_constructible_v<"
        "D3D12PresentSubmission>);\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_swapchain.cpp",
        "class SwapChainImpl final {\n"
        "  D3D12SwapChainHost &host_;\n"
        "  void Present() {\n"
        "    host_.SubmitPresent();\n"
        "    host_.PrepareSwapChainResize();\n"
        "  }\n"
        "};\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_tile_mapping.hpp",
        "class TileRangeView final {\n"
        "  static TileRangeView FromAbi(UINT count);\n"
        "};\n"
        "std::span<const WMTSparseTextureMappingOperation> operations;\n"
        "bool CollectLogicalTilesInRegion();\n"
        "bool CollectTilesInRegion();\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_tile_mapping.cpp",
        "struct D3D12TileMappingCounters {};\n"
        "void ApplySparseTileMappingOpsToResource() {}\n"
        "void RecordTileMappingMetalFailure() {}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_device_queue_state.hpp",
        "using ReplayResourceStateMap = int;\n"
        "class D3D12DeviceQueueState final {\n"
        "  ReplayResourceStateMap &BackendResourceStates() noexcept;\n"
        "};\n"
        "std::shared_ptr<D3D12DeviceQueueState> "
        "AcquireD3D12DeviceQueueState(IMTLD3D12Device &device);\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_device_queue_state.cpp",
        "class D3D12DeviceQueueStateRegistry final {\n"
        "  std::weak_ptr<D3D12DeviceQueueState> state;\n"
        "  void Clean() { if (entry->second.expired()) {} }\n"
        "  void Create() { std::make_shared<D3D12DeviceQueueState>(); }\n"
        "};\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_descriptor_diagnostics.hpp",
        "void D3D12DiagDescriptorFormat();\n"
        "void DescriptorRecordTypeName();\n"
        "void D3D12DiagLogTextureView();\n"
        "void D3D12DiagLogDSVReplayDescriptor();\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_descriptor_diagnostics.cpp",
        "void D3D12DiagDescriptorFormat() {}\n"
        "void DescriptorRecordTypeName() {}\n"
        "void D3D12DiagLogTextureView() {}\n"
        "void D3D12DiagLogDSVReplayDescriptor() {}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_replay_diagnostics.hpp",
        "class DiagnosticReadbackBuffer final {\n"
        "  DiagnosticReadbackBuffer(const DiagnosticReadbackBuffer &) = "
        "delete;\n"
        "  ~DiagnosticReadbackBuffer() noexcept;\n"
        "};\n"
        "struct IndexReadbackRetirementWork final {};\n"
        "struct VertexReadbackRetirementWork final {};\n"
        "struct ConstantBufferReadbackRetirementWork final {};\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_replay_diagnostics.cpp",
        "void DiagnosticReadbackBuffer::Reset() {}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_queue_config.hpp",
        "HRESULT NormalizeQueueDesc();\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_queue_config.cpp",
        "void IsSupportedQueueType() {}\n"
        "void IsSupportedQueuePriority() {}\n"
        "void IsSupportedQueueFlags() {}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_temporal_scaler.hpp",
        "struct CachedTemporalScaler final {};\n"
        "WMTPixelFormat "
        "TemporalUpscaleMotionVectorSourceFormat(WMTPixelFormat format) "
        "noexcept;\n"
        "WMTPixelFormat TemporalUpscaleMotionTextureFormat(\n"
        "    WMTPixelFormat source_format,\n"
        "    bool motion_vector_in_display_resolution) noexcept;\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_temporal_scaler.cpp",
        "WMTPixelFormat "
        "TemporalUpscaleMotionVectorSourceFormat(WMTPixelFormat format) "
        "noexcept { return format; }\n"
        "WMTPixelFormat TemporalUpscaleMotionTextureFormat(\n"
        "    WMTPixelFormat source_format,\n"
        "    bool motion_vector_in_display_resolution) noexcept {\n"
        "  return source_format;\n"
        "}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_texture_swizzle.hpp",
        "WMTTextureSwizzleChannels "
        "ShaderResourceViewSwizzle(WMTPixelFormat format, "
        "UINT component_mapping);\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_texture_swizzle.cpp",
        "void BaseShaderReadSwizzleForFormat() {}\n"
        "void TextureSwizzleFromD3D12Component() {}\n"
        "void ComposeTextureSwizzleComponent() {}\n"
        "void ShaderResourceViewSwizzle() {}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_texture_view.hpp",
        "void ResolveTextureViewFormat();\n"
        "void ResolveRenderTargetTextureViewFormat();\n"
        "void ResolveDepthStencilViewFormat();\n"
        "void CreateRenderTargetView();\n"
        "void CreateDepthStencilView();\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_texture_view.cpp",
        "void ResolveTextureViewFormat() {}\n"
        "void ResolveRenderTargetTextureViewFormat() {}\n"
        "void ResolveDepthStencilViewFormat() {}\n"
        "void CreateRenderTargetView() {}\n"
        "void CreateDepthStencilView() {}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_shader_binding.hpp",
        "void ForEachVisibleStage();\n"
        "void FindShaderForStage();\n"
        "void BindingTypeForRange();\n"
        "void ShaderBindingSlotCapacity();\n"
        "void ShaderArgumentQwordStride();\n"
        "void ShaderArgumentRangeCount();\n"
        "void IntersectDescriptorRangeWithShaderArgument();\n"
        "void ShaderArgumentAtRangeOffset();\n"
        "void ResolveShaderBindingSlot();\n"
        "void ResolveShaderBindingArgument();\n"
        "void ResolveShaderBindingArgumentBySlot();\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_shader_binding.cpp",
        "void FindShaderForStage() {}\n"
        "void ResolveShaderBindingSlot() {}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_argument_buffer_layout.hpp",
        "void AllocateArgumentBuffer();\n"
        "void AlignArgumentBufferSize();\n"
        "void AdvanceArgumentBufferEstimate();\n"
        "void EstimateShaderArgumentBufferSize();\n"
        "void EstimateGraphicsArgumentBufferSize();\n"
        "void EstimateComputeArgumentBufferSize();\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_argument_buffer_layout.cpp",
        "void AllocateArgumentBuffer() {}\n"
        "void AlignArgumentBufferSize() {}\n"
        "void AdvanceArgumentBufferEstimate() {}\n"
        "void EstimateShaderArgumentBufferSize() {}\n"
        "void EstimateGraphicsArgumentBufferSize() {}\n"
        "void EstimateComputeArgumentBufferSize() {}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_pipeline_write_policy.hpp",
        "void StencilOpWrites();\n"
        "void StencilFaceWrites();\n"
        "void PipelineWritesDepth();\n"
        "void PipelineWritesStencil();\n"
        "void AccessForDepthStencilPlane();\n"
        "void DepthStencilResourceStateForAccess();\n"
        "void ValidateComputeDispatch();\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_pipeline_write_policy.cpp",
        "void StencilOpWrites() {}\n"
        "void StencilFaceWrites() {}\n"
        "void PipelineWritesDepth() {}\n"
        "void PipelineWritesStencil() {}\n"
        "void AccessForDepthStencilPlane() {}\n"
        "void DepthStencilResourceStateForAccess() {}\n"
        "void ValidateComputeDispatch() {}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/meson.build",
        "d3d12_src = ['d3d12_command_queue.cpp', "
        "'d3d12_resource.cpp', "
        "'invalid_owner.cpp', "
        "'d3d12_submission_model.cpp', "
        "'d3d12_submission_model_header.cpp', "
        "'d3d12_device_queue_state.cpp', "
        "'d3d12_descriptor_diagnostics.cpp', "
        "'d3d12_replay_diagnostics.cpp', "
        "'d3d12_queue_config.cpp', "
        "'d3d12_temporal_scaler.cpp', "
        "'d3d12_texture_swizzle.cpp', "
        "'d3d12_texture_view.cpp', "
        "'d3d12_shader_binding.cpp', "
        "'d3d12_argument_buffer_layout.cpp', "
        "'d3d12_pipeline_write_policy.cpp', "
        "'d3d12_submission_service.cpp', "
        "'d3d12_swapchain.cpp', "
        "'d3d12_tile_mapping.cpp']\n"
        "files('../../tools/audit/d3d12_submission_model_spec.cpp')\n"
        "test('d3d12-submission-model', model_test)\n"
        "files('../../tools/audit/d3d12_copy_geometry_spec.cpp')\n"
        "test('d3d12-copy-geometry', copy_geometry_test)\n");
    const std::string valid_metal_policy =
        "void dxmt_residency_set_add_allocation_direct() {\n"
        "  [set addAllocation:allocation];\n"
        "}\n"
        "void dxmt_residency_set_remove_allocation_direct() {\n"
        "  [set removeAllocation:allocation];\n"
        "}\n"
        "void dxmt_residency_set_commit_direct() {\n"
        "  [set commit];\n"
        "}\n"
        "void dxmt_residency_set_request_direct() {\n"
        "  [set requestResidency];\n"
        "}\n"
        "void dxmt_residency_set_end_direct() {\n"
        "  [set endResidency];\n"
        "}\n"
        "void dxmt_residency_set_contains_allocation_direct() {\n"
        "  [set containsAllocation:allocation];\n"
        "}\n"
        "void dxmt_residency_set_allocation_count_direct() {\n"
        "  auto count = set.allocationCount;\n"
        "}\n"
        "void dxmt_owned_residency_set_add_allocation() "
        "DXMT_REQUIRES(owner_lock) {\n"
        "  dxmt_residency_set_add_allocation_direct();\n"
        "}\n"
        "void dxmt_owned_residency_set_remove_allocation() "
        "DXMT_REQUIRES(owner_lock) {\n"
        "  dxmt_residency_set_remove_allocation_direct();\n"
        "}\n"
        "void dxmt_owned_residency_set_commit() "
        "DXMT_REQUIRES(owner_lock) {\n"
        "  dxmt_residency_set_commit_direct();\n"
        "}\n"
        "void _MTLResidencySet_addAllocation() {\n"
        "  dxmt_residency_set_add_allocation_direct();\n"
        "}\n"
        "void _MTLResidencySet_removeAllocation() {\n"
        "  dxmt_residency_set_remove_allocation_direct();\n"
        "}\n"
        "void _MTLResidencySet_commit() {\n"
        "  dxmt_residency_set_commit_direct();\n"
        "}\n"
        "void _MTLResidencySet_requestResidency() {\n"
        "  dxmt_residency_set_request_direct();\n"
        "}\n"
        "void dxmt_metal4_argument_table_from_handle() {}\n"
        "/* DXMT_ACQUIRE(lock) DXMT_RELEASE(lock) */\n";
    testing::WriteFileAtomic(
        audit_repo / "src/winemetal4/unix/winemetal_unix.c",
        valid_metal_policy);
    testing::WriteFileAtomic(
        audit_repo / "src/winemetal4/winemetal.h",
        "#define STATIC_ASSERT(x) _Static_assert((x), #x)\n"
        "STATIC_ASSERT(offsetof(struct WMTCommandBufferDiagnosticInfo, "
        "fence_edges) == 296);\n"
        "STATIC_ASSERT(offsetof(struct WMTCommandBufferDiagnosticInfo, "
        "encoders) == 14888);\n"
        "STATIC_ASSERT(sizeof(struct WMTCommandBufferDiagnosticInfo) == "
        "23080);\n");
    testing::WriteFileAtomic(
        audit_repo / "tools/audit/policy-baseline.json",
        "{\"schema\":1,\"maximum_raw_nslock_messages\":0}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/dxmt/dxmt_command_queue.hpp",
        "RegisterLifetimeResidency(LifetimeResidencyAllocation allocation)\n"
        "ReplayAllocatorResidencyAllocation allocation\n"
        "ReplayTemporaryResidencyAllocation allocation\n"
        "friend class ResidencyOwnership;\n"
        "explicit ResidencyAllocation(WMT::Object allocation,\n"
        "class DXMT_CAPABILITY(\"device-residency\") "
        "DeviceResidencyMutex\n"
        "class DXMT_SCOPED_CAPABILITY DeviceResidencyLock\n"
        "class DXMT_CAPABILITY(\"residency-submission-owner\") "
        "DeviceResidencySubmissionOwner final {\n"
        "  void lock() DXMT_ACQUIRE() { mutex_.lock(); }\n"
        "  void unlock() DXMT_RELEASE() { mutex_.unlock(); }\n"
        "  dxmt::mutex mutex_;\n"
        "};\n"
        "class DXMT_SCOPED_CAPABILITY DeviceResidencySubmissionScope final {\n"
        "  DeviceResidencySubmissionScope("
        "DeviceResidencySubmissionOwner &owner) DXMT_ACQUIRE(owner)\n"
        "      : owner_(owner) { owner_.lock(); }\n"
        "  ~DeviceResidencySubmissionScope() DXMT_RELEASE() "
        "{ owner_.unlock(); }\n"
        "  DeviceResidencySubmissionOwner &owner_;\n"
        "};\n"
        "WMT::Reference<WMT::ResidencySet> set_;\n"
        "struct RetainedAllocation {\n"
        "WMT::Reference<WMT::Object> object;\n"
        "const ResidencyProvenance provenance;\n"
        "};\n"
        "using AllocationOwner = std::shared_ptr<RetainedAllocation>;\n"
        "void AddLocked(const AllocationOwner &allocation, Kind kind) "
        "DXMT_REQUIRES(mutex_);\n"
        "void RemoveLocked(WMT::Object allocation, Kind kind, "
        "AllocationOwner *retired) noexcept "
        "DXMT_REQUIRES(mutex_);\n"
        "void QueueDesiredStateLocked(const AllocationOwner &allocation, "
        "bool make_resident) DXMT_REQUIRES(mutex_);\n"
        "SubmissionBatch PrepareSubmissionBatchLocked() "
        "DXMT_REQUIRES(mutex_);\n"
        "struct SubmissionResult {\n"
        "  bool succeeded;\n"
        "  std::vector<WMT::Reference<WMT::Object>> retired_allocations;\n"
        "};\n"
        "SubmissionResult ApplyForSubmission() "
        "DXMT_REQUIRES(submission_owner_) DXMT_EXCLUDES(mutex_);\n"
        "std::unordered_map<obj_handle_t, Entry> entries_ "
        "DXMT_GUARDED_BY(mutex_);\n"
        "std::unordered_map<obj_handle_t, PendingMutation> "
        "pending_mutations_ DXMT_GUARDED_BY(mutex_);\n"
        "std::unordered_map<obj_handle_t, AllocationOwner> "
        "backend_entries_ DXMT_GUARDED_BY(mutex_);\n"
        "std::deque<AllocationOwner> retired_backend_owners;\n"
        "std::vector<WMT::Reference<WMT::Object>> "
        "retired_residency_allocations;\n"
        "std::atomic_uint64_t applied_generation_;\n"
        "std::atomic_uint64_t commit_count_;\n"
        "ResidencyOwnership::ReplayAllocatorBlock(block)\n"
        "using GpuRetainedOwner = std::variant<Rc<Sampler>>;\n"
        "void RetainGpuOwner(GpuRetainedOwner owner);\n"
        "std::unordered_map<uint64_t, std::vector<GpuRetainedOwner>> "
        "deferred_retained_owners_;\n"
        "class DeviceErrorTarget {};\n"
        "uint64_t RegisterDeviceErrorTarget("
        "const std::shared_ptr<DeviceErrorTarget> &target);\n"
        "std::unordered_map<uint64_t, std::weak_ptr<DeviceErrorTarget>> "
        "device_error_targets_;\n"
        "std::vector<DiagnosticReadback> pending_readbacks_;\n");
    testing::WriteFileAtomic(
        audit_repo / "src/dxmt/dxmt_command_queue.cpp",
        "void DeviceResidency::Add() {\n"
        "  auto owner = "
        "std::make_shared<RetainedAllocation>(allocation, provenance);\n"
        "  DeviceResidencyLock lock(mutex_);\n"
        "  AddLocked(owner, kind);\n"
        "}\n"
        "void DeviceResidency::AddLocked() {\n"
        "  QueueDesiredStateLocked(allocation, true);\n"
        "}\n"
        "void DeviceResidency::Remove() {\n"
        "  DeviceResidencyLock lock(mutex_);\n"
        "  RemoveLocked(allocation, kind);\n"
        "}\n"
        "void DeviceResidency::RemoveLocked() {\n"
        "  QueueDesiredStateLocked(allocation, false);\n"
        "}\n"
        "void DeviceResidency::QueueDesiredStateLocked() {\n"
        "  pending_mutations_.insert();\n"
        "}\n"
        "SubmissionBatch DeviceResidency::PrepareSubmissionBatchLocked() {\n"
        "  return {};\n"
        "}\n"
        "SubmissionResult DeviceResidency::ApplyForSubmission() {\n"
        "  SubmissionBatch batch;\n"
        "  {\n"
        "    DeviceResidencyLock lock(mutex_);\n"
        "    batch = PrepareSubmissionBatchLocked();\n"
        "  }\n"
        "  set_.addAllocation();\n"
        "  set_.removeAllocation();\n"
        "  log(\" source=\");\n"
        "  log(\" owner=\");\n"
        "  log(\" identity=\");\n"
        "  log(\" parent=\");\n"
        "  log(\" heapOffset=\");\n"
        "  log(\" size=\");\n"
        "  log(\" dimension=\");\n"
        "  log(\" component=\");\n"
        "  set_.commit();\n"
        "  set_.requestResidency();\n"
        "}\n"
        "void CommandQueue::CommitChunkInternal() {\n"
        "  DeviceResidencySubmissionScope scope("
        "device_residency_->submission_owner_);\n"
        "  device_residency_->ApplyForSubmission();\n"
        "  cmdbuf.commitAndGetStats();\n"
        "}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/dxmt/dxmt_occlusion_query.hpp",
        "struct Readback {};\n"
        "using DiagnosticReadback = std::variant<Readback>;\n"
        "static_assert("
        "std::is_nothrow_move_constructible_v<DiagnosticReadback>);\n"
        "std::vector<DiagnosticReadback> diagnostics;\n"
        "std::shared_ptr<LifetimeResidencyRegistration> "
        "residency_retirement_;\n"
        "std::unique_ptr<TimestampSampleOwner> sample_owner_;\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_resource.cpp",
        "struct DepthStencilUploadCommand final {};\n"
        "struct TextureUploadCommand final {};\n"
        "struct DepthStencilReadbackCommand final {};\n"
        "struct TextureReadbackCommand final {};\n"
        "using SynchronousBlitPayload = std::variant<"
        "DepthStencilUploadCommand>;\n"
        "class SynchronousBlitSubmission final {};\n"
        "bool HasLifetimeResidency() {\n"
        "  if (kind_ == ResourceKind::Placed && placement_heap_residency_)\n"
        "      return true;\n"
        "  return false;\n"
        "}\n"
        "void RegisterLifetimeResidency() {\n"
        "  if (kind_ == ResourceKind::Placed && placement_heap_residency_)\n"
        "      return;\n"
        "  auto kind = ResidencyProvenanceKind::PlacedResourceChild;\n"
        "  auto parent = placement_heap_.handle;\n"
        "  auto provenance = ResidencyProvenance{\n"
        "      .parent = placement_heap_.handle,\n"
        "  };\n"
        "}\n");
    Check(testing::AuditDx12Metal4Policy(audit_repo).empty(),
          "valid DX12/Metal4 ownership policy fixture was rejected");
    Check(testing::AuditDiagnosticMayBeBaselined(
              "src/d3d12/d3d12_command_queue.cpp",
              "clang-analyzer-deadcode.DeadStores"),
          "legacy general diagnostics must remain baselineable");
    Check(!testing::AuditDiagnosticMayBeBaselined(
              "src/d3d12/d3d12_submission_model.cpp",
              "clang-analyzer-deadcode.DeadStores"),
          "native replay model diagnostics must never be baselined");
    Check(!testing::AuditDiagnosticMayBeBaselined(
              "src/d3d12/d3d12_command_queue.cpp",
              "clang-diagnostic-thread-safety-analysis"),
          "thread-safety diagnostics must never be baselined");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_resource.hpp",
        valid_cpu_query_target +
            "using PendingCpuQueryResolveFn = "
            "std::function<void(Resource *)>;\n");
    const auto erased_cpu_query_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              erased_cpu_query_errors.begin(), erased_cpu_query_errors.end(),
              [](const auto &error) {
                return error.find(
                           "deferred CPU query resolve ownership must use a "
                           "typed move-only target") != std::string::npos;
              }),
          "type-erased deferred CPU query resolve was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_resource.hpp",
        valid_cpu_query_target);
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_command_queue.cpp",
        valid_production_submission_service +
            "dxmt::thread submission_worker_;\n");
    const auto per_queue_worker_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              per_queue_worker_errors.begin(), per_queue_worker_errors.end(),
              [](const auto &error) {
                return error.find(
                           "per-queue submission/retirement workers") !=
                       std::string::npos;
              }),
          "per-queue D3D12 submission worker was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_command_queue.cpp",
        valid_production_submission_service);
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_command_queue.cpp",
        valid_production_submission_service +
            "class ReplayGraphicsCompiledPayloadArena {};\n");
    const auto erased_replay_payload_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              erased_replay_payload_errors.begin(),
              erased_replay_payload_errors.end(), [](const auto &error) {
                return error.find(
                           "manual payload arenas and type-erased callbacks "
                           "are forbidden") != std::string::npos;
              }),
          "type-erased replay payload arena was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_command_queue.cpp",
        valid_production_submission_service);
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_command_queue.cpp",
        valid_production_submission_service +
            "std::mutex dxmt_queue_mutex;\n");
    const auto direct_queue_lock_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              direct_queue_lock_errors.begin(),
              direct_queue_lock_errors.end(), [](const auto &error) {
                return error.find(
                           "direct DXMT queue locking is forbidden") !=
                       std::string::npos;
              }),
          "direct DXMT queue lock was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_command_queue.cpp",
        valid_production_submission_service);
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_temporal_scaler.cpp",
        "#include \"d3d12_command_queue.hpp\"\n"
        "WMTPixelFormat "
        "TemporalUpscaleMotionVectorSourceFormat(WMTPixelFormat format) "
        "noexcept { return format; }\n"
        "WMTPixelFormat TemporalUpscaleMotionTextureFormat(\n"
        "    WMTPixelFormat source_format,\n"
        "    bool motion_vector_in_display_resolution) noexcept {\n"
        "  return source_format;\n"
        "}\n");
    const auto temporal_scaler_dependency_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              temporal_scaler_dependency_errors.begin(),
              temporal_scaler_dependency_errors.end(),
              [](const auto &error) {
                return error.find(
                           "value-domain module must not pull the command "
                           "queue into its include closure") !=
                       std::string::npos;
              }),
          "temporal scaler reverse queue dependency was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_temporal_scaler.cpp",
        "WMTPixelFormat "
        "TemporalUpscaleMotionVectorSourceFormat(WMTPixelFormat format) "
        "noexcept { return format; }\n"
        "WMTPixelFormat TemporalUpscaleMotionTextureFormat(\n"
        "    WMTPixelFormat source_format,\n"
        "    bool motion_vector_in_display_resolution) noexcept {\n"
        "  return source_format;\n"
        "}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_texture_swizzle.cpp",
        "#include \"d3d12_command_queue.hpp\"\n"
        "void BaseShaderReadSwizzleForFormat() {}\n"
        "void TextureSwizzleFromD3D12Component() {}\n"
        "void ComposeTextureSwizzleComponent() {}\n"
        "void ShaderResourceViewSwizzle() {}\n");
    const auto texture_swizzle_dependency_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              texture_swizzle_dependency_errors.begin(),
              texture_swizzle_dependency_errors.end(),
              [](const auto &error) {
                return error.find(
                           "value-domain module must not pull the command "
                           "queue into its include closure") !=
                       std::string::npos;
              }),
          "descriptor swizzle replay dependency was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_texture_swizzle.cpp",
        "void BaseShaderReadSwizzleForFormat() {}\n"
        "void TextureSwizzleFromD3D12Component() {}\n"
        "void ComposeTextureSwizzleComponent() {}\n"
        "void ShaderResourceViewSwizzle() {}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_queue_config.cpp",
        "#include \"d3d12_command_queue.hpp\"\n"
        "void IsSupportedQueueType() {}\n"
        "void IsSupportedQueuePriority() {}\n"
        "void IsSupportedQueueFlags() {}\n");
    const auto queue_config_dependency_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              queue_config_dependency_errors.begin(),
              queue_config_dependency_errors.end(),
              [](const auto &error) {
                return error.find(
                           "value-domain module must not pull the command "
                           "queue into its include closure") !=
                       std::string::npos;
              }),
          "queue configuration runtime dependency was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_queue_config.cpp",
        "void IsSupportedQueueType() {}\n"
        "void IsSupportedQueuePriority() {}\n"
        "void IsSupportedQueueFlags() {}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_descriptor_diagnostics.cpp",
        "#include \"d3d12_command_queue.hpp\"\n"
        "void D3D12DiagDescriptorFormat() {}\n"
        "void DescriptorRecordTypeName() {}\n"
        "void D3D12DiagLogTextureView() {}\n"
        "void D3D12DiagLogDSVReplayDescriptor() {}\n");
    const auto descriptor_diagnostics_dependency_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              descriptor_diagnostics_dependency_errors.begin(),
              descriptor_diagnostics_dependency_errors.end(),
              [](const auto &error) {
                return error.find(
                           "value-domain module must not pull the command "
                           "queue into its include closure") !=
                       std::string::npos;
              }),
          "descriptor diagnostics queue dependency was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_descriptor_diagnostics.cpp",
        "void D3D12DiagDescriptorFormat() {}\n"
        "void DescriptorRecordTypeName() {}\n"
        "void D3D12DiagLogTextureView() {}\n"
        "void D3D12DiagLogDSVReplayDescriptor() {}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_texture_view.cpp",
        "#include \"d3d12_command_queue.hpp\"\n"
        "void ResolveTextureViewFormat() {}\n"
        "void ResolveRenderTargetTextureViewFormat() {}\n"
        "void ResolveDepthStencilViewFormat() {}\n"
        "void CreateRenderTargetView() {}\n"
        "void CreateDepthStencilView() {}\n");
    const auto texture_view_dependency_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              texture_view_dependency_errors.begin(),
              texture_view_dependency_errors.end(),
              [](const auto &error) {
                return error.find(
                           "value-domain module must not pull the command "
                           "queue into its include closure") !=
                       std::string::npos;
              }),
          "texture view queue dependency was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_texture_view.cpp",
        "void ResolveTextureViewFormat() {}\n"
        "void ResolveRenderTargetTextureViewFormat() {}\n"
        "void ResolveDepthStencilViewFormat() {}\n"
        "void CreateRenderTargetView() {}\n"
        "void CreateDepthStencilView() {}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_shader_binding.cpp",
        "#include \"d3d12_command_queue.hpp\"\n"
        "void FindShaderForStage() {}\n"
        "void ResolveShaderBindingSlot() {}\n");
    const auto shader_binding_dependency_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              shader_binding_dependency_errors.begin(),
              shader_binding_dependency_errors.end(),
              [](const auto &error) {
                return error.find(
                           "value-domain module must not pull the command "
                           "queue into its include closure") !=
                       std::string::npos;
              }),
          "shader binding encoder dependency was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_shader_binding.cpp",
        "void FindShaderForStage() {}\n"
        "void ResolveShaderBindingSlot() {}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_argument_buffer_layout.cpp",
        "#include \"d3d12_command_queue.hpp\"\n"
        "void AllocateArgumentBuffer() {}\n"
        "void AlignArgumentBufferSize() {}\n"
        "void AdvanceArgumentBufferEstimate() {}\n"
        "void EstimateShaderArgumentBufferSize() {}\n"
        "void EstimateGraphicsArgumentBufferSize() {}\n"
        "void EstimateComputeArgumentBufferSize() {}\n");
    const auto argument_buffer_layout_dependency_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              argument_buffer_layout_dependency_errors.begin(),
              argument_buffer_layout_dependency_errors.end(),
              [](const auto &error) {
                return error.find(
                           "value-domain module must not pull the command "
                           "queue into its include closure") !=
                       std::string::npos;
              }),
          "argument buffer layout chunk dependency was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_argument_buffer_layout.cpp",
        "void AllocateArgumentBuffer() {}\n"
        "void AlignArgumentBufferSize() {}\n"
        "void AdvanceArgumentBufferEstimate() {}\n"
        "void EstimateShaderArgumentBufferSize() {}\n"
        "void EstimateGraphicsArgumentBufferSize() {}\n"
        "void EstimateComputeArgumentBufferSize() {}\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_pipeline_write_policy.cpp",
        "#include \"d3d12_command_queue.hpp\"\n"
        "void StencilOpWrites() {}\n"
        "void StencilFaceWrites() {}\n"
        "void PipelineWritesDepth() {}\n"
        "void PipelineWritesStencil() {}\n"
        "void AccessForDepthStencilPlane() {}\n"
        "void DepthStencilResourceStateForAccess() {}\n"
        "void ValidateComputeDispatch() {}\n");
    const auto pipeline_write_policy_dependency_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              pipeline_write_policy_dependency_errors.begin(),
              pipeline_write_policy_dependency_errors.end(),
              [](const auto &error) {
                return error.find(
                           "value-domain module must not pull the command "
                           "queue into its include closure") !=
                       std::string::npos;
              }),
          "pipeline write policy replay dependency was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_pipeline_write_policy.cpp",
        "void StencilOpWrites() {}\n"
        "void StencilFaceWrites() {}\n"
        "void PipelineWritesDepth() {}\n"
        "void PipelineWritesStencil() {}\n"
        "void AccessForDepthStencilPlane() {}\n"
        "void DepthStencilResourceStateForAccess() {}\n"
        "void ValidateComputeDispatch() {}\n");
    auto unguarded_submission_state = valid_production_submission_service;
    const auto guarded_pending =
        std::string("std::deque<PendingOperation> pending_operations_ "
                    "DXMT_GUARDED_BY(mutex_);");
    const auto guarded_pending_pos =
        unguarded_submission_state.find(guarded_pending);
    Check(guarded_pending_pos != std::string::npos,
          "guarded production packet fixture marker is missing");
    unguarded_submission_state.replace(
        guarded_pending_pos, guarded_pending.size(),
        "std::deque<PendingOperation> pending_operations_;");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_command_queue.cpp",
        unguarded_submission_state);
    const auto unguarded_submission_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              unguarded_submission_errors.begin(),
              unguarded_submission_errors.end(), [](const auto &error) {
                return error.find(
                           "missing direct typed Metal completion contract: "
                           "std::deque<PendingOperation> pending_operations_ "
                           "DXMT_GUARDED_BY(mutex_);") !=
                       std::string::npos;
              }),
          "unguarded production submission FIFO was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_command_queue.cpp",
        valid_production_submission_service);
    auto raw_fence_packet = valid_production_submission_service;
    const auto packet_marker =
        raw_fence_packet.find("struct PendingOperation final {\n");
    Check(packet_marker != std::string::npos,
          "typed production packet fixture marker is missing");
    raw_fence_packet.insert(
        packet_marker + std::string("struct PendingOperation final {\n").size(),
        "  Fence *fence;\n");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_command_queue.cpp",
        raw_fence_packet);
    const auto raw_fence_packet_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              raw_fence_packet_errors.begin(),
              raw_fence_packet_errors.end(), [](const auto &error) {
                return error.find(
                           "production submission packet must use a closed "
                           "typed payload with RAII ownership") !=
                       std::string::npos;
              }),
          "raw fence owner in production submission packet was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_command_queue.cpp",
        valid_production_submission_service);
    auto invalid_retirement_owner = valid_production_submission_service;
    const auto retirement_marker =
        invalid_retirement_owner.find(
            "class RetirementRecord final : public GpuCompletionTarget {};");
    Check(retirement_marker != std::string::npos,
          "production retirement owner fixture marker is missing");
    invalid_retirement_owner.replace(
        retirement_marker,
        std::string_view(
            "class RetirementRecord final : public GpuCompletionTarget {};")
            .size(),
        "class RetirementRecord final : public GpuCompletionTarget {\n"
        "  CommandQueueImpl *queue_;\n"
        "};");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_command_queue.cpp",
        invalid_retirement_owner);
    const auto upper_retirement_owner_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              upper_retirement_owner_errors.begin(),
              upper_retirement_owner_errors.end(), [](const auto &error) {
                return error.find(
                           "production retirement target must contain leaf "
                           "payload only") != std::string::npos;
              }),
          "upper-layer owner in production retirement target was not "
          "rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_command_queue.cpp",
        valid_production_submission_service);
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_command_queue.cpp",
        valid_production_submission_service +
            "class GpuCompletionPublication {};\n");
    const auto secondary_completion_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              secondary_completion_errors.begin(),
              secondary_completion_errors.end(), [](const auto &error) {
                return error.find(
                           "secondary completion publications are forbidden") !=
                       std::string::npos;
              }),
          "secondary D3D12 completion publication was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_command_queue.cpp",
        valid_production_submission_service);
    const auto valid_queue_header =
        ReadFile(audit_repo / "src/dxmt/dxmt_command_queue.hpp");
    testing::WriteFileAtomic(
        audit_repo / "src/dxmt/dxmt_command_queue.hpp",
        valid_queue_header +
            "void RetainUntilGpuComplete(std::function<void()> callback);\n");
    const auto callback_retention_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              callback_retention_errors.begin(),
              callback_retention_errors.end(), [](const auto &error) {
                return error.find("callback-style GPU retention is forbidden") !=
                       std::string::npos;
              }),
          "callback-style GPU retention was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/dxmt/dxmt_command_queue.hpp",
        valid_queue_header);
    testing::WriteFileAtomic(
        audit_repo / "src/dxmt/dxmt_command_queue.hpp",
        valid_queue_header +
            "uint64_t RegisterDeviceErrorCallback("
            "std::weak_ptr<std::function<void()>> callback);\n");
    const auto callback_device_error_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              callback_device_error_errors.begin(),
              callback_device_error_errors.end(), [](const auto &error) {
                return error.find(
                           "type-erased device-error callbacks are forbidden") !=
                       std::string::npos;
              }),
          "type-erased device-error callback was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/dxmt/dxmt_command_queue.hpp",
        valid_queue_header);
    testing::WriteFileAtomic(
        audit_repo / "src/dxmt/dxmt_command_queue.hpp",
        valid_queue_header +
            "std::vector<std::function<void()>> deferred_readbacks;\n");
    const auto callback_readback_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              callback_readback_errors.begin(),
              callback_readback_errors.end(), [](const auto &error) {
                return error.find(
                           "type-erased asynchronous readbacks are forbidden") !=
                       std::string::npos;
              }),
          "type-erased asynchronous readback was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/dxmt/dxmt_command_queue.hpp",
        valid_queue_header);
    const auto valid_query_header =
        ReadFile(audit_repo / "src/dxmt/dxmt_occlusion_query.hpp");
    testing::WriteFileAtomic(
        audit_repo / "src/dxmt/dxmt_occlusion_query.hpp",
        valid_query_header +
            "void setResidencyRetirement(std::function<void()> callback);\n");
    const auto callback_visibility_residency_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              callback_visibility_residency_errors.begin(),
              callback_visibility_residency_errors.end(),
              [](const auto &error) {
                return error.find(
                           "visibility readback residency must be represented "
                           "by typed leaf ownership") != std::string::npos;
              }),
          "type-erased visibility residency retirement was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/dxmt/dxmt_occlusion_query.hpp",
        valid_query_header);
    auto static_only_submission_owner = valid_queue_header;
    const auto runtime_acquire =
        static_only_submission_owner.find("owner_.lock();");
    Check(runtime_acquire != std::string::npos,
          "submission owner runtime acquire fixture marker is missing");
    static_only_submission_owner.replace(
        runtime_acquire, std::string_view("owner_.lock();").size(),
        "(void)owner_;");
    testing::WriteFileAtomic(
        audit_repo / "src/dxmt/dxmt_command_queue.hpp",
        static_only_submission_owner);
    const auto static_owner_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              static_owner_errors.begin(), static_owner_errors.end(),
              [](const auto &error) {
                return error.find(
                           "submission scope must acquire and release the "
                           "device-wide runtime owner") != std::string::npos;
              }),
          "static-only submission ownership capability was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/dxmt/dxmt_command_queue.hpp",
        valid_queue_header);
    auto missing_provenance_owner = valid_queue_header;
    const auto provenance_member =
        missing_provenance_owner.find(
            "const ResidencyProvenance provenance;");
    Check(provenance_member != std::string::npos,
          "retained provenance fixture marker is missing");
    missing_provenance_owner.erase(
        provenance_member,
        std::string_view("const ResidencyProvenance provenance;").size());
    testing::WriteFileAtomic(
        audit_repo / "src/dxmt/dxmt_command_queue.hpp",
        missing_provenance_owner);
    const auto missing_provenance_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              missing_provenance_errors.begin(),
              missing_provenance_errors.end(), [](const auto &error) {
                return error.find("must carry copied residency provenance") !=
                       std::string::npos;
              }),
          "residency owner without copied provenance was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/dxmt/dxmt_command_queue.hpp",
        valid_queue_header);
    const auto valid_resource_source =
        ReadFile(audit_repo / "src/d3d12/d3d12_resource.cpp");
    auto missing_placed_parent = valid_resource_source;
    const auto placed_parent =
        missing_placed_parent.find(".parent = placement_heap_.handle");
    Check(placed_parent != std::string::npos,
          "placed residency parent fixture marker is missing");
    missing_placed_parent.erase(
        placed_parent,
        std::string_view(".parent = placement_heap_.handle").size());
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_resource.cpp",
        missing_placed_parent);
    const auto missing_placed_parent_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              missing_placed_parent_errors.begin(),
              missing_placed_parent_errors.end(), [](const auto &error) {
                return error.find("placement heap parent") !=
                       std::string::npos;
              }),
          "placed residency without a parent allocation was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_resource.cpp",
        valid_resource_source);
    auto duplicate_placed_child = valid_resource_source;
    constexpr std::string_view placed_owner =
        "kind_ == ResourceKind::Placed && placement_heap_residency_";
    const auto first_placed_owner = duplicate_placed_child.find(placed_owner);
    const auto registration_placed_owner = duplicate_placed_child.find(
        placed_owner, first_placed_owner + placed_owner.size());
    Check(first_placed_owner != std::string::npos &&
              registration_placed_owner != std::string::npos,
          "placed backing-heap ownership fixture markers are missing");
    duplicate_placed_child.replace(
        registration_placed_owner, placed_owner.size(),
        "kind_ == ResourceKind::Placed && false");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_resource.cpp",
        duplicate_placed_child);
    const auto duplicate_placed_child_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              duplicate_placed_child_errors.begin(),
              duplicate_placed_child_errors.end(), [](const auto &error) {
                return error.find(
                           "inherit lifetime residency from their backing "
                           "heap") != std::string::npos;
              }),
          "duplicate placed-child lifetime residency was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/d3d12_resource.cpp",
        valid_resource_source);
    auto invalid_external_policy = valid_metal_policy;
    const auto external_add = invalid_external_policy.find(
        "  dxmt_residency_set_add_allocation_direct();",
        invalid_external_policy.find("void _MTLResidencySet_addAllocation()"));
    Check(external_add != std::string::npos,
          "external residency mutation fixture marker is missing");
    invalid_external_policy.insert(external_add,
                                   "  dxmt_nslock_scope_acquire();\n");
    testing::WriteFileAtomic(
        audit_repo / "src/winemetal4/unix/winemetal_unix.c",
        invalid_external_policy);
    const auto external_policy_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              external_policy_errors.begin(), external_policy_errors.end(),
              [](const auto &error) {
                return error.find(
                           "external residency thunk must not create a "
                           "second synchronization domain") !=
                       std::string::npos;
              }),
          "external residency mutation accepted a second lock domain");
    testing::WriteFileAtomic(
        audit_repo / "src/winemetal4/unix/winemetal_unix.c",
        valid_metal_policy);
    testing::WriteFileAtomic(
        audit_repo / "src/winemetal4/unix/winemetal_unix.c",
        valid_metal_policy +
            "void *dxmt_residency_set_lock_registry = nullptr;\n");
    const auto registry_policy_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              registry_policy_errors.begin(),
              registry_policy_errors.end(),
              [](const auto &error) {
                return error.find(
                           "universal residency-set lock registry is "
                           "forbidden") != std::string::npos;
              }),
          "universal residency lock registry was not rejected");
    testing::WriteFileAtomic(
        audit_repo / "src/winemetal4/unix/winemetal_unix.c",
        valid_metal_policy);
    const auto valid_submission_model =
        ReadFile(audit_repo / "include/dxmt_d3d12_submission_model.hpp");
    testing::WriteFileAtomic(
        audit_repo / "include/dxmt_d3d12_submission_model.hpp",
        valid_submission_model +
            "struct InvalidRetirement { std::function<void()> callback; };\n");
    const auto invalid_model_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              invalid_model_errors.begin(), invalid_model_errors.end(),
              [](const auto &error) {
                return error.find(
                           "type-erased callbacks are forbidden") !=
                       std::string::npos;
              }),
          "submission model accepted a type-erased completion callback");
    testing::WriteFileAtomic(
        audit_repo / "include/dxmt_d3d12_submission_model.hpp",
        valid_submission_model);

    // Thread-safety escape hatches are banned across the whole concurrency
    // tree, so a file that did not exist when the rule was written is covered
    // the moment it appears.
    const auto escape_probe = audit_repo / "src/dxmt/dxmt_escape_probe.hpp";
    const std::array<std::pair<const char *, const char *>, 3> escape_hatches =
        {{
            {"void Run() { }  // NOLINT(bugprone-use-after-move)\n",
             "inline clang-tidy suppression is forbidden"},
            {"void Run() DXMT_NO_THREAD_SAFETY_ANALYSIS { }\n",
             "thread-safety suppression is forbidden"},
            {"void Assert() const DXMT_ASSERT_CAPABILITY(mutex_) {}\n",
             "asserting a capability tells the analyzer the lock is held "
             "without proving it"},
        }};
    for (const auto &[contents, message] : escape_hatches) {
      testing::WriteFileAtomic(escape_probe, contents);
      const auto escape_errors = testing::AuditDx12Metal4Policy(audit_repo);
      Check(std::any_of(escape_errors.begin(), escape_errors.end(),
                        [message = message](const auto &error) {
                          return error.find(message) != std::string::npos;
                        }),
            "analyzer escape hatch in a newly added concurrency file was not "
            "rejected");
    }
    std::filesystem::remove(escape_probe);

    // The reviewed exceptions are a budget, not a blanket pass for the file
    // that carries them, and an asserted capability has to be paid for with a
    // run-time ownership check.
    const auto submission_service =
        audit_repo / "src/d3d12/d3d12_submission_service.cpp";
    const auto valid_submission_service = ReadFile(submission_service);
    const std::string two_reviewed_assertions =
        valid_submission_service +
        "void A() const DXMT_ASSERT_CAPABILITY(mutex_) { mutex_.assert_held(); }\n"
        "void B() const DXMT_ASSERT_CAPABILITY(mutex_) { mutex_.assert_held(); }\n";
    testing::WriteFileAtomic(submission_service, two_reviewed_assertions);
    Check(testing::AuditDx12Metal4Policy(audit_repo).empty(),
          "reviewed lock-ownership assertions were rejected");
    testing::WriteFileAtomic(
        submission_service,
        two_reviewed_assertions +
            "void C() const DXMT_ASSERT_CAPABILITY(mutex_) { "
            "mutex_.assert_held(); }\n");
    const auto over_budget_errors = testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(over_budget_errors.begin(), over_budget_errors.end(),
                      [](const auto &error) {
                        return error.find("reviewed exceptions for this file: "
                                          "2") != std::string::npos;
                      }),
          "an extra asserted lock capability beyond the reviewed budget was "
          "not rejected");
    testing::WriteFileAtomic(
        submission_service,
        valid_submission_service +
            "void A() const DXMT_ASSERT_CAPABILITY(mutex_) {}\n");
    const auto unchecked_assertion_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(unchecked_assertion_errors.begin(),
                      unchecked_assertion_errors.end(),
                      [](const auto &error) {
                        return error.find(
                                   "backed by a run-time dxmt::mutex "
                                   "ownership check") != std::string::npos;
                      }),
          "an asserted lock capability with no run-time check was not "
          "rejected");
    testing::WriteFileAtomic(submission_service, valid_submission_service);

    // Duplicated implementations are rejected between any two files in the
    // concurrency tree, at the 30-line threshold and not below it.
    const auto duplicate_left = audit_repo / "src/dxmt/dxmt_duplicate_a.hpp";
    const auto duplicate_right = audit_repo / "src/d3d12/d3d12_duplicate_b.hpp";
    // The copy is reindented and preceded by an extra line, so passing this
    // also proves the normalization the rule relies on.
    const auto build_block = [](std::size_t lines, const char *indent) {
      std::string block = std::string(indent) + "int Rebuild(int seed) {\n";
      for (std::size_t index = 0; index + 3 < lines; ++index)
        block += std::string(indent) + "  seed = seed * 31 + " +
                 std::to_string(index) + ";\n";
      block += std::string(indent) + "  return seed;\n";
      block += std::string(indent) + "}\n";
      return block;
    };
    testing::WriteFileAtomic(duplicate_left, build_block(29, ""));
    testing::WriteFileAtomic(duplicate_right,
                             "// copied out of dxmt_duplicate_a.hpp\n" +
                                 build_block(29, "      "));
    Check(testing::AuditDx12Metal4Policy(audit_repo).empty(),
          "a shared block below the duplicate threshold was rejected");
    testing::WriteFileAtomic(duplicate_left, build_block(30, ""));
    testing::WriteFileAtomic(duplicate_right,
                             "// copied out of dxmt_duplicate_a.hpp\n" +
                                 build_block(30, "      "));
    const auto duplicate_errors = testing::AuditDx12Metal4Policy(audit_repo);
    Check(std::any_of(
              duplicate_errors.begin(), duplicate_errors.end(),
              [](const auto &error) {
                return error.find(
                           "30 consecutive lines are a duplicate of") !=
                           std::string::npos &&
                       error.find("src/dxmt/dxmt_duplicate_a.hpp:1:") !=
                           std::string::npos &&
                       error.find("src/d3d12/d3d12_duplicate_b.hpp:2") !=
                           std::string::npos;
              }),
          "a duplicated implementation shared by two concurrency files was "
          "not rejected");
    Check(duplicate_errors.size() == 1,
          "a duplicated implementation must be reported once per file pair");
    std::filesystem::remove(duplicate_left);
    std::filesystem::remove(duplicate_right);
    Check(testing::AuditDx12Metal4Policy(audit_repo).empty(),
          "concurrency policy fixture was left dirty");

    testing::WriteFileAtomic(
        audit_repo / "src/d3d12/invalid_owner.cpp",
        "auto token = "
        "ResidencyOwnership::ReplayAllocatorBlock(resource);\n");
    const auto audit_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(audit_errors.size() == 1 &&
              audit_errors.front().find(
                  "Replay allocator residency tokens may only be created") !=
                  std::string::npos,
          "out-of-bound Replay allocator owner was not rejected");
    std::filesystem::remove(audit_repo / "src/d3d12/invalid_owner.cpp");
    testing::WriteFileAtomic(
        audit_repo / "src/dxmt/dxmt_command_queue.cpp",
        ReadFile(audit_repo / "src/dxmt/dxmt_command_queue.cpp") +
            "void WrongOwner::Run() {\n"
            "  device_residency_->ApplyForSubmission();\n"
            "}\n");
    const auto invalid_residency_errors =
        testing::AuditDx12Metal4Policy(audit_repo);
    Check(invalid_residency_errors.size() == 1 &&
              invalid_residency_errors.front().find(
                  "device residency may only be committed") !=
                  std::string::npos,
          "out-of-bound residency submission owner was not rejected");

    const auto counter = temp / "counter.txt";
    testing::WriteFileAtomic(counter, "0");
    std::vector<pid_t> children;
    for (int index = 0; index < 2; ++index) {
      const auto child = fork();
      Check(child >= 0, "fork failed");
      if (child == 0) {
        testing::WithFileLock(temp / "counter.lock", [&] {
          const auto value = std::stoi(ReadFile(counter));
          usleep(50000);
          testing::WriteFileAtomic(counter, std::to_string(value + 1));
        });
        _exit(0);
      }
      children.push_back(child);
    }
    for (const auto child : children) {
      int status = 0;
      Check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                WEXITSTATUS(status) == 0,
            "locked child failed");
    }
    Check(ReadFile(counter) == "2", "file lock did not serialize writers");
    std::filesystem::remove_all(temp);
    std::cout << "dxmt-builder tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "dxmt-builder test failure: " << error.what() << '\n';
    return 1;
  }
}
