// Copyright (c) 2026 GameSir Labs and contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
  InvalidShaderHash,
  InvalidCompilerVersion,
  InvalidShaderDebugName,
  InvalidSourceInfo,
  InvalidShaderPdbInfo,
  InvalidShaderStatistics,
  InvalidResourceDef,
  InvalidBitcode,
  InvalidLlvmModule,
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

struct BitcodeRecordInfo {
  uint32_t block_id = 0;
  uint32_t code = 0;
  uint32_t operand_count = 0;
  bool abbreviated = false;
};

struct BitcodeBlockInfo {
  uint32_t id = 0;
  uint32_t abbreviation_id_width = 0;
  uint32_t depth = 0;
  uint64_t start_bit = 0;
  uint64_t end_bit = 0;
  uint32_t record_count = 0;
};

struct BitcodeInfo {
  uint32_t magic = 0;
  bool has_wrapper = false;
  uint32_t wrapper_version = 0;
  uint32_t wrapper_offset = 0;
  uint32_t wrapper_size = 0;
  uint32_t wrapper_cpu_type = 0;
  std::vector<BitcodeBlockInfo> blocks;
  std::vector<BitcodeRecordInfo> records;
};

struct NamedMetadataInfo {
  std::string name;
  uint32_t operand_count = 0;
};

struct DxilShaderModelInfo {
  std::string kind;
  uint32_t major = 0;
  uint32_t minor = 0;
};

struct DxilEntryPointInfo {
  std::string name;
  std::string function_name;
  bool has_signature = false;
  bool has_resources = false;
  bool has_properties = false;
  uint32_t signature_operand_count = 0;
  uint32_t resource_operand_count = 0;
  uint32_t property_operand_count = 0;
  std::vector<uint32_t> properties;
};

struct LlvmModuleFlagInfo {
  std::string key;
  uint32_t behavior = 0;
  std::string value;
};

enum class LlvmTypeKind {
  Unknown,
  Void,
  Integer,
  Half,
  BFloat,
  Float,
  Double,
  X86Fp80,
  Fp128,
  PpcFp128,
  X86Mmx,
  Label,
  Metadata,
  Token,
  Pointer,
  Vector,
  Array,
  Struct,
  Function,
};

struct LlvmTypeInfo {
  LlvmTypeKind kind = LlvmTypeKind::Unknown;
  std::string text;
  std::string name;
  uint32_t bit_width = 0;
  uint32_t address_space = 0;
  uint64_t element_count = 0;
  bool is_scalable = false;
  bool is_opaque = false;
  bool is_var_arg = false;
  std::vector<LlvmTypeInfo> contained_types;
};

struct LlvmOperandInfo {
  std::string type;
  LlvmTypeInfo type_info;
  std::string text;
  bool is_integer = false;
  uint64_t integer_value = 0;
};

enum DxilOpcodeSemanticFlag : uint32_t {
  DxilOpcodeSemanticDerivative = 1u << 0,
  DxilOpcodeSemanticGradient = 1u << 1,
  DxilOpcodeSemanticFeedback = 1u << 2,
  DxilOpcodeSemanticWave = 1u << 3,
  DxilOpcodeSemanticRequiresUniformInputs = 1u << 4,
  DxilOpcodeSemanticBarrier = 1u << 5,
};

struct DxilOpcodeInfo {
  uint32_t opcode = 0;
  std::string_view name;
  std::string_view opcode_class;
  std::string_view category;
  std::string_view function_attribute;
  std::string_view overload_types;
  uint32_t min_shader_model_major = 0;
  uint32_t min_shader_model_minor = 0;
  uint32_t semantic_flags = 0;
  bool is_reserved = false;
};

struct LlvmInstructionInfo {
  std::string opcode_name;
  std::string result_name;
  std::string result_type;
  LlvmTypeInfo result_type_info;
  std::vector<LlvmOperandInfo> operands;
  bool is_call = false;
  std::string called_function;
  bool is_dx_intrinsic_call = false;
  std::optional<uint32_t> dxil_opcode;
  std::string dxil_opcode_name;
};

struct LlvmDxilOperationInfo {
  uint32_t instruction_index = 0;
  std::string called_function;
  uint32_t opcode = 0;
  std::string opcode_name;
  std::string opcode_class;
  std::string opcode_category;
  std::string opcode_function_attribute;
  std::string opcode_overload_types;
  uint32_t min_shader_model_major = 0;
  uint32_t min_shader_model_minor = 0;
  uint32_t semantic_flags = 0;
  bool opcode_known = false;
  bool opcode_reserved = false;
  std::string result_type;
  LlvmTypeInfo result_type_info;
  std::vector<LlvmOperandInfo> operands;
};

struct LlvmFunctionInfo {
  std::string name;
  std::string return_type;
  LlvmTypeInfo return_type_info;
  std::vector<std::string> argument_types;
  std::vector<LlvmTypeInfo> argument_type_infos;
  uint32_t instruction_count = 0;
  bool is_declaration = false;
  bool is_dx_intrinsic = false;
  std::vector<LlvmInstructionInfo> instructions;
  std::vector<LlvmDxilOperationInfo> dxil_operations;
};

struct LlvmGlobalInfo {
  std::string name;
  std::string value_type;
  LlvmTypeInfo value_type_info;
  bool is_constant = false;
  bool is_declaration = false;
};

struct LlvmModuleInfo {
  std::string module_identifier;
  std::string source_file_name;
  std::string target_triple;
  std::string data_layout;
  std::vector<NamedMetadataInfo> named_metadata;
  std::optional<DxilShaderModelInfo> shader_model;
  std::vector<uint32_t> dxil_version;
  std::vector<uint32_t> validator_version;
  std::vector<DxilEntryPointInfo> entry_points;
  std::vector<LlvmModuleFlagInfo> module_flags;
  std::vector<LlvmFunctionInfo> functions;
  std::vector<LlvmGlobalInfo> globals;

  bool hasNamedMetadata(std::string_view name) const;
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

struct ShaderHashInfo {
  uint32_t flags = 0;
  std::array<uint8_t, 16> digest = {};

  bool includes_source() const { return (flags & 1u) != 0; }
  bool is_populated() const;
};

struct CompilerVersionInfo {
  uint16_t major = 0;
  uint16_t minor = 0;
  uint32_t version_flags = 0;
  uint32_t commit_count = 0;
  uint32_t string_list_size = 0;
  std::string commit_sha;
  std::string custom_version_string;
  std::vector<std::string> strings;
};

struct ShaderDebugNameInfo {
  uint16_t flags = 0;
  std::string name;
};

struct SourceInfoSection {
  uint32_t aligned_size = 0;
  uint16_t flags = 0;
  uint16_t type = 0;
  std::span<const uint8_t> data;
};

struct SourceInfo {
  uint32_t aligned_size = 0;
  uint16_t flags = 0;
  uint16_t section_count = 0;
  std::vector<SourceInfoSection> sections;
};

struct ShaderPdbInfo {
  uint16_t version = 0;
  uint16_t compression_type = 0;
  uint32_t size_in_bytes = 0;
  uint32_t uncompressed_size_in_bytes = 0;
  std::span<const uint8_t> payload;
};

struct ShaderStatisticsInfo {
  std::vector<uint32_t> values;
};

struct ResourceBindingInfo {
  std::string name;
  uint32_t type = 0;
  uint32_t return_type = 0;
  uint32_t dimension = 0;
  uint32_t num_samples = 0;
  uint32_t bind_point = 0;
  uint32_t bind_count = 0;
  uint32_t flags = 0;
  uint32_t space = 0;
  uint32_t id = 0;
};

struct ConstantBufferInfo {
  std::string name;
  uint32_t variable_count = 0;
  uint32_t variable_offset = 0;
  uint32_t size = 0;
  uint32_t flags = 0;
  uint32_t type = 0;
};

struct ResourceDefInfo {
  uint32_t constant_buffer_count = 0;
  uint32_t constant_buffer_offset = 0;
  uint32_t bound_resource_count = 0;
  uint32_t bound_resource_offset = 0;
  uint32_t target = 0;
  uint32_t flags = 0;
  std::string creator;
  std::vector<ResourceBindingInfo> resources;
  std::vector<ConstantBufferInfo> constant_buffers;
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

struct RdatResourceInfo {
  std::string name;
  uint32_t resource_class = 0;
  uint32_t kind = 0;
  uint32_t id = 0;
  uint32_t space = 0;
  uint32_t lower_bound = 0;
  uint32_t upper_bound = 0;
  uint32_t flags = 0;
};

struct RdatFunctionInfo {
  std::string name;
  std::string unmangled_name;
  std::vector<uint32_t> resource_indices;
  std::vector<std::string> function_dependencies;
  uint32_t shader_kind = 0;
  uint32_t payload_size_in_bytes = 0;
  uint32_t attribute_size_in_bytes = 0;
  uint32_t feature_info1 = 0;
  uint32_t feature_info2 = 0;
  uint32_t shader_stage_flag = 0;
  uint32_t min_shader_target = 0;
  uint8_t minimum_expected_wave_lane_count = 0;
  uint8_t maximum_expected_wave_lane_count = 0;
  uint16_t shader_flags = 0;
  bool has_shader_info = false;
  uint32_t shader_info_table_type = 0;
  uint32_t shader_info_index = 0;

  uint64_t feature_flags() const {
    return (uint64_t(feature_info2) << 32) | feature_info1;
  }
};

struct RdatSignatureElementInfo {
  std::string semantic_name;
  std::vector<uint32_t> semantic_indices;
  uint8_t semantic_kind = 0;
  uint8_t component_type = 0;
  uint8_t interpolation_mode = 0;
  uint8_t start_row = 0xff;
  uint8_t cols = 0;
  uint8_t start_col = 0;
  uint8_t output_stream = 0;
  uint8_t usage_mask = 0;
  uint8_t dynamic_index_mask = 0;
};

struct RdatShaderInfo {
  uint32_t table_type = 0;
  uint32_t record_index = 0;
  std::vector<uint32_t> input_signature_indices;
  std::vector<uint32_t> output_signature_indices;
  std::vector<uint32_t> patch_constant_signature_indices;
  std::vector<uint32_t> primitive_signature_indices;
  std::span<const uint8_t> view_id_output_mask;
  std::span<const uint8_t> view_id_patch_constant_output_mask;
  std::span<const uint8_t> view_id_primitive_output_mask;
  std::span<const uint8_t> input_to_output_masks;
  std::span<const uint8_t> input_to_patch_constant_output_masks;
  std::span<const uint8_t> patch_constant_input_to_output_masks;
  uint32_t num_threads_x = 1;
  uint32_t num_threads_y = 1;
  uint32_t num_threads_z = 1;
  uint32_t group_shared_bytes_used = 0;
  uint32_t group_shared_bytes_dependent_on_view_id = 0;
  uint32_t payload_size_in_bytes = 0;
  uint16_t max_output_vertices = 0;
  uint16_t max_output_primitives = 0;
  uint8_t input_control_point_count = 0;
  uint8_t output_control_point_count = 0;
  uint8_t tessellator_domain = 0;
  uint8_t tessellator_output_primitive = 0;
  uint8_t input_primitive = 0;
  uint8_t output_topology = 0;
  uint8_t max_vertex_count = 0;
  uint8_t output_stream_mask = 0;
  uint8_t mesh_output_topology = 0;
};

struct RdatSubobjectInfo {
  uint32_t kind = 0;
  std::string name;
  uint32_t state_object_flags = 0;
  std::span<const uint8_t> root_signature;
  std::string associated_subobject;
  std::vector<std::string> associated_exports;
  uint32_t max_payload_size_in_bytes = 0;
  uint32_t max_attribute_size_in_bytes = 0;
  uint32_t max_trace_recursion_depth = 0;
  uint32_t raytracing_pipeline_flags = 0;
  uint32_t hit_group_type = 0;
  std::string any_hit;
  std::string closest_hit;
  std::string intersection;
};

struct RdatNodeIdInfo {
  std::string name;
  uint32_t index = 0;
};

struct RdatRecordDispatchGridInfo {
  uint16_t byte_offset = 0;
  uint8_t component_count = 0;
  uint16_t component_type = 0;
};

struct RdatNodeShaderFuncAttribInfo {
  uint32_t kind = 0;
  uint32_t node_id_index = 0;
  std::vector<uint32_t> values;
  uint32_t value = 0;
};

struct RdatNodeShaderIoAttribInfo {
  uint32_t kind = 0;
  uint32_t node_id_index = 0;
  uint32_t value = 0;
  RdatRecordDispatchGridInfo record_dispatch_grid;
};

struct RdatIoNodeInfo {
  uint32_t io_flags_and_kind = 0;
  std::vector<uint32_t> attribute_indices;
};

struct RdatNodeShaderInfo {
  uint32_t launch_type = 0;
  uint32_t group_shared_bytes_used = 0;
  std::vector<uint32_t> attribute_indices;
  std::vector<uint32_t> output_indices;
  std::vector<uint32_t> input_indices;
};

struct RdatPdbInfoSource {
  std::string name;
  std::string content;
};

struct RdatPdbInfoLibrary {
  std::string name;
  std::span<const uint8_t> data;
};

struct RdatPdbInfo {
  std::vector<uint32_t> source_indices;
  std::vector<uint32_t> library_indices;
  std::vector<std::string> arg_pairs;
  std::span<const uint8_t> hash;
  std::string pdb_name;
  uint32_t custom_toolchain_id = 0;
  std::span<const uint8_t> custom_toolchain_data;
  std::span<const uint8_t> whole_dxil;
};

struct RuntimeDataInfo {
  uint32_t version = 0;
  uint32_t part_count = 0;
  std::vector<RuntimeDataPartInfo> parts;
  std::vector<RdatResourceInfo> resources;
  std::vector<RdatFunctionInfo> functions;
  std::vector<RdatSignatureElementInfo> signature_elements;
  std::vector<RdatShaderInfo> shader_infos;
  std::vector<RdatSubobjectInfo> subobjects;
  std::vector<RdatNodeIdInfo> node_ids;
  std::vector<RdatNodeShaderFuncAttribInfo> node_function_attributes;
  std::vector<RdatNodeShaderIoAttribInfo> node_io_attributes;
  std::vector<RdatIoNodeInfo> io_nodes;
  std::vector<RdatNodeShaderInfo> node_shader_infos;
  std::vector<RdatPdbInfoSource> pdb_sources;
  std::vector<RdatPdbInfoLibrary> pdb_libraries;
  std::vector<RdatPdbInfo> pdb_infos;

  const RuntimeDataPartInfo *findPart(uint32_t type,
                                      size_t start_index = 0) const;
  const RdatShaderInfo *findShaderInfo(uint32_t table_type,
                                       uint32_t record_index) const;
  bool readString(uint32_t offset, std::string &out) const;
  bool readIndexArray(uint32_t offset, std::vector<uint32_t> &out) const;
  bool readBytes(uint32_t offset, uint32_t size,
                 std::span<const uint8_t> &out) const;
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

struct ShaderReflectionResourceInfo {
  std::string name;
  uint32_t resource_class = 0;
  uint32_t resource_type = 0;
  uint32_t resource_kind = 0;
  uint32_t return_type = 0;
  uint32_t dimension = 0;
  uint32_t num_samples = 0;
  uint32_t id = 0;
  uint32_t space = 0;
  uint32_t lower_bound = 0;
  uint32_t upper_bound = 0;
  uint32_t bind_point = 0;
  uint32_t bind_count = 0;
  uint32_t flags = 0;
  bool from_runtime_data = false;
  bool from_resource_def = false;
  bool from_psv = false;
};

struct ShaderReflectionInfo {
  bool valid = false;
  std::string entry_point_name;
  std::string function_name;
  std::string shader_model_kind;
  std::string shader_stage_name;
  uint32_t shader_kind = 0;
  uint32_t shader_model_major = 0;
  uint32_t shader_model_minor = 0;
  uint32_t dxil_major = 0;
  uint32_t dxil_minor = 0;
  bool has_llvm_module = false;
  bool has_runtime_data = false;
  bool has_pipeline_state_validation = false;
  bool has_resource_def = false;
  bool has_root_signature = false;
  uint32_t root_signature_offset = 0;
  std::span<const uint8_t> root_signature;
  bool uses_view_id = false;
  uint32_t num_threads_x = 1;
  uint32_t num_threads_y = 1;
  uint32_t num_threads_z = 1;
  uint32_t group_shared_bytes_used = 0;
  uint64_t feature_flags = 0;
  uint32_t min_shader_target = 0;
  uint32_t shader_flags = 0;
  std::vector<ShaderReflectionResourceInfo> resources;
  std::vector<RdatSignatureElementInfo> input_signature;
  std::vector<RdatSignatureElementInfo> output_signature;
  std::vector<RdatSignatureElementInfo> patch_constant_signature;
  std::vector<RdatSignatureElementInfo> primitive_signature;
  std::vector<PsvSignatureElement> psv_input_signature;
  std::vector<PsvSignatureElement> psv_output_signature;
  std::vector<PsvSignatureElement> psv_patch_constant_or_primitive_signature;
  std::vector<SignatureInfo> legacy_signatures;
  std::vector<LlvmDxilOperationInfo> dxil_operations;
};

struct ContainerInfo {
  std::array<uint8_t, 16> hash = {};
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
  const std::optional<BitcodeInfo> &bitcode() const { return bitcode_; }
  const std::optional<LlvmModuleInfo> &llvmModule() const { return llvm_module_; }
  const std::vector<SignatureInfo> &signatures() const { return signatures_; }
  const std::optional<FeatureInfo> &featureInfo() const { return feature_info_; }
  const std::optional<ShaderHashInfo> &shaderHash() const { return shader_hash_; }
  const std::optional<CompilerVersionInfo> &compilerVersion() const {
    return compiler_version_;
  }
  const std::optional<ShaderDebugNameInfo> &shaderDebugName() const {
    return shader_debug_name_;
  }
  const std::optional<SourceInfo> &sourceInfo() const { return source_info_; }
  const std::optional<ShaderPdbInfo> &shaderPdbInfo() const { return shader_pdb_info_; }
  const std::optional<ShaderStatisticsInfo> &shaderStatistics() const {
    return shader_statistics_;
  }
  const std::optional<ResourceDefInfo> &resourceDef() const { return resource_def_; }
  const std::optional<RuntimeDataInfo> &runtimeData() const { return runtime_data_; }
  const std::optional<PipelineStateValidationInfo> &pipelineStateValidation() const {
    return psv_info_;
  }
  const std::optional<ShaderReflectionInfo> &shaderReflection() const {
    return shader_reflection_;
  }
  const BlobPart *findPart(uint32_t fourcc, size_t start_index = 0) const {
    return container_.findPart(fourcc, start_index);
  }

private:
  ParseStatus parseContainer(std::span<const uint8_t> data);
  ParseStatus parseDxilProgram();
  ParseStatus parseBitcode();
  ParseStatus parseKnownParts();

  ContainerInfo container_;
  std::optional<DxilProgramInfo> dxil_program_;
  std::optional<BitcodeInfo> bitcode_;
  std::optional<LlvmModuleInfo> llvm_module_;
  std::vector<SignatureInfo> signatures_;
  std::optional<FeatureInfo> feature_info_;
  std::optional<ShaderHashInfo> shader_hash_;
  std::optional<CompilerVersionInfo> compiler_version_;
  std::optional<ShaderDebugNameInfo> shader_debug_name_;
  std::optional<SourceInfo> source_info_;
  std::optional<ShaderPdbInfo> shader_pdb_info_;
  std::optional<ShaderStatisticsInfo> shader_statistics_;
  std::optional<ResourceDefInfo> resource_def_;
  std::optional<RuntimeDataInfo> runtime_data_;
  std::optional<PipelineStateValidationInfo> psv_info_;
  std::optional<ShaderReflectionInfo> shader_reflection_;
};

ParseStatus ParseContainer(const void *data, size_t size, ContainerInfo &info);
ParseStatus ParseDxilProgram(const BlobPart &part, DxilProgramInfo &info);
ParseStatus ParseBitcode(std::span<const uint8_t> data, BitcodeInfo &info);
ParseStatus ParseLlvmModule(std::span<const uint8_t> data, LlvmModuleInfo &info);
ParseStatus ParseSignature(const BlobPart &part, SignatureInfo &info);
ParseStatus ParseFeatureInfo(const BlobPart &part, FeatureInfo &info);
ParseStatus ParseShaderHash(const BlobPart &part, ShaderHashInfo &info);
ParseStatus ParseCompilerVersion(const BlobPart &part, CompilerVersionInfo &info);
ParseStatus ParseShaderDebugName(const BlobPart &part, ShaderDebugNameInfo &info);
ParseStatus ParseSourceInfo(const BlobPart &part, SourceInfo &info);
ParseStatus ParseShaderPdbInfo(const BlobPart &part, ShaderPdbInfo &info);
ParseStatus ParseShaderStatistics(const BlobPart &part, ShaderStatisticsInfo &info);
ParseStatus ParseResourceDef(const BlobPart &part, ResourceDefInfo &info);
ParseStatus ParseRuntimeData(const BlobPart &part, RuntimeDataInfo &info);
ParseStatus ParsePipelineStateValidation(const BlobPart &part,
                                         PipelineStateValidationInfo &info);
ParseStatus BuildShaderReflection(const Parser &parser,
                                  ShaderReflectionInfo &info);
const DxilOpcodeInfo *FindDxilOpcodeInfo(uint32_t opcode);
const char *DxilOpcodeName(uint32_t opcode);
std::string DescribeContainerParts(const ContainerInfo &info);

} // namespace dxmt::dxil
