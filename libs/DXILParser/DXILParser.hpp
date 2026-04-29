// Copyright (c) 2026 GameSir Labs and contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dxmt::dxil {

constexpr uint32_t
MakeFourCC(char ch0, char ch1, char ch2, char ch3) {
  return uint32_t(uint8_t(ch0)) | (uint32_t(uint8_t(ch1)) << 8) |
         (uint32_t(uint8_t(ch2)) << 16) | (uint32_t(uint8_t(ch3)) << 24);
}

std::string FourCCString(uint32_t fourcc);
const char *RuntimeDataPartTypeName(uint32_t type);
const char *PsvShaderKindName(uint8_t shader_kind);

namespace fourcc {
constexpr uint32_t Container = MakeFourCC('D', 'X', 'B', 'C');
constexpr uint32_t ResourceDef = MakeFourCC('R', 'D', 'E', 'F');
constexpr uint32_t InputSignature = MakeFourCC('I', 'S', 'G', '1');
constexpr uint32_t OutputSignature = MakeFourCC('O', 'S', 'G', '1');
constexpr uint32_t PatchConstantSignature = MakeFourCC('P', 'S', 'G', '1');
constexpr uint32_t ShaderStatistics = MakeFourCC('S', 'T', 'A', 'T');
constexpr uint32_t ShaderDebugInfoDxil = MakeFourCC('I', 'L', 'D', 'B');
constexpr uint32_t ShaderDebugName = MakeFourCC('I', 'L', 'D', 'N');
constexpr uint32_t FeatureInfo = MakeFourCC('S', 'F', 'I', '0');
constexpr uint32_t PrivateData = MakeFourCC('P', 'R', 'I', 'V');
constexpr uint32_t RootSignature = MakeFourCC('R', 'T', 'S', '0');
constexpr uint32_t Dxil = MakeFourCC('D', 'X', 'I', 'L');
constexpr uint32_t PipelineStateValidation = MakeFourCC('P', 'S', 'V', '0');
constexpr uint32_t RuntimeData = MakeFourCC('R', 'D', 'A', 'T');
constexpr uint32_t ShaderHash = MakeFourCC('H', 'A', 'S', 'H');
constexpr uint32_t ShaderSourceInfo = MakeFourCC('S', 'R', 'C', 'I');
constexpr uint32_t ShaderPdbInfo = MakeFourCC('P', 'D', 'B', 'I');
constexpr uint32_t CompilerVersion = MakeFourCC('V', 'E', 'R', 'S');
} // namespace fourcc

enum class ParseStatus {
  Ok,
  InvalidArgument,
  Truncated,
  BadContainerMagic,
  InvalidContainerSize,
  InvalidPartOffset,
  InvalidPartSize,
  MissingDxilPart,
  InvalidDxilProgram,
  InvalidDxilMagic,
  InvalidDxilBitcodeRange,
  InvalidSignature,
  InvalidFeatureInfo,
  InvalidRuntimeData,
  InvalidPipelineStateValidation,
};

const char *StatusName(ParseStatus status);

struct BlobPart {
  uint32_t fourcc = 0;
  uint32_t offset = 0;
  std::span<const uint8_t> data;
};

struct DxilProgramInfo {
  uint32_t program_version = 0;
  uint32_t size_in_uint32 = 0;
  uint32_t dxil_version = 0;
  uint32_t bitcode_offset = 0;
  uint32_t bitcode_size = 0;
  std::span<const uint8_t> bitcode;

  uint32_t shader_kind() const { return program_version >> 16; }
  uint32_t major_version() const { return (program_version >> 4) & 0xf; }
  uint32_t minor_version() const { return program_version & 0xf; }
};

struct SignatureElement {
  std::string semantic_name;
  uint32_t stream = 0;
  uint32_t semantic_index = 0;
  uint32_t system_value = 0;
  uint32_t component_type = 0;
  uint32_t register_index = 0;
  uint8_t mask = 0;
  uint8_t read_write_mask = 0;
  uint32_t min_precision = 0;
};

struct SignatureInfo {
  uint32_t part_fourcc = 0;
  std::vector<SignatureElement> elements;
};

struct FeatureInfo {
  uint64_t feature_flags = 0;
};

struct RuntimeDataPartInfo {
  uint32_t type = 0;
  uint32_t size = 0;
  std::span<const uint8_t> data;
  bool is_table = false;
  uint32_t record_count = 0;
  uint32_t record_stride = 0;
  std::span<const uint8_t> table_data;
};

struct RuntimeDataInfo {
  uint32_t version = 0;
  uint32_t part_count = 0;
  std::vector<RuntimeDataPartInfo> parts;
};

struct PsvSignatureElement {
  std::string semantic_name;
  std::vector<uint32_t> semantic_indexes;
  uint8_t rows = 0;
  uint8_t start_row = 0;
  uint8_t cols = 0;
  uint8_t start_col = 0;
  bool allocated = false;
  uint8_t semantic_kind = 0;
  uint8_t component_type = 0;
  uint8_t interpolation_mode = 0;
  uint8_t dynamic_index_mask = 0;
  uint8_t output_stream = 0;
};

struct PsvResourceBindInfo {
  uint32_t resource_type = 0;
  uint32_t space = 0;
  uint32_t lower_bound = 0;
  uint32_t upper_bound = 0;
  uint32_t resource_kind = 0;
  uint32_t resource_flags = 0;
};

struct PipelineStateValidationInfo {
  uint32_t runtime_info_size = 0;
  std::span<const uint8_t> runtime_info;
  uint32_t resource_count = 0;
  uint32_t resource_bind_info_size = 0;
  std::vector<PsvResourceBindInfo> resources;
  bool has_runtime_info_1 = false;
  bool has_runtime_info_2 = false;
  bool has_runtime_info_3 = false;
  bool has_runtime_info_4 = false;
  uint8_t shader_stage = 0;
  bool uses_view_id = false;
  uint8_t input_elements = 0;
  uint8_t output_elements = 0;
  uint8_t patch_constant_or_primitive_elements = 0;
  uint8_t input_vectors = 0;
  uint8_t output_vectors[4] = {};
  uint8_t patch_constant_or_primitive_vectors = 0;
  uint32_t num_threads_x = 0;
  uint32_t num_threads_y = 0;
  uint32_t num_threads_z = 0;
  uint32_t entry_function_name_offset = 0;
  std::string entry_function_name;
  uint32_t num_bytes_group_shared_memory = 0;
  uint32_t string_table_size = 0;
  std::span<const uint8_t> string_table;
  uint32_t semantic_index_count = 0;
  std::span<const uint8_t> semantic_index_table;
  uint32_t signature_element_size = 0;
  std::vector<PsvSignatureElement> input_signature_elements;
  std::vector<PsvSignatureElement> output_signature_elements;
  std::vector<PsvSignatureElement> patch_constant_or_primitive_signature_elements;
  std::span<const uint8_t> dependency_payload;
};

struct ContainerInfo {
  uint16_t major_version = 0;
  uint16_t minor_version = 0;
  uint32_t container_size = 0;
  std::vector<BlobPart> parts;

  const BlobPart *findPart(uint32_t fourcc, size_t start_index = 0) const;
  bool hasPart(uint32_t fourcc) const { return findPart(fourcc) != nullptr; }
};

class Parser {
public:
  ParseStatus parse(const void *data, size_t size);
  ParseStatus parse(std::span<const uint8_t> data) {
    return parse(data.data(), data.size());
  }
  ParseStatus parseContainerOnly(const void *data, size_t size);
  ParseStatus parseContainerOnly(std::span<const uint8_t> data) {
    return parseContainerOnly(data.data(), data.size());
  }

  void reset();

  const ContainerInfo &container() const { return container_; }
  const std::optional<DxilProgramInfo> &dxilProgram() const { return dxil_program_; }
  const std::vector<SignatureInfo> &signatures() const { return signatures_; }
  const std::optional<FeatureInfo> &featureInfo() const { return feature_info_; }
  const std::optional<RuntimeDataInfo> &runtimeData() const { return runtime_data_; }
  const std::optional<PipelineStateValidationInfo> &pipelineStateValidation() const {
    return psv_info_;
  }
  const BlobPart *findPart(uint32_t fourcc, size_t start_index = 0) const {
    return container_.findPart(fourcc, start_index);
  }

private:
  ParseStatus parseContainer(std::span<const uint8_t> data);
  ParseStatus parseDxilProgram();
  ParseStatus parseKnownParts();

  ContainerInfo container_;
  std::optional<DxilProgramInfo> dxil_program_;
  std::vector<SignatureInfo> signatures_;
  std::optional<FeatureInfo> feature_info_;
  std::optional<RuntimeDataInfo> runtime_data_;
  std::optional<PipelineStateValidationInfo> psv_info_;
};

ParseStatus ParseContainer(const void *data, size_t size, ContainerInfo &info);
ParseStatus ParseDxilProgram(const BlobPart &part, DxilProgramInfo &info);
ParseStatus ParseSignature(const BlobPart &part, SignatureInfo &info);
ParseStatus ParseFeatureInfo(const BlobPart &part, FeatureInfo &info);
ParseStatus ParseRuntimeData(const BlobPart &part, RuntimeDataInfo &info);
ParseStatus ParsePipelineStateValidation(const BlobPart &part,
                                         PipelineStateValidationInfo &info);
std::string DescribeContainerParts(const ContainerInfo &info);

} // namespace dxmt::dxil
