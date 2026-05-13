#include "d3d12_root_signature.hpp"

#include "DXILParser/DXILParser.hpp"
#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "d3d12_agility.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace dxmt::d3d12 {
namespace {

constexpr uint32_t kRts0FourCC = dxil::MakeFourCC('R', 'T', 'S', '0');
constexpr uint32_t kDxbcFourCC = dxil::MakeFourCC('D', 'X', 'B', 'C');
constexpr size_t kDxbcHeaderSize = 32;
constexpr size_t kDxbcPartHeaderSize = 8;
constexpr size_t kRts0HeaderSize = 24;
constexpr size_t kRts0ParameterHeaderSize = 12;
constexpr size_t kRts0DescriptorTableHeaderSize = 8;
constexpr size_t kRts0DescriptorRange0Size = 20;
constexpr size_t kRts0DescriptorRange1Size = 24;
constexpr size_t kRts0RootConstantsSize = 12;
constexpr size_t kRts0RootDescriptor0Size = 8;
constexpr size_t kRts0RootDescriptor1Size = 12;
constexpr size_t kRts0StaticSamplerSize = 52;

uint32_t
ReadU32(std::span<const std::byte> data, size_t offset) {
  const auto *bytes = reinterpret_cast<const uint8_t *>(data.data());
  return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
         (uint32_t(bytes[offset + 2]) << 16) |
         (uint32_t(bytes[offset + 3]) << 24);
}

float
ReadF32(std::span<const std::byte> data, size_t offset) {
  uint32_t bits = ReadU32(data, offset);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void
WriteU32(std::vector<std::byte> &data, uint32_t value) {
  data.push_back(std::byte(value & 0xff));
  data.push_back(std::byte((value >> 8) & 0xff));
  data.push_back(std::byte((value >> 16) & 0xff));
  data.push_back(std::byte((value >> 24) & 0xff));
}

void
WriteU16(std::vector<std::byte> &data, uint16_t value) {
  data.push_back(std::byte(value & 0xff));
  data.push_back(std::byte((value >> 8) & 0xff));
}

void
WriteF32(std::vector<std::byte> &data, float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  WriteU32(data, bits);
}

void
PatchU32(std::vector<std::byte> &data, size_t offset, uint32_t value) {
  data[offset + 0] = std::byte(value & 0xff);
  data[offset + 1] = std::byte((value >> 8) & 0xff);
  data[offset + 2] = std::byte((value >> 16) & 0xff);
  data[offset + 3] = std::byte((value >> 24) & 0xff);
}

bool
CheckedEnd(size_t offset, size_t size, size_t limit, size_t &end) {
  if (offset > limit || size > limit - offset)
    return false;
  end = offset + size;
  return true;
}

uint32_t
VersionValue(D3D_ROOT_SIGNATURE_VERSION version) {
  if (version == D3D_ROOT_SIGNATURE_VERSION_1_2)
    return 3u;
  return version == D3D_ROOT_SIGNATURE_VERSION_1_1 ? 2u : 1u;
}

D3D_ROOT_SIGNATURE_VERSION
VersionFromValue(uint32_t version) {
  if (version == 3u)
    return D3D_ROOT_SIGNATURE_VERSION_1_2;
  return version == 2u ? D3D_ROOT_SIGNATURE_VERSION_1_1
                       : D3D_ROOT_SIGNATURE_VERSION_1_0;
}

struct RootSignatureStorage {
  D3D_ROOT_SIGNATURE_VERSION version = D3D_ROOT_SIGNATURE_VERSION_1_0;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned_desc = {};
  D3D12_ROOT_SIGNATURE_DESC desc_1_0 = {};
  D3D12_ROOT_SIGNATURE_DESC1 desc_1_1 = {};
  std::vector<D3D12_ROOT_PARAMETER> parameters_1_0;
  std::vector<D3D12_ROOT_PARAMETER1> parameters_1_1;
  std::vector<std::vector<D3D12_DESCRIPTOR_RANGE>> ranges_1_0;
  std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> ranges_1_1;
  std::vector<D3D12_STATIC_SAMPLER_DESC> static_samplers;
  std::vector<RootSignatureParameter> parameters;

  void fixPointers() {
    ranges_1_0.resize(parameters_1_0.size());
    ranges_1_1.resize(parameters_1_1.size());

    for (size_t i = 0; i < parameters_1_0.size(); i++) {
      if (parameters_1_0[i].ParameterType ==
          D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        parameters_1_0[i].DescriptorTable.NumDescriptorRanges =
            uint32_t(ranges_1_0[i].size());
        parameters_1_0[i].DescriptorTable.pDescriptorRanges =
            ranges_1_0[i].empty() ? nullptr : ranges_1_0[i].data();
      }
    }

    for (size_t i = 0; i < parameters_1_1.size(); i++) {
      if (parameters_1_1[i].ParameterType ==
          D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        parameters_1_1[i].DescriptorTable.NumDescriptorRanges =
            uint32_t(ranges_1_1[i].size());
        parameters_1_1[i].DescriptorTable.pDescriptorRanges =
            ranges_1_1[i].empty() ? nullptr : ranges_1_1[i].data();
      }
    }

    desc_1_0.NumParameters = uint32_t(parameters_1_0.size());
    desc_1_0.pParameters =
        parameters_1_0.empty() ? nullptr : parameters_1_0.data();
    desc_1_0.NumStaticSamplers = uint32_t(static_samplers.size());
    desc_1_0.pStaticSamplers =
        static_samplers.empty() ? nullptr : static_samplers.data();

    desc_1_1.NumParameters = uint32_t(parameters_1_1.size());
    desc_1_1.pParameters =
        parameters_1_1.empty() ? nullptr : parameters_1_1.data();
    desc_1_1.NumStaticSamplers = uint32_t(static_samplers.size());
    desc_1_1.pStaticSamplers =
        static_samplers.empty() ? nullptr : static_samplers.data();

    versioned_desc.Version = version;
    if (version != D3D_ROOT_SIGNATURE_VERSION_1_0)
      versioned_desc.Desc_1_1 = desc_1_1;
    else
      versioned_desc.Desc_1_0 = desc_1_0;
  }
};

UINT
NormalizeDescriptorCount(UINT count) {
  return count == UINT_MAX ? UINT_MAX : count;
}

void
BuildBindingParameters(RootSignatureStorage &storage) {
  storage.parameters.clear();
  const auto count = storage.version == D3D_ROOT_SIGNATURE_VERSION_1_0
                         ? storage.parameters_1_0.size()
                         : storage.parameters_1_1.size();
  storage.parameters.resize(count);

  for (size_t i = 0; i < count; i++) {
    auto &dst = storage.parameters[i];
    if (storage.version != D3D_ROOT_SIGNATURE_VERSION_1_0) {
      const auto &src = storage.parameters_1_1[i];
      dst.parameter_type = src.ParameterType;
      dst.visibility = src.ShaderVisibility;
      switch (src.ParameterType) {
      case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
        dst.ranges.reserve(storage.ranges_1_1[i].size());
        for (const auto &range : storage.ranges_1_1[i]) {
          dst.ranges.push_back({
              .range_type = range.RangeType,
              .base_shader_register = range.BaseShaderRegister,
              .register_space = range.RegisterSpace,
              .descriptor_count = NormalizeDescriptorCount(range.NumDescriptors),
              .offset_in_descriptors_from_table_start =
                  range.OffsetInDescriptorsFromTableStart,
              .flags = range.Flags,
          });
        }
        break;
      case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
        dst.constants = src.Constants;
        break;
      case D3D12_ROOT_PARAMETER_TYPE_CBV:
      case D3D12_ROOT_PARAMETER_TYPE_SRV:
      case D3D12_ROOT_PARAMETER_TYPE_UAV:
        dst.descriptor = src.Descriptor;
        dst.descriptor_flags = src.Descriptor.Flags;
        break;
      }
    } else {
      const auto &src = storage.parameters_1_0[i];
      dst.parameter_type = src.ParameterType;
      dst.visibility = src.ShaderVisibility;
      switch (src.ParameterType) {
      case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
        dst.ranges.reserve(storage.ranges_1_0[i].size());
        for (const auto &range : storage.ranges_1_0[i]) {
          dst.ranges.push_back({
              .range_type = range.RangeType,
              .base_shader_register = range.BaseShaderRegister,
              .register_space = range.RegisterSpace,
              .descriptor_count = NormalizeDescriptorCount(range.NumDescriptors),
              .offset_in_descriptors_from_table_start =
                  range.OffsetInDescriptorsFromTableStart,
              .flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE,
          });
        }
        break;
      case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
        dst.constants = src.Constants;
        break;
      case D3D12_ROOT_PARAMETER_TYPE_CBV:
      case D3D12_ROOT_PARAMETER_TYPE_SRV:
      case D3D12_ROOT_PARAMETER_TYPE_UAV:
        dst.descriptor.ShaderRegister = src.Descriptor.ShaderRegister;
        dst.descriptor.RegisterSpace = src.Descriptor.RegisterSpace;
        dst.descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
        break;
      }
    }
  }
}

bool
ValidateDesc0(const D3D12_ROOT_SIGNATURE_DESC &desc) {
  if (desc.NumParameters && !desc.pParameters)
    return false;
  if (desc.NumStaticSamplers && !desc.pStaticSamplers)
    return false;

  for (UINT i = 0; i < desc.NumParameters; i++) {
    const auto &parameter = desc.pParameters[i];
    if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
        parameter.DescriptorTable.NumDescriptorRanges &&
        !parameter.DescriptorTable.pDescriptorRanges)
      return false;
    if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
        parameter.DescriptorTable.NumDescriptorRanges) {
      bool sampler = false;
      bool non_sampler = false;
      for (UINT j = 0; j < parameter.DescriptorTable.NumDescriptorRanges; j++) {
        const auto &range = parameter.DescriptorTable.pDescriptorRanges[j];
        if (range.NumDescriptors == 0)
          return false;
        if (range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
          sampler = true;
        else
          non_sampler = true;
      }
      if (sampler && non_sampler)
        return false;
    }
  }

  UINT root_cost = 0;
  for (UINT i = 0; i < desc.NumParameters; i++) {
    const auto &parameter = desc.pParameters[i];
    switch (parameter.ParameterType) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
      root_cost += 1;
      break;
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
      root_cost += parameter.Constants.Num32BitValues + 1;
      break;
    case D3D12_ROOT_PARAMETER_TYPE_CBV:
    case D3D12_ROOT_PARAMETER_TYPE_SRV:
    case D3D12_ROOT_PARAMETER_TYPE_UAV:
      root_cost += 2;
      break;
    default:
      return false;
    }
  }
  if (root_cost > D3D12_MAX_ROOT_COST)
    return false;

  return true;
}

bool
ValidateDesc1(const D3D12_ROOT_SIGNATURE_DESC1 &desc) {
  if (desc.NumParameters && !desc.pParameters)
    return false;
  if (desc.NumStaticSamplers && !desc.pStaticSamplers)
    return false;

  for (UINT i = 0; i < desc.NumParameters; i++) {
    const auto &parameter = desc.pParameters[i];
    if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
        parameter.DescriptorTable.NumDescriptorRanges &&
        !parameter.DescriptorTable.pDescriptorRanges)
      return false;
    if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
        parameter.DescriptorTable.NumDescriptorRanges) {
      bool sampler = false;
      bool non_sampler = false;
      for (UINT j = 0; j < parameter.DescriptorTable.NumDescriptorRanges; j++) {
        const auto &range = parameter.DescriptorTable.pDescriptorRanges[j];
        if (range.NumDescriptors == 0)
          return false;
        if (range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
          sampler = true;
        else
          non_sampler = true;
      }
      if (sampler && non_sampler)
        return false;
    }
  }

  UINT root_cost = 0;
  for (UINT i = 0; i < desc.NumParameters; i++) {
    const auto &parameter = desc.pParameters[i];
    switch (parameter.ParameterType) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
      root_cost += 1;
      break;
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
      root_cost += parameter.Constants.Num32BitValues + 1;
      break;
    case D3D12_ROOT_PARAMETER_TYPE_CBV:
    case D3D12_ROOT_PARAMETER_TYPE_SRV:
    case D3D12_ROOT_PARAMETER_TYPE_UAV:
      root_cost += 2;
      break;
    default:
      return false;
    }
  }
  if (root_cost > D3D12_MAX_ROOT_COST)
    return false;

  return true;
}

RootSignatureStorage
CloneFromDesc0(const D3D12_ROOT_SIGNATURE_DESC &desc) {
  RootSignatureStorage storage = {};
  storage.version = D3D_ROOT_SIGNATURE_VERSION_1_0;
  storage.desc_1_0.Flags = desc.Flags;
  storage.desc_1_1.Flags = desc.Flags;
  if (desc.NumStaticSamplers)
    storage.static_samplers.assign(
        desc.pStaticSamplers, desc.pStaticSamplers + desc.NumStaticSamplers);
  storage.parameters_1_0.resize(desc.NumParameters);
  storage.parameters_1_1.resize(desc.NumParameters);
  storage.ranges_1_0.resize(desc.NumParameters);
  storage.ranges_1_1.resize(desc.NumParameters);

  for (UINT i = 0; i < desc.NumParameters; i++) {
    const auto &src = desc.pParameters[i];
    auto &dst0 = storage.parameters_1_0[i];
    auto &dst1 = storage.parameters_1_1[i];
    dst0.ParameterType = src.ParameterType;
    dst0.ShaderVisibility = src.ShaderVisibility;
    dst1.ParameterType = src.ParameterType;
    dst1.ShaderVisibility = src.ShaderVisibility;

    switch (src.ParameterType) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
      if (src.DescriptorTable.NumDescriptorRanges)
        storage.ranges_1_0[i].assign(
            src.DescriptorTable.pDescriptorRanges,
            src.DescriptorTable.pDescriptorRanges +
                src.DescriptorTable.NumDescriptorRanges);
      storage.ranges_1_1[i].reserve(storage.ranges_1_0[i].size());
      for (const auto &range : storage.ranges_1_0[i]) {
        storage.ranges_1_1[i].push_back({
            .RangeType = range.RangeType,
            .NumDescriptors = range.NumDescriptors,
            .BaseShaderRegister = range.BaseShaderRegister,
            .RegisterSpace = range.RegisterSpace,
            .Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE,
            .OffsetInDescriptorsFromTableStart =
                range.OffsetInDescriptorsFromTableStart,
        });
      }
      break;
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
      dst0.Constants = src.Constants;
      dst1.Constants = src.Constants;
      break;
    case D3D12_ROOT_PARAMETER_TYPE_CBV:
    case D3D12_ROOT_PARAMETER_TYPE_SRV:
    case D3D12_ROOT_PARAMETER_TYPE_UAV:
      dst0.Descriptor = src.Descriptor;
      dst1.Descriptor.ShaderRegister = src.Descriptor.ShaderRegister;
      dst1.Descriptor.RegisterSpace = src.Descriptor.RegisterSpace;
      dst1.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
      break;
    default:
      break;
    }
  }

  storage.fixPointers();
  BuildBindingParameters(storage);
  return storage;
}

RootSignatureStorage
CloneFromDesc1(const D3D12_ROOT_SIGNATURE_DESC1 &desc,
               D3D_ROOT_SIGNATURE_VERSION version) {
  RootSignatureStorage storage = {};
  storage.version = version;
  storage.desc_1_0.Flags = desc.Flags;
  storage.desc_1_1.Flags = desc.Flags;
  if (desc.NumStaticSamplers)
    storage.static_samplers.assign(
        desc.pStaticSamplers, desc.pStaticSamplers + desc.NumStaticSamplers);
  storage.parameters_1_0.resize(desc.NumParameters);
  storage.parameters_1_1.resize(desc.NumParameters);
  storage.ranges_1_0.resize(desc.NumParameters);
  storage.ranges_1_1.resize(desc.NumParameters);

  for (UINT i = 0; i < desc.NumParameters; i++) {
    const auto &src = desc.pParameters[i];
    auto &dst0 = storage.parameters_1_0[i];
    auto &dst1 = storage.parameters_1_1[i];
    dst0.ParameterType = src.ParameterType;
    dst0.ShaderVisibility = src.ShaderVisibility;
    dst1.ParameterType = src.ParameterType;
    dst1.ShaderVisibility = src.ShaderVisibility;

    switch (src.ParameterType) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
      if (src.DescriptorTable.NumDescriptorRanges)
        storage.ranges_1_1[i].assign(
            src.DescriptorTable.pDescriptorRanges,
            src.DescriptorTable.pDescriptorRanges +
                src.DescriptorTable.NumDescriptorRanges);
      storage.ranges_1_0[i].reserve(storage.ranges_1_1[i].size());
      for (const auto &range : storage.ranges_1_1[i]) {
        storage.ranges_1_0[i].push_back({
            .RangeType = range.RangeType,
            .NumDescriptors = range.NumDescriptors,
            .BaseShaderRegister = range.BaseShaderRegister,
            .RegisterSpace = range.RegisterSpace,
            .OffsetInDescriptorsFromTableStart =
                range.OffsetInDescriptorsFromTableStart,
        });
      }
      break;
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
      dst0.Constants = src.Constants;
      dst1.Constants = src.Constants;
      break;
    case D3D12_ROOT_PARAMETER_TYPE_CBV:
    case D3D12_ROOT_PARAMETER_TYPE_SRV:
    case D3D12_ROOT_PARAMETER_TYPE_UAV:
      dst0.Descriptor.ShaderRegister = src.Descriptor.ShaderRegister;
      dst0.Descriptor.RegisterSpace = src.Descriptor.RegisterSpace;
      dst1.Descriptor = src.Descriptor;
      break;
    default:
      break;
    }
  }

  storage.fixPointers();
  BuildBindingParameters(storage);
  return storage;
}

std::optional<RootSignatureStorage>
CloneFromVersionedDesc(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC &desc) {
  switch (desc.Version) {
  case D3D_ROOT_SIGNATURE_VERSION_1_0:
    if (!ValidateDesc0(desc.Desc_1_0))
      return std::nullopt;
    return CloneFromDesc0(desc.Desc_1_0);
  case D3D_ROOT_SIGNATURE_VERSION_1_1:
    if (!ValidateDesc1(desc.Desc_1_1))
      return std::nullopt;
    return CloneFromDesc1(desc.Desc_1_1, D3D_ROOT_SIGNATURE_VERSION_1_1);
  case D3D_ROOT_SIGNATURE_VERSION_1_2:
    if (!ValidateDesc1(desc.Desc_1_1))
      return std::nullopt;
    return CloneFromDesc1(desc.Desc_1_1, D3D_ROOT_SIGNATURE_VERSION_1_2);
  default:
    return std::nullopt;
  }
}

bool
ExtractRts0Part(std::span<const std::byte> blob,
                std::span<const std::byte> &rts0) {
  if (blob.size() < sizeof(uint32_t))
    return false;

  if (ReadU32(blob, 0) != kDxbcFourCC) {
    rts0 = blob;
    return true;
  }

  dxil::ContainerInfo container = {};
  auto status = dxil::ParseContainer(blob.data(), blob.size(), container);
  if (status != dxil::ParseStatus::Ok)
    return false;

  const auto *part = container.findPart(kRts0FourCC);
  if (!part)
    return false;

  rts0 = std::span<const std::byte>(
      reinterpret_cast<const std::byte *>(part->data.data()), part->data.size());
  return true;
}

bool
ParseRts0(std::span<const std::byte> rts0, RootSignatureStorage &storage) {
  if (rts0.size() < kRts0HeaderSize)
    return false;

  const auto version_value = ReadU32(rts0, 0);
  if (version_value != 1 && version_value != 2 && version_value != 3)
    return false;

  const auto parameter_count = ReadU32(rts0, 4);
  const auto parameter_offset = ReadU32(rts0, 8);
  const auto sampler_count = ReadU32(rts0, 12);
  const auto sampler_offset = ReadU32(rts0, 16);
  const auto flags = D3D12_ROOT_SIGNATURE_FLAGS(ReadU32(rts0, 20));
  size_t end = 0;

  if (parameter_count) {
    if (!CheckedEnd(parameter_offset,
                    size_t(parameter_count) * kRts0ParameterHeaderSize,
                    rts0.size(), end))
      return false;
  }

  if (sampler_count) {
    if (!CheckedEnd(sampler_offset, size_t(sampler_count) * kRts0StaticSamplerSize,
                    rts0.size(), end))
      return false;
  }

  storage = {};
  storage.version = VersionFromValue(version_value);
  storage.desc_1_0.Flags = flags;
  storage.desc_1_1.Flags = flags;
  storage.parameters_1_0.resize(parameter_count);
  storage.parameters_1_1.resize(parameter_count);
  storage.ranges_1_0.resize(parameter_count);
  storage.ranges_1_1.resize(parameter_count);
  storage.static_samplers.resize(sampler_count);

  for (uint32_t i = 0; i < parameter_count; i++) {
    const auto header_offset = parameter_offset + size_t(i) * kRts0ParameterHeaderSize;
    const auto parameter_type =
        D3D12_ROOT_PARAMETER_TYPE(ReadU32(rts0, header_offset + 0));
    const auto shader_visibility =
        D3D12_SHADER_VISIBILITY(ReadU32(rts0, header_offset + 4));
    const auto data_offset = ReadU32(rts0, header_offset + 8);
    auto &param0 = storage.parameters_1_0[i];
    auto &param1 = storage.parameters_1_1[i];
    param0.ParameterType = parameter_type;
    param0.ShaderVisibility = shader_visibility;
    param1.ParameterType = parameter_type;
    param1.ShaderVisibility = shader_visibility;

    switch (parameter_type) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
      if (!CheckedEnd(data_offset, kRts0DescriptorTableHeaderSize,
                      rts0.size(), end))
        return false;
      const auto range_count = ReadU32(rts0, data_offset + 0);
      const auto ranges_offset = ReadU32(rts0, data_offset + 4);
      const auto range_size = version_value == 2 ? kRts0DescriptorRange1Size
                                                 : kRts0DescriptorRange0Size;
      if (!CheckedEnd(ranges_offset, size_t(range_count) * range_size,
                      rts0.size(), end))
        return false;

      storage.ranges_1_0[i].reserve(range_count);
      storage.ranges_1_1[i].reserve(range_count);
      for (uint32_t j = 0; j < range_count; j++) {
        const auto range_offset = ranges_offset + size_t(j) * range_size;
        const auto type = D3D12_DESCRIPTOR_RANGE_TYPE(ReadU32(rts0, range_offset + 0));
        const auto count = ReadU32(rts0, range_offset + 4);
        const auto shader_register = ReadU32(rts0, range_offset + 8);
        const auto register_space = ReadU32(rts0, range_offset + 12);
        const auto range_flags =
            version_value == 2
                ? D3D12_DESCRIPTOR_RANGE_FLAGS(ReadU32(rts0, range_offset + 16))
                : D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
        const auto range_desc_offset =
            ReadU32(rts0, range_offset + (version_value == 2 ? 20 : 16));

        storage.ranges_1_0[i].push_back({
            .RangeType = type,
            .NumDescriptors = count,
            .BaseShaderRegister = shader_register,
            .RegisterSpace = register_space,
            .OffsetInDescriptorsFromTableStart = range_desc_offset,
        });
        storage.ranges_1_1[i].push_back({
            .RangeType = type,
            .NumDescriptors = count,
            .BaseShaderRegister = shader_register,
            .RegisterSpace = register_space,
            .Flags = range_flags,
            .OffsetInDescriptorsFromTableStart = range_desc_offset,
        });
      }
      break;
    }
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
      if (!CheckedEnd(data_offset, kRts0RootConstantsSize, rts0.size(), end))
        return false;
      param0.Constants.ShaderRegister = ReadU32(rts0, data_offset + 0);
      param0.Constants.RegisterSpace = ReadU32(rts0, data_offset + 4);
      param0.Constants.Num32BitValues = ReadU32(rts0, data_offset + 8);
      param1.Constants = param0.Constants;
      break;
    case D3D12_ROOT_PARAMETER_TYPE_CBV:
    case D3D12_ROOT_PARAMETER_TYPE_SRV:
    case D3D12_ROOT_PARAMETER_TYPE_UAV: {
      const auto descriptor_size = version_value == 2 ? kRts0RootDescriptor1Size
                                                      : kRts0RootDescriptor0Size;
      if (!CheckedEnd(data_offset, descriptor_size, rts0.size(), end))
        return false;
      const auto shader_register = ReadU32(rts0, data_offset + 0);
      const auto register_space = ReadU32(rts0, data_offset + 4);
      const auto descriptor_flags =
          version_value == 2
              ? D3D12_ROOT_DESCRIPTOR_FLAGS(ReadU32(rts0, data_offset + 8))
              : D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
      param0.Descriptor.ShaderRegister = shader_register;
      param0.Descriptor.RegisterSpace = register_space;
      param1.Descriptor.ShaderRegister = shader_register;
      param1.Descriptor.RegisterSpace = register_space;
      param1.Descriptor.Flags = descriptor_flags;
      break;
    }
    default:
      return false;
    }
  }

  for (uint32_t i = 0; i < sampler_count; i++) {
    const auto offset = sampler_offset + size_t(i) * kRts0StaticSamplerSize;
    auto &sampler = storage.static_samplers[i];
    sampler.Filter = D3D12_FILTER(ReadU32(rts0, offset + 0));
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE(ReadU32(rts0, offset + 4));
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE(ReadU32(rts0, offset + 8));
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE(ReadU32(rts0, offset + 12));
    sampler.MipLODBias = ReadF32(rts0, offset + 16);
    sampler.MaxAnisotropy = ReadU32(rts0, offset + 20);
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC(ReadU32(rts0, offset + 24));
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR(ReadU32(rts0, offset + 28));
    sampler.MinLOD = ReadF32(rts0, offset + 32);
    sampler.MaxLOD = ReadF32(rts0, offset + 36);
    sampler.ShaderRegister = ReadU32(rts0, offset + 40);
    sampler.RegisterSpace = ReadU32(rts0, offset + 44);
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY(ReadU32(rts0, offset + 48));
  }

  storage.fixPointers();
  BuildBindingParameters(storage);
  return true;
}

bool
ParseRootSignatureBlob(std::span<const std::byte> blob,
                       RootSignatureStorage &storage) {
  std::span<const std::byte> rts0;
  return ExtractRts0Part(blob, rts0) && ParseRts0(rts0, storage);
}

std::vector<std::byte>
SerializeRts0(const RootSignatureStorage &storage,
              D3D_ROOT_SIGNATURE_VERSION version) {
  const auto version_value = VersionValue(version);
  const auto parameter_count = version == D3D_ROOT_SIGNATURE_VERSION_1_0
                                   ? storage.parameters_1_0.size()
                                   : storage.parameters_1_1.size();
  std::vector<std::byte> data;
  data.reserve(kRts0HeaderSize + parameter_count * kRts0ParameterHeaderSize);

  WriteU32(data, version_value);
  WriteU32(data, uint32_t(parameter_count));
  WriteU32(data, parameter_count ? uint32_t(kRts0HeaderSize) : 0u);
  WriteU32(data, uint32_t(storage.static_samplers.size()));
  const auto sampler_offset_patch = data.size();
  WriteU32(data, 0);
  WriteU32(data, uint32_t(storage.desc_1_1.Flags));

  std::vector<size_t> parameter_offset_patches;
  parameter_offset_patches.reserve(parameter_count);
  for (size_t i = 0; i < parameter_count; i++) {
    const auto parameter_type =
        version == D3D_ROOT_SIGNATURE_VERSION_1_0
            ? storage.parameters_1_0[i].ParameterType
            : storage.parameters_1_1[i].ParameterType;
    const auto shader_visibility =
        version == D3D_ROOT_SIGNATURE_VERSION_1_0
            ? storage.parameters_1_0[i].ShaderVisibility
            : storage.parameters_1_1[i].ShaderVisibility;
    WriteU32(data, uint32_t(parameter_type));
    WriteU32(data, uint32_t(shader_visibility));
    parameter_offset_patches.push_back(data.size());
    WriteU32(data, 0);
  }

  for (size_t i = 0; i < parameter_count; i++) {
    PatchU32(data, parameter_offset_patches[i], uint32_t(data.size()));
    const auto parameter_type =
        version == D3D_ROOT_SIGNATURE_VERSION_1_0
            ? storage.parameters_1_0[i].ParameterType
            : storage.parameters_1_1[i].ParameterType;
    switch (parameter_type) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
      const auto &ranges0 = storage.ranges_1_0[i];
      const auto &ranges1 = storage.ranges_1_1[i];
      WriteU32(data, uint32_t(version == D3D_ROOT_SIGNATURE_VERSION_1_0
                                  ? ranges0.size()
                                  : ranges1.size()));
      WriteU32(data, uint32_t(data.size() + sizeof(uint32_t)));
      if (version != D3D_ROOT_SIGNATURE_VERSION_1_0) {
        for (const auto &range : ranges1) {
          WriteU32(data, uint32_t(range.RangeType));
          WriteU32(data, range.NumDescriptors);
          WriteU32(data, range.BaseShaderRegister);
          WriteU32(data, range.RegisterSpace);
          WriteU32(data, uint32_t(range.Flags));
          WriteU32(data, range.OffsetInDescriptorsFromTableStart);
        }
      } else {
        for (const auto &range : ranges0) {
          WriteU32(data, uint32_t(range.RangeType));
          WriteU32(data, range.NumDescriptors);
          WriteU32(data, range.BaseShaderRegister);
          WriteU32(data, range.RegisterSpace);
          WriteU32(data, range.OffsetInDescriptorsFromTableStart);
        }
      }
      break;
    }
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
      WriteU32(data, storage.parameters_1_0[i].Constants.ShaderRegister);
      WriteU32(data, storage.parameters_1_0[i].Constants.RegisterSpace);
      WriteU32(data, storage.parameters_1_0[i].Constants.Num32BitValues);
      break;
    case D3D12_ROOT_PARAMETER_TYPE_CBV:
    case D3D12_ROOT_PARAMETER_TYPE_SRV:
    case D3D12_ROOT_PARAMETER_TYPE_UAV:
      if (version != D3D_ROOT_SIGNATURE_VERSION_1_0) {
        const auto &descriptor = storage.parameters_1_1[i].Descriptor;
        WriteU32(data, descriptor.ShaderRegister);
        WriteU32(data, descriptor.RegisterSpace);
        WriteU32(data, uint32_t(descriptor.Flags));
      } else {
        const auto &descriptor = storage.parameters_1_0[i].Descriptor;
        WriteU32(data, descriptor.ShaderRegister);
        WriteU32(data, descriptor.RegisterSpace);
      }
      break;
    default:
      break;
    }
  }

  if (!storage.static_samplers.empty()) {
    PatchU32(data, sampler_offset_patch, uint32_t(data.size()));
    for (const auto &sampler : storage.static_samplers) {
      WriteU32(data, uint32_t(sampler.Filter));
      WriteU32(data, uint32_t(sampler.AddressU));
      WriteU32(data, uint32_t(sampler.AddressV));
      WriteU32(data, uint32_t(sampler.AddressW));
      WriteF32(data, sampler.MipLODBias);
      WriteU32(data, sampler.MaxAnisotropy);
      WriteU32(data, uint32_t(sampler.ComparisonFunc));
      WriteU32(data, uint32_t(sampler.BorderColor));
      WriteF32(data, sampler.MinLOD);
      WriteF32(data, sampler.MaxLOD);
      WriteU32(data, sampler.ShaderRegister);
      WriteU32(data, sampler.RegisterSpace);
      WriteU32(data, uint32_t(sampler.ShaderVisibility));
    }
  }

  return data;
}

std::vector<std::byte>
BuildDxbcContainer(std::span<const std::byte> rts0) {
  const uint32_t part_count = 1;
  const uint32_t part_offset = uint32_t(kDxbcHeaderSize + sizeof(uint32_t));
  const uint32_t container_size =
      part_offset + uint32_t(kDxbcPartHeaderSize + rts0.size());

  std::vector<std::byte> data;
  data.reserve(container_size);
  WriteU32(data, kDxbcFourCC);
  for (uint32_t i = 0; i < 4; i++)
    WriteU32(data, 0);
  WriteU16(data, 1);
  WriteU16(data, 0);
  WriteU32(data, container_size);
  WriteU32(data, part_count);
  WriteU32(data, part_offset);
  WriteU32(data, kRts0FourCC);
  WriteU32(data, uint32_t(rts0.size()));
  data.insert(data.end(), rts0.begin(), rts0.end());
  return data;
}

std::vector<std::byte>
SerializeRootSignature(const RootSignatureStorage &storage,
                       D3D_ROOT_SIGNATURE_VERSION version) {
  auto rts0 = SerializeRts0(storage, version);
  return BuildDxbcContainer(rts0);
}

class BlobImpl final : public ComObjectWithInitialRef<ID3DBlob> {
public:
  explicit BlobImpl(std::vector<std::byte> &&data) : data_(std::move(data)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D10Blob)) {
      *object = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  void *STDMETHODCALLTYPE GetBufferPointer() override {
    return data_.empty() ? nullptr : data_.data();
  }

  SIZE_T STDMETHODCALLTYPE GetBufferSize() override {
    return data_.size();
  }

private:
  std::vector<std::byte> data_;
};

Com<ID3DBlob>
CreateBlob(std::vector<std::byte> &&data) {
  return Com<ID3DBlob>::transfer(new BlobImpl(std::move(data)));
}

Com<ID3DBlob>
CreateErrorBlob(const char *message) {
  std::vector<std::byte> data;
  const auto length = std::strlen(message) + 1;
  data.resize(length);
  std::memcpy(data.data(), message, length);
  return CreateBlob(std::move(data));
}

void
SetErrorBlob(ID3DBlob **error_blob, const char *message) {
  InitReturnPtr(error_blob);
  if (error_blob)
    *error_blob = CreateErrorBlob(message).takeOwnership();
}

class RootSignatureImpl final : public ComObjectWithInitialRef<ID3D12RootSignature>,
                                public RootSignature {
public:
  RootSignatureImpl(IMTLD3D12Device *device, RootSignatureStorage &&storage,
                    std::vector<std::byte> &&serialized_blob)
      : device_(device), storage_(std::move(storage)),
        serialized_blob_(std::move(serialized_blob)) {}

  ULONG STDMETHODCALLTYPE AddRefPrivate() override {
    ComObjectWithInitialRef<ID3D12RootSignature>::AddRefPrivate();
    return 0;
  }

  void STDMETHODCALLTYPE ReleasePrivate() override {
    ComObjectWithInitialRef<ID3D12RootSignature>::ReleasePrivate();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12RootSignature)) {
      *object = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12RootSignature), riid))
      WARN("D3D12RootSignature: unknown interface query ", str::format(riid));
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,
                                           void *data) override {
    return private_data_.getData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,
                                           const void *data) override {
    return private_data_.setData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,
                                                   const IUnknown *data) override {
    return private_data_.setInterface(guid, data);
  }

  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *name) override {
    name_ = name ? str::fromws(name) : std::string();
    return private_data_.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  const D3D12_VERSIONED_ROOT_SIGNATURE_DESC &GetVersionedDesc() const override {
    return storage_.versioned_desc;
  }

  std::span<const std::byte> GetSerializedBlob() const override {
    return serialized_blob_;
  }

  std::span<const RootSignatureParameter> GetParameters() const override {
    return storage_.parameters;
  }

  std::span<const D3D12_STATIC_SAMPLER_DESC>
  GetStaticSamplers() const override {
    return storage_.static_samplers;
  }

private:
  Com<IMTLD3D12Device> device_;
  RootSignatureStorage storage_;
  std::vector<std::byte> serialized_blob_;
  ComPrivateData private_data_;
  std::string name_;
};

class RootSignatureDeserializerImpl final
    : public ComObjectWithInitialRef<ID3D12RootSignatureDeserializer,
                                     ID3D12VersionedRootSignatureDeserializer> {
public:
  explicit RootSignatureDeserializerImpl(RootSignatureStorage &&storage)
      : storage_(std::move(storage)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;

    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(ID3D12RootSignatureDeserializer)) {
      *object = ref(static_cast<ID3D12RootSignatureDeserializer *>(this));
      return S_OK;
    }
    if (riid == __uuidof(ID3D12VersionedRootSignatureDeserializer)) {
      *object = ref(static_cast<ID3D12VersionedRootSignatureDeserializer *>(this));
      return S_OK;
    }

    return E_NOINTERFACE;
  }

  const D3D12_ROOT_SIGNATURE_DESC *STDMETHODCALLTYPE
  GetRootSignatureDesc() override {
    return &storage_.desc_1_0;
  }

  HRESULT STDMETHODCALLTYPE
  GetRootSignatureDescAtVersion(
      D3D_ROOT_SIGNATURE_VERSION version,
      const D3D12_VERSIONED_ROOT_SIGNATURE_DESC **desc) override {
    if (!desc)
      return E_POINTER;
    *desc = nullptr;

    switch (version) {
    case D3D_ROOT_SIGNATURE_VERSION_1_0:
      converted_desc_.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
      converted_desc_.Desc_1_0 = storage_.desc_1_0;
      *desc = &converted_desc_;
      return S_OK;
    case D3D_ROOT_SIGNATURE_VERSION_1_1:
      converted_desc_.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
      converted_desc_.Desc_1_1 = storage_.desc_1_1;
      *desc = &converted_desc_;
      return S_OK;
    default:
      return WARN_E_INVALIDARG(__func__);
    }
  }

  const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *STDMETHODCALLTYPE
  GetUnconvertedRootSignatureDesc() override {
    return &storage_.versioned_desc;
  }

private:
  RootSignatureStorage storage_;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC converted_desc_ = {};
};

HRESULT
CreateRootSignatureDeserializerImpl(std::span<const std::byte> blob,
                                    REFIID iid, void **deserializer) {
  InitReturnPtr(deserializer);
  if (!deserializer)
    return E_POINTER;

  RootSignatureStorage storage = {};
  if (!ParseRootSignatureBlob(blob, storage))
    return WARN_E_INVALIDARG(__func__);

  auto object = Com<ID3D12VersionedRootSignatureDeserializer>::transfer(
      new RootSignatureDeserializerImpl(std::move(storage)));
  return object->QueryInterface(iid, deserializer);
}

HRESULT
CreateRootSignatureDeserializerFromStorage(RootSignatureStorage &&storage,
                                           REFIID iid, void **deserializer) {
  InitReturnPtr(deserializer);
  if (!deserializer)
    return E_POINTER;

  auto object = Com<ID3D12VersionedRootSignatureDeserializer>::transfer(
      new RootSignatureDeserializerImpl(std::move(storage)));
  return object->QueryInterface(iid, deserializer);
}

HRESULT
SerializeVersionedRootSignatureImpl(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *root_signature_desc,
    ID3DBlob **blob, ID3DBlob **error_blob) {
  InitReturnPtr(blob);
  InitReturnPtr(error_blob);
  if (!blob)
    return E_POINTER;
  if (!root_signature_desc) {
    SetErrorBlob(error_blob, "Root signature desc is null");
    return WARN_E_INVALIDARG(__func__);
  }

  auto storage = CloneFromVersionedDesc(*root_signature_desc);
  if (!storage) {
    SetErrorBlob(error_blob, "Invalid root signature desc");
    return WARN_E_INVALIDARG(__func__);
  }

  auto serialized = SerializeRootSignature(*storage, root_signature_desc->Version);
  *blob = CreateBlob(std::move(serialized)).takeOwnership();
  return S_OK;
}

} // namespace

Com<ID3D12RootSignature>
CreateRootSignatureFromBlob(IMTLD3D12Device *device,
                            std::span<const std::byte> blob) {
  RootSignatureStorage storage = {};
  if (!device || !ParseRootSignatureBlob(blob, storage))
    return nullptr;

  std::vector<std::byte> serialized_blob(blob.begin(), blob.end());
  return Com<ID3D12RootSignature>::transfer(
      new RootSignatureImpl(device, std::move(storage),
                            std::move(serialized_blob)));
}

HRESULT
CreateRootSignatureDeserializer(std::span<const std::byte> blob, REFIID iid,
                                void **deserializer) {
  return CreateRootSignatureDeserializerImpl(blob, iid, deserializer);
}

HRESULT
CreateRootSignatureDeserializerFromSubobjectInLibrary(
    std::span<const std::byte> library_blob, const WCHAR *subobject_name,
    REFIID iid, void **deserializer) {
  InitReturnPtr(deserializer);
  if (!deserializer)
    return E_POINTER;
  if ((!library_blob.data() && library_blob.size()) || !subobject_name)
    return WARN_E_INVALIDARG(__func__);

  dxil::ContainerInfo container = {};
  auto status = dxil::ParseContainer(library_blob.data(), library_blob.size(), container);
  if (status != dxil::ParseStatus::Ok)
    return WARN_E_INVALIDARG(__func__);

  const auto *rdat_part = container.findPart(dxil::fourcc::RuntimeData);
  if (!rdat_part)
    return WARN_E_INVALIDARG(__func__);

  dxil::RuntimeDataInfo rdat = {};
  status = dxil::ParseRuntimeData(*rdat_part, rdat);
  if (status != dxil::ParseStatus::Ok)
    return WARN_E_INVALIDARG(__func__);

  const std::string requested = str::fromws(subobject_name);
  for (const auto &subobject : rdat.subobjects) {
    if ((subobject.kind == 1 || subobject.kind == 2) &&
        subobject.name == requested && !subobject.root_signature.empty()) {
      std::span<const std::byte> root_signature(
          reinterpret_cast<const std::byte *>(subobject.root_signature.data()),
          subobject.root_signature.size());
      RootSignatureStorage storage = {};
      if (!ParseRootSignatureBlob(root_signature, storage))
        return WARN_E_INVALIDARG(__func__);
      return CreateRootSignatureDeserializerFromStorage(std::move(storage), iid,
                                                        deserializer);
    }
  }

  return WARN_E_INVALIDARG(__func__);
}

HRESULT
SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *root_signature_desc,
    ID3DBlob **blob, ID3DBlob **error_blob) {
  return SerializeVersionedRootSignatureImpl(root_signature_desc, blob, error_blob);
}

} // namespace dxmt::d3d12

extern "C" HRESULT __stdcall
D3D12CreateRootSignatureDeserializer(const void *data, SIZE_T data_size,
                                     REFIID iid, void **deserializer) {
  dxmt::InitReturnPtr(deserializer);
  if (!data && data_size)
    return WARN_E_INVALIDARG(__func__);
  return dxmt::d3d12::CreateRootSignatureDeserializer(
      std::span<const std::byte>(static_cast<const std::byte *>(data), data_size),
      iid, deserializer);
}

extern "C" HRESULT __stdcall
D3D12CreateVersionedRootSignatureDeserializer(const void *data, SIZE_T data_size,
                                              REFIID iid, void **deserializer) {
  return D3D12CreateRootSignatureDeserializer(data, data_size, iid, deserializer);
}

extern "C" HRESULT __stdcall
DXMTCreateRootSignatureDeserializerFromSubobjectInLibrary(
    const void *library_blob, SIZE_T size, LPCWSTR subobject_name,
    REFIID iid, void **deserializer) {
  if (!library_blob && size)
    return WARN_E_INVALIDARG(__func__);
  return dxmt::d3d12::CreateRootSignatureDeserializerFromSubobjectInLibrary(
      std::span<const std::byte>(
          static_cast<const std::byte *>(library_blob), size),
      subobject_name, iid, deserializer);
}

extern "C" HRESULT __stdcall
D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *root_signature_desc,
    ID3DBlob **blob, ID3DBlob **error_blob) {
  return dxmt::d3d12::SerializeVersionedRootSignature(
      root_signature_desc, blob, error_blob);
}

extern "C" HRESULT __stdcall
D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC *root_signature_desc,
                            D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob **blob,
                            ID3DBlob **error_blob) {
  dxmt::InitReturnPtr(blob);
  dxmt::InitReturnPtr(error_blob);
  if (!blob)
    return E_POINTER;
  if (version != D3D_ROOT_SIGNATURE_VERSION_1_0) {
    dxmt::d3d12::SetErrorBlob(error_blob, "Unsupported root signature version");
    return WARN_E_INVALIDARG(__func__);
  }
  if (!root_signature_desc) {
    dxmt::d3d12::SetErrorBlob(error_blob, "Root signature desc is null");
    return WARN_E_INVALIDARG(__func__);
  }

  D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned = {};
  versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
  versioned.Desc_1_0 = *root_signature_desc;
  return D3D12SerializeVersionedRootSignature(&versioned, blob, error_blob);
}
