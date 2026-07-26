#pragma once

// Stateless queries over a pipeline's DXIL shader set and its Metal shader
// reflection. Nothing here touches the command queue instance or its nested
// state types, so it can be compiled and analysed on its own.

#include "d3d12_pipeline.hpp"

namespace dxmt::d3d12 {

// True when the pipeline carries a geometry shader.

// True when the shader declares at least one buffer SRV argument, i.e. the
// native descriptor-table ABI has to bind a null buffer for the stage.
[[nodiscard]] bool NativeShaderUsesBufferSrv(const PipelineDxilShader &shader);

} // namespace dxmt::d3d12
