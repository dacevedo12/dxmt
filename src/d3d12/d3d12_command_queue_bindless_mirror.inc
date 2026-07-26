// Bindless mirror slot fill/verify and descriptor binding encode.
// Included inside class CommandQueueImpl; not a standalone translation unit.
//
// Only the instance shims are left here. Each extracted helper takes the Metal
// device (and, for the diagnostic probes, the current frame sequence)
// explicitly, so the shims below are the only places that still need `device_`.
//
// ResolveDescriptorBufferOffset, MakeConstantBufferBindingFromDescriptor,
// MakeShaderResourceBindingFromDescriptor,
// MakeUnorderedAccessBindingFromDescriptor, ClearShaderResourceBinding,
// ClearUnorderedAccessBinding, BindConstantBufferDescriptor,
// BindShaderResourceDescriptor, BindUnorderedAccessDescriptor and
// BindSamplerDescriptor now live in d3d12_legacy_binding_encode.hpp.
//
// BindlessMirrorFillKind, BindlessMirrorVerifyEnabled() and
// BindlessMirrorVerifyShouldLog() live in d3d12_bindless_mirror_fill.hpp;
// VerifyBindlessMirrorSamplerSlot, VerifyBindlessMirrorTextureSlot and
// FillBindlessMirrorSlot in d3d12_bindless_mirror_slot_fill.hpp.
//
// BuildBindlessTextureWindowPayload and
// BuildBindlessTextureWindowPayloadFromSnapshot now live in
// d3d12_bindless_window_probe.hpp.
private:
  void VerifyBindlessMirrorSamplerDescriptor(ArgumentEncodingContext &enc,
                                             const DescriptorRecord &record,
                                             PipelineStage stage,
                                             const DXMT12_MTL4_SHADER_ARGUMENT *arg) {
    dxmt::d3d12::VerifyBindlessMirrorSamplerDescriptor(
        device_->GetMTLDevice(), enc, record, stage, arg);
  }

  BindlessMirrorFillKind MaybeFillBindlessMirrorSlot(
      ArgumentEncodingContext &enc, D3D12_DESCRIPTOR_RANGE_TYPE range_type,
      const DescriptorRecord &record, PipelineStage stage,
      const DXMT12_MTL4_SHADER_ARGUMENT *arg) {
    return dxmt::d3d12::MaybeFillBindlessMirrorSlot(
        device_->GetMTLDevice(), enc, range_type, record, stage, arg);
  }

  std::optional<DescriptorTextureSlotPayload>
  BuildBindlessTextureWindowPayload(
      const DescriptorRecord &record,
      const DXMT12_MTL4_SHADER_ARGUMENT &argument) {
    return dxmt::d3d12::BuildBindlessTextureWindowPayload(
        device_->GetMTLDevice(), record, argument);
  }

  Rc<Sampler> CreateStaticSampler(const D3D12_STATIC_SAMPLER_DESC &desc) {
    return CreateD3D12StaticSampler(device_->GetMTLDevice(), desc);
  }

  // kGraphicsBindingFingerprintOffset/Prime and the HashGraphicsBinding*
  // mixers live in d3d12_binding_fingerprint.hpp.
