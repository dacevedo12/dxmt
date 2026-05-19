#include "nvngx_d3d12.hpp"
#include "com/com_pointer.hpp"
#include "../d3d12/d3d12_interfaces.hpp"
#include "../dxgi/dxgi_interfaces.h"
#include "log/log.hpp"
#include "nvngx_feature.hpp"
#include "nvngx_parameter.hpp"
#include <cmath>

namespace dxmt {

namespace {

bool g_d3d12_temporal_scaler_supported = false;
bool g_d3d12_temporal_scaler_known = false;
void
UpdateD3D12TemporalScalerSupport(ID3D12Device *device) {
  g_d3d12_temporal_scaler_known = true;
  g_d3d12_temporal_scaler_supported = false;
  if (!device)
    return;

  Com<IMTLD3D12Device> dxmt_device = nullptr;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxmt_device))))
    return;

  g_d3d12_temporal_scaler_supported =
      dxmt_device->GetMTLDevice().supportsFXTemporalScaler();
}

bool
QueryD3D12TemporalScalerSupport(IDXGIAdapter *adapter) {
  if (!adapter)
    return false;

  Com<IMTLDXGIAdapter> dxmt_adapter = nullptr;
  if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&dxmt_adapter))))
    return false;

  return dxmt_adapter->GetMTLDevice().supportsFXTemporalScaler();
}

NVNGX_RESULT
NVNGX_DLSS_GetOptimalSettingsCallbackD3D12(NVNGXParameter *params) {
  unsigned int width;
  unsigned int height;
  unsigned int out_width;
  unsigned int out_height;
  float scale = 0.0f;
  NVNGX_PERFQUALITY perf_quality_value;

  if (params->Get(NVNGX_Parameter_Width, &width) != NVNGX_RESULT_OK ||
      params->Get(NVNGX_Parameter_Height, &height) != NVNGX_RESULT_OK ||
      params->Get(NVNGX_Parameter_PerfQualityValue,
                  reinterpret_cast<int *>(&perf_quality_value)) !=
          NVNGX_RESULT_OK)
    return NVNGX_RESULT_FAIL;

  switch (perf_quality_value) {
  case NVNGX_PERFQUALITY_ULTRA_PERFORMANCE:
    scale = 1.0f / 3.0f;
    break;
  case NVNGX_PERFQUALITY_MAXPERF:
    scale = 0.5f;
    break;
  case NVNGX_PERFQUALITY_MAXQUALITY:
    scale = 1.0f / 1.5f;
    break;
  case NVNGX_PERFQUALITY_ULTRA_QUALITY:
    scale = 1.0f / 1.3f;
    break;
  case NVNGX_PERFQUALITY_DLAA:
    scale = 1.0f;
    break;
  default:
    scale = 58.0f / 100.0f;
    break;
  }

  out_width = std::ceil(width * scale);
  out_height = std::ceil(height * scale);

  params->Set(NVNGX_Parameter_Scale, scale);
  params->Set(NVNGX_Parameter_SuperSampling_ScaleFactor, scale);
  params->Set(NVNGX_Parameter_OutWidth, out_width);
  params->Set(NVNGX_Parameter_OutHeight, out_height);
  params->Set(NVNGX_Parameter_Sharpness, 0.0f);

  params->Set(NVNGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width,
              static_cast<unsigned int>(std::ceil(width / 3.0f)));
  params->Set(NVNGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height,
              static_cast<unsigned int>(std::ceil(height / 3.0f)));

  params->Set(NVNGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, width);
  params->Set(NVNGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, height);

  params->Set("DLSSMode", 1);

  params->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_DLAA, 0u);
  params->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, 0u);
  params->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_Quality, 0u);
  params->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_Balanced, 0u);
  params->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_Performance, 0u);
  params->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, 0u);

  return NVNGX_RESULT_OK;
}

NVNGX_RESULT
NVNGX_DLSS_GetStatsCallbackD3D12(NVNGXParameter *params) {
  params->Set("SizeInBytes", 0ull);
  return NVNGX_RESULT_OK;
}

NVNGX_RESULT
CreateD3D12CapabilityParameters(NVNGXParameter **out_params,
                                bool temporal_scaler_supported) {
  if (!out_params)
    return NVNGX_RESULT_INVALID_PARAMETER;

  auto out_parameters = new ParametersImpl();
  const unsigned int available = temporal_scaler_supported ? 1u : 0u;
  const unsigned int init_result =
      temporal_scaler_supported ? static_cast<unsigned int>(NVNGX_RESULT_OK)
                                : static_cast<unsigned int>(NVNGX_RESULT_FEATURE_NOT_SUPPORTED);
  out_parameters->Set("SuperSampling.Available", available);
  out_parameters->Set(NVSDK_NGX_EParameter_SuperSampling_Available, available);
  out_parameters->Set("SuperSampling.MinDriverVersionMajor", 0);
  out_parameters->Set("SuperSampling.MinDriverVersionMinor", 0);
  out_parameters->Set("SuperSampling.NeedsUpdatedDriver", 0);
  out_parameters->Set("SuperSampling.FeatureInitResult", init_result);
  out_parameters->Set("DLSS.Available", available);
  out_parameters->Set("DLSS.NeedsUpdatedDriver", 0);
  out_parameters->Set("DLSS.FeatureInitResult", init_result);
  out_parameters->Set("FrameGeneration.Available", 0u);
  out_parameters->Set("FrameGeneration.FeatureInitResult",
                      static_cast<unsigned int>(NVNGX_RESULT_FEATURE_NOT_SUPPORTED));
  out_parameters->Set("DLSSG.Available", 0u);
  out_parameters->Set("DLSSG.FeatureInitResult",
                      static_cast<unsigned int>(NVNGX_RESULT_FEATURE_NOT_SUPPORTED));
  out_parameters->Set("RayReconstruction.Available", 0u);
  out_parameters->Set("RayReconstruction.FeatureInitResult",
                      static_cast<unsigned int>(NVNGX_RESULT_FEATURE_NOT_SUPPORTED));
  out_parameters->Set("DLSS_RR.Available", 0u);
  out_parameters->Set("Snippet.OptLevel", 0);
  out_parameters->Set("Snippet.IsDevBranch", 0);
  out_parameters->Set(NVNGX_Parameter_DLSSOptimalSettingsCallback,
                      reinterpret_cast<void *>(&NVNGX_DLSS_GetOptimalSettingsCallbackD3D12));
  out_parameters->Set("DLSSGetStatsCallback",
                      reinterpret_cast<void *>(&NVNGX_DLSS_GetStatsCallbackD3D12));
  out_parameters->Set(NVNGX_Parameter_Sharpness, 0.0f);
  out_parameters->Set(NVNGX_Parameter_MV_Scale_X, 1.0f);
  out_parameters->Set(NVNGX_Parameter_MV_Scale_Y, 1.0f);
  out_parameters->Set("MV.Offset.X", 0.0f);
  out_parameters->Set("MV.Offset.Y", 0.0f);
  out_parameters->Set("DLSS.Exposure.Scale", 1.0f);
  out_parameters->Set(NVNGX_Parameter_PerfQualityValue, 2);
  out_parameters->Set("RTXValue", 0);
  out_parameters->Set("CreationNodeMask", 1);
  out_parameters->Set("VisibilityNodeMask", 1);
  out_parameters->Set("DLSS.Enable.Output.Subrects", 1);

  *out_params = out_parameters;
  return NVNGX_RESULT_OK;
}

} // namespace

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Init_Ext(unsigned long long id, const wchar_t *path,
                         ID3D12Device *device, unsigned int sdk_version,
                         const void *feature_info) {
  if (!device)
    return NVNGX_RESULT_INVALID_PARAMETER;

  UpdateD3D12TemporalScalerSupport(device);
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Init(unsigned long long id, const wchar_t *path,
                     ID3D12Device *device, const void *feature_info,
                     unsigned int sdk_version) {
  if (!device)
    return NVNGX_RESULT_INVALID_PARAMETER;

  UpdateD3D12TemporalScalerSupport(device);
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Init_ProjectID(const char *project, unsigned int engine_type,
                               const char *engine_version,
                               const wchar_t *path, ID3D12Device *device,
                               unsigned int sdk_version,
                               const void *feature_info) {
  if (!device)
    return NVNGX_RESULT_INVALID_PARAMETER;

  UpdateD3D12TemporalScalerSupport(device);
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Init_with_ProjectID(
    const char *project, unsigned int engine_type, const char *engine_version,
    const wchar_t *path, ID3D12Device *device, const void *feature_info,
    unsigned int sdk_version) {
  if (!device)
    return NVNGX_RESULT_INVALID_PARAMETER;

  UpdateD3D12TemporalScalerSupport(device);
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Shutdown() {
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Shutdown1(ID3D12Device *device) {
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_GetScratchBufferSize(unsigned int feature,
                                     const NVNGXParameter *params,
                                     size_t *out_size) {
  if (!out_size)
    return NVNGX_RESULT_INVALID_PARAMETER;

  *out_size = 0;
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList *command_list,
                              unsigned int feature, NVNGXParameter *params,
                              unsigned int **out_handle) {
  if (out_handle)
    *out_handle = nullptr;

  if (!command_list || !params || !out_handle)
    return NVNGX_RESULT_INVALID_PARAMETER;

  auto parameters = static_cast<ParametersImpl *>(params);
  Com<IMTLD3D12GraphicsCommandListExt> command_list_ext = nullptr;
  if (FAILED(command_list->QueryInterface(IID_PPV_ARGS(&command_list_ext))))
    return NVNGX_RESULT_INVALID_PARAMETER;

  if (feature == NVNGX_FEATURE_SUPERSAMPLING) {
    BOOL feature_supported = FALSE;
    if (FAILED(command_list_ext->CheckFeatureSupport(
            MTL_D3D12_FEATURE_METALFX_TEMPORAL_SCALER, &feature_supported,
            sizeof(feature_supported))) ||
        !feature_supported)
      return NVNGX_RESULT_FEATURE_NOT_SUPPORTED;

    auto dlss = std::make_unique<DLSSFeature>();
    dlss->feature = NVNGX_FEATURE_SUPERSAMPLING;
    if (NVNGX_FAILED(parameters->Get(NVNGX_Parameter_Width, &dlss->width)))
      return NVNGX_RESULT_INVALID_PARAMETER;
    if (NVNGX_FAILED(parameters->Get(NVNGX_Parameter_Height, &dlss->height)))
      return NVNGX_RESULT_INVALID_PARAMETER;
    if (NVNGX_FAILED(parameters->Get(NVNGX_Parameter_OutWidth,
                                     &dlss->target_width)))
      return NVNGX_RESULT_INVALID_PARAMETER;
    if (NVNGX_FAILED(parameters->Get(NVNGX_Parameter_OutHeight,
                                     &dlss->target_height)))
      return NVNGX_RESULT_INVALID_PARAMETER;
    if (NVNGX_FAILED(
            parameters->Get(NVNGX_Parameter_PerfQualityValue, &dlss->quality)))
      dlss->quality = 0;
    if (NVNGX_FAILED(parameters->Get(
            NVNGX_Parameter_DLSS_Feature_Create_Flags, &dlss->flag)))
      dlss->flag = 0;
    if (NVNGX_FAILED(parameters->Get("DLSS.Enable.Output.Subrects",
                                     &dlss->enable_output_subrects)))
      dlss->enable_output_subrects = 0;

    *out_handle = &dlss.release()->handle;
    return NVNGX_RESULT_OK;
  }

  WARN("NVSDK_NGX_D3D12_CreateFeature: feature ", feature, " is not supported");
  return NVNGX_RESULT_FEATURE_NOT_SUPPORTED;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList *command_list,
                                const unsigned int *handle,
                                NVNGXParameter *params, void *callback) {
  if (!command_list || !handle || !params)
    return NVNGX_RESULT_INVALID_PARAMETER;

  auto parameters = static_cast<ParametersImpl *>(params);
  switch (static_cast<CommonFeature *>((void *)handle)->feature) {
  case NVNGX_FEATURE_SUPERSAMPLING: {
    auto dlss = static_cast<DLSSFeature *>((void *)handle);
    Com<IMTLD3D12GraphicsCommandListExt> command_list_ext = nullptr;
    if (FAILED(command_list->QueryInterface(IID_PPV_ARGS(&command_list_ext))))
      return NVNGX_RESULT_INVALID_PARAMETER;

    ID3D12Resource *input = nullptr;
    ID3D12Resource *output = nullptr;
    ID3D12Resource *depth = nullptr;
    ID3D12Resource *motion_vectors = nullptr;
    ID3D12Resource *exposure = nullptr;

    if (NVNGX_FAILED(parameters->Get(NVNGX_Parameter_Color, &input)))
      return NVNGX_RESULT_INVALID_PARAMETER;
    if (NVNGX_FAILED(parameters->Get(NVNGX_Parameter_Output, &output)))
      return NVNGX_RESULT_INVALID_PARAMETER;
    if (NVNGX_FAILED(parameters->Get(NVNGX_Parameter_Depth, &depth)))
      return NVNGX_RESULT_INVALID_PARAMETER;
    if (NVNGX_FAILED(parameters->Get(NVNGX_Parameter_MotionVectors,
                                     &motion_vectors)))
      return NVNGX_RESULT_INVALID_PARAMETER;
    parameters->Get(NVNGX_Parameter_ExposureTexture, &exposure);

    uint32_t width = 0;
    uint32_t height = 0;
    if (NVNGX_FAILED(parameters->Get(
            NVNGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &width)))
      width = dlss->width;
    if (NVNGX_FAILED(parameters->Get(
            NVNGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &height)))
      height = dlss->height;

    if (!width || !height)
      return NVNGX_RESULT_INVALID_PARAMETER;

    MTL_TEMPORAL_UPSCALE_D3D12_DESC desc = {};
    desc.InputContentWidth = width;
    desc.InputContentHeight = height;
    desc.Color = input;
    desc.Output = output;
    desc.Depth = depth;
    desc.MotionVector = motion_vectors;
    desc.ExposureTexture = exposure;
    desc.DepthReversed = bool(dlss->flag & NVNGX_DLSS_FLAG_DEPTH_INVERTED);
    desc.AutoExposure = bool(dlss->flag & NVNGX_DLSS_FLAG_AUTO_EXPOSURE);
    desc.MotionVectorInDisplayRes =
        !bool(dlss->flag & NVNGX_DLSS_FLAG_MV_LOWRES);

    if (NVNGX_FAILED(parameters->Get(NVNGX_Parameter_Jitter_Offset_X,
                                     &desc.JitterOffsetX)))
      desc.JitterOffsetX = 0.0f;
    if (NVNGX_FAILED(parameters->Get(NVNGX_Parameter_Jitter_Offset_Y,
                                     &desc.JitterOffsetY)))
      desc.JitterOffsetY = 0.0f;

    int reset = 0;
    parameters->Get(NVNGX_Parameter_Reset, &reset);
    desc.InReset = reset;

    if (NVNGX_FAILED(parameters->Get(NVNGX_Parameter_MV_Scale_X,
                                     &desc.MotionVectorScaleX)))
      desc.MotionVectorScaleX = 1.0f;
    if (NVNGX_FAILED(parameters->Get(NVNGX_Parameter_MV_Scale_Y,
                                     &desc.MotionVectorScaleY)))
      desc.MotionVectorScaleY = 1.0f;
    if (NVNGX_FAILED(parameters->Get(NVNGX_Parameter_DLSS_Pre_Exposure,
                                     &desc.PreExposure)))
      desc.PreExposure = 0.0f;

    command_list_ext->TemporalUpscale(&desc);
    return NVNGX_RESULT_OK;
  }
  default:
    break;
  }

  return NVNGX_RESULT_FEATURE_NOT_SUPPORTED;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_ReleaseFeature(unsigned int *handle) {
  if (!handle)
    return NVNGX_RESULT_OK;

  switch (static_cast<CommonFeature *>((void *)handle)->feature) {
  case NVNGX_FEATURE_SUPERSAMPLING:
    delete static_cast<DLSSFeature *>((void *)handle);
    break;
  default:
    break;
  }
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_AllocateParameters(NVNGXParameter **out_params) {
  if (!out_params)
    return NVNGX_RESULT_INVALID_PARAMETER;

  *out_params = new ParametersImpl();
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_DestroyParameters(NVNGXParameter *params) {
  delete static_cast<ParametersImpl *>(params);
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_GetCapabilityParameters(NVNGXParameter **out_params) {
  return CreateD3D12CapabilityParameters(
      out_params,
      !g_d3d12_temporal_scaler_known || g_d3d12_temporal_scaler_supported);
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_GetParameters(NVNGXParameter **out_params) {
  return CreateD3D12CapabilityParameters(
      out_params,
      !g_d3d12_temporal_scaler_known || g_d3d12_temporal_scaler_supported);
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_GetFeatureRequirements(
    IDXGIAdapter *adapter, const NVNGX_FeatureDiscoveryInfo *discovery_info,
    NVNGX_FeatureRequirement *requirement) {
  if (!discovery_info || !requirement)
    return NVNGX_RESULT_INVALID_PARAMETER;

  if (discovery_info->FeatureID == NVNGX_FEATURE_SUPERSAMPLING) {
    requirement->FeatureSupported = QueryD3D12TemporalScalerSupport(adapter)
                                        ? NVNGX_FEATURE_SUPPORT_RESULT_SUPPORTED
                                        : NVNGX_FEATURE_SUPPORT_RESULT_UNSUPPORTED;
    requirement->MinHWArchitecture = 0;
    strcpy_s(requirement->MinOSVersion, "10.0.16299.0");
    return requirement->FeatureSupported == NVNGX_FEATURE_SUPPORT_RESULT_SUPPORTED
               ? NVNGX_RESULT_OK
               : NVNGX_RESULT_FAIL;
  }

  requirement->FeatureSupported = NVNGX_FEATURE_SUPPORT_RESULT_UNSUPPORTED;
  requirement->MinHWArchitecture = 0;
  strcpy_s(requirement->MinOSVersion, "10.0.16299.0");
  return NVNGX_RESULT_FAIL;
}

} // namespace dxmt
