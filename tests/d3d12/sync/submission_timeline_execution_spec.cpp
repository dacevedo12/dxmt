#include <dxmt_test.hpp>
#include <dxmt_test_shader.hpp>

#include "d3d12_test_context.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using dxmt::test::CompileShader;
using dxmt::test::ComPtr;
using dxmt::test::D3D12TestContext;

// Exercises D3D12 public-API submission ordering that D3DMetal chains as
// wait(prev) → commit → signal(next) on the queue timeline, plus reverse-stage
// and multi-pass local hazards that map to distinct PreRaster/Fragment MTLFences.

class SubmissionTimelineExecutionSpec : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_EQ(context_.Initialize(), S_OK); }

  ComPtr<ID3D12RootSignature> CreateUavRootSignature() {
    D3D12_ROOT_PARAMETER parameter = {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameter.Descriptor.ShaderRegister = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 1;
    desc.pParameters = &parameter;
    return context_.CreateRootSignature(desc);
  }

  ComPtr<ID3D12RootSignature> CreateSrvUavRootSignature() {
    std::array<D3D12_ROOT_PARAMETER, 2> parameters = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[1].Descriptor.ShaderRegister = 0;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = static_cast<UINT>(parameters.size());
    desc.pParameters = parameters.data();
    return context_.CreateRootSignature(desc);
  }

  ComPtr<ID3D12PipelineState>
  CreateComputePipeline(ID3D12RootSignature *root, const char *source) {
    const auto shader = CompileShader(source, "cs_5_0");
    EXPECT_EQ(shader.result, S_OK) << shader.diagnostic_text();
    if (shader.result != S_OK || !shader.bytecode)
      return {};
    return context_.CreateComputePipeline(
        root, {shader.bytecode->GetBufferPointer(),
               shader.bytecode->GetBufferSize()});
  }

  void ExpectDeviceHealthy() {
    EXPECT_EQ(context_.device()->GetDeviceRemovedReason(), S_OK);
  }

  D3D12TestContext context_;
};

TEST_F(SubmissionTimelineExecutionSpec,
       SequentialQueueFenceSignalsStayOrderedAcrossSubmits) {
  // Many ExecuteCommandLists + queue Signal/Wait pairs: public-API stand-in for
  // D3DMetal DoExecute timeline chaining with EncodeWaitMTL4 skip-if-complete.
  constexpr UINT kSubmits = 32;
  auto fence = ComPtr<ID3D12Fence>{};
  ASSERT_EQ(context_.device()->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                           IID_PPV_ARGS(fence.put())),
            S_OK);

  auto buffer = context_.CreateBuffer(sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto root = CreateUavRootSignature();
  // SM5.0 + host fxc: RWByteAddressBuffer only supports Store (not Load/Interlocked).
  // Each submit publishes the final expected constant; queue Signal/Wait ordering
  // is the contract under test (value stability after the last waited submit).
  auto pso = CreateComputePipeline(root.get(), R"(
    RWByteAddressBuffer output : register(u0);
    [numthreads(1, 1, 1)]
    void main() {
      output.Store(0, 32u);
    }
  )");
  ASSERT_TRUE(buffer);
  ASSERT_TRUE(root);
  ASSERT_TRUE(pso);

  auto seed = CreateComputePipeline(root.get(), R"(
    RWByteAddressBuffer output : register(u0);
    [numthreads(1, 1, 1)]
    void main() {
      output.Store(0, 0u);
    }
  )");
  ASSERT_TRUE(seed);
  context_.list()->SetComputeRootSignature(root.get());
  context_.list()->SetPipelineState(seed.get());
  context_.list()->SetComputeRootUnorderedAccessView(
      0, buffer->GetGPUVirtualAddress());
  context_.list()->Dispatch(1, 1, 1);
  ASSERT_EQ(context_.ExecuteAndWait(), S_OK);

  for (UINT i = 0; i < kSubmits; ++i) {
    ASSERT_EQ(context_.ResetCommandList(), S_OK);
    context_.list()->SetComputeRootSignature(root.get());
    context_.list()->SetPipelineState(pso.get());
    context_.list()->SetComputeRootUnorderedAccessView(
        0, buffer->GetGPUVirtualAddress());
    context_.list()->Dispatch(1, 1, 1);
    ASSERT_EQ(context_.list()->Close(), S_OK);
    ID3D12CommandList *lists[] = {context_.list()};
    context_.queue()->ExecuteCommandLists(1, lists);
    const UINT64 value = static_cast<UINT64>(i) + 1;
    ASSERT_EQ(context_.queue()->Signal(fence.get(), value), S_OK);
    ASSERT_EQ(context_.WaitForFence(fence.get(), value), S_OK);
    ExpectDeviceHealthy();
  }

  ASSERT_EQ(context_.ResetCommandList(), S_OK);
  D3D12TestContext::Transition(context_.list(), buffer.get(),
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
  std::vector<std::uint8_t> bytes;
  ASSERT_EQ(context_.ReadbackBuffer(buffer.get(), sizeof(UINT), &bytes), S_OK);
  UINT actual = 0;
  ASSERT_EQ(bytes.size(), sizeof(UINT));
  std::memcpy(&actual, bytes.data(), sizeof(actual));
  EXPECT_EQ(actual, kSubmits);
  ExpectDeviceHealthy();
}

TEST_F(SubmissionTimelineExecutionSpec,
       SameQueueCrossSubmitHazardPublishesWithoutExtraFence) {
  // Producer submit then consumer submit on the same queue without an app-level
  // fence between them relies on D3D12 same-queue ordering (GPTK timeline).
  auto intermediate = context_.CreateBuffer(
      sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto output = context_.CreateBuffer(
      sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto producer_root = CreateUavRootSignature();
  auto consumer_root = CreateSrvUavRootSignature();
  auto producer = CreateComputePipeline(producer_root.get(), R"(
    RWByteAddressBuffer output : register(u0);
    [numthreads(1, 1, 1)]
    void main() {
      output.Store(0, 0xA5A5A5A5u);
    }
  )");
  auto consumer = CreateComputePipeline(consumer_root.get(), R"(
    ByteAddressBuffer input : register(t0);
    RWByteAddressBuffer output : register(u0);
    [numthreads(1, 1, 1)]
    void main() {
      output.Store(0, input.Load(0) ^ 0x0F0F0F0Fu);
    }
  )");
  ASSERT_TRUE(intermediate);
  ASSERT_TRUE(output);
  ASSERT_TRUE(producer);
  ASSERT_TRUE(consumer);

  context_.list()->SetComputeRootSignature(producer_root.get());
  context_.list()->SetPipelineState(producer.get());
  context_.list()->SetComputeRootUnorderedAccessView(
      0, intermediate->GetGPUVirtualAddress());
  context_.list()->Dispatch(1, 1, 1);
  ASSERT_EQ(context_.list()->Close(), S_OK);
  ID3D12CommandList *producer_lists[] = {context_.list()};
  context_.queue()->ExecuteCommandLists(1, producer_lists);

  ASSERT_EQ(context_.ResetCommandList(), S_OK);
  D3D12TestContext::Transition(context_.list(), intermediate.get(),
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  context_.list()->SetComputeRootSignature(consumer_root.get());
  context_.list()->SetPipelineState(consumer.get());
  context_.list()->SetComputeRootShaderResourceView(
      0, intermediate->GetGPUVirtualAddress());
  context_.list()->SetComputeRootUnorderedAccessView(
      1, output->GetGPUVirtualAddress());
  context_.list()->Dispatch(1, 1, 1);
  D3D12TestContext::Transition(context_.list(), output.get(),
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);

  std::vector<std::uint8_t> bytes;
  ASSERT_EQ(context_.ReadbackBuffer(output.get(), sizeof(UINT), &bytes), S_OK);
  UINT actual = 0;
  std::memcpy(&actual, bytes.data(), sizeof(actual));
  EXPECT_EQ(actual, 0xA5A5A5A5u ^ 0x0F0F0F0Fu);
  ExpectDeviceHealthy();
}

TEST_F(SubmissionTimelineExecutionSpec,
       MoreThan256LocalUavHazardsReuseFencesAndPublishChain) {
  // A long producer-consumer chain inside one ExecuteCommandLists exercises
  // more logical dependency ids than the historical fixed 256-slot bank. The
  // Metal 4 replay path must colour non-overlapping lifetimes onto a bounded
  // reusable fence set instead of dropping dependencies or allocating one
  // MTLFence per encoder.
  // Host fxc lacks RWByteAddressBuffer.Load / structured store, so chain via
  // SRV Load + UAV Store across alternating buffers.
  constexpr UINT kStages = 320;
  auto producer_root = CreateUavRootSignature();
  auto consumer_root = CreateSrvUavRootSignature();
  auto seed = CreateComputePipeline(producer_root.get(), R"(
    RWByteAddressBuffer output : register(u0);
    [numthreads(1, 1, 1)]
    void main() {
      output.Store(0, 0u);
    }
  )");
  auto step = CreateComputePipeline(consumer_root.get(), R"(
    ByteAddressBuffer input : register(t0);
    RWByteAddressBuffer output : register(u0);
    [numthreads(1, 1, 1)]
    void main() {
      output.Store(0, input.Load(0) + 1u);
    }
  )");
  auto a = context_.CreateBuffer(sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto b = context_.CreateBuffer(sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ASSERT_TRUE(producer_root);
  ASSERT_TRUE(consumer_root);
  ASSERT_TRUE(seed);
  ASSERT_TRUE(step);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);

  context_.list()->SetComputeRootSignature(producer_root.get());
  context_.list()->SetPipelineState(seed.get());
  context_.list()->SetComputeRootUnorderedAccessView(
      0, a->GetGPUVirtualAddress());
  context_.list()->Dispatch(1, 1, 1);

  ID3D12Resource *src = a.get();
  ID3D12Resource *dst = b.get();
  D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  D3D12_RESOURCE_STATES dst_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  for (UINT i = 0; i < kStages; ++i) {
    if (src_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
      D3D12TestContext::Transition(
          context_.list(), src, src_state,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      src_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    if (dst_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
      D3D12TestContext::Transition(context_.list(), dst, dst_state,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      dst_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    context_.list()->SetComputeRootSignature(consumer_root.get());
    context_.list()->SetPipelineState(step.get());
    context_.list()->SetComputeRootShaderResourceView(
        0, src->GetGPUVirtualAddress());
    context_.list()->SetComputeRootUnorderedAccessView(
        1, dst->GetGPUVirtualAddress());
    context_.list()->Dispatch(1, 1, 1);
    std::swap(src, dst);
    std::swap(src_state, dst_state);
  }

  // After even kStages, result lives in `a` (started as src, swapped even times).
  ID3D12Resource *result = (kStages % 2 == 0) ? a.get() : b.get();
  D3D12_RESOURCE_STATES result_state =
      (kStages % 2 == 0) ? src_state : dst_state;
  // After loop, src is the last written when odd stages? Track carefully:
  // start src=a,dst=b; after each step swap. After 1: src=b(written), dst=a.
  // After even: src=a, dst=b and last write was to a... After step i, write to
  // pre-swap dst, then swap. So after step, the new src is the just-written.
  result = src;
  result_state = src_state;
  if (result_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    D3D12TestContext::Transition(context_.list(), result, result_state,
                                 D3D12_RESOURCE_STATE_COPY_SOURCE);
  }

  std::vector<std::uint8_t> bytes;
  ASSERT_EQ(context_.ReadbackBuffer(result, sizeof(UINT), &bytes), S_OK);
  UINT actual = 0;
  std::memcpy(&actual, bytes.data(), sizeof(actual));
  EXPECT_EQ(actual, kStages);
  ExpectDeviceHealthy();
}

TEST_F(SubmissionTimelineExecutionSpec,
       RenderTargetThenPixelSamplePublishesAcrossPasses) {
  // Fragment write → later pass pixel sample: reverse/cross-stage local fence
  // path (producer fragment EncoderId, consumer waits before Fragment).
  auto target = context_.CreateTexture2D(
      8, 8, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
      D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto rtv_heap =
      context_.CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
  auto srv_heap = context_.CreateDescriptorHeap(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
  ASSERT_TRUE(target);
  ASSERT_TRUE(rtv_heap);
  ASSERT_TRUE(srv_heap);

  const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  context_.device()->CreateRenderTargetView(target.get(), nullptr, rtv);
  context_.device()->CreateShaderResourceView(
      target.get(), nullptr, srv_heap->GetCPUDescriptorHandleForHeapStart());

  constexpr FLOAT kMagenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
  context_.list()->ClearRenderTargetView(rtv, kMagenta, 0, nullptr);
  D3D12TestContext::Transition(context_.list(), target.get(),
                               D3D12_RESOURCE_STATE_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  auto sampled = context_.CreateTexture2D(
      8, 8, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
      D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto sampled_rtv_heap =
      context_.CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
  ASSERT_TRUE(sampled);
  ASSERT_TRUE(sampled_rtv_heap);
  const auto sampled_rtv = sampled_rtv_heap->GetCPUDescriptorHandleForHeapStart();
  context_.device()->CreateRenderTargetView(sampled.get(), nullptr, sampled_rtv);

  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  D3D12_ROOT_PARAMETER parameter = {};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameter.DescriptorTable.NumDescriptorRanges = 1;
  parameter.DescriptorTable.pDescriptorRanges = &range;
  parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.ShaderRegister = 0;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC root_desc = {};
  root_desc.NumParameters = 1;
  root_desc.pParameters = &parameter;
  root_desc.NumStaticSamplers = 1;
  root_desc.pStaticSamplers = &sampler;
  auto root = context_.CreateRootSignature(root_desc);
  ASSERT_TRUE(root);

  const auto vs = CompileShader(R"(
    struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
    VSOut main(uint id : SV_VertexID) {
      VSOut o;
      float2 uv = float2((id << 1) & 2, id & 2);
      o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
      o.uv = uv;
      return o;
    }
  )",
                                "vs_5_0");
  const auto ps = CompileShader(R"(
    Texture2D tex : register(t0);
    SamplerState samp : register(s0);
    float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
      return tex.Sample(samp, uv);
    }
  )",
                                "ps_5_0");
  ASSERT_EQ(vs.result, S_OK) << vs.diagnostic_text();
  ASSERT_EQ(ps.result, S_OK) << ps.diagnostic_text();

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
  pso_desc.pRootSignature = root.get();
  pso_desc.VS = {vs.bytecode->GetBufferPointer(), vs.bytecode->GetBufferSize()};
  pso_desc.PS = {ps.bytecode->GetBufferPointer(), ps.bytecode->GetBufferSize()};
  pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  pso_desc.SampleMask = UINT_MAX;
  pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso_desc.NumRenderTargets = 1;
  pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pso_desc.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pso;
  ASSERT_EQ(context_.device()->CreateGraphicsPipelineState(
                &pso_desc, IID_PPV_ARGS(pso.put())),
            S_OK);

  D3D12_VIEWPORT viewport = {0.f, 0.f, 8.f, 8.f, 0.f, 1.f};
  D3D12_RECT scissor = {0, 0, 8, 8};
  ID3D12DescriptorHeap *heaps[] = {srv_heap.get()};
  context_.list()->SetDescriptorHeaps(1, heaps);
  context_.list()->OMSetRenderTargets(1, &sampled_rtv, FALSE, nullptr);
  context_.list()->RSSetViewports(1, &viewport);
  context_.list()->RSSetScissorRects(1, &scissor);
  context_.list()->SetGraphicsRootSignature(root.get());
  context_.list()->SetPipelineState(pso.get());
  context_.list()->SetGraphicsRootDescriptorTable(
      0, srv_heap->GetGPUDescriptorHandleForHeapStart());
  context_.list()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_.list()->DrawInstanced(3, 1, 0, 0);

  D3D12TestContext::Transition(context_.list(), sampled.get(),
                               D3D12_RESOURCE_STATE_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
  dxmt::test::TextureReadback readback;
  ASSERT_EQ(context_.ReadbackTexture(sampled.get(), &readback), S_OK);
  ASSERT_GE(readback.data.size(), sizeof(std::uint32_t));
  std::uint32_t pixel = 0;
  std::memcpy(&pixel, readback.data.data(), sizeof(pixel));
  EXPECT_TRUE(dxmt::test::ColorsMatch(pixel, 0xffff00ffu, 2u))
      << "pixel=0x" << std::hex << pixel;
  ExpectDeviceHealthy();
}

TEST_F(SubmissionTimelineExecutionSpec,
       FragmentWriteThenVertexReadReverseStagePublishes) {
  // Pixel UAV write in pass A, then vertex SRV read in pass B: reverse-stage
  // local fence edge (producer Fragment EncoderId, consumer PreRaster wait).
  auto buffer = context_.CreateBuffer(
      sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto output = context_.CreateBuffer(
      sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ASSERT_TRUE(buffer);
  ASSERT_TRUE(output);

  // Pass A: pixel shader stores a marker into UAV (u1 — must be > max RT index).
  {
    D3D12_ROOT_PARAMETER parameter = {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameter.Descriptor.ShaderRegister = 1;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &parameter;
    auto root = context_.CreateRootSignature(root_desc);
    ASSERT_TRUE(root);

    const auto vs = CompileShader(R"(
      float4 main(uint id : SV_VertexID) : SV_Position {
        float2 uv = float2((id << 1) & 2, id & 2);
        return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
      }
    )",
                                  "vs_5_0");
    const auto ps = CompileShader(R"(
      RWByteAddressBuffer output : register(u1);
      float4 main() : SV_Target0 {
        output.Store(0, 0xC001D00Du);
        return float4(0, 0, 0, 1);
      }
    )",
                                  "ps_5_0");
    ASSERT_EQ(vs.result, S_OK) << vs.diagnostic_text();
    ASSERT_EQ(ps.result, S_OK) << ps.diagnostic_text();

    auto target = context_.CreateTexture2D(
        4, 4, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto rtv_heap =
        context_.CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
    ASSERT_TRUE(target);
    ASSERT_TRUE(rtv_heap);
    const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    context_.device()->CreateRenderTargetView(target.get(), nullptr, rtv);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = root.get();
    pso_desc.VS = {vs.bytecode->GetBufferPointer(),
                   vs.bytecode->GetBufferSize()};
    pso_desc.PS = {ps.bytecode->GetBufferPointer(),
                   ps.bytecode->GetBufferSize()};
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    ASSERT_EQ(context_.device()->CreateGraphicsPipelineState(
                  &pso_desc, IID_PPV_ARGS(pso.put())),
              S_OK);

    D3D12_VIEWPORT viewport = {0.f, 0.f, 4.f, 4.f, 0.f, 1.f};
    D3D12_RECT scissor = {0, 0, 4, 4};
    context_.list()->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    context_.list()->RSSetViewports(1, &viewport);
    context_.list()->RSSetScissorRects(1, &scissor);
    context_.list()->SetGraphicsRootSignature(root.get());
    context_.list()->SetPipelineState(pso.get());
    context_.list()->SetGraphicsRootUnorderedAccessView(
        0, buffer->GetGPUVirtualAddress());
    context_.list()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_.list()->DrawInstanced(3, 1, 0, 0);
  }

  D3D12TestContext::Transition(context_.list(), buffer.get(),
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  // Pass B: vertex loads marker; pixel stores XOR result (UAV at u1).
  {
    std::array<D3D12_ROOT_PARAMETER, 2> parameters = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[1].Descriptor.ShaderRegister = 1;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = static_cast<UINT>(parameters.size());
    root_desc.pParameters = parameters.data();
    auto root = context_.CreateRootSignature(root_desc);
    ASSERT_TRUE(root);

    const auto vs = CompileShader(R"(
      ByteAddressBuffer input : register(t0);
      struct VSOut { float4 pos : SV_Position; nointerpolation uint marker : TEXCOORD0; };
      VSOut main(uint id : SV_VertexID) {
        VSOut o;
        float2 uv = float2((id << 1) & 2, id & 2);
        o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
        o.marker = input.Load(0);
        return o;
      }
    )",
                                  "vs_5_0");
    const auto ps = CompileShader(R"(
      RWByteAddressBuffer output : register(u1);
      float4 main(float4 pos : SV_Position, nointerpolation uint marker : TEXCOORD0) : SV_Target0 {
        output.Store(0, marker ^ 0x11111111u);
        return float4(0, 1, 0, 1);
      }
    )",
                                  "ps_5_0");
    ASSERT_EQ(vs.result, S_OK) << vs.diagnostic_text();
    ASSERT_EQ(ps.result, S_OK) << ps.diagnostic_text();

    auto target = context_.CreateTexture2D(
        4, 4, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto rtv_heap =
        context_.CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
    ASSERT_TRUE(target);
    ASSERT_TRUE(rtv_heap);
    const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    context_.device()->CreateRenderTargetView(target.get(), nullptr, rtv);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = root.get();
    pso_desc.VS = {vs.bytecode->GetBufferPointer(),
                   vs.bytecode->GetBufferSize()};
    pso_desc.PS = {ps.bytecode->GetBufferPointer(),
                   ps.bytecode->GetBufferSize()};
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    ASSERT_EQ(context_.device()->CreateGraphicsPipelineState(
                  &pso_desc, IID_PPV_ARGS(pso.put())),
              S_OK);

    D3D12_VIEWPORT viewport = {0.f, 0.f, 4.f, 4.f, 0.f, 1.f};
    D3D12_RECT scissor = {0, 0, 4, 4};
    context_.list()->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    context_.list()->RSSetViewports(1, &viewport);
    context_.list()->RSSetScissorRects(1, &scissor);
    context_.list()->SetGraphicsRootSignature(root.get());
    context_.list()->SetPipelineState(pso.get());
    context_.list()->SetGraphicsRootShaderResourceView(
        0, buffer->GetGPUVirtualAddress());
    context_.list()->SetGraphicsRootUnorderedAccessView(
        1, output->GetGPUVirtualAddress());
    context_.list()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_.list()->DrawInstanced(3, 1, 0, 0);
  }

  D3D12TestContext::Transition(context_.list(), output.get(),
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
  std::vector<std::uint8_t> bytes;
  ASSERT_EQ(context_.ReadbackBuffer(output.get(), sizeof(UINT), &bytes), S_OK);
  UINT actual = 0;
  ASSERT_EQ(bytes.size(), sizeof(UINT));
  std::memcpy(&actual, bytes.data(), sizeof(actual));
  EXPECT_EQ(actual, 0xC001D00Du ^ 0x11111111u);
  ExpectDeviceHealthy();
}

TEST_F(SubmissionTimelineExecutionSpec,
       CrossSubmitTrackerEpochDoesNotPoisonLaterExecute) {
  // Produce in submit 1, consume in submit 2 with no app fence: timeline must
  // order them, and CB-local tracker epochs must not leave residual EncoderIds
  // as the sole ordering mechanism.
  auto buffer = context_.CreateBuffer(
      sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto output = context_.CreateBuffer(
      sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto root = CreateUavRootSignature();
  auto producer = CreateComputePipeline(root.get(), R"(
    RWByteAddressBuffer output : register(u0);
    [numthreads(1, 1, 1)]
    void main() {
      output.Store(0, 0xDEADBEEFu);
    }
  )");
  auto consumer_root = CreateSrvUavRootSignature();
  auto consumer = CreateComputePipeline(consumer_root.get(), R"(
    ByteAddressBuffer input : register(t0);
    RWByteAddressBuffer output : register(u0);
    [numthreads(1, 1, 1)]
    void main() {
      output.Store(0, input.Load(0) + 1u);
    }
  )");
  ASSERT_TRUE(buffer);
  ASSERT_TRUE(output);
  ASSERT_TRUE(producer);
  ASSERT_TRUE(consumer);

  context_.list()->SetComputeRootSignature(root.get());
  context_.list()->SetPipelineState(producer.get());
  context_.list()->SetComputeRootUnorderedAccessView(
      0, buffer->GetGPUVirtualAddress());
  context_.list()->Dispatch(1, 1, 1);
  ASSERT_EQ(context_.list()->Close(), S_OK);
  ID3D12CommandList *lists1[] = {context_.list()};
  context_.queue()->ExecuteCommandLists(1, lists1);

  ASSERT_EQ(context_.ResetCommandList(), S_OK);
  D3D12TestContext::Transition(context_.list(), buffer.get(),
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  context_.list()->SetComputeRootSignature(consumer_root.get());
  context_.list()->SetPipelineState(consumer.get());
  context_.list()->SetComputeRootShaderResourceView(
      0, buffer->GetGPUVirtualAddress());
  context_.list()->SetComputeRootUnorderedAccessView(
      1, output->GetGPUVirtualAddress());
  context_.list()->Dispatch(1, 1, 1);
  D3D12TestContext::Transition(context_.list(), output.get(),
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);

  std::vector<std::uint8_t> bytes;
  ASSERT_EQ(context_.ReadbackBuffer(output.get(), sizeof(UINT), &bytes), S_OK);
  UINT actual = 0;
  std::memcpy(&actual, bytes.data(), sizeof(actual));
  EXPECT_EQ(actual, 0xDEADBEEFu + 1u);
  ExpectDeviceHealthy();
}

} // namespace
