// Copyright (c) 2026 GameSir Labs and contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "DXILParser/DXILParser.hpp"

#include <algorithm>
#include <sstream>

namespace dxmt::dxil {

namespace {

constexpr size_t kContainerHeaderSize = 32;
constexpr size_t kPartHeaderSize = 8;
constexpr size_t kDxilProgramHeaderSize = 24;
constexpr size_t kDxilBitcodeHeaderOffset = 8;
constexpr size_t kDxilSignatureHeaderSize = 8;
constexpr size_t kDxilSignatureElementSize = 32;
constexpr size_t kFeatureInfoSize = 8;
constexpr size_t kRuntimeDataHeaderSize = 8;
constexpr size_t kRuntimeDataPartHeaderSize = 8;
constexpr size_t kRuntimeDataTableHeaderSize = 8;
constexpr size_t kPsvRuntimeInfo1Size = 36;
constexpr size_t kPsvRuntimeInfo2Size = 48;
constexpr size_t kPsvRuntimeInfo3Size = 52;
constexpr size_t kPsvRuntimeInfo4Size = 56;
constexpr size_t kPsvResourceBindInfo0Size = 16;
constexpr size_t kPsvResourceBindInfo1Size = 24;
constexpr size_t kPsvSignatureElement0Size = 16;
constexpr uint32_t kDxilMagicValue = MakeFourCC('D', 'X', 'I', 'L');

uint16_t
ReadU16(std::span<const uint8_t> data, size_t offset) {
  return uint16_t(data[offset]) | (uint16_t(data[offset + 1]) << 8);
}

uint32_t
ReadU32(std::span<const uint8_t> data, size_t offset) {
  return uint32_t(data[offset]) | (uint32_t(data[offset + 1]) << 8) |
         (uint32_t(data[offset + 2]) << 16) |
         (uint32_t(data[offset + 3]) << 24);
}

uint64_t
ReadU64(std::span<const uint8_t> data, size_t offset) {
  return uint64_t(ReadU32(data, offset)) | (uint64_t(ReadU32(data, offset + 4)) << 32);
}

bool
CheckedEnd(size_t offset, size_t size, size_t limit, size_t &end) {
  if (offset > limit || size > limit - offset)
    return false;
  end = offset + size;
  return true;
}

bool
ReadString(std::span<const uint8_t> data, size_t offset, std::string &out) {
  if (offset >= data.size())
    return false;

  size_t end = offset;
  while (end < data.size() && data[end] != 0)
    end++;
  if (end == data.size())
    return false;

  out.assign(reinterpret_cast<const char *>(data.data() + offset), end - offset);
  return true;
}

bool
ReadU32Array(std::span<const uint8_t> data, uint32_t offset, uint32_t count,
             std::vector<uint32_t> &out) {
  const auto byte_offset = size_t(offset) * sizeof(uint32_t);
  size_t bytes = 0;
  if (!CheckedEnd(byte_offset, size_t(count) * sizeof(uint32_t), data.size(), bytes))
    return false;

  out.clear();
  out.reserve(count);
  for (uint32_t i = 0; i < count; i++)
    out.push_back(ReadU32(data, byte_offset + size_t(i) * sizeof(uint32_t)));
  return true;
}

bool
IsRdatTablePart(uint32_t type) {
  switch (type) {
  case 3:  // ResourceTable
  case 4:  // FunctionTable
  case 6:  // SubobjectTable
  case 7:  // NodeIDTable
  case 8:  // NodeShaderIOAttribTable
  case 9:  // NodeShaderFuncAttribTable
  case 10: // IONodeTable
  case 11: // NodeShaderInfoTable
  case 12: // Reserved mesh nodes preview table
  case 13: // SignatureElementTable
  case 14: // VSInfoTable
  case 15: // PSInfoTable
  case 16: // HSInfoTable
  case 17: // DSInfoTable
  case 18: // GSInfoTable
  case 19: // CSInfoTable
  case 20: // MSInfoTable
  case 21: // ASInfoTable
  case 0x10001: // DxilPdbInfoTable
  case 0x10002: // DxilPdbInfoSourceTable
  case 0x10003: // DxilPdbInfoLibraryTable
    return true;
  default:
    return false;
  }
}

} // namespace

std::string
FourCCString(uint32_t fourcc) {
  char text[5] = {
      char(fourcc & 0xff),
      char((fourcc >> 8) & 0xff),
      char((fourcc >> 16) & 0xff),
      char((fourcc >> 24) & 0xff),
      0,
  };
  return text;
}

const char *
RuntimeDataPartTypeName(uint32_t type) {
  switch (type) {
  case 0:
    return "Invalid";
  case 1:
    return "StringBuffer";
  case 2:
    return "IndexArrays";
  case 3:
    return "ResourceTable";
  case 4:
    return "FunctionTable";
  case 5:
    return "RawBytes";
  case 6:
    return "SubobjectTable";
  case 7:
    return "NodeIDTable";
  case 8:
    return "NodeShaderIOAttribTable";
  case 9:
    return "NodeShaderFuncAttribTable";
  case 10:
    return "IONodeTable";
  case 11:
    return "NodeShaderInfoTable";
  case 12:
    return "ReservedMeshNodesPreviewInfoTable";
  case 13:
    return "SignatureElementTable";
  case 14:
    return "VSInfoTable";
  case 15:
    return "PSInfoTable";
  case 16:
    return "HSInfoTable";
  case 17:
    return "DSInfoTable";
  case 18:
    return "GSInfoTable";
  case 19:
    return "CSInfoTable";
  case 20:
    return "MSInfoTable";
  case 21:
    return "ASInfoTable";
  case 0x10001:
    return "DxilPdbInfoTable";
  case 0x10002:
    return "DxilPdbInfoSourceTable";
  case 0x10003:
    return "DxilPdbInfoLibraryTable";
  default:
    return "Unknown";
  }
}

const char *
PsvShaderKindName(uint8_t shader_kind) {
  switch (shader_kind) {
  case 0:
    return "Pixel";
  case 1:
    return "Vertex";
  case 2:
    return "Geometry";
  case 3:
    return "Hull";
  case 4:
    return "Domain";
  case 5:
    return "Compute";
  case 6:
    return "Library";
  case 7:
    return "RayGeneration";
  case 8:
    return "Intersection";
  case 9:
    return "AnyHit";
  case 10:
    return "ClosestHit";
  case 11:
    return "Miss";
  case 12:
    return "Callable";
  case 13:
    return "Mesh";
  case 14:
    return "Amplification";
  case 15:
    return "Node";
  default:
    return "Invalid";
  }
}

const char *
StatusName(ParseStatus status) {
  switch (status) {
  case ParseStatus::Ok:
    return "ok";
  case ParseStatus::InvalidArgument:
    return "invalid argument";
  case ParseStatus::Truncated:
    return "truncated";
  case ParseStatus::BadContainerMagic:
    return "bad container magic";
  case ParseStatus::InvalidContainerSize:
    return "invalid container size";
  case ParseStatus::InvalidPartOffset:
    return "invalid part offset";
  case ParseStatus::InvalidPartSize:
    return "invalid part size";
  case ParseStatus::MissingDxilPart:
    return "missing DXIL part";
  case ParseStatus::InvalidDxilProgram:
    return "invalid DXIL program";
  case ParseStatus::InvalidDxilMagic:
    return "invalid DXIL magic";
  case ParseStatus::InvalidDxilBitcodeRange:
    return "invalid DXIL bitcode range";
  case ParseStatus::InvalidSignature:
    return "invalid signature";
  case ParseStatus::InvalidFeatureInfo:
    return "invalid feature info";
  case ParseStatus::InvalidRuntimeData:
    return "invalid runtime data";
  case ParseStatus::InvalidPipelineStateValidation:
    return "invalid pipeline state validation";
  }
  return "unknown";
}

const BlobPart *
ContainerInfo::findPart(uint32_t fourcc, size_t start_index) const {
  for (size_t i = start_index; i < parts.size(); i++) {
    if (parts[i].fourcc == fourcc)
      return &parts[i];
  }
  return nullptr;
}

void
Parser::reset() {
  container_ = {};
  dxil_program_.reset();
  signatures_.clear();
  feature_info_.reset();
  runtime_data_.reset();
  psv_info_.reset();
}

ParseStatus
Parser::parse(const void *data, size_t size) {
  if (!data && size)
    return ParseStatus::InvalidArgument;

  reset();
  auto bytes = std::span<const uint8_t>(static_cast<const uint8_t *>(data), size);
  auto status = parseContainer(bytes);
  if (status != ParseStatus::Ok)
    return status;

  status = parseDxilProgram();
  if (status != ParseStatus::Ok)
    return status;

  return parseKnownParts();
}

ParseStatus
Parser::parseContainerOnly(const void *data, size_t size) {
  if (!data && size)
    return ParseStatus::InvalidArgument;

  reset();
  auto bytes = std::span<const uint8_t>(static_cast<const uint8_t *>(data), size);
  return parseContainer(bytes);
}

ParseStatus
Parser::parseContainer(std::span<const uint8_t> data) {
  if (data.size() < kContainerHeaderSize)
    return ParseStatus::Truncated;

  if (ReadU32(data, 0) != fourcc::Container)
    return ParseStatus::BadContainerMagic;

  const auto container_size = ReadU32(data, 24);
  const auto part_count = ReadU32(data, 28);
  if (container_size < kContainerHeaderSize || container_size > data.size())
    return ParseStatus::InvalidContainerSize;

  size_t part_index_table_end = 0;
  if (!CheckedEnd(kContainerHeaderSize, size_t(part_count) * sizeof(uint32_t),
                  container_size, part_index_table_end))
    return ParseStatus::InvalidContainerSize;

  container_.major_version = ReadU16(data, 20);
  container_.minor_version = ReadU16(data, 22);
  container_.container_size = container_size;
  container_.parts.reserve(part_count);

  for (uint32_t i = 0; i < part_count; i++) {
    const auto part_offset = ReadU32(data, kContainerHeaderSize + size_t(i) * sizeof(uint32_t));
    if (part_offset < part_index_table_end || part_offset > container_size)
      return ParseStatus::InvalidPartOffset;

    size_t part_header_end = 0;
    if (!CheckedEnd(part_offset, kPartHeaderSize, container_size, part_header_end))
      return ParseStatus::InvalidPartOffset;

    const auto part_fourcc = ReadU32(data, part_offset);
    const auto part_size = ReadU32(data, part_offset + 4);
    size_t part_end = 0;
    if (!CheckedEnd(part_header_end, part_size, container_size, part_end))
      return ParseStatus::InvalidPartSize;

    container_.parts.push_back({
        .fourcc = part_fourcc,
        .offset = part_offset,
        .data = std::span<const uint8_t>(data.data() + part_header_end, part_size),
    });
  }

  return ParseStatus::Ok;
}

ParseStatus
Parser::parseDxilProgram() {
  const auto *dxil = container_.findPart(fourcc::Dxil);
  if (!dxil)
    return ParseStatus::MissingDxilPart;

  DxilProgramInfo info = {};
  auto status = ParseDxilProgram(*dxil, info);
  if (status != ParseStatus::Ok)
    return status;

  dxil_program_ = info;
  return ParseStatus::Ok;
}

ParseStatus
Parser::parseKnownParts() {
  signatures_.clear();

  for (auto fourcc : {fourcc::InputSignature, fourcc::OutputSignature,
                     fourcc::PatchConstantSignature}) {
    size_t start = 0;
    while (const auto *part = container_.findPart(fourcc, start)) {
      SignatureInfo info = {};
      auto status = ParseSignature(*part, info);
      if (status != ParseStatus::Ok)
        return status;
      signatures_.push_back(std::move(info));
      start = size_t(part - container_.parts.data()) + 1;
    }
  }

  if (const auto *part = container_.findPart(fourcc::FeatureInfo)) {
    FeatureInfo info = {};
    auto status = ParseFeatureInfo(*part, info);
    if (status != ParseStatus::Ok)
      return status;
    feature_info_ = info;
  }

  if (const auto *part = container_.findPart(fourcc::RuntimeData)) {
    RuntimeDataInfo info = {};
    auto status = ParseRuntimeData(*part, info);
    if (status != ParseStatus::Ok)
      return status;
    runtime_data_ = std::move(info);
  }

  if (const auto *part = container_.findPart(fourcc::PipelineStateValidation)) {
    PipelineStateValidationInfo info = {};
    auto status = ParsePipelineStateValidation(*part, info);
    if (status != ParseStatus::Ok)
      return status;
    psv_info_ = std::move(info);
  }

  return ParseStatus::Ok;
}

ParseStatus
ParseContainer(const void *data, size_t size, ContainerInfo &info) {
  Parser parser;
  auto status = parser.parseContainerOnly(data, size);
  if (status != ParseStatus::Ok)
    return status;

  info = parser.container();
  return ParseStatus::Ok;
}

ParseStatus
ParseDxilProgram(const BlobPart &part, DxilProgramInfo &info) {
  const auto data = part.data;
  if (data.size() < kDxilProgramHeaderSize)
    return ParseStatus::InvalidDxilProgram;

  const auto program_size_in_uint32 = ReadU32(data, 4);
  size_t program_size = 0;
  if (!CheckedEnd(0, size_t(program_size_in_uint32) * sizeof(uint32_t),
                  data.size(), program_size))
    return ParseStatus::InvalidDxilProgram;
  if (program_size < kDxilProgramHeaderSize)
    return ParseStatus::InvalidDxilProgram;

  const auto dxil_magic = ReadU32(data, kDxilBitcodeHeaderOffset);
  if (dxil_magic != kDxilMagicValue)
    return ParseStatus::InvalidDxilMagic;

  const auto bitcode_offset = ReadU32(data, kDxilBitcodeHeaderOffset + 8);
  const auto bitcode_size = ReadU32(data, kDxilBitcodeHeaderOffset + 12);
  if (!bitcode_size)
    return ParseStatus::InvalidDxilBitcodeRange;

  size_t bitcode_start = 0;
  if (!CheckedEnd(kDxilBitcodeHeaderOffset, bitcode_offset, program_size, bitcode_start))
    return ParseStatus::InvalidDxilBitcodeRange;

  size_t bitcode_end = 0;
  if (!CheckedEnd(bitcode_start, bitcode_size, program_size, bitcode_end))
    return ParseStatus::InvalidDxilBitcodeRange;

  info.program_version = ReadU32(data, 0);
  info.size_in_uint32 = program_size_in_uint32;
  info.dxil_version = ReadU32(data, kDxilBitcodeHeaderOffset + 4);
  info.bitcode_offset = uint32_t(bitcode_start);
  info.bitcode_size = bitcode_size;
  info.bitcode = std::span<const uint8_t>(data.data() + bitcode_start, bitcode_size);
  return ParseStatus::Ok;
}

ParseStatus
ParseSignature(const BlobPart &part, SignatureInfo &info) {
  const auto data = part.data;
  if (data.size() < kDxilSignatureHeaderSize)
    return ParseStatus::InvalidSignature;

  const auto count = ReadU32(data, 0);
  const auto offset = ReadU32(data, 4);
  size_t elements_end = 0;
  if (!CheckedEnd(offset, size_t(count) * kDxilSignatureElementSize, data.size(), elements_end))
    return ParseStatus::InvalidSignature;

  info = {};
  info.part_fourcc = part.fourcc;
  info.elements.reserve(count);

  for (uint32_t i = 0; i < count; i++) {
    const auto element_offset = offset + size_t(i) * kDxilSignatureElementSize;
    SignatureElement element = {};
    element.stream = ReadU32(data, element_offset + 0);
    const auto semantic_name_offset = ReadU32(data, element_offset + 4);
    element.semantic_index = ReadU32(data, element_offset + 8);
    element.system_value = ReadU32(data, element_offset + 12);
    element.component_type = ReadU32(data, element_offset + 16);
    element.register_index = ReadU32(data, element_offset + 20);
    element.mask = data[element_offset + 24];
    element.read_write_mask = data[element_offset + 25];
    element.min_precision = ReadU32(data, element_offset + 28);

    if (!ReadString(data, semantic_name_offset, element.semantic_name))
      return ParseStatus::InvalidSignature;

    info.elements.push_back(std::move(element));
  }

  return ParseStatus::Ok;
}

ParseStatus
ParseFeatureInfo(const BlobPart &part, FeatureInfo &info) {
  if (part.data.size() < kFeatureInfoSize)
    return ParseStatus::InvalidFeatureInfo;

  info.feature_flags = ReadU64(part.data, 0);
  return ParseStatus::Ok;
}

ParseStatus
ParseRuntimeData(const BlobPart &part, RuntimeDataInfo &info) {
  const auto data = part.data;
  if (data.size() < kRuntimeDataHeaderSize)
    return ParseStatus::InvalidRuntimeData;

  const auto version = ReadU32(data, 0);
  const auto part_count = ReadU32(data, 4);
  size_t offset_table_end = 0;
  if (!CheckedEnd(kRuntimeDataHeaderSize, size_t(part_count) * sizeof(uint32_t),
                  data.size(), offset_table_end))
    return ParseStatus::InvalidRuntimeData;

  info = {};
  info.version = version;
  info.part_count = part_count;
  info.parts.reserve(part_count);

  for (uint32_t i = 0; i < part_count; i++) {
    const auto part_offset = ReadU32(data, kRuntimeDataHeaderSize + size_t(i) * sizeof(uint32_t));
    if ((part_offset & 3) || part_offset < offset_table_end || part_offset > data.size())
      return ParseStatus::InvalidRuntimeData;

    size_t part_header_end = 0;
    if (!CheckedEnd(part_offset, kRuntimeDataPartHeaderSize, data.size(), part_header_end))
      return ParseStatus::InvalidRuntimeData;

    RuntimeDataPartInfo part_info = {};
    part_info.type = ReadU32(data, part_offset + 0);
    part_info.size = ReadU32(data, part_offset + 4);

    const auto aligned_size = (size_t(part_info.size) + 3u) & ~size_t(3u);
    size_t part_end = 0;
    if (!CheckedEnd(part_header_end, aligned_size, data.size(), part_end))
      return ParseStatus::InvalidRuntimeData;

    part_info.data = std::span<const uint8_t>(data.data() + part_header_end, part_info.size);
    part_info.is_table = IsRdatTablePart(part_info.type);
    if (part_info.is_table) {
      if (part_info.data.size() < kRuntimeDataTableHeaderSize)
        return ParseStatus::InvalidRuntimeData;
      part_info.record_count = ReadU32(part_info.data, 0);
      part_info.record_stride = ReadU32(part_info.data, 4);
      if (part_info.record_stride & 3)
        return ParseStatus::InvalidRuntimeData;
      size_t table_data_end = 0;
      if (!CheckedEnd(kRuntimeDataTableHeaderSize,
                      size_t(part_info.record_count) * part_info.record_stride,
                      part_info.data.size(), table_data_end))
        return ParseStatus::InvalidRuntimeData;
      part_info.table_data = std::span<const uint8_t>(
          part_info.data.data() + kRuntimeDataTableHeaderSize,
          table_data_end - kRuntimeDataTableHeaderSize);
    }

    info.parts.push_back(part_info);
  }

  return ParseStatus::Ok;
}

ParseStatus
ParsePipelineStateValidation(const BlobPart &part,
                             PipelineStateValidationInfo &info) {
  const auto data = part.data;
  if (data.size() < sizeof(uint32_t))
    return ParseStatus::InvalidPipelineStateValidation;

  info = {};
  size_t offset = 0;
  auto read_u32 = [&](uint32_t &value) {
    size_t end = 0;
    if (!CheckedEnd(offset, sizeof(uint32_t), data.size(), end))
      return false;
    value = ReadU32(data, offset);
    offset = end;
    return true;
  };

  if (!read_u32(info.runtime_info_size))
    return ParseStatus::InvalidPipelineStateValidation;

  size_t runtime_info_end = 0;
  if (!CheckedEnd(offset, info.runtime_info_size, data.size(), runtime_info_end))
    return ParseStatus::InvalidPipelineStateValidation;
  info.runtime_info = std::span<const uint8_t>(data.data() + offset, info.runtime_info_size);
  offset = runtime_info_end;

  if (info.runtime_info_size >= kPsvRuntimeInfo1Size) {
    info.has_runtime_info_1 = true;
    info.shader_stage = info.runtime_info[24];
    info.uses_view_id = info.runtime_info[25] != 0;
    info.input_elements = info.runtime_info[28];
    info.output_elements = info.runtime_info[29];
    info.patch_constant_or_primitive_elements = info.runtime_info[30];
    info.input_vectors = info.runtime_info[31];
    std::copy_n(info.runtime_info.data() + 32, 4, info.output_vectors);
    info.patch_constant_or_primitive_vectors = info.runtime_info[26];
  }

  if (info.runtime_info_size >= kPsvRuntimeInfo2Size) {
    info.has_runtime_info_2 = true;
    info.num_threads_x = ReadU32(info.runtime_info, 36);
    info.num_threads_y = ReadU32(info.runtime_info, 40);
    info.num_threads_z = ReadU32(info.runtime_info, 44);
  }

  if (info.runtime_info_size >= kPsvRuntimeInfo3Size) {
    info.has_runtime_info_3 = true;
    info.entry_function_name_offset = ReadU32(info.runtime_info, 48);
  }

  if (info.runtime_info_size >= kPsvRuntimeInfo4Size) {
    info.has_runtime_info_4 = true;
    info.num_bytes_group_shared_memory = ReadU32(info.runtime_info, 52);
  }

  if (!read_u32(info.resource_count))
    return ParseStatus::InvalidPipelineStateValidation;

  if (info.resource_count) {
    if (!read_u32(info.resource_bind_info_size))
      return ParseStatus::InvalidPipelineStateValidation;
    if (info.resource_bind_info_size < kPsvResourceBindInfo0Size ||
        info.resource_bind_info_size & 3)
      return ParseStatus::InvalidPipelineStateValidation;

    size_t resources_end = 0;
    if (!CheckedEnd(offset, size_t(info.resource_count) * info.resource_bind_info_size,
                    data.size(), resources_end))
      return ParseStatus::InvalidPipelineStateValidation;

    info.resources.reserve(info.resource_count);
    for (uint32_t i = 0; i < info.resource_count; i++) {
      const auto resource_offset = offset + size_t(i) * info.resource_bind_info_size;
      PsvResourceBindInfo resource = {};
      resource.resource_type = ReadU32(data, resource_offset + 0);
      resource.space = ReadU32(data, resource_offset + 4);
      resource.lower_bound = ReadU32(data, resource_offset + 8);
      resource.upper_bound = ReadU32(data, resource_offset + 12);
      if (info.resource_bind_info_size >= kPsvResourceBindInfo1Size) {
        resource.resource_kind = ReadU32(data, resource_offset + 16);
        resource.resource_flags = ReadU32(data, resource_offset + 20);
      }
      info.resources.push_back(resource);
    }
    offset = resources_end;
  }

  if (info.has_runtime_info_1) {
    if (!read_u32(info.string_table_size))
      return ParseStatus::InvalidPipelineStateValidation;
    if (info.string_table_size & 3)
      return ParseStatus::InvalidPipelineStateValidation;

    size_t string_table_end = 0;
    if (!CheckedEnd(offset, info.string_table_size, data.size(), string_table_end))
      return ParseStatus::InvalidPipelineStateValidation;
    info.string_table = std::span<const uint8_t>(data.data() + offset,
                                                 info.string_table_size);
    if (info.string_table_size && info.string_table[info.string_table_size - 1] != 0)
      return ParseStatus::InvalidPipelineStateValidation;
    offset = string_table_end;

    if (info.has_runtime_info_3 && info.string_table_size &&
        !ReadString(info.string_table, info.entry_function_name_offset,
                    info.entry_function_name))
      return ParseStatus::InvalidPipelineStateValidation;

    if (!read_u32(info.semantic_index_count))
      return ParseStatus::InvalidPipelineStateValidation;

    size_t semantic_index_table_end = 0;
    if (!CheckedEnd(offset, size_t(info.semantic_index_count) * sizeof(uint32_t),
                    data.size(), semantic_index_table_end))
      return ParseStatus::InvalidPipelineStateValidation;
    info.semantic_index_table = std::span<const uint8_t>(
        data.data() + offset, semantic_index_table_end - offset);
    offset = semantic_index_table_end;

    const auto signature_count = uint32_t(info.input_elements) +
                                 uint32_t(info.output_elements) +
                                 uint32_t(info.patch_constant_or_primitive_elements);
    if (signature_count) {
      if (!read_u32(info.signature_element_size))
        return ParseStatus::InvalidPipelineStateValidation;
      if (info.signature_element_size < kPsvSignatureElement0Size ||
          info.signature_element_size & 3)
        return ParseStatus::InvalidPipelineStateValidation;
    }

    auto parse_signature_elements = [&](uint32_t count,
                                        std::vector<PsvSignatureElement> &elements) {
      size_t elements_end = 0;
      if (!CheckedEnd(offset, size_t(count) * info.signature_element_size,
                      data.size(), elements_end))
        return false;

      elements.clear();
      elements.reserve(count);
      for (uint32_t i = 0; i < count; i++) {
        const auto element_offset = offset + size_t(i) * info.signature_element_size;
        PsvSignatureElement element = {};
        const auto semantic_name_offset = ReadU32(data, element_offset + 0);
        const auto semantic_indexes_offset = ReadU32(data, element_offset + 4);
        element.rows = data[element_offset + 8];
        element.start_row = data[element_offset + 9];
        const auto cols_and_start = data[element_offset + 10];
        element.cols = cols_and_start & 0xf;
        element.start_col = (cols_and_start >> 4) & 0x3;
        element.allocated = (cols_and_start & 0x40) != 0;
        element.semantic_kind = data[element_offset + 11];
        element.component_type = data[element_offset + 12];
        element.interpolation_mode = data[element_offset + 13];
        const auto dynamic_mask_and_stream = data[element_offset + 14];
        element.dynamic_index_mask = dynamic_mask_and_stream & 0xf;
        element.output_stream = (dynamic_mask_and_stream >> 4) & 0x3;

        if (info.string_table_size &&
            !ReadString(info.string_table, semantic_name_offset, element.semantic_name))
          return false;
        if (!ReadU32Array(info.semantic_index_table, semantic_indexes_offset,
                          element.rows, element.semantic_indexes))
          return false;

        elements.push_back(std::move(element));
      }

      offset = elements_end;
      return true;
    };

    if (!parse_signature_elements(info.input_elements,
                                  info.input_signature_elements) ||
        !parse_signature_elements(info.output_elements,
                                  info.output_signature_elements) ||
        !parse_signature_elements(
            info.patch_constant_or_primitive_elements,
            info.patch_constant_or_primitive_signature_elements))
      return ParseStatus::InvalidPipelineStateValidation;
  }

  // Dependency masks/tables are PSV-version and shader-stage dependent. Keep
  // them available as an opaque slice until the PSO validator consumes them.
  info.dependency_payload = std::span<const uint8_t>(data.data() + offset,
                                                    data.size() - offset);
  return ParseStatus::Ok;
}

std::string
DescribeContainerParts(const ContainerInfo &info) {
  std::ostringstream stream;
  stream << "DXContainer v" << info.major_version << "." << info.minor_version
         << " size=" << info.container_size << " parts=" << info.parts.size();
  for (const auto &part : info.parts) {
    stream << " " << FourCCString(part.fourcc) << "@" << part.offset
           << "+" << part.data.size();
  }
  return stream.str();
}

} // namespace dxmt::dxil
