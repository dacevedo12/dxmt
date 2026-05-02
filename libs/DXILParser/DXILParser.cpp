// Copyright (c) 2026 GameSir Labs and contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "DXILParser/DXILParser.hpp"
#include "DXILParser/DXILFormatConstants.hpp"
#include "DXILParser/DXILOpcodeInfo.hpp"

#if DXILPARSER_USE_LLVM
#include <llvm/ADT/StringRef.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/Support/raw_ostream.h>
#endif

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <sstream>
#include <utility>

namespace dxmt::dxil {

namespace {

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
CheckedAdd64(uint64_t offset, uint64_t size, uint64_t limit, uint64_t &end) {
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
ReadStringList(std::span<const uint8_t> data, std::vector<std::string> &out) {
  out.clear();
  size_t offset = 0;
  while (offset < data.size()) {
    if (data[offset] == 0) {
      offset++;
      continue;
    }

    std::string text;
    if (!ReadString(data, offset, text))
      return false;

    offset += text.size() + 1;
    out.push_back(std::move(text));
  }
  return true;
}

bool
ExtractBitstream(std::span<const uint8_t> data, std::span<const uint8_t> &bitstream,
                 BitcodeInfo *wrapper_info = nullptr) {
  if (data.size() < sizeof(uint32_t))
    return false;

  bitstream = data;
  const auto magic = ReadU32(data, 0);
  if (magic != kBitcodeWrapperMagicValue)
    return true;

  if (data.size() < kBitcodeWrapperHeaderSize)
    return false;

  const auto wrapper_offset = ReadU32(data, 8);
  const auto wrapper_size = ReadU32(data, 12);
  size_t bitstream_end = 0;
  if (!CheckedEnd(wrapper_offset, wrapper_size, data.size(), bitstream_end))
    return false;

  bitstream = std::span<const uint8_t>(data.data() + wrapper_offset,
                                       wrapper_size);
  if (wrapper_info) {
    wrapper_info->has_wrapper = true;
    wrapper_info->wrapper_version = ReadU32(data, 4);
    wrapper_info->wrapper_offset = wrapper_offset;
    wrapper_info->wrapper_size = wrapper_size;
    wrapper_info->wrapper_cpu_type = ReadU32(data, 16);
  }
  return true;
}

#if DXILPARSER_USE_LLVM
std::string
MetadataString(const llvm::Metadata *metadata) {
  if (!metadata)
    return {};

  if (const auto *string = llvm::dyn_cast<llvm::MDString>(metadata))
    return string->getString().str();

  if (const auto *value = llvm::dyn_cast<llvm::ValueAsMetadata>(metadata)) {
    if (const auto *named_value = value->getValue();
        named_value && named_value->hasName())
      return named_value->getName().str();
  }

  return {};
}

std::string
MetadataText(const llvm::Metadata *metadata) {
  if (!metadata)
    return {};

  std::string text;
  llvm::raw_string_ostream stream(text);
  metadata->print(stream);
  return stream.str();
}

std::string
TypeString(const llvm::Type *type) {
  if (!type)
    return {};

  std::string text;
  llvm::raw_string_ostream stream(text);
  type->print(stream);
  return stream.str();
}

LlvmTypeInfo
ParseLlvmType(const llvm::Type *type, uint32_t depth = 0) {
  LlvmTypeInfo info = {};
  if (!type)
    return info;

  info.text = TypeString(type);
  if (depth > 16)
    return info;

  switch (type->getTypeID()) {
  case llvm::Type::VoidTyID:
    info.kind = LlvmTypeKind::Void;
    return info;
  case llvm::Type::HalfTyID:
    info.kind = LlvmTypeKind::Half;
    info.bit_width = 16;
    return info;
  case llvm::Type::BFloatTyID:
    info.kind = LlvmTypeKind::BFloat;
    info.bit_width = 16;
    return info;
  case llvm::Type::FloatTyID:
    info.kind = LlvmTypeKind::Float;
    info.bit_width = 32;
    return info;
  case llvm::Type::DoubleTyID:
    info.kind = LlvmTypeKind::Double;
    info.bit_width = 64;
    return info;
  case llvm::Type::X86_FP80TyID:
    info.kind = LlvmTypeKind::X86Fp80;
    info.bit_width = 80;
    return info;
  case llvm::Type::FP128TyID:
    info.kind = LlvmTypeKind::Fp128;
    info.bit_width = 128;
    return info;
  case llvm::Type::PPC_FP128TyID:
    info.kind = LlvmTypeKind::PpcFp128;
    info.bit_width = 128;
    return info;
  case llvm::Type::X86_MMXTyID:
    info.kind = LlvmTypeKind::X86Mmx;
    info.bit_width = 64;
    return info;
  case llvm::Type::LabelTyID:
    info.kind = LlvmTypeKind::Label;
    return info;
  case llvm::Type::MetadataTyID:
    info.kind = LlvmTypeKind::Metadata;
    return info;
  case llvm::Type::TokenTyID:
    info.kind = LlvmTypeKind::Token;
    return info;
  case llvm::Type::IntegerTyID:
    info.kind = LlvmTypeKind::Integer;
    info.bit_width = type->getIntegerBitWidth();
    return info;
  case llvm::Type::PointerTyID: {
    info.kind = LlvmTypeKind::Pointer;
    const auto *pointer = llvm::cast<llvm::PointerType>(type);
    info.address_space = pointer->getAddressSpace();
    info.is_opaque = pointer->isOpaque();
    return info;
  }
  case llvm::Type::FixedVectorTyID:
  case llvm::Type::ScalableVectorTyID: {
    info.kind = LlvmTypeKind::Vector;
    const auto *vector = llvm::cast<llvm::VectorType>(type);
    const auto element_count = vector->getElementCount();
    info.element_count = element_count.getKnownMinValue();
    info.is_scalable = element_count.isScalable();
    info.contained_types.push_back(
        ParseLlvmType(vector->getElementType(), depth + 1));
    return info;
  }
  case llvm::Type::ArrayTyID: {
    info.kind = LlvmTypeKind::Array;
    const auto *array = llvm::cast<llvm::ArrayType>(type);
    info.element_count = array->getNumElements();
    info.contained_types.push_back(
        ParseLlvmType(array->getElementType(), depth + 1));
    return info;
  }
  case llvm::Type::StructTyID: {
    info.kind = LlvmTypeKind::Struct;
    const auto *structure = llvm::cast<llvm::StructType>(type);
    if (structure->hasName())
      info.name = structure->getName().str();
    info.is_opaque = structure->isOpaque();
    if (!info.is_opaque) {
      info.contained_types.reserve(structure->getNumElements());
      for (const auto *element : structure->elements())
        info.contained_types.push_back(ParseLlvmType(element, depth + 1));
    }
    return info;
  }
  case llvm::Type::FunctionTyID: {
    info.kind = LlvmTypeKind::Function;
    const auto *function = llvm::cast<llvm::FunctionType>(type);
    info.is_var_arg = function->isVarArg();
    info.contained_types.reserve(function->getNumParams() + 1);
    info.contained_types.push_back(
        ParseLlvmType(function->getReturnType(), depth + 1));
    for (const auto *parameter : function->params())
      info.contained_types.push_back(ParseLlvmType(parameter, depth + 1));
    return info;
  }
  default:
    return info;
  }
}

std::string
ValueOperandText(const llvm::Value *value) {
  if (!value)
    return {};

  std::string text;
  llvm::raw_string_ostream stream(text);
  value->printAsOperand(stream, false);
  return stream.str();
}

std::optional<uint64_t>
ConstantUInt64(const llvm::Value *value) {
  const auto *integer = llvm::dyn_cast_or_null<llvm::ConstantInt>(value);
  if (!integer)
    return std::nullopt;

  return integer->getValue().getLimitedValue();
}

std::optional<uint32_t>
ConstantUInt32(const llvm::Value *value) {
  auto value64 = ConstantUInt64(value);
  if (!value64 || *value64 > std::numeric_limits<uint32_t>::max())
    return std::nullopt;
  return uint32_t(*value64);
}

std::optional<uint32_t>
IntegerOperandUInt32(const std::vector<LlvmOperandInfo> &operands,
                     size_t index) {
  if (index >= operands.size() || !operands[index].is_integer ||
      operands[index].integer_value > std::numeric_limits<uint32_t>::max())
    return std::nullopt;
  return uint32_t(operands[index].integer_value);
}

std::optional<uint64_t>
IntegerOperandUInt64(const std::vector<LlvmOperandInfo> &operands,
                     size_t index) {
  if (index >= operands.size() || !operands[index].is_integer)
    return std::nullopt;
  return operands[index].integer_value;
}

void
AppendTypedOperand(DxilTypedOperationInfo &typed,
                   const std::vector<LlvmOperandInfo> &operands,
                   uint32_t index,
                   std::string_view name) {
  if (index >= operands.size())
    return;

  const auto &operand = operands[index];
  typed.operands.push_back({
      .operand_index = index,
      .name = std::string(name),
      .type = operand.type,
      .type_info = operand.type_info,
      .text = operand.text,
      .is_integer = operand.is_integer,
      .integer_value = operand.integer_value,
  });
}

void
AppendTypedOperands(DxilTypedOperationInfo &typed,
                    const std::vector<LlvmOperandInfo> &operands,
                    std::initializer_list<std::string_view> names) {
  uint32_t index = 1;
  for (const auto name : names)
    AppendTypedOperand(typed, operands, index++, name);
}

void
AppendRemainingTypedOperands(DxilTypedOperationInfo &typed,
                             const std::vector<LlvmOperandInfo> &operands,
                             uint32_t first_index,
                             std::string_view prefix) {
  for (uint32_t index = first_index; index < operands.size(); index++) {
    std::ostringstream name;
    name << prefix << (index - first_index);
    AppendTypedOperand(typed, operands, index, name.str());
  }
}

bool
SetTypedUInt32(const std::vector<LlvmOperandInfo> &operands,
               size_t index,
               uint32_t &out,
               bool &has_out) {
  if (auto value = IntegerOperandUInt32(operands, index)) {
    out = *value;
    has_out = true;
    return true;
  }
  return false;
}

bool
SetTypedBool(const std::vector<LlvmOperandInfo> &operands,
             size_t index,
             bool &out,
             bool &has_out) {
  if (auto value = IntegerOperandUInt32(operands, index)) {
    out = *value != 0;
    has_out = true;
    return true;
  }
  return false;
}

DxilSystemValueKind
SystemValueKindFromOpcode(std::string_view name) {
  if (name == "ThreadId")
    return DxilSystemValueKind::ThreadId;
  if (name == "GroupId")
    return DxilSystemValueKind::GroupId;
  if (name == "ThreadIdInGroup")
    return DxilSystemValueKind::ThreadIdInGroup;
  if (name == "FlattenedThreadIdInGroup")
    return DxilSystemValueKind::FlattenedThreadIdInGroup;
  if (name == "DispatchRaysIndex")
    return DxilSystemValueKind::DispatchRaysIndex;
  if (name == "DispatchRaysDimensions")
    return DxilSystemValueKind::DispatchRaysDimensions;
  if (name == "DomainLocation")
    return DxilSystemValueKind::DomainLocation;
  if (name == "OutputControlPointID")
    return DxilSystemValueKind::OutputControlPointID;
  if (name == "PrimitiveID")
    return DxilSystemValueKind::PrimitiveID;
  if (name == "ViewID")
    return DxilSystemValueKind::ViewID;
  if (name == "SampleIndex")
    return DxilSystemValueKind::SampleIndex;
  if (name == "Coverage")
    return DxilSystemValueKind::Coverage;
  if (name == "InnerCoverage")
    return DxilSystemValueKind::InnerCoverage;
  if (name == "GSInstanceID")
    return DxilSystemValueKind::GSInstanceID;
  if (name == "InstanceID")
    return DxilSystemValueKind::InstanceID;
  if (name == "InstanceIndex")
    return DxilSystemValueKind::InstanceIndex;
  if (name == "PrimitiveIndex")
    return DxilSystemValueKind::PrimitiveIndex;
  if (name == "GeometryIndex")
    return DxilSystemValueKind::GeometryIndex;
  if (name == "HitKind")
    return DxilSystemValueKind::HitKind;
  if (name == "RayFlags")
    return DxilSystemValueKind::RayFlags;
  return DxilSystemValueKind::Unknown;
}

bool
IsSampleOpcode(std::string_view name) {
  return name == "Sample" || name == "SampleBias" ||
         name == "SampleLevel" || name == "SampleGrad" ||
         name == "SampleCmp" || name == "SampleCmpLevelZero" ||
         name == "CalculateLOD" ||
         name.starts_with("WriteSamplerFeedback");
}

bool
IsGatherOpcode(std::string_view name) {
  return name == "TextureGather" || name == "TextureGatherCmp";
}

DxilTypedOperationInfo
ParseDxilTypedOperation(std::string_view name,
                        const std::vector<LlvmOperandInfo> &operands) {
  DxilTypedOperationInfo typed = {};

  if (name == "CreateHandle") {
    typed.kind = DxilTypedOperationKind::CreateHandle;
    AppendTypedOperands(typed, operands,
                        {"resource_class", "range_id", "index",
                         "non_uniform_index"});
    SetTypedUInt32(operands, 1, typed.resource_class,
                   typed.has_resource_class);
    SetTypedUInt32(operands, 2, typed.resource_range_id,
                   typed.has_resource_range_id);
    SetTypedUInt32(operands, 3, typed.resource_index,
                   typed.has_resource_index);
    SetTypedBool(operands, 4, typed.non_uniform, typed.has_non_uniform);
    return typed;
  }

  if (name == "CreateHandleFromBinding") {
    typed.kind = DxilTypedOperationKind::CreateHandleFromBinding;
    AppendTypedOperands(typed, operands,
                        {"binding", "index", "non_uniform_index"});
    if (auto binding = IntegerOperandUInt64(operands, 1)) {
      typed.resource_lower_bound = uint32_t(*binding & 0xffffffffu);
      typed.resource_space = uint32_t((*binding >> 32) & 0xffffffffu);
      typed.has_resource_binding = true;
    }
    SetTypedUInt32(operands, 2, typed.resource_index,
                   typed.has_resource_index);
    SetTypedBool(operands, 3, typed.non_uniform, typed.has_non_uniform);
    return typed;
  }

  if (name == "CreateHandleFromHeap") {
    typed.kind = DxilTypedOperationKind::CreateHandleFromHeap;
    AppendTypedOperands(typed, operands,
                        {"resource_class", "heap_index",
                         "non_uniform_index"});
    SetTypedUInt32(operands, 1, typed.resource_class,
                   typed.has_resource_class);
    SetTypedUInt32(operands, 2, typed.resource_index,
                   typed.has_resource_index);
    SetTypedBool(operands, 3, typed.non_uniform, typed.has_non_uniform);
    return typed;
  }

  if (name == "AnnotateHandle") {
    typed.kind = DxilTypedOperationKind::AnnotateHandle;
    AppendTypedOperands(typed, operands, {"handle", "properties"});
    return typed;
  }

  if (name == "CBufferLoad" || name == "CBufferLoadLegacy") {
    typed.kind = DxilTypedOperationKind::CBufferLoad;
    typed.is_read = true;
    AppendTypedOperands(typed, operands, {"handle", "index"});
    SetTypedUInt32(operands, 2, typed.resource_index,
                   typed.has_resource_index);
    return typed;
  }

  if (name == "TextureLoad") {
    typed.kind = DxilTypedOperationKind::TextureLoad;
    typed.is_read = true;
    AppendTypedOperands(typed, operands,
                        {"handle", "mip_or_sample", "coord0", "coord1",
                         "coord2", "offset0", "offset1", "offset2"});
    return typed;
  }

  if (name == "TextureStore") {
    typed.kind = DxilTypedOperationKind::TextureStore;
    typed.is_write = true;
    AppendTypedOperands(typed, operands,
                        {"handle", "coord0", "coord1", "coord2", "value0",
                         "value1", "value2", "value3", "mask"});
    SetTypedUInt32(operands, 9, typed.mask, typed.has_mask);
    return typed;
  }

  if (name == "BufferLoad") {
    typed.kind = DxilTypedOperationKind::BufferLoad;
    typed.is_read = true;
    AppendTypedOperands(typed, operands, {"handle", "index", "wot"});
    SetTypedUInt32(operands, 2, typed.resource_index,
                   typed.has_resource_index);
    return typed;
  }

  if (name == "BufferStore") {
    typed.kind = DxilTypedOperationKind::BufferStore;
    typed.is_write = true;
    AppendTypedOperands(typed, operands,
                        {"handle", "index", "wot", "value0", "value1",
                         "value2", "value3", "mask"});
    SetTypedUInt32(operands, 2, typed.resource_index,
                   typed.has_resource_index);
    SetTypedUInt32(operands, 8, typed.mask, typed.has_mask);
    return typed;
  }

  if (name == "RawBufferLoad") {
    typed.kind = DxilTypedOperationKind::RawBufferLoad;
    typed.is_read = true;
    AppendTypedOperands(typed, operands,
                        {"handle", "index", "element_offset", "mask",
                         "alignment"});
    SetTypedUInt32(operands, 2, typed.resource_index,
                   typed.has_resource_index);
    SetTypedUInt32(operands, 4, typed.mask, typed.has_mask);
    SetTypedUInt32(operands, 5, typed.alignment, typed.has_alignment);
    return typed;
  }

  if (name == "RawBufferStore") {
    typed.kind = DxilTypedOperationKind::RawBufferStore;
    typed.is_write = true;
    AppendTypedOperands(typed, operands,
                        {"handle", "index", "element_offset", "value0",
                         "value1", "value2", "value3", "mask",
                         "alignment"});
    SetTypedUInt32(operands, 2, typed.resource_index,
                   typed.has_resource_index);
    SetTypedUInt32(operands, 8, typed.mask, typed.has_mask);
    SetTypedUInt32(operands, 9, typed.alignment, typed.has_alignment);
    return typed;
  }

  if (IsSampleOpcode(name)) {
    typed.kind = DxilTypedOperationKind::Sample;
    typed.is_read = true;
    typed.is_sample = true;
    AppendTypedOperands(typed, operands, {"texture_handle", "sampler_handle"});
    AppendRemainingTypedOperands(typed, operands, 3, "sample_operand");
    return typed;
  }

  if (IsGatherOpcode(name)) {
    typed.kind = DxilTypedOperationKind::Gather;
    typed.is_read = true;
    typed.is_gather = true;
    AppendTypedOperands(typed, operands, {"texture_handle", "sampler_handle"});
    AppendRemainingTypedOperands(typed, operands, 3, "gather_operand");
    return typed;
  }

  if (name == "AtomicBinOp") {
    typed.kind = DxilTypedOperationKind::Atomic;
    typed.is_write = true;
    typed.is_atomic = true;
    AppendTypedOperands(typed, operands,
                        {"handle", "atomic_operation", "offset0", "offset1",
                         "offset2", "new_value"});
    SetTypedUInt32(operands, 2, typed.atomic_operation,
                   typed.has_atomic_operation);
    return typed;
  }

  if (name == "AtomicCompareExchange") {
    typed.kind = DxilTypedOperationKind::Atomic;
    typed.is_write = true;
    typed.is_atomic = true;
    AppendTypedOperands(typed, operands,
                        {"handle", "offset0", "offset1", "offset2",
                         "compare_value", "new_value"});
    return typed;
  }

  if (name == "LoadInput" || name == "LoadOutputControlPoint" ||
      name == "LoadPatchConstant") {
    typed.kind = DxilTypedOperationKind::LoadInput;
    typed.is_read = true;
    AppendTypedOperands(typed, operands,
                        {"signature_element_id", "row_index", "column_index",
                         "vertex_or_control_point_index"});
    SetTypedUInt32(operands, 1, typed.signature_element_id,
                   typed.has_signature_element_id);
    SetTypedUInt32(operands, 2, typed.row_index, typed.has_row_index);
    SetTypedUInt32(operands, 3, typed.column_index, typed.has_column_index);
    return typed;
  }

  if (name == "StoreOutput" || name == "StorePatchConstant" ||
      name == "StoreVertexOutput" || name == "StorePrimitiveOutput") {
    typed.kind = DxilTypedOperationKind::StoreOutput;
    typed.is_write = true;
    AppendTypedOperands(typed, operands,
                        {"signature_element_id", "row_index", "column_index",
                         "value"});
    SetTypedUInt32(operands, 1, typed.signature_element_id,
                   typed.has_signature_element_id);
    SetTypedUInt32(operands, 2, typed.row_index, typed.has_row_index);
    SetTypedUInt32(operands, 3, typed.column_index, typed.has_column_index);
    return typed;
  }

  const auto system_value = SystemValueKindFromOpcode(name);
  if (system_value != DxilSystemValueKind::Unknown) {
    typed.kind = DxilTypedOperationKind::SystemValue;
    typed.system_value = system_value;
    AppendTypedOperands(typed, operands, {"component_index"});
    SetTypedUInt32(operands, 1, typed.component_index,
                   typed.has_component_index);
    return typed;
  }

  return typed;
}

DxilSemanticOperationKind
ClassifyDxilOperation(std::string_view name,
                      std::string_view opcode_class,
                      std::string_view category,
                      uint32_t semantic_flags) {
  if (name == "LoadInput" || name == "LoadOutputControlPoint" ||
      name == "LoadPatchConstant" || name == "AttributeAtVertex")
    return DxilSemanticOperationKind::SignatureInput;
  if (name == "StoreOutput" || name == "StorePatchConstant" ||
      name == "StoreVertexOutput" || name == "StorePrimitiveOutput")
    return DxilSemanticOperationKind::SignatureOutput;
  if (name == "CreateHandle" || name == "CreateHandleForLib" ||
      name == "CreateHandleFromBinding" || name == "CreateHandleFromHeap" ||
      name == "AnnotateHandle")
    return DxilSemanticOperationKind::ResourceHandle;
  if (name.find("Sample") != std::string_view::npos ||
      name.find("Gather") != std::string_view::npos ||
      name.find("SamplerFeedback") != std::string_view::npos)
    return DxilSemanticOperationKind::ResourceSample;
  if (name.find("Store") != std::string_view::npos ||
      name.find("Atomic") != std::string_view::npos ||
      name == "BufferUpdateCounter")
    return DxilSemanticOperationKind::ResourceWrite;
  if (name.find("Load") != std::string_view::npos ||
      name == "CBufferLoad" || name == "CBufferLoadLegacy")
    return DxilSemanticOperationKind::ResourceRead;
  if (name == "GetDimensions" || name == "CheckAccessFullyMapped" ||
      name == "CalculateLOD")
    return DxilSemanticOperationKind::ResourceQuery;
  if (semantic_flags & DxilOpcodeSemanticBarrier)
    return DxilSemanticOperationKind::Barrier;
  if (semantic_flags & DxilOpcodeSemanticWave)
    return DxilSemanticOperationKind::Wave;
  if (semantic_flags & (DxilOpcodeSemanticDerivative |
                        DxilOpcodeSemanticGradient))
    return DxilSemanticOperationKind::Derivative;
  if (category.find("Ray") != std::string_view::npos ||
      category.find("HitObject") != std::string_view::npos ||
      opcode_class.find("RayQuery") != std::string_view::npos)
    return DxilSemanticOperationKind::Raytracing;
  if (category.find("Mesh") != std::string_view::npos ||
      category.find("Amplification") != std::string_view::npos)
    return DxilSemanticOperationKind::Mesh;
  if (category.find("Node") != std::string_view::npos ||
      name.find("Node") != std::string_view::npos)
    return DxilSemanticOperationKind::Node;
  if (category.find("float") != std::string_view::npos ||
      category.find("int") != std::string_view::npos ||
      opcode_class == "Unary" || opcode_class == "Binary" ||
      opcode_class == "Tertiary" || opcode_class == "Quaternary" ||
      opcode_class == "Dot")
    return DxilSemanticOperationKind::Math;
  return DxilSemanticOperationKind::Unknown;
}

LlvmOperandInfo
ParseLlvmOperand(const llvm::Value *value) {
  LlvmOperandInfo info = {};
  if (!value)
    return info;

  info.type = TypeString(value->getType());
  info.type_info = ParseLlvmType(value->getType());
  info.text = ValueOperandText(value);
  if (auto integer = ConstantUInt64(value)) {
    info.is_integer = true;
    info.integer_value = *integer;
  }
  return info;
}

std::string
DxilOperationName(std::string_view called_function) {
  constexpr std::string_view prefix = "dx.op.";
  if (!called_function.starts_with(prefix))
    return {};

  auto name = called_function.substr(prefix.size());
  const auto overload_offset = name.find('.');
  if (overload_offset != std::string_view::npos)
    name = name.substr(0, overload_offset);
  return std::string(name);
}

LlvmInstructionInfo
ParseLlvmInstruction(const llvm::Instruction &instruction) {
  LlvmInstructionInfo info = {};
  info.opcode_name = instruction.getOpcodeName();
  info.result_type = TypeString(instruction.getType());
  info.result_type_info = ParseLlvmType(instruction.getType());
  if (instruction.hasName())
    info.result_name = instruction.getName().str();

  info.operands.reserve(instruction.getNumOperands());
  for (const auto &operand : instruction.operands())
    info.operands.push_back(ParseLlvmOperand(operand.get()));

  const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
  if (!call)
    return info;

  info.is_call = true;
  if (const auto *called = call->getCalledFunction()) {
    info.called_function = called->getName().str();
  } else {
    info.is_indirect_call = true;
    info.called_function = ValueOperandText(call->getCalledOperand());
  }

  info.is_dx_intrinsic_call =
      std::string_view(info.called_function).starts_with("dx.op.");
  if (info.is_dx_intrinsic_call) {
    if (call->arg_size() > 0)
      info.dxil_opcode = ConstantUInt32(call->getArgOperand(0));
    info.dxil_opcode_name = DxilOperationName(info.called_function);
  }

  return info;
}

LlvmDxilOperationInfo
ParseDxilOperation(const LlvmInstructionInfo &instruction,
                   uint32_t instruction_index) {
  LlvmDxilOperationInfo info = {};
  info.instruction_index = instruction_index;
  info.called_function = instruction.called_function;
  info.opcode = instruction.dxil_opcode.value_or(0);
  info.opcode_name = instruction.dxil_opcode_name;
  if (const auto *opcode_info = FindDxilOpcodeInfo(info.opcode)) {
    info.opcode_known = true;
    info.opcode_reserved = opcode_info->is_reserved;
    info.opcode_name = std::string(opcode_info->name);
    info.opcode_class = std::string(opcode_info->opcode_class);
    info.opcode_category = std::string(opcode_info->category);
    info.opcode_function_attribute =
        std::string(opcode_info->function_attribute);
    info.opcode_overload_types = std::string(opcode_info->overload_types);
    info.min_shader_model_major = opcode_info->min_shader_model_major;
    info.min_shader_model_minor = opcode_info->min_shader_model_minor;
    info.semantic_flags = opcode_info->semantic_flags;
  }
  info.result_type = instruction.result_type;
  info.result_type_info = instruction.result_type_info;
  info.operands = instruction.operands;
  info.semantic_kind = ClassifyDxilOperation(
      info.opcode_name, info.opcode_class, info.opcode_category,
      info.semantic_flags);
  info.typed = ParseDxilTypedOperation(info.opcode_name, info.operands);
  if (info.semantic_kind == DxilSemanticOperationKind::SignatureInput ||
      info.semantic_kind == DxilSemanticOperationKind::SignatureOutput) {
    if (info.typed.has_signature_element_id) {
      info.signature_element_id = info.typed.signature_element_id;
      info.has_signature_element_id = true;
    } else if (auto id = IntegerOperandUInt32(info.operands, 1)) {
      info.signature_element_id = *id;
      info.has_signature_element_id = true;
    }
  }
  if (info.semantic_kind == DxilSemanticOperationKind::ResourceHandle) {
    if (info.typed.has_resource_range_id) {
      info.resource_id = info.typed.resource_range_id;
      info.has_resource_id = true;
    } else if (auto id = IntegerOperandUInt32(info.operands, 2)) {
      info.resource_id = *id;
      info.has_resource_id = true;
    } else if (auto id = IntegerOperandUInt32(info.operands, 1)) {
      info.resource_id = *id;
      info.has_resource_id = true;
    }
  }
  return info;
}

std::optional<uint32_t>
MetadataUInt32(const llvm::Metadata *metadata) {
  if (!metadata)
    return std::nullopt;

  const auto *constant = llvm::dyn_cast<llvm::ConstantAsMetadata>(metadata);
  if (!constant)
    return std::nullopt;

  const auto *integer = llvm::dyn_cast<llvm::ConstantInt>(constant->getValue());
  if (!integer)
    return std::nullopt;

  return uint32_t(integer->getZExtValue());
}

std::optional<uint64_t>
MetadataUInt64(const llvm::Metadata *metadata) {
  if (!metadata)
    return std::nullopt;

  const auto *constant = llvm::dyn_cast<llvm::ConstantAsMetadata>(metadata);
  if (!constant)
    return std::nullopt;

  const auto *integer = llvm::dyn_cast<llvm::ConstantInt>(constant->getValue());
  if (!integer)
    return std::nullopt;

  return integer->getZExtValue();
}

std::vector<uint32_t>
MetadataUInt32List(const llvm::MDNode *node) {
  std::vector<uint32_t> values;
  if (!node)
    return values;

  values.reserve(node->getNumOperands());
  for (const auto &operand : node->operands()) {
    auto value = MetadataUInt32(operand.get());
    if (!value)
      break;
    values.push_back(*value);
  }
  return values;
}

const llvm::MDNode *
MetadataNode(const llvm::Metadata *metadata) {
  return llvm::dyn_cast_or_null<llvm::MDNode>(metadata);
}

std::string
MetadataValueName(const llvm::Metadata *metadata) {
  if (!metadata)
    return {};

  const auto *value = llvm::dyn_cast<llvm::ValueAsMetadata>(metadata);
  if (!value)
    return {};

  const auto *named_value = value->getValue();
  if (named_value && named_value->hasName())
    return named_value->getName().str();
  return ValueOperandText(named_value);
}

DxilMetadataTagValueInfo
ParseMetadataTagValue(const llvm::MDNode *node) {
  DxilMetadataTagValueInfo info = {};
  if (!node)
    return info;

  if (node->getNumOperands() > 0) {
    if (auto tag = MetadataUInt32(node->getOperand(0).get()))
      info.tag = *tag;
  }
  if (node->getNumOperands() > 1) {
    const auto *value = node->getOperand(1).get();
    if (auto uint_value = MetadataUInt64(value)) {
      info.has_uint_value = true;
      info.uint_value = *uint_value;
    }
    info.string_value = MetadataString(value);
    info.text = MetadataText(value);
  }
  if (info.text.empty())
    info.text = MetadataText(node);
  return info;
}

std::vector<DxilMetadataTagValueInfo>
ParseMetadataTagValues(const llvm::MDNode *node) {
  std::vector<DxilMetadataTagValueInfo> tags;
  if (!node)
    return tags;

  tags.reserve(node->getNumOperands());
  for (const auto &operand : node->operands()) {
    const auto *tag_node = MetadataNode(operand.get());
    if (!tag_node)
      continue;
    tags.push_back(ParseMetadataTagValue(tag_node));
  }
  return tags;
}

std::vector<DxilMetadataTagValueInfo>
ParseTrailingMetadataTags(const llvm::MDNode *node, uint32_t first_tag_operand) {
  if (!node || node->getNumOperands() <= first_tag_operand)
    return {};

  const auto *tags = MetadataNode(node->getOperand(first_tag_operand).get());
  return ParseMetadataTagValues(tags);
}

DxilMetadataSignatureElementInfo
ParseDxilMetadataSignatureElement(const llvm::MDNode *node) {
  DxilMetadataSignatureElementInfo info = {};
  if (!node)
    return info;

  info.text = MetadataText(node);
  if (node->getNumOperands() > 0) {
    if (auto id = MetadataUInt32(node->getOperand(0).get()))
      info.id = *id;
  }
  if (node->getNumOperands() > 1)
    info.semantic_name = MetadataString(node->getOperand(1).get());

  if (node->getNumOperands() >= 11 &&
      MetadataNode(node->getOperand(4).get())) {
    if (auto component_type = MetadataUInt32(node->getOperand(2).get()))
      info.component_type = *component_type;
    if (auto semantic_kind = MetadataUInt32(node->getOperand(3).get()))
      info.semantic_kind = *semantic_kind;
    info.semantic_indices =
        MetadataUInt32List(MetadataNode(node->getOperand(4).get()));
    if (auto interpolation_mode = MetadataUInt32(node->getOperand(5).get()))
      info.interpolation_mode = *interpolation_mode;
    if (auto rows = MetadataUInt32(node->getOperand(6).get()))
      info.rows = *rows;
    if (auto cols = MetadataUInt32(node->getOperand(7).get()))
      info.cols = *cols;
    if (auto start_row = MetadataUInt32(node->getOperand(8).get()))
      info.start_row = *start_row;
    if (auto start_col = MetadataUInt32(node->getOperand(9).get()))
      info.start_col = *start_col;
    info.tags = ParseTrailingMetadataTags(node, 10);
    return info;
  }

  if (node->getNumOperands() > 2) {
    const auto *semantic_indices = MetadataNode(node->getOperand(2).get());
    if (semantic_indices)
      info.semantic_indices = MetadataUInt32List(semantic_indices);
    else if (auto semantic_index = MetadataUInt32(node->getOperand(2).get()))
      info.semantic_indices.push_back(*semantic_index);
  }
  if (node->getNumOperands() > 3) {
    if (auto rows = MetadataUInt32(node->getOperand(3).get()))
      info.rows = *rows;
  }
  if (node->getNumOperands() > 4) {
    if (auto cols = MetadataUInt32(node->getOperand(4).get()))
      info.cols = *cols;
  }
  if (node->getNumOperands() > 5) {
    if (auto start_row = MetadataUInt32(node->getOperand(5).get()))
      info.start_row = *start_row;
  }
  if (node->getNumOperands() > 6) {
    if (auto start_col = MetadataUInt32(node->getOperand(6).get()))
      info.start_col = *start_col;
  }
  if (node->getNumOperands() > 7) {
    if (auto semantic_kind = MetadataUInt32(node->getOperand(7).get()))
      info.semantic_kind = *semantic_kind;
  }
  if (node->getNumOperands() > 8) {
    if (auto component_type = MetadataUInt32(node->getOperand(8).get()))
      info.component_type = *component_type;
  }
  if (node->getNumOperands() > 9) {
    if (auto interpolation_mode = MetadataUInt32(node->getOperand(9).get()))
      info.interpolation_mode = *interpolation_mode;
  }
  if (node->getNumOperands() > 10) {
    if (auto dynamic_index_mask = MetadataUInt32(node->getOperand(10).get()))
      info.dynamic_index_mask = *dynamic_index_mask;
  }
  if (node->getNumOperands() > 11) {
    if (auto stream = MetadataUInt32(node->getOperand(11).get()))
      info.stream = *stream;
  }
  info.tags = ParseTrailingMetadataTags(node, 12);
  return info;
}

std::vector<DxilMetadataSignatureElementInfo>
ParseDxilMetadataSignatureList(const llvm::MDNode *node) {
  std::vector<DxilMetadataSignatureElementInfo> elements;
  if (!node)
    return elements;

  elements.reserve(node->getNumOperands());
  for (const auto &operand : node->operands()) {
    if (const auto *element = MetadataNode(operand.get()))
      elements.push_back(ParseDxilMetadataSignatureElement(element));
  }
  return elements;
}

DxilMetadataResourceInfo
ParseDxilMetadataResource(const llvm::MDNode *node,
                          DxilMetadataResourceClass resource_class) {
  DxilMetadataResourceInfo info = {};
  info.resource_class = resource_class;
  if (!node)
    return info;

  info.text = MetadataText(node);
  if (node->getNumOperands() > 0) {
    if (auto id = MetadataUInt32(node->getOperand(0).get()))
      info.id = *id;
  }
  if (node->getNumOperands() > 1)
    info.global_name = MetadataValueName(node->getOperand(1).get());
  if (node->getNumOperands() > 2)
    info.name = MetadataString(node->getOperand(2).get());
  if (node->getNumOperands() > 3) {
    if (auto space = MetadataUInt32(node->getOperand(3).get()))
      info.space = *space;
  }
  if (node->getNumOperands() > 4) {
    if (auto lower_bound = MetadataUInt32(node->getOperand(4).get()))
      info.lower_bound = *lower_bound;
  }
  if (node->getNumOperands() > 5) {
    if (auto range_size = MetadataUInt32(node->getOperand(5).get()))
      info.range_size = *range_size;
  }
  if (info.range_size == std::numeric_limits<uint32_t>::max())
    info.upper_bound = std::numeric_limits<uint32_t>::max();
  else if (info.range_size)
    info.upper_bound = info.lower_bound + info.range_size - 1;
  else
    info.upper_bound = info.lower_bound;

  for (uint32_t i = 6; i < node->getNumOperands(); i++) {
    const auto *operand = node->getOperand(i).get();
    if (const auto *tag_node = MetadataNode(operand)) {
      info.tags = ParseMetadataTagValues(tag_node);
      break;
    }
    if (auto value = MetadataUInt32(operand)) {
      info.numeric_operands.push_back(*value);
      if (i == 6)
        info.kind = *value;
      else if (i == 7)
        info.element_type = *value;
      else if (i == 8)
        info.flags = *value;
    }
  }
  return info;
}

std::vector<DxilMetadataResourceInfo>
ParseDxilMetadataResourceLists(const llvm::MDNode *node) {
  std::vector<DxilMetadataResourceInfo> resources;
  if (!node)
    return resources;

  const DxilMetadataResourceClass classes[] = {
      DxilMetadataResourceClass::Srv,
      DxilMetadataResourceClass::Uav,
      DxilMetadataResourceClass::Cbv,
      DxilMetadataResourceClass::Sampler,
  };

  for (uint32_t list_index = 0; list_index < node->getNumOperands(); list_index++) {
    const auto *list = MetadataNode(node->getOperand(list_index).get());
    if (!list)
      continue;

    const auto resource_class = list_index < (sizeof(classes) / sizeof(classes[0]))
                                    ? classes[list_index]
                                    : DxilMetadataResourceClass::Unknown;
    resources.reserve(resources.size() + list->getNumOperands());
    for (const auto &operand : list->operands()) {
      if (const auto *resource = MetadataNode(operand.get()))
        resources.push_back(ParseDxilMetadataResource(resource, resource_class));
    }
  }
  return resources;
}

std::string
BasicBlockName(const llvm::BasicBlock &block) {
  if (block.hasName())
    return block.getName().str();

  std::string text;
  llvm::raw_string_ostream stream(text);
  block.printAsOperand(stream, false);
  return stream.str();
}

LlvmBasicBlockInfo
ParseLlvmBasicBlockInfo(const llvm::BasicBlock &block,
                        uint32_t instruction_start) {
  LlvmBasicBlockInfo info = {};
  info.name = BasicBlockName(block);
  info.instruction_start = instruction_start;
  info.instruction_count = uint32_t(block.size());

  const auto *terminator = block.getTerminator();
  if (terminator) {
    info.terminator_opcode = terminator->getOpcodeName();
    info.has_return = llvm::isa<llvm::ReturnInst>(terminator);
    info.has_branch = llvm::isa<llvm::BranchInst>(terminator);
    info.has_switch = llvm::isa<llvm::SwitchInst>(terminator);
    info.has_unreachable = llvm::isa<llvm::UnreachableInst>(terminator);
    for (const auto *successor : llvm::successors(&block))
      info.successors.push_back(BasicBlockName(*successor));
  }

  return info;
}

LlvmFunctionInfo *
FindLlvmFunctionInfo(LlvmModuleInfo &module, std::string_view name) {
  if (name.empty())
    return nullptr;
  auto function = std::find_if(
      module.functions.begin(), module.functions.end(),
      [name](const LlvmFunctionInfo &info) { return info.name == name; });
  return function != module.functions.end() ? &*function : nullptr;
}

void
BuildLlvmCallGraph(LlvmModuleInfo &module) {
  module.call_graph = {};

  for (auto &function : module.functions) {
    function.called_functions.clear();
    function.has_indirect_calls = false;
    function.is_entry_reachable = false;
    function.is_recursive = false;

    for (uint32_t i = 0; i < function.instructions.size(); i++) {
      const auto &instruction = function.instructions[i];
      if (!instruction.is_call)
        continue;

      LlvmCallGraphEdgeInfo edge = {};
      edge.caller = function.name;
      edge.callee = instruction.called_function;
      edge.instruction_index = i;
      edge.is_indirect = instruction.is_indirect_call;
      edge.is_dx_intrinsic = instruction.is_dx_intrinsic_call;
      module.call_graph.has_indirect_calls =
          module.call_graph.has_indirect_calls || edge.is_indirect;
      function.has_indirect_calls = function.has_indirect_calls || edge.is_indirect;
      module.call_graph.edges.push_back(edge);

      if (!edge.is_indirect && !edge.is_dx_intrinsic &&
          FindLlvmFunctionInfo(module, edge.callee))
        function.called_functions.push_back(edge.callee);
    }
  }

  std::vector<std::string> worklist;
  for (const auto &entry : module.entry_points) {
    if (!entry.function_name.empty())
      worklist.push_back(entry.function_name);
  }
  if (worklist.empty()) {
    for (const auto &function : module.functions) {
      if (!function.is_declaration && !function.is_dx_intrinsic) {
        worklist.push_back(function.name);
        break;
      }
    }
  }

  std::vector<std::string> reachable;
  while (!worklist.empty()) {
    const auto name = std::move(worklist.back());
    worklist.pop_back();
    if (std::find(reachable.begin(), reachable.end(), name) != reachable.end())
      continue;

    auto *function = FindLlvmFunctionInfo(module, name);
    if (!function || function->is_declaration)
      continue;

    function->is_entry_reachable = true;
    reachable.push_back(function->name);
    for (const auto &callee : function->called_functions)
      worklist.push_back(callee);
  }
  module.call_graph.entry_reachable_functions = reachable;

  for (auto &function : module.functions) {
    if (function.is_declaration)
      continue;

    std::vector<std::string> stack;
    std::vector<std::string> visited;
    stack.push_back(function.name);
    while (!stack.empty()) {
      const auto current = std::move(stack.back());
      stack.pop_back();
      const auto *current_function = FindLlvmFunctionInfo(module, current);
      if (!current_function)
        continue;
      for (const auto &callee : current_function->called_functions) {
        if (callee == function.name) {
          function.is_recursive = true;
          module.call_graph.has_recursion = true;
          if (std::find(module.call_graph.recursive_functions.begin(),
                        module.call_graph.recursive_functions.end(),
                        function.name) ==
              module.call_graph.recursive_functions.end())
            module.call_graph.recursive_functions.push_back(function.name);
          stack.clear();
          break;
        }
        if (std::find(visited.begin(), visited.end(), callee) == visited.end()) {
          visited.push_back(callee);
          stack.push_back(callee);
        }
      }
    }
  }

  for (const auto &function : module.functions) {
    if (!function.is_dx_intrinsic)
      continue;

    const auto used = std::any_of(
        module.call_graph.edges.begin(), module.call_graph.edges.end(),
        [&](const LlvmCallGraphEdgeInfo &edge) {
          return edge.is_dx_intrinsic && edge.callee == function.name;
        });
    if (!used)
      module.call_graph.unused_dx_intrinsic_declarations.push_back(function.name);
  }
}
#endif

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
ReadNullableRdatString(const RuntimeDataInfo &info, uint32_t offset,
                       std::string &out) {
  out.clear();
  return offset == kRdatNullRef || info.readString(offset, out);
}

bool
ReadNullableRdatIndexArray(const RuntimeDataInfo &info, uint32_t offset,
                           std::vector<uint32_t> &out) {
  out.clear();
  return offset == kRdatNullRef || info.readIndexArray(offset, out);
}

bool
ReadNullableRdatStringArray(const RuntimeDataInfo &info, uint32_t offset,
                            std::vector<std::string> &out) {
  out.clear();

  std::vector<uint32_t> string_offsets;
  if (!ReadNullableRdatIndexArray(info, offset, string_offsets))
    return false;

  out.reserve(string_offsets.size());
  for (const auto string_offset : string_offsets) {
    std::string value;
    if (!ReadNullableRdatString(info, string_offset, value))
      return false;
    out.push_back(std::move(value));
  }
  return true;
}

bool
ReadNullableRdatRecordArray(const RuntimeDataInfo &info, uint32_t offset,
                            size_t record_count, std::vector<uint32_t> &out) {
  if (!ReadNullableRdatIndexArray(info, offset, out))
    return false;

  for (const auto index : out) {
    if (index >= record_count)
      return false;
  }
  return true;
}

bool
ReadNullableRdatBytes(const RuntimeDataInfo &info, uint32_t offset,
                      uint32_t size, std::span<const uint8_t> &out) {
  out = {};
  if (!size)
    return true;
  return offset != kRdatNullRef && info.readBytes(offset, size, out);
}

bool
ReadRdatNumThreads(const RuntimeDataInfo &info, uint32_t offset,
                   uint32_t &x, uint32_t &y, uint32_t &z) {
  x = 1;
  y = 1;
  z = 1;
  std::vector<uint32_t> values;
  if (!ReadNullableRdatIndexArray(info, offset, values))
    return false;
  if (values.size() > 0)
    x = values[0];
  if (values.size() > 1)
    y = values[1];
  if (values.size() > 2)
    z = values[2];
  return true;
}

uint32_t
PsvMaskDwordCount(uint32_t vectors) {
  return ((vectors * 4u) + 31u) / 32u;
}

uint32_t
PsvInputOutputTableDwordCount(uint32_t input_vectors,
                              uint32_t output_vectors) {
  return PsvMaskDwordCount(output_vectors) * input_vectors * 4u;
}

bool
ReadU32Words(std::span<const uint8_t> data, size_t &offset,
             uint32_t word_count, std::vector<uint32_t> &out) {
  size_t end = 0;
  if (!CheckedEnd(offset, size_t(word_count) * sizeof(uint32_t),
                  data.size(), end))
    return false;

  out.clear();
  out.reserve(word_count);
  for (uint32_t i = 0; i < word_count; i++)
    out.push_back(ReadU32(data, offset + size_t(i) * sizeof(uint32_t)));
  offset = end;
  return true;
}

bool
ReadPsvComponentMask(std::span<const uint8_t> data, size_t &offset,
                     uint32_t vectors, PsvComponentMaskInfo &out) {
  out = {};
  out.vector_count = vectors;
  return ReadU32Words(data, offset, PsvMaskDwordCount(vectors),
                      out.mask_words);
}

bool
ReadPsvDependencyTable(std::span<const uint8_t> data, size_t &offset,
                       uint32_t input_vectors, uint32_t output_vectors,
                       PsvDependencyTableInfo &out) {
  out = {};
  out.input_vectors = input_vectors;
  out.output_vectors = output_vectors;
  return ReadU32Words(data, offset,
                      PsvInputOutputTableDwordCount(input_vectors,
                                                    output_vectors),
                      out.mask_words);
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

struct BitcodeAbbrevOperand {
  bool literal = false;
  uint64_t value = 0;
  uint32_t encoding = 0;
};

using BitcodeAbbrev = std::vector<BitcodeAbbrevOperand>;

class BitReader {
public:
  explicit BitReader(std::span<const uint8_t> data)
      : data_(data), bit_limit_(uint64_t(data.size()) * 8) {}

  uint64_t bitOffset() const { return bit_offset_; }

  bool readBits(uint32_t width, uint64_t &value) {
    if (width > 64)
      return false;
    uint64_t end = 0;
    if (!CheckedAdd64(bit_offset_, width, bit_limit_, end))
      return false;

    value = 0;
    for (uint32_t i = 0; i < width; i++) {
      const auto bit = (data_[size_t((bit_offset_ + i) >> 3)] >>
                        ((bit_offset_ + i) & 7)) &
                       1u;
      value |= uint64_t(bit) << i;
    }
    bit_offset_ = end;
    return true;
  }

  bool readVbr(uint32_t width, uint64_t &value) {
    if (width < 2 || width > 32)
      return false;

    value = 0;
    uint32_t shift = 0;
    const uint64_t payload_mask = (uint64_t(1) << (width - 1)) - 1;
    const uint64_t continue_bit = uint64_t(1) << (width - 1);

    for (;;) {
      uint64_t piece = 0;
      if (!readBits(width, piece))
        return false;

      if (shift >= 64 || ((piece & payload_mask) << shift) >> shift !=
                              (piece & payload_mask))
        return false;
      value |= (piece & payload_mask) << shift;

      if ((piece & continue_bit) == 0)
        return true;
      shift += width - 1;
    }
  }

  bool align32() {
    const auto aligned = (bit_offset_ + 31u) & ~uint64_t(31u);
    if (aligned > bit_limit_)
      return false;
    bit_offset_ = aligned;
    return true;
  }

  bool skipBits(uint64_t bits) {
    uint64_t end = 0;
    if (!CheckedAdd64(bit_offset_, bits, bit_limit_, end))
      return false;
    bit_offset_ = end;
    return true;
  }

  bool seek(uint64_t bit_offset) {
    if (bit_offset > bit_limit_)
      return false;
    bit_offset_ = bit_offset;
    return true;
  }

private:
  std::span<const uint8_t> data_;
  uint64_t bit_offset_ = 0;
  uint64_t bit_limit_ = 0;
};

class BitcodeParser {
public:
  explicit BitcodeParser(std::span<const uint8_t> data, BitcodeInfo &info)
      : reader_(data), info_(info), bit_limit_(uint64_t(data.size()) * 8) {}

  bool parse() {
    uint64_t magic = 0;
    if (!reader_.readBits(32, magic) || magic != kBitcodeMagicValue)
      return false;

    info_ = {};
    info_.magic = uint32_t(magic);
    return parseBlock(std::numeric_limits<uint32_t>::max(), 2, 0, bit_limit_);
  }

private:
  bool parseBlock(uint32_t block_id, uint32_t abbrev_width, uint32_t depth,
                  uint64_t end_bit) {
    if (depth > 64 || abbrev_width == 0 || abbrev_width > 32)
      return false;

    const auto block_index = info_.blocks.size();
    info_.blocks.push_back({
        .id = block_id,
        .abbreviation_id_width = abbrev_width,
        .depth = depth,
        .start_bit = reader_.bitOffset(),
    });

    auto abbrevs = findBlockInfoAbbrevs(block_id);
    uint32_t block_info_set_bid = 0;

    while (reader_.bitOffset() < end_bit) {
      uint64_t abbrev_id = 0;
      if (!reader_.readBits(abbrev_width, abbrev_id))
        return false;

      if (abbrev_id == bitc::EndBlock) {
        if (!reader_.align32() || reader_.bitOffset() > end_bit)
          return false;
        info_.blocks[block_index].end_bit = reader_.bitOffset();
        return true;
      }

      if (abbrev_id == bitc::EnterSubblock) {
        uint64_t child_block_id = 0;
        uint64_t child_abbrev_width = 0;
        uint64_t child_block_words = 0;
        if (!reader_.readVbr(8, child_block_id) ||
            !reader_.readVbr(4, child_abbrev_width) ||
            !reader_.align32() ||
            !reader_.readBits(32, child_block_words))
          return false;

        uint64_t child_end = 0;
        if (!CheckedAdd64(reader_.bitOffset(), child_block_words * 32,
                          end_bit, child_end))
          return false;

        if (!parseBlock(uint32_t(child_block_id), uint32_t(child_abbrev_width),
                        depth + 1, child_end))
          return false;
        if (reader_.bitOffset() > child_end)
          return false;
        if (!reader_.seek(child_end))
          return false;
        continue;
      }

      if (abbrev_id == bitc::DefineAbbrev) {
        BitcodeAbbrev abbrev;
        if (!readAbbrev(abbrev))
          return false;
        if (block_id == bitc::BlockInfoBlockId && block_info_set_bid)
          addBlockInfoAbbrev(block_info_set_bid, std::move(abbrev));
        else
          abbrevs.push_back(std::move(abbrev));
        continue;
      }

      BitcodeRecordInfo record = {
          .block_id = block_id,
          .abbreviated = abbrev_id != bitc::UnabbrevRecord,
      };

      std::vector<uint64_t> record_values;
      if (abbrev_id == bitc::UnabbrevRecord) {
        uint64_t code = 0;
        uint64_t operand_count = 0;
        if (!reader_.readVbr(6, code) || !reader_.readVbr(6, operand_count))
          return false;
        record.code = uint32_t(code);
        record.operand_count = uint32_t(operand_count);
        record_values.push_back(code);
        for (uint64_t i = 0; i < operand_count; i++) {
          uint64_t value = 0;
          if (!reader_.readVbr(6, value))
            return false;
          if (record_values.size() < 16)
            record_values.push_back(value);
        }
      } else {
        const auto app_abbrev = abbrev_id - bitc::FirstApplicationAbbrev;
        if (app_abbrev >= abbrevs.size())
          return false;
        uint32_t operand_count = 0;
        if (!readAbbreviatedRecord(abbrevs[size_t(app_abbrev)],
                                   record_values, operand_count))
          return false;
        if (record_values.empty())
          return false;
        record.code = uint32_t(record_values[0]);
        record.operand_count = operand_count > 0 ? operand_count - 1 : 0;
      }

      if (block_id == bitc::BlockInfoBlockId &&
          record.code == bitc::BlockInfoSetBid && record_values.size() >= 2)
        block_info_set_bid = uint32_t(record_values[1]);

      info_.blocks[block_index].record_count++;
      info_.records.push_back(record);
    }

    if (reader_.bitOffset() != end_bit)
      return false;
    info_.blocks[block_index].end_bit = end_bit;
    return true;
  }

  bool readAbbrev(BitcodeAbbrev &abbrev) {
    uint64_t operand_count = 0;
    if (!reader_.readVbr(5, operand_count))
      return false;
    abbrev.clear();
    abbrev.reserve(size_t(operand_count));

    for (uint64_t i = 0; i < operand_count; i++) {
      uint64_t is_literal = 0;
      if (!reader_.readBits(1, is_literal))
        return false;
      if (is_literal) {
        uint64_t value = 0;
        if (!reader_.readVbr(8, value))
          return false;
        abbrev.push_back({.literal = true, .value = value});
        continue;
      }

      uint64_t encoding = 0;
      if (!reader_.readBits(3, encoding) || encoding < 1 || encoding > 5)
        return false;
      BitcodeAbbrevOperand operand = {
          .literal = false,
          .encoding = uint32_t(encoding),
      };
      if (encoding == 1 || encoding == 2) {
        if (!reader_.readVbr(5, operand.value))
          return false;
      }
      abbrev.push_back(operand);
    }

    return true;
  }

  bool readAbbreviatedRecord(const BitcodeAbbrev &abbrev,
                             std::vector<uint64_t> &values,
                             uint32_t &operand_count) {
    values.clear();
    operand_count = 0;

    for (size_t i = 0; i < abbrev.size(); i++) {
      const auto &operand = abbrev[i];
      if (operand.literal) {
        pushRecordValue(values, operand.value, operand_count);
        continue;
      }

      if (operand.encoding == 3) {
        if (i + 1 >= abbrev.size())
          return false;
        uint64_t length = 0;
        if (!reader_.readVbr(6, length))
          return false;
        for (uint64_t item = 0; item < length; item++) {
          uint64_t value = 0;
          if (!readEncodedValue(abbrev[i + 1], value))
            return false;
          pushRecordValue(values, value, operand_count);
        }
        i++;
        continue;
      }

      if (operand.encoding == 5) {
        uint64_t length = 0;
        if (!reader_.readVbr(6, length) ||
            length > std::numeric_limits<uint64_t>::max() / 8 ||
            !reader_.align32() ||
            !reader_.skipBits(length * 8) || !reader_.align32())
          return false;
        operand_count++;
        continue;
      }

      uint64_t value = 0;
      if (!readEncodedValue(operand, value))
        return false;
      pushRecordValue(values, value, operand_count);
    }

    return true;
  }

  bool readEncodedValue(const BitcodeAbbrevOperand &operand, uint64_t &value) {
    if (operand.literal) {
      value = operand.value;
      return true;
    }

    switch (operand.encoding) {
    case 1:
      return reader_.readBits(uint32_t(operand.value), value);
    case 2:
      return reader_.readVbr(uint32_t(operand.value), value);
    case 4:
      return reader_.readBits(6, value);
    default:
      return false;
    }
  }

  void pushRecordValue(std::vector<uint64_t> &values, uint64_t value,
                       uint32_t &operand_count) {
    operand_count++;
    if (values.size() < 16)
      values.push_back(value);
  }

  std::vector<BitcodeAbbrev> findBlockInfoAbbrevs(uint32_t block_id) const {
    for (const auto &entry : block_info_abbrevs_) {
      if (entry.first == block_id)
        return entry.second;
    }
    return {};
  }

  void addBlockInfoAbbrev(uint32_t block_id, BitcodeAbbrev &&abbrev) {
    for (auto &entry : block_info_abbrevs_) {
      if (entry.first == block_id) {
        entry.second.push_back(std::move(abbrev));
        return;
      }
    }
    block_info_abbrevs_.push_back({block_id, {std::move(abbrev)}});
  }

  BitReader reader_;
  BitcodeInfo &info_;
  uint64_t bit_limit_ = 0;
  std::vector<std::pair<uint32_t, std::vector<BitcodeAbbrev>>> block_info_abbrevs_;
};

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

bool
ShaderHashInfo::is_populated() const {
  return std::any_of(digest.begin(), digest.end(),
                     [](uint8_t value) { return value != 0; });
}

bool
LlvmModuleInfo::hasNamedMetadata(std::string_view name) const {
  return std::any_of(named_metadata.begin(), named_metadata.end(),
                     [name](const NamedMetadataInfo &metadata) {
                       return metadata.name == name;
                     });
}

const DxilOpcodeInfo *
FindDxilOpcodeInfo(uint32_t opcode) {
  constexpr auto kOpcodeCount =
      sizeof(kDxilOpcodeInfos) / sizeof(kDxilOpcodeInfos[0]);
  if (opcode >= kOpcodeCount)
    return nullptr;

  const auto &info = kDxilOpcodeInfos[opcode];
  return info.opcode == opcode ? &info : nullptr;
}

const char *
DxilOpcodeName(uint32_t opcode) {
  if (const auto *info = FindDxilOpcodeInfo(opcode))
    return info->name.data();
  return "Unknown";
}

const RuntimeDataPartInfo *
RuntimeDataInfo::findPart(uint32_t type, size_t start_index) const {
  for (size_t i = start_index; i < parts.size(); i++) {
    if (parts[i].type == type)
      return &parts[i];
  }
  return nullptr;
}

const RdatShaderInfo *
RuntimeDataInfo::findShaderInfo(uint32_t table_type,
                                uint32_t record_index) const {
  for (const auto &info : shader_infos) {
    if (info.table_type == table_type && info.record_index == record_index)
      return &info;
  }
  return nullptr;
}

bool
RuntimeDataInfo::readString(uint32_t offset, std::string &out) const {
  const auto *strings = findPart(rdat::StringBuffer);
  return strings && ReadString(strings->data, offset, out);
}

bool
RuntimeDataInfo::readIndexArray(uint32_t offset, std::vector<uint32_t> &out) const {
  const auto *indices = findPart(rdat::IndexArrays);
  if (!indices)
    return false;

  if (offset == kRdatNullRef)
    return false;

  const auto count = indices->data.size() / sizeof(uint32_t);
  if (count < 1 || offset >= count - 1)
    return false;

  const auto byte_offset = size_t(offset) * sizeof(uint32_t);
  const auto element_count = ReadU32(indices->data, byte_offset);
  if (element_count > count - offset - 1)
    return false;

  out.clear();
  out.reserve(element_count);
  for (uint32_t i = 0; i < element_count; i++)
    out.push_back(ReadU32(indices->data, byte_offset + sizeof(uint32_t) * (i + 1)));
  return true;
}

bool
RuntimeDataInfo::readBytes(uint32_t offset, uint32_t size,
                           std::span<const uint8_t> &out) const {
  const auto *raw_bytes = findPart(rdat::RawBytes);
  if (!raw_bytes)
    return false;

  size_t end = 0;
  if (!CheckedEnd(offset, size, raw_bytes->data.size(), end))
    return false;

  out = std::span<const uint8_t>(raw_bytes->data.data() + offset, size);
  return true;
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
  case ParseStatus::InvalidShaderHash:
    return "invalid shader hash";
  case ParseStatus::InvalidCompilerVersion:
    return "invalid compiler version";
  case ParseStatus::InvalidShaderDebugName:
    return "invalid shader debug name";
  case ParseStatus::InvalidSourceInfo:
    return "invalid source info";
  case ParseStatus::InvalidShaderPdbInfo:
    return "invalid shader PDB info";
  case ParseStatus::InvalidShaderStatistics:
    return "invalid shader statistics";
  case ParseStatus::InvalidResourceDef:
    return "invalid resource definition";
  case ParseStatus::InvalidBitcode:
    return "invalid bitcode";
  case ParseStatus::InvalidLlvmModule:
    return "invalid LLVM module";
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
  bitcode_.reset();
  llvm_module_.reset();
  signatures_.clear();
  feature_info_.reset();
  shader_hash_.reset();
  compiler_version_.reset();
  shader_debug_name_.reset();
  source_info_.reset();
  shader_pdb_info_.reset();
  shader_statistics_.reset();
  resource_def_.reset();
  runtime_data_.reset();
  psv_info_.reset();
  shader_reflection_.reset();
  dxil_validation_.reset();
  dxil_translation_.reset();
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

  status = parseBitcode();
  if (status != ParseStatus::Ok)
    return status;

#if DXILPARSER_USE_LLVM
  LlvmModuleInfo module_info = {};
  status = ParseLlvmModule(dxil_program_->bitcode, module_info);
  if (status != ParseStatus::Ok)
    return status;
  llvm_module_ = std::move(module_info);
#endif

  status = parseKnownParts();
  if (status != ParseStatus::Ok)
    return status;

  ShaderReflectionInfo reflection_info = {};
  status = BuildShaderReflection(*this, reflection_info);
  if (status != ParseStatus::Ok)
    return status;
  shader_reflection_ = std::move(reflection_info);

  DxilValidationInfo validation_info = {};
  status = ValidateDxil(*this, validation_info);
  if (status != ParseStatus::Ok)
    return status;
  dxil_validation_ = std::move(validation_info);

  DxilTranslationInfo translation_info = {};
  status = BuildDxilTranslationInfo(*this, translation_info);
  if (status != ParseStatus::Ok)
    return status;
  dxil_translation_ = std::move(translation_info);
  return ParseStatus::Ok;
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
  std::copy_n(data.data() + 4, container_.hash.size(), container_.hash.begin());
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
Parser::parseBitcode() {
  if (!dxil_program_)
    return ParseStatus::MissingDxilPart;

  BitcodeInfo info = {};
  auto status = ParseBitcode(dxil_program_->bitcode, info);
  if (status != ParseStatus::Ok)
    return status;

  bitcode_ = std::move(info);
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

  if (const auto *part = container_.findPart(fourcc::ShaderHash)) {
    ShaderHashInfo info = {};
    auto status = ParseShaderHash(*part, info);
    if (status != ParseStatus::Ok)
      return status;
    shader_hash_ = info;
  }

  if (const auto *part = container_.findPart(fourcc::CompilerVersion)) {
    CompilerVersionInfo info = {};
    auto status = ParseCompilerVersion(*part, info);
    if (status != ParseStatus::Ok)
      return status;
    compiler_version_ = std::move(info);
  }

  if (const auto *part = container_.findPart(fourcc::ShaderDebugName)) {
    ShaderDebugNameInfo info = {};
    auto status = ParseShaderDebugName(*part, info);
    if (status != ParseStatus::Ok)
      return status;
    shader_debug_name_ = std::move(info);
  }

  if (const auto *part = container_.findPart(fourcc::ShaderSourceInfo)) {
    SourceInfo info = {};
    auto status = ParseSourceInfo(*part, info);
    if (status != ParseStatus::Ok)
      return status;
    source_info_ = std::move(info);
  }

  if (const auto *part = container_.findPart(fourcc::ShaderPdbInfo)) {
    ShaderPdbInfo info = {};
    auto status = ParseShaderPdbInfo(*part, info);
    if (status != ParseStatus::Ok)
      return status;
    shader_pdb_info_ = info;
  }

  if (const auto *part = container_.findPart(fourcc::ShaderStatistics)) {
    ShaderStatisticsInfo info = {};
    auto status = ParseShaderStatistics(*part, info);
    if (status != ParseStatus::Ok)
      return status;
    shader_statistics_ = std::move(info);
  }

  if (const auto *part = container_.findPart(fourcc::ResourceDef)) {
    ResourceDefInfo info = {};
    auto status = ParseResourceDef(*part, info);
    if (status != ParseStatus::Ok)
      return status;
    resource_def_ = std::move(info);
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
ParseBitcode(std::span<const uint8_t> data, BitcodeInfo &info) {
  BitcodeInfo wrapper = {};
  std::span<const uint8_t> bitstream;
  if (!ExtractBitstream(data, bitstream, &wrapper))
    return ParseStatus::InvalidBitcode;

  BitcodeParser parser(bitstream, info);
  if (!parser.parse())
    return ParseStatus::InvalidBitcode;

  if (wrapper.has_wrapper) {
    info.has_wrapper = true;
    info.wrapper_version = wrapper.wrapper_version;
    info.wrapper_offset = wrapper.wrapper_offset;
    info.wrapper_size = wrapper.wrapper_size;
    info.wrapper_cpu_type = wrapper.wrapper_cpu_type;
  }
  return ParseStatus::Ok;
}

ParseStatus
ParseLlvmModule(std::span<const uint8_t> data, LlvmModuleInfo &info) {
#if DXILPARSER_USE_LLVM
  std::span<const uint8_t> bitstream;
  if (!ExtractBitstream(data, bitstream))
    return ParseStatus::InvalidLlvmModule;

  llvm::LLVMContext context;
  const auto buffer = llvm::MemoryBufferRef(
      llvm::StringRef(reinterpret_cast<const char *>(bitstream.data()),
                      bitstream.size()),
      "dxil");
  auto module_or_error = llvm::parseBitcodeFile(buffer, context);
  if (!module_or_error) {
    llvm::consumeError(module_or_error.takeError());
    return ParseStatus::InvalidLlvmModule;
  }

  const auto module = std::move(*module_or_error);
  info = {};
  info.module_identifier = module->getModuleIdentifier();
  info.source_file_name = module->getSourceFileName();
  info.target_triple = module->getTargetTriple();
  info.data_layout = module->getDataLayoutStr();

  for (const auto &metadata : module->named_metadata()) {
    info.named_metadata.push_back({
        .name = metadata.getName().str(),
        .operand_count = metadata.getNumOperands(),
    });
  }

  if (const auto *shader_model = module->getNamedMetadata("dx.shaderModel");
      shader_model && shader_model->getNumOperands()) {
    const auto *node = shader_model->getOperand(0);
    if (node && node->getNumOperands() >= 3) {
      DxilShaderModelInfo shader_model_info = {};
      shader_model_info.kind = MetadataString(node->getOperand(0).get());
      if (auto major = MetadataUInt32(node->getOperand(1).get()))
        shader_model_info.major = *major;
      if (auto minor = MetadataUInt32(node->getOperand(2).get()))
        shader_model_info.minor = *minor;
      info.shader_model = std::move(shader_model_info);
    }
  }

  if (const auto *version = module->getNamedMetadata("dx.version");
      version && version->getNumOperands())
    info.dxil_version = MetadataUInt32List(version->getOperand(0));

  if (const auto *validator_version = module->getNamedMetadata("dx.valver");
      validator_version && validator_version->getNumOperands())
    info.validator_version = MetadataUInt32List(validator_version->getOperand(0));

  if (const auto *resources = module->getNamedMetadata("dx.resources");
      resources && resources->getNumOperands()) {
    for (const auto *resource_lists : resources->operands()) {
      auto parsed = ParseDxilMetadataResourceLists(resource_lists);
      info.resources.insert(info.resources.end(),
                            std::make_move_iterator(parsed.begin()),
                            std::make_move_iterator(parsed.end()));
    }
  }

  if (const auto *entry_points = module->getNamedMetadata("dx.entryPoints")) {
    info.entry_points.reserve(entry_points->getNumOperands());
    for (const auto *entry : entry_points->operands()) {
      if (!entry || entry->getNumOperands() < 2)
        continue;

      DxilEntryPointInfo entry_info = {};
      entry_info.function_name = MetadataString(entry->getOperand(0).get());
      entry_info.name = MetadataString(entry->getOperand(1).get());
      if (entry->getNumOperands() > 2) {
        if (const auto *signature = llvm::dyn_cast_or_null<llvm::MDNode>(
                entry->getOperand(2).get())) {
          entry_info.has_signature = true;
          entry_info.signature_operand_count = signature->getNumOperands();
          if (signature->getNumOperands() > 0)
            entry_info.input_signature = ParseDxilMetadataSignatureList(
                MetadataNode(signature->getOperand(0).get()));
          if (signature->getNumOperands() > 1)
            entry_info.output_signature = ParseDxilMetadataSignatureList(
                MetadataNode(signature->getOperand(1).get()));
          if (signature->getNumOperands() > 2)
            entry_info.patch_constant_signature = ParseDxilMetadataSignatureList(
                MetadataNode(signature->getOperand(2).get()));
          info.signature_elements.insert(
              info.signature_elements.end(),
              entry_info.input_signature.begin(),
              entry_info.input_signature.end());
          info.signature_elements.insert(
              info.signature_elements.end(),
              entry_info.output_signature.begin(),
              entry_info.output_signature.end());
          info.signature_elements.insert(
              info.signature_elements.end(),
              entry_info.patch_constant_signature.begin(),
              entry_info.patch_constant_signature.end());
        }
      }
      if (entry->getNumOperands() > 3) {
        if (const auto *resources = llvm::dyn_cast_or_null<llvm::MDNode>(
                entry->getOperand(3).get())) {
          entry_info.has_resources = true;
          entry_info.resource_operand_count = resources->getNumOperands();
          entry_info.resources = ParseDxilMetadataResourceLists(resources);
        }
      }
      if (entry->getNumOperands() > 4) {
        if (const auto *properties = llvm::dyn_cast_or_null<llvm::MDNode>(
                entry->getOperand(4).get())) {
          entry_info.has_properties = true;
          entry_info.property_operand_count = properties->getNumOperands();
          entry_info.properties = MetadataUInt32List(properties);
          entry_info.property_tags = ParseMetadataTagValues(properties);
        }
      }
      info.entry_points.push_back(std::move(entry_info));
    }
  }

  if (const auto *module_flags = module->getModuleFlagsMetadata()) {
    info.module_flags.reserve(module_flags->getNumOperands());
    for (const auto *flag : module_flags->operands()) {
      if (!flag || flag->getNumOperands() < 3)
        continue;

      LlvmModuleFlagInfo flag_info = {};
      if (auto behavior = MetadataUInt32(flag->getOperand(0).get()))
        flag_info.behavior = *behavior;
      flag_info.key = MetadataString(flag->getOperand(1).get());
      flag_info.value = MetadataText(flag->getOperand(2).get());
      info.module_flags.push_back(std::move(flag_info));
    }
  }

  info.functions.reserve(module->size());
  for (const auto &function : module->functions()) {
    LlvmFunctionInfo function_info = {};
    function_info.name = function.getName().str();
    function_info.return_type = TypeString(function.getReturnType());
    function_info.return_type_info = ParseLlvmType(function.getReturnType());
    function_info.is_declaration = function.isDeclaration();
    function_info.is_dx_intrinsic = function.getName().startswith("dx.op.");
    function_info.argument_types.reserve(function.arg_size());
    function_info.argument_type_infos.reserve(function.arg_size());
    for (const auto &argument : function.args()) {
      function_info.argument_types.push_back(TypeString(argument.getType()));
      function_info.argument_type_infos.push_back(
          ParseLlvmType(argument.getType()));
    }

    if (!function.isDeclaration()) {
      for (const auto &block : function) {
        function_info.basic_blocks.push_back(ParseLlvmBasicBlockInfo(
            block, uint32_t(function_info.instructions.size())));
        function_info.instruction_count += uint32_t(block.size());
        function_info.instructions.reserve(
            function_info.instructions.size() + block.size());
        for (const auto &instruction : block) {
          auto instruction_info = ParseLlvmInstruction(instruction);
          if (instruction_info.is_dx_intrinsic_call &&
              instruction_info.dxil_opcode)
            function_info.dxil_operations.push_back(ParseDxilOperation(
                instruction_info, uint32_t(function_info.instructions.size())));
          function_info.instructions.push_back(std::move(instruction_info));
        }
      }
    }

    info.functions.push_back(std::move(function_info));
  }

  BuildLlvmCallGraph(info);

  info.globals.reserve(module->global_size());
  for (const auto &global : module->globals()) {
    info.globals.push_back({
        .name = global.getName().str(),
        .value_type = TypeString(global.getValueType()),
        .value_type_info = ParseLlvmType(global.getValueType()),
        .is_constant = global.isConstant(),
        .is_declaration = global.isDeclaration(),
    });
  }

  return ParseStatus::Ok;
#else
  (void)data;
  (void)info;
  return ParseStatus::InvalidLlvmModule;
#endif
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
ParseShaderHash(const BlobPart &part, ShaderHashInfo &info) {
  if (part.data.size() < kShaderHashSize)
    return ParseStatus::InvalidShaderHash;

  info = {};
  info.flags = ReadU32(part.data, 0);
  std::copy_n(part.data.data() + sizeof(uint32_t), info.digest.size(),
              info.digest.begin());
  return ParseStatus::Ok;
}

ParseStatus
ParseCompilerVersion(const BlobPart &part, CompilerVersionInfo &info) {
  const auto data = part.data;
  if (data.size() < kCompilerVersionHeaderSize)
    return ParseStatus::InvalidCompilerVersion;

  info = {};
  info.major = ReadU16(data, 0);
  info.minor = ReadU16(data, 2);
  info.version_flags = ReadU32(data, 4);
  info.commit_count = ReadU32(data, 8);
  info.string_list_size = ReadU32(data, 12);

  const auto aligned_size = (size_t(info.string_list_size) + 3u) & ~size_t(3u);
  size_t strings_end = 0;
  if (!CheckedEnd(kCompilerVersionHeaderSize, aligned_size, data.size(),
                  strings_end))
    return ParseStatus::InvalidCompilerVersion;

  const auto string_list = std::span<const uint8_t>(
      data.data() + kCompilerVersionHeaderSize, info.string_list_size);
  if (!ReadStringList(string_list, info.strings))
    return ParseStatus::InvalidCompilerVersion;

  if (!info.strings.empty())
    info.commit_sha = info.strings[0];
  if (info.strings.size() > 1)
    info.custom_version_string = info.strings[1];
  return ParseStatus::Ok;
}

ParseStatus
ParseShaderDebugName(const BlobPart &part, ShaderDebugNameInfo &info) {
  const auto data = part.data;
  if (data.size() < kShaderDebugNameHeaderSize)
    return ParseStatus::InvalidShaderDebugName;

  info = {};
  info.flags = ReadU16(data, 0);
  const auto name_length = ReadU16(data, 2);
  size_t name_end = 0;
  if (!CheckedEnd(kShaderDebugNameHeaderSize, size_t(name_length), data.size(),
                  name_end))
    return ParseStatus::InvalidShaderDebugName;
  if (name_end >= data.size() || data[name_end] != 0)
    return ParseStatus::InvalidShaderDebugName;

  info.name.assign(reinterpret_cast<const char *>(data.data() +
                                                 kShaderDebugNameHeaderSize),
                   name_length);
  return ParseStatus::Ok;
}

ParseStatus
ParseSourceInfo(const BlobPart &part, SourceInfo &info) {
  const auto data = part.data;
  if (data.size() < kSourceInfoHeaderSize)
    return ParseStatus::InvalidSourceInfo;

  info = {};
  info.aligned_size = ReadU32(data, 0);
  info.flags = ReadU16(data, 4);
  info.section_count = ReadU16(data, 6);
  if ((info.aligned_size & 3) || info.aligned_size < kSourceInfoHeaderSize ||
      info.aligned_size > data.size())
    return ParseStatus::InvalidSourceInfo;

  size_t offset = kSourceInfoHeaderSize;
  info.sections.reserve(info.section_count);
  for (uint16_t i = 0; i < info.section_count; i++) {
    size_t section_header_end = 0;
    if (!CheckedEnd(offset, kSourceInfoSectionHeaderSize, info.aligned_size,
                    section_header_end))
      return ParseStatus::InvalidSourceInfo;

    SourceInfoSection section = {};
    section.aligned_size = ReadU32(data, offset + 0);
    section.flags = ReadU16(data, offset + 4);
    section.type = ReadU16(data, offset + 6);
    if ((section.aligned_size & 3) ||
        section.aligned_size < kSourceInfoSectionHeaderSize)
      return ParseStatus::InvalidSourceInfo;

    size_t section_end = 0;
    if (!CheckedEnd(offset, section.aligned_size, info.aligned_size,
                    section_end))
      return ParseStatus::InvalidSourceInfo;

    section.data = std::span<const uint8_t>(
        data.data() + section_header_end, section_end - section_header_end);
    info.sections.push_back(section);
    offset = section_end;
  }

  return ParseStatus::Ok;
}

ParseStatus
ParseShaderPdbInfo(const BlobPart &part, ShaderPdbInfo &info) {
  const auto data = part.data;
  if (data.size() < kShaderPdbInfoHeaderSize)
    return ParseStatus::InvalidShaderPdbInfo;

  info = {};
  info.version = ReadU16(data, 0);
  info.compression_type = ReadU16(data, 2);
  info.size_in_bytes = ReadU32(data, 4);
  info.uncompressed_size_in_bytes = ReadU32(data, 8);

  size_t payload_end = 0;
  if (!CheckedEnd(kShaderPdbInfoHeaderSize, info.size_in_bytes, data.size(),
                  payload_end))
    return ParseStatus::InvalidShaderPdbInfo;
  info.payload = std::span<const uint8_t>(data.data() + kShaderPdbInfoHeaderSize,
                                          info.size_in_bytes);
  return ParseStatus::Ok;
}

ParseStatus
ParseShaderStatistics(const BlobPart &part, ShaderStatisticsInfo &info) {
  if (part.data.size() & 3)
    return ParseStatus::InvalidShaderStatistics;

  info = {};
  info.values.reserve(part.data.size() / sizeof(uint32_t));
  for (size_t offset = 0; offset < part.data.size(); offset += sizeof(uint32_t))
    info.values.push_back(ReadU32(part.data, offset));
  return ParseStatus::Ok;
}

ParseStatus
ParseResourceDef(const BlobPart &part, ResourceDefInfo &info) {
  const auto data = part.data;
  if (data.size() < kResourceDefHeaderSize)
    return ParseStatus::InvalidResourceDef;

  info = {};
  info.constant_buffer_count = ReadU32(data, 0);
  info.constant_buffer_offset = ReadU32(data, 4);
  info.bound_resource_count = ReadU32(data, 8);
  info.bound_resource_offset = ReadU32(data, 12);
  info.target = ReadU32(data, 16);
  info.flags = ReadU32(data, 20);

  const auto creator_offset = ReadU32(data, 24);
  if (creator_offset && !ReadString(data, creator_offset, info.creator))
    return ParseStatus::InvalidResourceDef;

  size_t cbuffers_end = 0;
  if (!CheckedEnd(info.constant_buffer_offset,
                  size_t(info.constant_buffer_count) *
                      kResourceDefConstantBufferSize,
                  data.size(), cbuffers_end))
    return ParseStatus::InvalidResourceDef;

  const auto shader_major = (info.target >> 4) & 0xf;
  const auto shader_minor = info.target & 0xf;
  const auto resource_stride =
      (shader_major > 5 || (shader_major == 5 && shader_minor >= 1))
          ? kResourceDefResourceBindingExtendedSize
          : kResourceDefResourceBindingSize;

  size_t resources_end = 0;
  if (!CheckedEnd(info.bound_resource_offset,
                  size_t(info.bound_resource_count) * resource_stride,
                  data.size(), resources_end))
    return ParseStatus::InvalidResourceDef;

  info.constant_buffers.reserve(info.constant_buffer_count);
  for (uint32_t i = 0; i < info.constant_buffer_count; i++) {
    const auto offset = info.constant_buffer_offset +
                        size_t(i) * kResourceDefConstantBufferSize;
    ConstantBufferInfo cbuffer = {};
    const auto name_offset = ReadU32(data, offset + 0);
    cbuffer.variable_count = ReadU32(data, offset + 4);
    cbuffer.variable_offset = ReadU32(data, offset + 8);
    cbuffer.size = ReadU32(data, offset + 12);
    cbuffer.flags = ReadU32(data, offset + 16);
    cbuffer.type = ReadU32(data, offset + 20);
    if (name_offset && !ReadString(data, name_offset, cbuffer.name))
      return ParseStatus::InvalidResourceDef;
    info.constant_buffers.push_back(std::move(cbuffer));
  }

  info.resources.reserve(info.bound_resource_count);
  for (uint32_t i = 0; i < info.bound_resource_count; i++) {
    const auto offset = info.bound_resource_offset + size_t(i) * resource_stride;
    ResourceBindingInfo resource = {};
    const auto name_offset = ReadU32(data, offset + 0);
    resource.type = ReadU32(data, offset + 4);
    resource.return_type = ReadU32(data, offset + 8);
    resource.dimension = ReadU32(data, offset + 12);
    resource.num_samples = ReadU32(data, offset + 16);
    resource.bind_point = ReadU32(data, offset + 20);
    resource.bind_count = ReadU32(data, offset + 24);
    resource.flags = ReadU32(data, offset + 28);
    if (resource_stride >= kResourceDefResourceBindingExtendedSize) {
      resource.space = ReadU32(data, offset + 32);
      resource.id = ReadU32(data, offset + 36);
    }
    if (name_offset && !ReadString(data, name_offset, resource.name))
      return ParseStatus::InvalidResourceDef;
    info.resources.push_back(std::move(resource));
  }

  return ParseStatus::Ok;
}

ParseStatus
ParseRuntimeDataResourceTable(RuntimeDataInfo &info) {
  const auto *table = info.findPart(rdat::ResourceTable);
  if (!table)
    return ParseStatus::Ok;
  if (!table->record_count) {
    info.resources.clear();
    return ParseStatus::Ok;
  }
  if (!table->is_table || table->record_stride < kRdatResourceRecordSize)
    return ParseStatus::InvalidRuntimeData;

  info.resources.clear();
  info.resources.reserve(table->record_count);
  for (uint32_t i = 0; i < table->record_count; i++) {
    const auto offset = size_t(i) * table->record_stride;
    const auto record = table->table_data.subspan(offset, kRdatResourceRecordSize);

    RdatResourceInfo resource = {};
    resource.resource_class = ReadU32(record, 0);
    resource.kind = ReadU32(record, 4);
    resource.id = ReadU32(record, 8);
    resource.space = ReadU32(record, 12);
    resource.lower_bound = ReadU32(record, 16);
    resource.upper_bound = ReadU32(record, 20);
    const auto name_offset = ReadU32(record, 24);
    resource.flags = ReadU32(record, 28);
    if (!ReadNullableRdatString(info, name_offset, resource.name))
      return ParseStatus::InvalidRuntimeData;
    info.resources.push_back(std::move(resource));
  }

  return ParseStatus::Ok;
}

ParseStatus
ParseRuntimeDataSubobjectTable(RuntimeDataInfo &info) {
  const auto *table = info.findPart(rdat::SubobjectTable);
  if (!table) {
    info.subobjects.clear();
    return ParseStatus::Ok;
  }
  if (!table->record_count) {
    info.subobjects.clear();
    return ParseStatus::Ok;
  }
  if (!table->is_table || table->record_stride < kRdatSubobjectRecordSize)
    return ParseStatus::InvalidRuntimeData;

  info.subobjects.clear();
  info.subobjects.reserve(table->record_count);
  for (uint32_t i = 0; i < table->record_count; i++) {
    const auto offset = size_t(i) * table->record_stride;
    const auto record = table->table_data.subspan(offset, kRdatSubobjectRecordSize);

    RdatSubobjectInfo subobject = {};
    subobject.kind = ReadU32(record, 0);
    if (!ReadNullableRdatString(info, ReadU32(record, 4), subobject.name))
      return ParseStatus::InvalidRuntimeData;

    const auto payload = record.subspan(8);
    switch (subobject.kind) {
    case 0: // StateObjectConfig
      subobject.state_object_flags = ReadU32(payload, 0);
      break;
    case 1: // GlobalRootSignature
    case 2: // LocalRootSignature
      if (!ReadNullableRdatBytes(info, ReadU32(payload, 0), ReadU32(payload, 4),
                                 subobject.root_signature))
        return ParseStatus::InvalidRuntimeData;
      break;
    case 8: // SubobjectToExportsAssociation
      if (!ReadNullableRdatString(info, ReadU32(payload, 0),
                                  subobject.associated_subobject) ||
          !ReadNullableRdatStringArray(info, ReadU32(payload, 4),
                                       subobject.associated_exports))
        return ParseStatus::InvalidRuntimeData;
      break;
    case 9: // RaytracingShaderConfig
      subobject.max_payload_size_in_bytes = ReadU32(payload, 0);
      subobject.max_attribute_size_in_bytes = ReadU32(payload, 4);
      break;
    case 10: // RaytracingPipelineConfig
      subobject.max_trace_recursion_depth = ReadU32(payload, 0);
      break;
    case 11: // HitGroup
      subobject.hit_group_type = ReadU32(payload, 0);
      if (!ReadNullableRdatString(info, ReadU32(payload, 4),
                                  subobject.any_hit) ||
          !ReadNullableRdatString(info, ReadU32(payload, 8),
                                  subobject.closest_hit) ||
          !ReadNullableRdatString(info, ReadU32(payload, 12),
                                  subobject.intersection))
        return ParseStatus::InvalidRuntimeData;
      break;
    case 12: // RaytracingPipelineConfig1
      subobject.max_trace_recursion_depth = ReadU32(payload, 0);
      subobject.raytracing_pipeline_flags = ReadU32(payload, 4);
      break;
    default:
      break;
    }

    info.subobjects.push_back(std::move(subobject));
  }

  return ParseStatus::Ok;
}

ParseStatus
ParseRuntimeDataNodeIdTable(RuntimeDataInfo &info) {
  const auto *table = info.findPart(rdat::NodeIDTable);
  if (!table) {
    info.node_ids.clear();
    return ParseStatus::Ok;
  }
  if (!table->record_count) {
    info.node_ids.clear();
    return ParseStatus::Ok;
  }
  if (!table->is_table || table->record_stride < kRdatNodeIdRecordSize)
    return ParseStatus::InvalidRuntimeData;

  info.node_ids.clear();
  info.node_ids.reserve(table->record_count);
  for (uint32_t i = 0; i < table->record_count; i++) {
    const auto offset = size_t(i) * table->record_stride;
    const auto record = table->table_data.subspan(offset, kRdatNodeIdRecordSize);

    RdatNodeIdInfo node_id = {};
    if (!ReadNullableRdatString(info, ReadU32(record, 0), node_id.name))
      return ParseStatus::InvalidRuntimeData;
    node_id.index = ReadU32(record, 4);
    info.node_ids.push_back(std::move(node_id));
  }

  return ParseStatus::Ok;
}

ParseStatus
ParseRuntimeDataNodeShaderFuncAttribTable(RuntimeDataInfo &info) {
  const auto *table = info.findPart(rdat::NodeShaderFuncAttribTable);
  if (!table) {
    info.node_function_attributes.clear();
    return ParseStatus::Ok;
  }
  if (!table->record_count) {
    info.node_function_attributes.clear();
    return ParseStatus::Ok;
  }
  if (!table->is_table ||
      table->record_stride < kRdatNodeShaderFuncAttribRecordSize)
    return ParseStatus::InvalidRuntimeData;

  info.node_function_attributes.clear();
  info.node_function_attributes.reserve(table->record_count);
  for (uint32_t i = 0; i < table->record_count; i++) {
    const auto offset = size_t(i) * table->record_stride;
    const auto record = table->table_data.subspan(
        offset, kRdatNodeShaderFuncAttribRecordSize);

    RdatNodeShaderFuncAttribInfo attribute = {};
    attribute.kind = ReadU32(record, 0);
    const auto payload = ReadU32(record, 4);
    switch (attribute.kind) {
    case 1: // ID
    case 3: // ShareInputOf
      if (payload >= info.node_ids.size())
        return ParseStatus::InvalidRuntimeData;
      attribute.node_id_index = payload;
      break;
    case 2: // NumThreads
    case 4: // DispatchGrid
    case 7: // MaxDispatchGrid
      if (!ReadNullableRdatIndexArray(info, payload, attribute.values))
        return ParseStatus::InvalidRuntimeData;
      break;
    case 5: // MaxRecursionDepth
    case 6: // LocalRootArgumentsTableIndex
    case 8: // Reserved_MeshNodePreview1
    case 9: // Reserved_MeshNodePreview2
      attribute.value = payload;
      break;
    default:
      break;
    }
    info.node_function_attributes.push_back(std::move(attribute));
  }

  return ParseStatus::Ok;
}

ParseStatus
ParseRuntimeDataNodeShaderIoAttribTable(RuntimeDataInfo &info) {
  const auto *table = info.findPart(rdat::NodeShaderIOAttribTable);
  if (!table) {
    info.node_io_attributes.clear();
    return ParseStatus::Ok;
  }
  if (!table->record_count) {
    info.node_io_attributes.clear();
    return ParseStatus::Ok;
  }
  if (!table->is_table ||
      table->record_stride < kRdatNodeShaderIoAttribRecordSize)
    return ParseStatus::InvalidRuntimeData;

  info.node_io_attributes.clear();
  info.node_io_attributes.reserve(table->record_count);
  for (uint32_t i = 0; i < table->record_count; i++) {
    const auto offset = size_t(i) * table->record_stride;
    const auto record = table->table_data.subspan(
        offset, kRdatNodeShaderIoAttribRecordSize);

    RdatNodeShaderIoAttribInfo attribute = {};
    attribute.kind = ReadU32(record, 0);
    switch (attribute.kind) {
    case 1: { // OutputID
      const auto node_id_index = ReadU32(record, 4);
      if (node_id_index >= info.node_ids.size())
        return ParseStatus::InvalidRuntimeData;
      attribute.node_id_index = node_id_index;
      break;
    }
    case 5: { // RecordDispatchGrid
      const auto component = ReadU16(record, 6);
      attribute.record_dispatch_grid.byte_offset = ReadU16(record, 4);
      attribute.record_dispatch_grid.component_count = component & 0x3u;
      attribute.record_dispatch_grid.component_type = component >> 2;
      break;
    }
    case 2: // MaxRecords
    case 3: // MaxRecordsSharedWith
    case 4: // RecordSizeInBytes
    case 6: // OutputArraySize
    case 7: // AllowSparseNodes
    case 8: // RecordAlignmentInBytes
      attribute.value = ReadU32(record, 4);
      break;
    default:
      break;
    }
    info.node_io_attributes.push_back(std::move(attribute));
  }

  return ParseStatus::Ok;
}

ParseStatus
ParseRuntimeDataIoNodeTable(RuntimeDataInfo &info) {
  const auto *table = info.findPart(rdat::IONodeTable);
  if (!table) {
    info.io_nodes.clear();
    return ParseStatus::Ok;
  }
  if (!table->record_count) {
    info.io_nodes.clear();
    return ParseStatus::Ok;
  }
  if (!table->is_table || table->record_stride < kRdatIoNodeRecordSize)
    return ParseStatus::InvalidRuntimeData;

  info.io_nodes.clear();
  info.io_nodes.reserve(table->record_count);
  for (uint32_t i = 0; i < table->record_count; i++) {
    const auto offset = size_t(i) * table->record_stride;
    const auto record = table->table_data.subspan(offset, kRdatIoNodeRecordSize);

    RdatIoNodeInfo node = {};
    node.io_flags_and_kind = ReadU32(record, 0);
    if (!ReadNullableRdatRecordArray(info, ReadU32(record, 4),
                                     info.node_io_attributes.size(),
                                     node.attribute_indices))
      return ParseStatus::InvalidRuntimeData;
    info.io_nodes.push_back(std::move(node));
  }

  return ParseStatus::Ok;
}

ParseStatus
ParseRuntimeDataNodeShaderInfoTable(RuntimeDataInfo &info) {
  const auto *table = info.findPart(rdat::NodeShaderInfoTable);
  if (!table) {
    info.node_shader_infos.clear();
    return ParseStatus::Ok;
  }
  if (!table->record_count) {
    info.node_shader_infos.clear();
    return ParseStatus::Ok;
  }
  if (!table->is_table || table->record_stride < kRdatNodeShaderInfoRecordSize)
    return ParseStatus::InvalidRuntimeData;

  info.node_shader_infos.clear();
  info.node_shader_infos.reserve(table->record_count);
  for (uint32_t i = 0; i < table->record_count; i++) {
    const auto offset = size_t(i) * table->record_stride;
    const auto record = table->table_data.subspan(offset, kRdatNodeShaderInfoRecordSize);

    RdatNodeShaderInfo shader = {};
    shader.launch_type = ReadU32(record, 0);
    shader.group_shared_bytes_used = ReadU32(record, 4);
    if (!ReadNullableRdatRecordArray(info, ReadU32(record, 8),
                                     info.node_function_attributes.size(),
                                     shader.attribute_indices) ||
        !ReadNullableRdatRecordArray(info, ReadU32(record, 12),
                                     info.io_nodes.size(),
                                     shader.output_indices) ||
        !ReadNullableRdatRecordArray(info, ReadU32(record, 16),
                                     info.io_nodes.size(),
                                     shader.input_indices))
      return ParseStatus::InvalidRuntimeData;
    info.node_shader_infos.push_back(std::move(shader));
  }

  return ParseStatus::Ok;
}

ParseStatus
ParseRuntimeDataFunctionTable(RuntimeDataInfo &info) {
  const auto *table = info.findPart(rdat::FunctionTable);
  if (!table)
    return ParseStatus::Ok;
  if (!table->record_count) {
    info.functions.clear();
    return ParseStatus::Ok;
  }
  if (!table->is_table || table->record_stride < kRdatFunctionRecordSize)
    return ParseStatus::InvalidRuntimeData;

  info.functions.clear();
  info.functions.reserve(table->record_count);
  for (uint32_t i = 0; i < table->record_count; i++) {
    const auto offset = size_t(i) * table->record_stride;
    const auto record = table->table_data.subspan(offset, kRdatFunctionRecordSize);

    RdatFunctionInfo function = {};
    const auto name_offset = ReadU32(record, 0);
    const auto unmangled_name_offset = ReadU32(record, 4);
    const auto resources_offset = ReadU32(record, 8);
    const auto dependencies_offset = ReadU32(record, 12);
    function.shader_kind = ReadU32(record, 16);
    function.payload_size_in_bytes = ReadU32(record, 20);
    function.attribute_size_in_bytes = ReadU32(record, 24);
    function.feature_info1 = ReadU32(record, 28);
    function.feature_info2 = ReadU32(record, 32);
    function.shader_stage_flag = ReadU32(record, 36);
    function.min_shader_target = ReadU32(record, 40);
    if (table->record_stride >= kRdatFunctionRecord2Size) {
      const auto record2 = table->table_data.subspan(offset, kRdatFunctionRecord2Size);
      function.minimum_expected_wave_lane_count = record2[44];
      function.maximum_expected_wave_lane_count = record2[45];
      function.shader_flags = ReadU16(record2, 46);
      function.shader_info_index = ReadU32(record2, 48);
      switch (function.shader_kind) {
      case 0:
        function.shader_info_table_type = rdat::PSInfoTable;
        function.has_shader_info = function.shader_info_index != kRdatNullRef;
        break;
      case 1:
        function.shader_info_table_type = rdat::VSInfoTable;
        function.has_shader_info = function.shader_info_index != kRdatNullRef;
        break;
      case 2:
        function.shader_info_table_type = rdat::GSInfoTable;
        function.has_shader_info = function.shader_info_index != kRdatNullRef;
        break;
      case 3:
        function.shader_info_table_type = rdat::HSInfoTable;
        function.has_shader_info = function.shader_info_index != kRdatNullRef;
        break;
      case 4:
        function.shader_info_table_type = rdat::DSInfoTable;
        function.has_shader_info = function.shader_info_index != kRdatNullRef;
        break;
      case 5:
        function.shader_info_table_type = rdat::CSInfoTable;
        function.has_shader_info = function.shader_info_index != kRdatNullRef;
        break;
      case 13:
        function.shader_info_table_type = rdat::MSInfoTable;
        function.has_shader_info = function.shader_info_index != kRdatNullRef;
        break;
      case 14:
        function.shader_info_table_type = rdat::ASInfoTable;
        function.has_shader_info = function.shader_info_index != kRdatNullRef;
        break;
      case 15:
        function.shader_info_table_type = rdat::NodeShaderInfoTable;
        function.has_shader_info = function.shader_info_index != kRdatNullRef;
        break;
      default:
        break;
      }
    }

    if (!ReadNullableRdatString(info, name_offset, function.name) ||
        !ReadNullableRdatString(info, unmangled_name_offset,
                                function.unmangled_name) ||
        !ReadNullableRdatIndexArray(info, resources_offset,
                                    function.resource_indices))
      return ParseStatus::InvalidRuntimeData;

    for (const auto resource_index : function.resource_indices) {
      if (resource_index >= info.resources.size())
        return ParseStatus::InvalidRuntimeData;
    }

    std::vector<uint32_t> dependency_offsets;
    if (!ReadNullableRdatIndexArray(info, dependencies_offset, dependency_offsets))
      return ParseStatus::InvalidRuntimeData;

    function.function_dependencies.reserve(dependency_offsets.size());
    for (const auto dependency_offset : dependency_offsets) {
      std::string dependency;
      if (!ReadNullableRdatString(info, dependency_offset, dependency))
        return ParseStatus::InvalidRuntimeData;
      function.function_dependencies.push_back(std::move(dependency));
    }

    if (function.has_shader_info) {
      if (function.shader_info_table_type == rdat::NodeShaderInfoTable) {
        if (function.shader_info_index >= info.node_shader_infos.size())
          return ParseStatus::InvalidRuntimeData;
      } else if (!info.findShaderInfo(function.shader_info_table_type,
                                      function.shader_info_index)) {
        return ParseStatus::InvalidRuntimeData;
      }
    }

    info.functions.push_back(std::move(function));
  }

  return ParseStatus::Ok;
}

ParseStatus
ParseRuntimeDataSignatureElementTable(RuntimeDataInfo &info) {
  const auto *table = info.findPart(rdat::SignatureElementTable);
  if (!table)
    return ParseStatus::Ok;
  if (!table->record_count) {
    info.signature_elements.clear();
    return ParseStatus::Ok;
  }
  if (!table->is_table ||
      table->record_stride < kRdatSignatureElementRecordPayloadSize)
    return ParseStatus::InvalidRuntimeData;

  info.signature_elements.clear();
  info.signature_elements.reserve(table->record_count);
  for (uint32_t i = 0; i < table->record_count; i++) {
    const auto offset = size_t(i) * table->record_stride;
    const auto record = table->table_data.subspan(
        offset, kRdatSignatureElementRecordPayloadSize);

    RdatSignatureElementInfo element = {};
    const auto semantic_name_offset = ReadU32(record, 0);
    const auto semantic_indices_offset = ReadU32(record, 4);
    element.semantic_kind = record[8];
    element.component_type = record[9];
    element.interpolation_mode = record[10];
    element.start_row = record[11];
    const auto cols_and_stream = record[12];
    const auto usage_and_dyn_index_masks = record[13];
    element.cols = (cols_and_stream & 3u) + 1u;
    element.start_col = (cols_and_stream >> 2) & 3u;
    element.output_stream = (cols_and_stream >> 4) & 3u;
    element.usage_mask = usage_and_dyn_index_masks & 0xfu;
    element.dynamic_index_mask = (usage_and_dyn_index_masks >> 4) & 0xfu;

    if (!ReadNullableRdatString(info, semantic_name_offset,
                                element.semantic_name) ||
        !ReadNullableRdatIndexArray(info, semantic_indices_offset,
                                    element.semantic_indices))
      return ParseStatus::InvalidRuntimeData;

    info.signature_elements.push_back(std::move(element));
  }

  return ParseStatus::Ok;
}

bool
ReadRdatSignatureArray(const RuntimeDataInfo &info, uint32_t offset,
                       std::vector<uint32_t> &out) {
  return ReadNullableRdatRecordArray(info, offset, info.signature_elements.size(), out);
}

ParseStatus
ParseRuntimeDataShaderInfoTable(RuntimeDataInfo &info, uint32_t table_type,
                                size_t minimum_record_size) {
  const auto *table = info.findPart(table_type);
  if (!table || !table->record_count)
    return ParseStatus::Ok;
  if (!table->is_table || table->record_stride < minimum_record_size)
    return ParseStatus::InvalidRuntimeData;

  for (uint32_t i = 0; i < table->record_count; i++) {
    const auto offset = size_t(i) * table->record_stride;
    const auto record = table->table_data.subspan(offset, minimum_record_size);

    RdatShaderInfo shader = {};
    shader.table_type = table_type;
    shader.record_index = i;

    switch (table_type) {
    case rdat::VSInfoTable:
      if (!ReadRdatSignatureArray(info, ReadU32(record, 0),
                                  shader.input_signature_indices) ||
          !ReadRdatSignatureArray(info, ReadU32(record, 4),
                                  shader.output_signature_indices) ||
          !ReadNullableRdatBytes(info, ReadU32(record, 8), ReadU32(record, 12),
                                 shader.view_id_output_mask))
        return ParseStatus::InvalidRuntimeData;
      break;

    case rdat::PSInfoTable:
      if (!ReadRdatSignatureArray(info, ReadU32(record, 0),
                                  shader.input_signature_indices) ||
          !ReadRdatSignatureArray(info, ReadU32(record, 4),
                                  shader.output_signature_indices))
        return ParseStatus::InvalidRuntimeData;
      break;

    case rdat::HSInfoTable:
      if (!ReadRdatSignatureArray(info, ReadU32(record, 0),
                                  shader.input_signature_indices) ||
          !ReadRdatSignatureArray(info, ReadU32(record, 4),
                                  shader.output_signature_indices) ||
          !ReadRdatSignatureArray(info, ReadU32(record, 8),
                                  shader.patch_constant_signature_indices) ||
          !ReadNullableRdatBytes(info, ReadU32(record, 12), ReadU32(record, 16),
                                 shader.view_id_output_mask) ||
          !ReadNullableRdatBytes(info, ReadU32(record, 20), ReadU32(record, 24),
                                 shader.view_id_patch_constant_output_mask) ||
          !ReadNullableRdatBytes(info, ReadU32(record, 28), ReadU32(record, 32),
                                 shader.input_to_output_masks) ||
          !ReadNullableRdatBytes(info, ReadU32(record, 36), ReadU32(record, 40),
                                 shader.input_to_patch_constant_output_masks))
        return ParseStatus::InvalidRuntimeData;
      shader.input_control_point_count = record[44];
      shader.output_control_point_count = record[45];
      shader.tessellator_domain = record[46];
      shader.tessellator_output_primitive = record[47];
      break;

    case rdat::DSInfoTable:
      if (!ReadRdatSignatureArray(info, ReadU32(record, 0),
                                  shader.input_signature_indices) ||
          !ReadRdatSignatureArray(info, ReadU32(record, 4),
                                  shader.output_signature_indices) ||
          !ReadRdatSignatureArray(info, ReadU32(record, 8),
                                  shader.patch_constant_signature_indices) ||
          !ReadNullableRdatBytes(info, ReadU32(record, 12), ReadU32(record, 16),
                                 shader.view_id_output_mask) ||
          !ReadNullableRdatBytes(info, ReadU32(record, 20), ReadU32(record, 24),
                                 shader.input_to_output_masks) ||
          !ReadNullableRdatBytes(info, ReadU32(record, 28), ReadU32(record, 32),
                                 shader.patch_constant_input_to_output_masks))
        return ParseStatus::InvalidRuntimeData;
      shader.input_control_point_count = record[36];
      shader.tessellator_domain = record[37];
      break;

    case rdat::GSInfoTable:
      if (!ReadRdatSignatureArray(info, ReadU32(record, 0),
                                  shader.input_signature_indices) ||
          !ReadRdatSignatureArray(info, ReadU32(record, 4),
                                  shader.output_signature_indices) ||
          !ReadNullableRdatBytes(info, ReadU32(record, 8), ReadU32(record, 12),
                                 shader.view_id_output_mask) ||
          !ReadNullableRdatBytes(info, ReadU32(record, 16), ReadU32(record, 20),
                                 shader.input_to_output_masks))
        return ParseStatus::InvalidRuntimeData;
      shader.input_primitive = record[24];
      shader.output_topology = record[25];
      shader.max_vertex_count = record[26];
      shader.output_stream_mask = record[27];
      break;

    case rdat::CSInfoTable:
      if (!ReadRdatNumThreads(info, ReadU32(record, 0), shader.num_threads_x,
                              shader.num_threads_y, shader.num_threads_z))
        return ParseStatus::InvalidRuntimeData;
      shader.group_shared_bytes_used = ReadU32(record, 4);
      break;

    case rdat::MSInfoTable:
      if (!ReadRdatSignatureArray(info, ReadU32(record, 0),
                                  shader.output_signature_indices) ||
          !ReadRdatSignatureArray(info, ReadU32(record, 4),
                                  shader.primitive_signature_indices) ||
          !ReadNullableRdatBytes(info, ReadU32(record, 8), ReadU32(record, 12),
                                 shader.view_id_output_mask) ||
          !ReadNullableRdatBytes(info, ReadU32(record, 16), ReadU32(record, 20),
                                 shader.view_id_primitive_output_mask) ||
          !ReadRdatNumThreads(info, ReadU32(record, 24), shader.num_threads_x,
                              shader.num_threads_y, shader.num_threads_z))
        return ParseStatus::InvalidRuntimeData;
      shader.group_shared_bytes_used = ReadU32(record, 28);
      shader.group_shared_bytes_dependent_on_view_id = ReadU32(record, 32);
      shader.payload_size_in_bytes = ReadU32(record, 36);
      shader.max_output_vertices = ReadU16(record, 40);
      shader.max_output_primitives = ReadU16(record, 42);
      shader.mesh_output_topology = record[44];
      break;

    case rdat::ASInfoTable:
      if (!ReadRdatNumThreads(info, ReadU32(record, 0), shader.num_threads_x,
                              shader.num_threads_y, shader.num_threads_z))
        return ParseStatus::InvalidRuntimeData;
      shader.group_shared_bytes_used = ReadU32(record, 4);
      shader.payload_size_in_bytes = ReadU32(record, 8);
      break;

    default:
      return ParseStatus::InvalidRuntimeData;
    }

    info.shader_infos.push_back(std::move(shader));
  }

  return ParseStatus::Ok;
}

ParseStatus
ParseRuntimeDataShaderInfoTables(RuntimeDataInfo &info) {
  info.shader_infos.clear();

  const std::pair<uint32_t, size_t> tables[] = {
      {rdat::VSInfoTable, kRdatVSInfoRecordSize},
      {rdat::PSInfoTable, kRdatPSInfoRecordSize},
      {rdat::HSInfoTable, kRdatHSInfoRecordSize},
      {rdat::DSInfoTable, kRdatDSInfoRecordSize},
      {rdat::GSInfoTable, kRdatGSInfoRecordSize},
      {rdat::CSInfoTable, kRdatCSInfoRecordSize},
      {rdat::MSInfoTable, kRdatMSInfoRecordSize},
      {rdat::ASInfoTable, kRdatASInfoRecordSize},
  };

  for (const auto &[table_type, record_size] : tables) {
    auto status = ParseRuntimeDataShaderInfoTable(info, table_type, record_size);
    if (status != ParseStatus::Ok)
      return status;
  }

  return ParseStatus::Ok;
}

ParseStatus
ParseRuntimeDataPdbSourceTable(RuntimeDataInfo &info) {
  const auto *table = info.findPart(rdat::DxilPdbInfoSourceTable);
  if (!table) {
    info.pdb_sources.clear();
    return ParseStatus::Ok;
  }
  if (!table->record_count) {
    info.pdb_sources.clear();
    return ParseStatus::Ok;
  }
  if (!table->is_table || table->record_stride < kRdatPdbInfoSourceRecordSize)
    return ParseStatus::InvalidRuntimeData;

  info.pdb_sources.clear();
  info.pdb_sources.reserve(table->record_count);
  for (uint32_t i = 0; i < table->record_count; i++) {
    const auto offset = size_t(i) * table->record_stride;
    const auto record = table->table_data.subspan(offset, kRdatPdbInfoSourceRecordSize);

    RdatPdbInfoSource source = {};
    if (!ReadNullableRdatString(info, ReadU32(record, 0), source.name) ||
        !ReadNullableRdatString(info, ReadU32(record, 4), source.content))
      return ParseStatus::InvalidRuntimeData;
    info.pdb_sources.push_back(std::move(source));
  }

  return ParseStatus::Ok;
}

ParseStatus
ParseRuntimeDataPdbLibraryTable(RuntimeDataInfo &info) {
  const auto *table = info.findPart(rdat::DxilPdbInfoLibraryTable);
  if (!table) {
    info.pdb_libraries.clear();
    return ParseStatus::Ok;
  }
  if (!table->record_count) {
    info.pdb_libraries.clear();
    return ParseStatus::Ok;
  }
  if (!table->is_table || table->record_stride < kRdatPdbInfoLibraryRecordSize)
    return ParseStatus::InvalidRuntimeData;

  info.pdb_libraries.clear();
  info.pdb_libraries.reserve(table->record_count);
  for (uint32_t i = 0; i < table->record_count; i++) {
    const auto offset = size_t(i) * table->record_stride;
    const auto record = table->table_data.subspan(offset, kRdatPdbInfoLibraryRecordSize);

    RdatPdbInfoLibrary library = {};
    if (!ReadNullableRdatString(info, ReadU32(record, 0), library.name) ||
        !ReadNullableRdatBytes(info, ReadU32(record, 4), ReadU32(record, 8),
                               library.data))
      return ParseStatus::InvalidRuntimeData;
    info.pdb_libraries.push_back(std::move(library));
  }

  return ParseStatus::Ok;
}

ParseStatus
ParseRuntimeDataPdbInfoTable(RuntimeDataInfo &info) {
  const auto *table = info.findPart(rdat::DxilPdbInfoTable);
  if (!table) {
    info.pdb_infos.clear();
    return ParseStatus::Ok;
  }
  if (!table->record_count) {
    info.pdb_infos.clear();
    return ParseStatus::Ok;
  }
  if (!table->is_table || table->record_stride < kRdatPdbInfoRecordSize)
    return ParseStatus::InvalidRuntimeData;

  info.pdb_infos.clear();
  info.pdb_infos.reserve(table->record_count);
  for (uint32_t i = 0; i < table->record_count; i++) {
    const auto offset = size_t(i) * table->record_stride;
    const auto record = table->table_data.subspan(offset, kRdatPdbInfoRecordSize);

    RdatPdbInfo pdb = {};
    if (!ReadNullableRdatRecordArray(info, ReadU32(record, 0),
                                     info.pdb_sources.size(),
                                     pdb.source_indices) ||
        !ReadNullableRdatRecordArray(info, ReadU32(record, 4),
                                     info.pdb_libraries.size(),
                                     pdb.library_indices) ||
        !ReadNullableRdatStringArray(info, ReadU32(record, 8),
                                     pdb.arg_pairs) ||
        !ReadNullableRdatBytes(info, ReadU32(record, 12), ReadU32(record, 16),
                               pdb.hash) ||
        !ReadNullableRdatString(info, ReadU32(record, 20), pdb.pdb_name) ||
        !ReadNullableRdatBytes(info, ReadU32(record, 28), ReadU32(record, 32),
                               pdb.custom_toolchain_data) ||
        !ReadNullableRdatBytes(info, ReadU32(record, 36), ReadU32(record, 40),
                               pdb.whole_dxil))
      return ParseStatus::InvalidRuntimeData;
    pdb.custom_toolchain_id = ReadU32(record, 24);
    info.pdb_infos.push_back(std::move(pdb));
  }

  return ParseStatus::Ok;
}

ParseStatus
ParseRuntimeDataPdbInfoTables(RuntimeDataInfo &info) {
  auto status = ParseRuntimeDataPdbSourceTable(info);
  if (status != ParseStatus::Ok)
    return status;

  status = ParseRuntimeDataPdbLibraryTable(info);
  if (status != ParseStatus::Ok)
    return status;

  return ParseRuntimeDataPdbInfoTable(info);
}

ParseStatus
ParseRuntimeDataCoreTables(RuntimeDataInfo &info) {
  auto status = ParseRuntimeDataResourceTable(info);
  if (status != ParseStatus::Ok)
    return status;

  status = ParseRuntimeDataSubobjectTable(info);
  if (status != ParseStatus::Ok)
    return status;

  status = ParseRuntimeDataNodeIdTable(info);
  if (status != ParseStatus::Ok)
    return status;

  status = ParseRuntimeDataNodeShaderFuncAttribTable(info);
  if (status != ParseStatus::Ok)
    return status;

  status = ParseRuntimeDataNodeShaderIoAttribTable(info);
  if (status != ParseStatus::Ok)
    return status;

  status = ParseRuntimeDataIoNodeTable(info);
  if (status != ParseStatus::Ok)
    return status;

  status = ParseRuntimeDataNodeShaderInfoTable(info);
  if (status != ParseStatus::Ok)
    return status;

  status = ParseRuntimeDataSignatureElementTable(info);
  if (status != ParseStatus::Ok)
    return status;

  status = ParseRuntimeDataShaderInfoTables(info);
  if (status != ParseStatus::Ok)
    return status;

  status = ParseRuntimeDataFunctionTable(info);
  if (status != ParseStatus::Ok)
    return status;

  return ParseRuntimeDataPdbInfoTables(info);
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
    if (part_info.type == rdat::IndexArrays && (part_info.size & 3))
      return ParseStatus::InvalidRuntimeData;
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

  return ParseRuntimeDataCoreTables(info);
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

  const auto dependency_offset = offset;
  if (info.has_runtime_info_1) {
    if (info.uses_view_id) {
      for (uint32_t i = 0; i < info.view_id_output_masks.size(); i++) {
        if (!info.output_vectors[i])
          continue;
        if (!ReadPsvComponentMask(data, offset, info.output_vectors[i],
                                  info.view_id_output_masks[i]))
          return ParseStatus::InvalidPipelineStateValidation;
      }

      if ((info.shader_stage == 3 || info.shader_stage == 13) &&
          info.patch_constant_or_primitive_vectors) {
        if (!ReadPsvComponentMask(
                data, offset, info.patch_constant_or_primitive_vectors,
                info.view_id_patch_constant_or_primitive_output_mask))
          return ParseStatus::InvalidPipelineStateValidation;
      }
    }

    for (uint32_t i = 0; i < info.input_to_output_tables.size(); i++) {
      if (info.shader_stage == 13 || !info.output_vectors[i] ||
          !info.input_vectors)
        continue;
      if (!ReadPsvDependencyTable(data, offset, info.input_vectors,
                                  info.output_vectors[i],
                                  info.input_to_output_tables[i]))
        return ParseStatus::InvalidPipelineStateValidation;
    }

    if (info.shader_stage == 3 && info.patch_constant_or_primitive_vectors &&
        info.input_vectors) {
      if (!ReadPsvDependencyTable(data, offset, info.input_vectors,
                                  info.patch_constant_or_primitive_vectors,
                                  info.input_to_patch_constant_output_table))
        return ParseStatus::InvalidPipelineStateValidation;
    }

    if (info.shader_stage == 4 && info.output_vectors[0] &&
        info.patch_constant_or_primitive_vectors) {
      if (!ReadPsvDependencyTable(data, offset,
                                  info.patch_constant_or_primitive_vectors,
                                  info.output_vectors[0],
                                  info.patch_constant_input_to_output_table))
        return ParseStatus::InvalidPipelineStateValidation;
    }
  }

  info.dependency_payload = std::span<const uint8_t>(
      data.data() + dependency_offset, data.size() - dependency_offset);
  return ParseStatus::Ok;
}

namespace {

std::string_view
CanonicalShaderName(std::string_view name) {
  if (!name.empty() && name.front() == '\1')
    name.remove_prefix(1);
  return name;
}

bool
ShaderNamesEqual(std::string_view lhs, std::string_view rhs) {
  if (lhs.empty() || rhs.empty())
    return false;
  return lhs == rhs ||
         CanonicalShaderName(lhs) == CanonicalShaderName(rhs);
}

uint32_t
ShaderBindingCount(uint32_t lower_bound, uint32_t upper_bound) {
  if (upper_bound < lower_bound)
    return 0;
  if (upper_bound == std::numeric_limits<uint32_t>::max())
    return std::numeric_limits<uint32_t>::max();
  return upper_bound - lower_bound + 1;
}

const DxilEntryPointInfo *
FindReflectionEntryPoint(const LlvmModuleInfo &module) {
  if (module.entry_points.empty())
    return nullptr;
  auto entry = std::find_if(
      module.entry_points.begin(), module.entry_points.end(),
      [](const DxilEntryPointInfo &info) {
        return !info.name.empty() || !info.function_name.empty();
      });
  return entry != module.entry_points.end() ? &*entry : &module.entry_points[0];
}

const LlvmFunctionInfo *
FindReflectionLlvmFunction(const LlvmModuleInfo &module,
                           std::string_view entry_point_name,
                           std::string_view function_name) {
  auto match = [&](const LlvmFunctionInfo &function) {
    return ShaderNamesEqual(function.name, function_name) ||
           ShaderNamesEqual(function.name, entry_point_name);
  };

  auto function = std::find_if(module.functions.begin(), module.functions.end(),
                               match);
  if (function != module.functions.end())
    return &*function;

  const LlvmFunctionInfo *candidate = nullptr;
  for (const auto &function_info : module.functions) {
    if (function_info.is_declaration || function_info.is_dx_intrinsic)
      continue;
    if (candidate)
      return nullptr;
    candidate = &function_info;
  }
  return candidate;
}

const RdatFunctionInfo *
FindReflectionRdatFunction(const RuntimeDataInfo &runtime_data,
                           std::string_view entry_point_name,
                           std::string_view function_name) {
  if (runtime_data.functions.empty())
    return nullptr;
  if (runtime_data.functions.size() == 1)
    return &runtime_data.functions[0];

  const RdatFunctionInfo *best = nullptr;
  uint32_t best_score = 0;
  for (const auto &function : runtime_data.functions) {
    uint32_t score = 0;
    if (ShaderNamesEqual(function.name, function_name))
      score = std::max(score, 4u);
    if (ShaderNamesEqual(function.unmangled_name, function_name))
      score = std::max(score, 4u);
    if (ShaderNamesEqual(function.name, entry_point_name))
      score = std::max(score, 3u);
    if (ShaderNamesEqual(function.unmangled_name, entry_point_name))
      score = std::max(score, 3u);
    if (score > best_score) {
      best = &function;
      best_score = score;
    }
  }
  return best;
}

void
AppendReflectionRdatResource(ShaderReflectionInfo &info,
                             const RdatResourceInfo &resource) {
  ShaderReflectionResourceInfo reflected = {};
  reflected.name = resource.name;
  reflected.resource_class = resource.resource_class;
  reflected.resource_kind = resource.kind;
  reflected.id = resource.id;
  reflected.space = resource.space;
  reflected.lower_bound = resource.lower_bound;
  reflected.upper_bound = resource.upper_bound;
  reflected.bind_point = resource.lower_bound;
  reflected.bind_count =
      ShaderBindingCount(resource.lower_bound, resource.upper_bound);
  reflected.flags = resource.flags;
  reflected.from_runtime_data = true;
  info.resources.push_back(std::move(reflected));
}

void
AppendReflectionResourceDef(ShaderReflectionInfo &info,
                            const ResourceBindingInfo &resource) {
  ShaderReflectionResourceInfo reflected = {};
  reflected.name = resource.name;
  reflected.resource_type = resource.type;
  reflected.return_type = resource.return_type;
  reflected.dimension = resource.dimension;
  reflected.num_samples = resource.num_samples;
  reflected.id = resource.id;
  reflected.space = resource.space;
  reflected.bind_point = resource.bind_point;
  reflected.bind_count = resource.bind_count;
  reflected.lower_bound = resource.bind_point;
  if (resource.bind_count == std::numeric_limits<uint32_t>::max())
    reflected.upper_bound = std::numeric_limits<uint32_t>::max();
  else if (resource.bind_count)
    reflected.upper_bound = resource.bind_point + resource.bind_count - 1;
  else
    reflected.upper_bound = resource.bind_point;
  reflected.flags = resource.flags;
  reflected.from_resource_def = true;
  info.resources.push_back(std::move(reflected));
}

uint32_t
DxilMetadataResourceElementStride(const DxilMetadataResourceInfo &resource) {
  for (const auto &tag : resource.tags) {
    if (tag.tag == 1 && tag.has_uint_value)
      return uint32_t(std::min<uint64_t>(
          tag.uint_value, std::numeric_limits<uint32_t>::max()));
  }
  return 0;
}

void
AppendReflectionMetadataResource(ShaderReflectionInfo &info,
                                 const DxilMetadataResourceInfo &resource) {
  ShaderReflectionResourceInfo reflected = {};
  reflected.name = resource.name;
  reflected.resource_class = uint32_t(resource.resource_class);
  reflected.resource_kind = resource.kind;
  reflected.element_stride = DxilMetadataResourceElementStride(resource);
  reflected.id = resource.id;
  reflected.space = resource.space;
  reflected.lower_bound = resource.lower_bound;
  reflected.upper_bound = resource.upper_bound;
  reflected.bind_point = resource.lower_bound;
  reflected.bind_count =
      ShaderBindingCount(resource.lower_bound, resource.upper_bound);
  reflected.flags = resource.flags;
  reflected.from_metadata = true;
  info.resources.push_back(std::move(reflected));
}

void
AppendReflectionPsvResource(ShaderReflectionInfo &info,
                            const PsvResourceBindInfo &resource,
                            uint32_t index) {
  ShaderReflectionResourceInfo reflected = {};
  reflected.resource_type = resource.resource_type;
  reflected.resource_kind = resource.resource_kind;
  reflected.id = index;
  reflected.space = resource.space;
  reflected.lower_bound = resource.lower_bound;
  reflected.upper_bound = resource.upper_bound;
  reflected.bind_point = resource.lower_bound;
  reflected.bind_count =
      ShaderBindingCount(resource.lower_bound, resource.upper_bound);
  reflected.flags = resource.resource_flags;
  reflected.from_psv = true;
  info.resources.push_back(std::move(reflected));
}

void
CopyRdatSignature(const RuntimeDataInfo &runtime_data,
                  const std::vector<uint32_t> &indices,
                  std::vector<RdatSignatureElementInfo> &out) {
  out.clear();
  out.reserve(indices.size());
  for (const auto index : indices) {
    if (index < runtime_data.signature_elements.size())
      out.push_back(runtime_data.signature_elements[index]);
  }
}

RdatSignatureElementInfo
ConvertMetadataSignatureElement(const DxilMetadataSignatureElementInfo &element) {
  RdatSignatureElementInfo out = {};
  out.semantic_name = element.semantic_name;
  out.semantic_indices = element.semantic_indices;
  out.semantic_kind = uint8_t(element.semantic_kind);
  out.component_type = uint8_t(element.component_type);
  out.interpolation_mode = uint8_t(element.interpolation_mode);
  out.start_row = element.start_row > 0xffu ? 0xffu : uint8_t(element.start_row);
  out.cols = element.cols > 0xffu ? 0xffu : uint8_t(element.cols);
  out.start_col = element.start_col > 0xffu ? 0xffu : uint8_t(element.start_col);
  out.output_stream = element.stream > 0xffu ? 0xffu : uint8_t(element.stream);
  out.dynamic_index_mask = element.dynamic_index_mask > 0xffu
                               ? 0xffu
                               : uint8_t(element.dynamic_index_mask);
  return out;
}

void
CopyMetadataSignature(const std::vector<DxilMetadataSignatureElementInfo> &in,
                      std::vector<RdatSignatureElementInfo> &out) {
  out.clear();
  out.reserve(in.size());
  for (const auto &element : in)
    out.push_back(ConvertMetadataSignatureElement(element));
}

void
ApplyRdatShaderInfo(ShaderReflectionInfo &info,
                    const RuntimeDataInfo &runtime_data,
                    const RdatShaderInfo &shader_info) {
  CopyRdatSignature(runtime_data, shader_info.input_signature_indices,
                    info.input_signature);
  CopyRdatSignature(runtime_data, shader_info.output_signature_indices,
                    info.output_signature);
  CopyRdatSignature(runtime_data, shader_info.patch_constant_signature_indices,
                    info.patch_constant_signature);
  CopyRdatSignature(runtime_data, shader_info.primitive_signature_indices,
                    info.primitive_signature);
  info.num_threads_x = shader_info.num_threads_x;
  info.num_threads_y = shader_info.num_threads_y;
  info.num_threads_z = shader_info.num_threads_z;
  info.group_shared_bytes_used = shader_info.group_shared_bytes_used;
  info.uses_view_id = info.uses_view_id ||
                      !shader_info.view_id_output_mask.empty() ||
                      !shader_info.view_id_patch_constant_output_mask.empty() ||
                      !shader_info.view_id_primitive_output_mask.empty();
}

void
AppendLlvmDxilOperations(ShaderReflectionInfo &info,
                         const LlvmModuleInfo &module,
                         const LlvmFunctionInfo *function) {
  if (function) {
    info.dxil_operations = function->dxil_operations;
    return;
  }

  for (const auto &function_info : module.functions) {
    if (function_info.is_declaration || function_info.is_dx_intrinsic)
      continue;
    info.dxil_operations.insert(info.dxil_operations.end(),
                                function_info.dxil_operations.begin(),
                                function_info.dxil_operations.end());
  }
}

uint32_t
TranslationSourceMask(const ShaderReflectionResourceInfo &resource) {
  uint32_t source_mask = 0;
  if (resource.from_runtime_data)
    source_mask |= DxilTranslationSourceRuntimeData;
  if (resource.from_metadata)
    source_mask |= DxilTranslationSourceMetadata;
  if (resource.from_resource_def)
    source_mask |= DxilTranslationSourceResourceDef;
  if (resource.from_psv)
    source_mask |= DxilTranslationSourcePipelineStateValidation;
  return source_mask;
}

DxilTranslationResourceClass
TranslationResourceClassFromMetadata(DxilMetadataResourceClass resource_class) {
  switch (resource_class) {
  case DxilMetadataResourceClass::Srv:
    return DxilTranslationResourceClass::Srv;
  case DxilMetadataResourceClass::Uav:
    return DxilTranslationResourceClass::Uav;
  case DxilMetadataResourceClass::Cbv:
    return DxilTranslationResourceClass::Cbv;
  case DxilMetadataResourceClass::Sampler:
    return DxilTranslationResourceClass::Sampler;
  case DxilMetadataResourceClass::Unknown:
    break;
  }
  return DxilTranslationResourceClass::Unknown;
}

DxilTranslationResourceClass
TranslationResourceClassFromValue(uint32_t resource_class) {
  switch (resource_class) {
  case 0:
    return DxilTranslationResourceClass::Srv;
  case 1:
    return DxilTranslationResourceClass::Uav;
  case 2:
    return DxilTranslationResourceClass::Cbv;
  case 3:
    return DxilTranslationResourceClass::Sampler;
  default:
    return DxilTranslationResourceClass::Unknown;
  }
}

DxilTranslationResourceClass
TranslationResourceClassFromResourceDefType(uint32_t resource_type) {
  switch (resource_type) {
  case 0:
    return DxilTranslationResourceClass::Cbv;
  case 3:
    return DxilTranslationResourceClass::Sampler;
  case 4:
  case 6:
  case 8:
  case 9:
  case 10:
  case 11:
  case 13:
    return DxilTranslationResourceClass::Uav;
  case 1:
  case 2:
  case 5:
  case 7:
  case 12:
    return DxilTranslationResourceClass::Srv;
  default:
    return DxilTranslationResourceClass::Unknown;
  }
}

DxilTranslationResourceClass
ClassifyTranslationResource(const ShaderReflectionResourceInfo &resource) {
  if (resource.from_metadata) {
    return TranslationResourceClassFromMetadata(
        static_cast<DxilMetadataResourceClass>(resource.resource_class));
  }
  if (resource.from_runtime_data) {
    const auto result =
        TranslationResourceClassFromValue(resource.resource_class);
    if (result != DxilTranslationResourceClass::Unknown)
      return result;
  }
  return TranslationResourceClassFromResourceDefType(resource.resource_type);
}

DxilTranslationResourceInfo
BuildTranslationResource(const ShaderReflectionResourceInfo &resource) {
  DxilTranslationResourceInfo out = {};
  out.name = resource.name;
  out.resource_class = ClassifyTranslationResource(resource);
  out.source_mask = TranslationSourceMask(resource);
  out.id = resource.id;
  out.space = resource.space;
  out.lower_bound = resource.lower_bound;
  out.upper_bound = resource.upper_bound;
  out.bind_count = resource.bind_count;
  out.unbounded = resource.upper_bound == std::numeric_limits<uint32_t>::max() ||
                  resource.bind_count == std::numeric_limits<uint32_t>::max();
  out.resource_type = resource.resource_type;
  out.resource_kind = resource.resource_kind;
  out.return_type = resource.return_type;
  out.dimension = resource.dimension;
  out.num_samples = resource.num_samples;
  out.element_stride = resource.element_stride;
  out.flags = resource.flags;
  return out;
}

void
ApplyOperationToTranslationResource(DxilTranslationResourceInfo &resource,
                                    const DxilTranslationOperationInfo &operation) {
  if (!operation.has_resource_id || operation.resource_id != resource.id)
    return;

  switch (operation.semantic_kind) {
  case DxilSemanticOperationKind::ResourceHandle:
    resource.referenced_by_handle = true;
    break;
  case DxilSemanticOperationKind::ResourceRead:
    resource.read = true;
    break;
  case DxilSemanticOperationKind::ResourceWrite:
    resource.written = true;
    break;
  case DxilSemanticOperationKind::ResourceSample:
    resource.sampled = true;
    resource.read = true;
    break;
  case DxilSemanticOperationKind::ResourceQuery:
    resource.queried = true;
    break;
  default:
    break;
  }
}

bool
MetadataSignatureElementMatches(
    const DxilMetadataSignatureElementInfo &metadata,
    const RdatSignatureElementInfo &element) {
  return metadata.semantic_name == element.semantic_name &&
         metadata.semantic_indices == element.semantic_indices &&
         metadata.semantic_kind == element.semantic_kind &&
         metadata.component_type == element.component_type &&
         metadata.start_row == element.start_row &&
         metadata.start_col == element.start_col;
}

std::string
NormalizedSignatureSemanticName(std::string_view semantic_name) {
  std::string out;
  out.reserve(semantic_name.size());
  for (char ch : semantic_name) {
    if (ch >= 'a' && ch <= 'z')
      out.push_back(char(ch - 'a' + 'A'));
    else
      out.push_back(ch);
  }
  return out;
}

uint8_t
BuildSignatureComponentMask(uint8_t usage_mask, uint8_t start_col,
                            uint8_t cols) {
  if (usage_mask)
    return usage_mask;
  if (!cols || start_col >= 4)
    return 0;

  uint8_t mask = 0;
  const auto end_col = std::min<uint8_t>(4, uint8_t(start_col + cols));
  for (uint8_t component = start_col; component < end_col; component++)
    mask |= uint8_t(1u << component);
  return mask;
}

void
NormalizeTranslationSignatureElement(
    DxilTranslationSignatureElementInfo &element) {
  if (!element.semantic_indices.empty()) {
    element.semantic_index = element.semantic_indices.front();
    element.has_semantic_index = true;
  }

  element.component_start = element.start_col < 4 ? element.start_col : 0;
  element.component_count = element.cols;
  element.component_mask = BuildSignatureComponentMask(
      element.usage_mask, element.component_start, element.component_count);

  auto semantic_name = NormalizedSignatureSemanticName(element.semantic_name);
  std::ostringstream key;
  key << semantic_name << element.semantic_index;
  element.semantic_key = key.str();
}

const DxilMetadataSignatureElementInfo *
FindMetadataSignatureElement(
    const std::vector<DxilMetadataSignatureElementInfo> &metadata,
    const RdatSignatureElementInfo &element) {
  const auto it = std::find_if(
      metadata.begin(), metadata.end(),
      [&](const DxilMetadataSignatureElementInfo &candidate) {
        return MetadataSignatureElementMatches(candidate, element);
      });
  return it != metadata.end() ? &*it : nullptr;
}

DxilTranslationSignatureElementInfo
BuildTranslationSignatureElement(
    DxilTranslationSignatureKind kind,
    const RdatSignatureElementInfo &element,
    uint32_t source_mask,
    const DxilMetadataSignatureElementInfo *metadata) {
  DxilTranslationSignatureElementInfo out = {};
  out.kind = kind;
  out.source_mask = source_mask;
  if (metadata) {
    out.element_id = metadata->id;
    out.has_element_id = true;
    out.rows = metadata->rows > 0xffu ? 0xffu : uint8_t(metadata->rows);
  }
  out.semantic_name = element.semantic_name;
  out.semantic_indices = element.semantic_indices;
  out.semantic_kind = element.semantic_kind;
  out.component_type = element.component_type;
  out.interpolation_mode = element.interpolation_mode;
  out.start_row = element.start_row;
  out.cols = element.cols;
  out.start_col = element.start_col;
  out.output_stream = element.output_stream;
  out.usage_mask = element.usage_mask;
  out.dynamic_index_mask = element.dynamic_index_mask;
  out.allocated = element.start_row != 0xff;
  NormalizeTranslationSignatureElement(out);
  return out;
}

DxilTranslationSignatureElementInfo
BuildTranslationMetadataSignatureElement(
    DxilTranslationSignatureKind kind,
    const DxilMetadataSignatureElementInfo &element) {
  DxilTranslationSignatureElementInfo out = {};
  out.kind = kind;
  out.source_mask = DxilTranslationSourceMetadata;
  out.element_id = element.id;
  out.has_element_id = true;
  out.semantic_name = element.semantic_name;
  out.semantic_indices = element.semantic_indices;
  out.rows = element.rows > 0xffu ? 0xffu : uint8_t(element.rows);
  out.cols = element.cols > 0xffu ? 0xffu : uint8_t(element.cols);
  out.start_row = element.start_row > 0xffu ? 0xffu : uint8_t(element.start_row);
  out.start_col = element.start_col > 0xffu ? 0xffu : uint8_t(element.start_col);
  out.semantic_kind = element.semantic_kind > 0xffu
                          ? 0xffu
                          : uint8_t(element.semantic_kind);
  out.component_type = element.component_type > 0xffu
                           ? 0xffu
                           : uint8_t(element.component_type);
  out.interpolation_mode = element.interpolation_mode > 0xffu
                               ? 0xffu
                               : uint8_t(element.interpolation_mode);
  out.dynamic_index_mask = element.dynamic_index_mask > 0xffu
                               ? 0xffu
                               : uint8_t(element.dynamic_index_mask);
  out.output_stream = element.stream > 0xffu ? 0xffu : uint8_t(element.stream);
  out.allocated = out.start_row != 0xff;
  NormalizeTranslationSignatureElement(out);
  return out;
}

DxilTranslationSignatureElementInfo
BuildTranslationPsvSignatureElement(DxilTranslationSignatureKind kind,
                                    const PsvSignatureElement &element) {
  DxilTranslationSignatureElementInfo out = {};
  out.kind = kind;
  out.source_mask = DxilTranslationSourcePipelineStateValidation;
  out.semantic_name = element.semantic_name;
  out.semantic_indices = element.semantic_indexes;
  out.rows = element.rows;
  out.cols = element.cols;
  out.start_row = element.start_row;
  out.start_col = element.start_col;
  out.semantic_kind = element.semantic_kind;
  out.component_type = element.component_type;
  out.interpolation_mode = element.interpolation_mode;
  out.dynamic_index_mask = element.dynamic_index_mask;
  out.output_stream = element.output_stream;
  out.allocated = element.allocated;
  NormalizeTranslationSignatureElement(out);
  return out;
}

DxilTranslationSignatureElementInfo
BuildTranslationLegacySignatureElement(DxilTranslationSignatureKind kind,
                                       const SignatureElement &element) {
  DxilTranslationSignatureElementInfo out = {};
  out.kind = kind;
  out.source_mask = DxilTranslationSourceLegacySignature;
  out.semantic_name = element.semantic_name;
  out.semantic_indices.push_back(element.semantic_index);
  out.start_row = element.register_index > 0xffu
                      ? 0xffu
                      : uint8_t(element.register_index);
  out.cols = 4;
  out.start_col = 0;
  out.semantic_kind = element.system_value > 0xffu
                          ? 0xffu
                          : uint8_t(element.system_value);
  out.component_type = element.component_type > 0xffu
                           ? 0xffu
                           : uint8_t(element.component_type);
  out.usage_mask = element.mask;
  out.dynamic_index_mask = element.read_write_mask;
  out.output_stream = element.stream > 0xffu ? 0xffu : uint8_t(element.stream);
  out.allocated = true;
  NormalizeTranslationSignatureElement(out);
  return out;
}

void
AppendTranslationSignatures(
    DxilTranslationInfo &info,
    DxilTranslationSignatureKind kind,
    const std::vector<RdatSignatureElementInfo> &elements,
    uint32_t source_mask,
    const std::vector<DxilMetadataSignatureElementInfo> &metadata) {
  for (const auto &element : elements) {
    auto *metadata_element = FindMetadataSignatureElement(metadata, element);
    auto out =
        BuildTranslationSignatureElement(kind, element, source_mask,
                                         metadata_element);
    if (metadata_element)
      out.source_mask |= DxilTranslationSourceMetadata;
    info.signatures.push_back(std::move(out));
  }
}

void
AppendTranslationMetadataSignatures(
    DxilTranslationInfo &info,
    DxilTranslationSignatureKind kind,
    const std::vector<DxilMetadataSignatureElementInfo> &elements) {
  for (const auto &element : elements)
    info.signatures.push_back(
        BuildTranslationMetadataSignatureElement(kind, element));
}

void
AppendTranslationPsvSignatures(DxilTranslationInfo &info,
                               DxilTranslationSignatureKind kind,
                               const std::vector<PsvSignatureElement> &elements) {
  for (const auto &element : elements)
    info.signatures.push_back(BuildTranslationPsvSignatureElement(kind, element));
}

bool
HasTranslationSignatureKind(const DxilTranslationInfo &info,
                            DxilTranslationSignatureKind kind) {
  return std::any_of(info.signatures.begin(), info.signatures.end(),
                     [kind](const DxilTranslationSignatureElementInfo &element) {
                       return element.kind == kind;
                     });
}

DxilTranslationSignatureKind
LegacySignatureKind(uint32_t part_fourcc) {
  if (part_fourcc == fourcc::OutputSignature)
    return DxilTranslationSignatureKind::Output;
  if (part_fourcc == fourcc::PatchConstantSignature)
    return DxilTranslationSignatureKind::PatchConstant;
  return DxilTranslationSignatureKind::Input;
}

const LlvmBasicBlockInfo *
FindBasicBlockForInstruction(const LlvmFunctionInfo &function,
                             uint32_t instruction_index) {
  for (const auto &block : function.basic_blocks) {
    if (instruction_index >= block.instruction_start &&
        instruction_index < block.instruction_start + block.instruction_count)
      return &block;
  }
  return nullptr;
}

bool
ShouldExposeTranslationFunction(const LlvmFunctionInfo &function,
                                std::string_view entry_function_name) {
  return !function.is_declaration && !function.is_dx_intrinsic &&
         (function.is_entry_reachable ||
          ShaderNamesEqual(function.name, entry_function_name));
}

DxilTranslationOperationInfo
BuildTranslationOperation(const LlvmFunctionInfo &function,
                          const LlvmDxilOperationInfo &operation) {
  DxilTranslationOperationInfo out = {};
  out.function_name = function.name;
  if (const auto *block =
          FindBasicBlockForInstruction(function, operation.instruction_index))
    out.basic_block_name = block->name;
  out.instruction_index = operation.instruction_index;
  out.opcode = operation.opcode;
  out.opcode_name = operation.opcode_name;
  out.opcode_class = operation.opcode_class;
  out.opcode_category = operation.opcode_category;
  out.semantic_kind = operation.semantic_kind;
  out.min_shader_model_major = operation.min_shader_model_major;
  out.min_shader_model_minor = operation.min_shader_model_minor;
  out.semantic_flags = operation.semantic_flags;
  out.opcode_known = operation.opcode_known;
  out.opcode_reserved = operation.opcode_reserved;
  out.resource_id = operation.resource_id;
  out.has_resource_id = operation.has_resource_id;
  out.signature_element_id = operation.signature_element_id;
  out.has_signature_element_id = operation.has_signature_element_id;
  out.result_type = operation.result_type;
  out.result_type_info = operation.result_type_info;
  out.operands = operation.operands;
  out.typed = operation.typed;
  return out;
}

void
AppendTranslationIrInfo(DxilTranslationInfo &info,
                        const LlvmModuleInfo &module) {
  info.call_graph_edges = module.call_graph.edges;
  info.entry_reachable_functions = module.call_graph.entry_reachable_functions;
  info.recursive_functions = module.call_graph.recursive_functions;
  info.unused_dx_intrinsic_declarations =
      module.call_graph.unused_dx_intrinsic_declarations;
  info.has_indirect_calls = module.call_graph.has_indirect_calls;
  info.has_recursion = module.call_graph.has_recursion;

  for (const auto &function : module.functions) {
    if (!ShouldExposeTranslationFunction(function, info.function_name))
      continue;

    DxilTranslationFunctionInfo function_info = {};
    function_info.name = function.name;
    function_info.is_entry_function =
        ShaderNamesEqual(function.name, info.function_name) ||
        ShaderNamesEqual(function.name, info.entry_point_name);
    function_info.is_entry_reachable = function.is_entry_reachable;
    function_info.is_recursive = function.is_recursive;
    function_info.has_indirect_calls = function.has_indirect_calls;
    function_info.called_functions = function.called_functions;
    info.functions.push_back(std::move(function_info));

    for (const auto &block : function.basic_blocks) {
      DxilTranslationBasicBlockInfo block_info = {};
      block_info.function_name = function.name;
      block_info.name = block.name;
      block_info.instruction_start = block.instruction_start;
      block_info.instruction_count = block.instruction_count;
      block_info.terminator_opcode = block.terminator_opcode;
      block_info.successors = block.successors;
      block_info.has_return = block.has_return;
      block_info.has_branch = block.has_branch;
      block_info.has_switch = block.has_switch;
      block_info.has_unreachable = block.has_unreachable;
      info.basic_blocks.push_back(std::move(block_info));
    }

    for (const auto &operation : function.dxil_operations)
      info.operations.push_back(BuildTranslationOperation(function, operation));
  }
}

std::string
LowerAscii(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (char ch : value) {
    if (ch >= 'A' && ch <= 'Z')
      out.push_back(char(ch - 'A' + 'a'));
    else
      out.push_back(ch);
  }
  return out;
}

std::string
FormatVersion(uint32_t major, uint32_t minor) {
  std::ostringstream stream;
  stream << major << "." << minor;
  return stream.str();
}

std::string
FormatBinding(uint32_t space, uint32_t lower_bound, uint32_t upper_bound) {
  std::ostringstream stream;
  stream << "space" << space << " [" << lower_bound << ", ";
  if (upper_bound == std::numeric_limits<uint32_t>::max())
    stream << "unbounded";
  else
    stream << upper_bound;
  stream << "]";
  return stream.str();
}

uint32_t
ShaderKindFromModelKind(std::string_view kind) {
  const auto lower = LowerAscii(kind);
  if (lower == "ps")
    return 0;
  if (lower == "vs")
    return 1;
  if (lower == "gs")
    return 2;
  if (lower == "hs")
    return 3;
  if (lower == "ds")
    return 4;
  if (lower == "cs")
    return 5;
  if (lower == "lib")
    return 6;
  if (lower == "raygeneration")
    return 7;
  if (lower == "intersection")
    return 8;
  if (lower == "anyhit")
    return 9;
  if (lower == "closesthit")
    return 10;
  if (lower == "miss")
    return 11;
  if (lower == "callable")
    return 12;
  if (lower == "ms")
    return 13;
  if (lower == "as")
    return 14;
  if (lower == "node")
    return 15;
  return std::numeric_limits<uint32_t>::max();
}

bool
ShaderModelAtLeast(uint32_t major, uint32_t minor,
                   uint32_t required_major, uint32_t required_minor) {
  return major > required_major ||
         (major == required_major && minor >= required_minor);
}

const LlvmFunctionInfo *
FindLlvmFunction(const LlvmModuleInfo &module, std::string_view name) {
  if (name.empty())
    return nullptr;
  auto function = std::find_if(
      module.functions.begin(), module.functions.end(),
      [name](const LlvmFunctionInfo &info) {
        return ShaderNamesEqual(info.name, name);
      });
  return function != module.functions.end() ? &*function : nullptr;
}

size_t
CountParts(const ContainerInfo &container, uint32_t fourcc) {
  return std::count_if(container.parts.begin(), container.parts.end(),
                       [fourcc](const BlobPart &part) {
                         return part.fourcc == fourcc;
                       });
}

void
AddDiagnostic(DxilValidationInfo &info,
              DxilValidationSeverity severity,
              DxilValidationCategory category,
              std::string code,
              std::string message,
              std::string function_name = {},
              uint32_t instruction_index = 0,
              std::optional<uint32_t> opcode = std::nullopt) {
  switch (severity) {
  case DxilValidationSeverity::Info:
    info.info_count++;
    break;
  case DxilValidationSeverity::Warning:
    info.warning_count++;
    break;
  case DxilValidationSeverity::Error:
    info.error_count++;
    break;
  }

  DxilValidationDiagnostic diagnostic = {};
  diagnostic.severity = severity;
  diagnostic.category = category;
  diagnostic.code = std::move(code);
  diagnostic.message = std::move(message);
  diagnostic.function_name = std::move(function_name);
  diagnostic.instruction_index = instruction_index;
  diagnostic.has_instruction = !diagnostic.function_name.empty();
  if (opcode) {
    diagnostic.opcode = *opcode;
    diagnostic.has_opcode = true;
  }
  info.diagnostics.push_back(std::move(diagnostic));
}

void
AddError(DxilValidationInfo &info,
         DxilValidationCategory category,
         std::string code,
         std::string message,
         std::string function_name = {},
         uint32_t instruction_index = 0,
         std::optional<uint32_t> opcode = std::nullopt) {
  AddDiagnostic(info, DxilValidationSeverity::Error, category,
                std::move(code), std::move(message),
                std::move(function_name), instruction_index, opcode);
}

void
AddWarning(DxilValidationInfo &info,
           DxilValidationCategory category,
           std::string code,
           std::string message) {
  AddDiagnostic(info, DxilValidationSeverity::Warning, category,
                std::move(code), std::move(message));
}

void
ValidateContainerInfo(const Parser &parser, DxilValidationInfo &info) {
  const auto &container = parser.container();
  if (CountParts(container, fourcc::Dxil) != 1) {
    AddError(info, DxilValidationCategory::Container, "dxil-part-count",
             "DXContainer must contain exactly one DXIL part");
  }

  for (auto fourcc : {fourcc::FeatureInfo, fourcc::ShaderHash,
                     fourcc::CompilerVersion, fourcc::ResourceDef,
                     fourcc::RuntimeData, fourcc::PipelineStateValidation}) {
    if (CountParts(container, fourcc) > 1) {
      AddWarning(info, DxilValidationCategory::Container,
                 "duplicate-singleton-part",
                 FourCCString(fourcc) + " appears more than once");
    }
  }

  struct PartRange {
    uint32_t fourcc = 0;
    uint32_t begin = 0;
    uint32_t end = 0;
  };
  std::vector<PartRange> ranges;
  ranges.reserve(container.parts.size());
  for (const auto &part : container.parts) {
    ranges.push_back({
        .fourcc = part.fourcc,
        .begin = part.offset,
        .end = uint32_t(part.offset + kPartHeaderSize + part.data.size()),
    });
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const PartRange &lhs, const PartRange &rhs) {
              return lhs.begin < rhs.begin;
            });
  for (size_t i = 1; i < ranges.size(); i++) {
    if (ranges[i].begin < ranges[i - 1].end) {
      AddError(info, DxilValidationCategory::Container, "overlapping-parts",
               FourCCString(ranges[i - 1].fourcc) + " overlaps " +
                   FourCCString(ranges[i].fourcc));
    }
  }
}

void
ValidateProgramInfo(const Parser &parser, DxilValidationInfo &info) {
  const auto &dxil_program = parser.dxilProgram();
  if (!dxil_program) {
    AddError(info, DxilValidationCategory::Program, "missing-dxil-program",
             "DXIL program header was not parsed");
    return;
  }

  if (dxil_program->shader_kind() > 15) {
    AddError(info, DxilValidationCategory::Program, "invalid-shader-kind",
             "DXIL program has an unknown shader kind");
  }
  if (dxil_program->major_version() < 6) {
    AddError(info, DxilValidationCategory::Program, "invalid-shader-model",
             "DXIL shader model must be 6.x or newer");
  }
  if ((dxil_program->dxil_version >> 16) == 0) {
    AddError(info, DxilValidationCategory::Program, "invalid-dxil-version",
             "DXIL version in the program header is zero");
  }

  if (const auto *part = parser.container().findPart(fourcc::Dxil)) {
    const auto declared_size = size_t(dxil_program->size_in_uint32) *
                               sizeof(uint32_t);
    if (declared_size != part->data.size()) {
      AddWarning(info, DxilValidationCategory::Program,
                 "dxil-size-mismatch",
                 "DXIL program size does not match the DXIL part size");
    }
  }
}

void
ValidateModuleMetadata(const Parser &parser, DxilValidationInfo &info) {
  const auto &module = parser.llvmModule();
  if (!module) {
    AddWarning(info, DxilValidationCategory::Metadata,
               "llvm-module-unavailable",
               "LLVM module information is unavailable; instruction validation is limited");
    return;
  }

  const auto &dxil_program = parser.dxilProgram();
  if (!module->shader_model) {
    AddError(info, DxilValidationCategory::Metadata,
             "missing-shader-model",
             "LLVM module is missing dx.shaderModel metadata");
  } else if (dxil_program) {
    const auto metadata_kind = ShaderKindFromModelKind(module->shader_model->kind);
    if (metadata_kind == std::numeric_limits<uint32_t>::max()) {
      AddError(info, DxilValidationCategory::Metadata,
               "unknown-shader-model-kind",
               "dx.shaderModel uses an unknown shader kind '" +
                   module->shader_model->kind + "'");
    } else if (metadata_kind != dxil_program->shader_kind()) {
      AddError(info, DxilValidationCategory::Metadata,
               "shader-kind-mismatch",
               "DXIL header shader kind " +
                   std::string(PsvShaderKindName(uint8_t(dxil_program->shader_kind()))) +
                   " does not match dx.shaderModel kind " +
                   module->shader_model->kind);
    }
    if (module->shader_model->major != dxil_program->major_version() ||
        module->shader_model->minor != dxil_program->minor_version()) {
      AddError(info, DxilValidationCategory::Metadata,
               "shader-model-mismatch",
               "DXIL header shader model " +
                   FormatVersion(dxil_program->major_version(),
                                 dxil_program->minor_version()) +
                   " does not match dx.shaderModel " +
                   FormatVersion(module->shader_model->major,
                                 module->shader_model->minor));
    }
  }

  if (module->dxil_version.size() < 2) {
    AddError(info, DxilValidationCategory::Metadata,
             "missing-dxil-version",
             "LLVM module is missing dx.version metadata");
  } else if (dxil_program) {
    const auto header_major = dxil_program->dxil_version >> 16;
    const auto header_minor = dxil_program->dxil_version & 0xffffu;
    if (module->dxil_version[0] != header_major ||
        module->dxil_version[1] != header_minor) {
      AddError(info, DxilValidationCategory::Metadata,
               "dxil-version-mismatch",
               "DXIL header version " +
                   FormatVersion(header_major, header_minor) +
                   " does not match dx.version " +
                   FormatVersion(module->dxil_version[0],
                                 module->dxil_version[1]));
    }
  }

  if (module->validator_version.size() < 2) {
    AddWarning(info, DxilValidationCategory::Metadata,
               "missing-validator-version",
               "LLVM module is missing dx.valver metadata");
  }

  if (module->entry_points.empty()) {
    AddError(info, DxilValidationCategory::Metadata,
             "missing-entry-points",
             "LLVM module is missing dx.entryPoints metadata");
  }

  const bool is_library =
      module->shader_model &&
      ShaderKindFromModelKind(module->shader_model->kind) == 6;
  for (const auto &entry : module->entry_points) {
    if (!is_library && entry.function_name.empty()) {
      AddError(info, DxilValidationCategory::Metadata,
               "missing-entry-function",
               "dx.entryPoints contains an entry without a function reference");
      continue;
    }
    if (!entry.function_name.empty() &&
        !FindLlvmFunction(*module, entry.function_name)) {
      AddError(info, DxilValidationCategory::Metadata,
               "missing-entry-function",
               "Entry point function '" + entry.function_name +
                   "' is not present in the LLVM module");
    }
  }
}

void
ValidateLlvmFlow(const Parser &parser, DxilValidationInfo &info) {
  const auto &module = parser.llvmModule();
  if (!module)
    return;

  if (module->call_graph.has_indirect_calls) {
    AddWarning(info, DxilValidationCategory::Instruction,
               "indirect-call",
               "LLVM module contains indirect calls; DXIL validation is limited");
  }

  for (const auto &function : module->call_graph.recursive_functions) {
    AddError(info, DxilValidationCategory::Instruction, "recursive-call",
             "DXIL function '" + function + "' participates in recursion");
  }

  for (const auto &function : module->functions) {
    if (!function.is_declaration && !function.is_dx_intrinsic &&
        !function.is_entry_reachable) {
      AddWarning(info, DxilValidationCategory::Instruction,
                 "unreachable-function",
                 "DXIL function '" + function.name +
                     "' is not reachable from an entry point");
    }
  }
}

bool
HasMetadataSignatureElementId(const LlvmModuleInfo &module, uint32_t id) {
  return std::any_of(
      module.signature_elements.begin(), module.signature_elements.end(),
      [id](const DxilMetadataSignatureElementInfo &element) {
        return element.id == id;
      });
}

bool
HasMetadataResourceId(const LlvmModuleInfo &module, uint32_t id) {
  return std::any_of(
      module.resources.begin(), module.resources.end(),
      [id](const DxilMetadataResourceInfo &resource) {
        return resource.id == id;
      });
}

bool
HasRuntimeDataResourceId(const RuntimeDataInfo &runtime_data, uint32_t id) {
  return std::any_of(
      runtime_data.resources.begin(), runtime_data.resources.end(),
      [id](const RdatResourceInfo &resource) {
        return resource.id == id;
      });
}

void
ValidateDxilOperations(const Parser &parser, DxilValidationInfo &info) {
  const auto &module = parser.llvmModule();
  if (!module)
    return;

  uint32_t shader_model_major = 0;
  uint32_t shader_model_minor = 0;
  if (const auto &reflection = parser.shaderReflection()) {
    shader_model_major = reflection->shader_model_major;
    shader_model_minor = reflection->shader_model_minor;
  } else if (const auto &dxil_program = parser.dxilProgram()) {
    shader_model_major = dxil_program->major_version();
    shader_model_minor = dxil_program->minor_version();
  }

  for (const auto &function : module->functions) {
    if (function.is_dx_intrinsic && !function.is_declaration) {
      AddError(info, DxilValidationCategory::Instruction,
               "defined-dx-intrinsic",
               "dx.op intrinsic '" + function.name +
                   "' must be a declaration");
    }

    if (function.is_declaration)
      continue;

    for (uint32_t i = 0; i < function.instructions.size(); i++) {
      const auto &instruction = function.instructions[i];
      if (!instruction.is_dx_intrinsic_call)
        continue;

      if (!instruction.dxil_opcode) {
        AddError(info, DxilValidationCategory::Instruction,
                 "missing-constant-opcode",
                 "dx.op call does not use a constant opcode operand",
                 function.name, i);
        continue;
      }

      const auto opcode = *instruction.dxil_opcode;
      const auto *opcode_info = FindDxilOpcodeInfo(opcode);
      if (!opcode_info) {
        AddError(info, DxilValidationCategory::Instruction,
                 "unknown-opcode",
                 "dx.op call uses unknown opcode " + std::to_string(opcode),
                 function.name, i, opcode);
        continue;
      }
      if (opcode_info->is_reserved) {
        AddError(info, DxilValidationCategory::Instruction,
                 "reserved-opcode",
                 "dx.op call uses reserved opcode " + std::to_string(opcode),
                 function.name, i, opcode);
      }

      if (shader_model_major &&
          !ShaderModelAtLeast(shader_model_major, shader_model_minor,
                              opcode_info->min_shader_model_major,
                              opcode_info->min_shader_model_minor)) {
        AddError(info, DxilValidationCategory::Instruction,
                 "opcode-shader-model",
                 "Opcode " + std::string(opcode_info->name) +
                     " requires shader model " +
                     FormatVersion(opcode_info->min_shader_model_major,
                                   opcode_info->min_shader_model_minor) +
                     " but shader is " +
                     FormatVersion(shader_model_major, shader_model_minor),
                 function.name, i, opcode);
      }

      if (!instruction.dxil_opcode_name.empty() &&
          LowerAscii(instruction.dxil_opcode_name) !=
              LowerAscii(opcode_info->opcode_class)) {
        AddError(info, DxilValidationCategory::Instruction,
                 "opcode-function-mismatch",
                 "dx.op function class '" + instruction.dxil_opcode_name +
                     "' does not match opcode " +
                     std::string(opcode_info->name) + " class " +
                     std::string(opcode_info->opcode_class),
                 function.name, i, opcode);
      }

      const auto operation = std::find_if(
          function.dxil_operations.begin(), function.dxil_operations.end(),
          [i](const LlvmDxilOperationInfo &op) {
            return op.instruction_index == i;
          });
      if (operation == function.dxil_operations.end())
        continue;

      if (operation->has_signature_element_id &&
          !module->signature_elements.empty() &&
          !HasMetadataSignatureElementId(*module,
                                         operation->signature_element_id)) {
        AddError(info, DxilValidationCategory::Instruction,
                 "invalid-signature-reference",
                 "dx.op references missing signature element " +
                     std::to_string(operation->signature_element_id),
                 function.name, i, opcode);
      }

      if (operation->has_resource_id) {
        const bool has_metadata_resources = !module->resources.empty();
        const bool metadata_match =
            has_metadata_resources &&
            HasMetadataResourceId(*module, operation->resource_id);
        const bool rdat_match =
            parser.runtimeData() &&
            HasRuntimeDataResourceId(*parser.runtimeData(),
                                     operation->resource_id);
        if ((has_metadata_resources || parser.runtimeData()) &&
            !metadata_match && !rdat_match) {
          AddError(info, DxilValidationCategory::Instruction,
                   "invalid-resource-reference",
                   "dx.op references missing resource id " +
                       std::to_string(operation->resource_id),
                   function.name, i, opcode);
        }
      }
    }
  }
}

void
ValidateRuntimeDataInfo(const Parser &parser, DxilValidationInfo &info) {
  const auto &runtime_data = parser.runtimeData();
  if (!runtime_data)
    return;

  if (runtime_data->functions.empty()) {
    AddWarning(info, DxilValidationCategory::RuntimeData,
               "missing-rdat-functions",
               "RDAT is present but has no function records");
  }

  for (const auto &function : runtime_data->functions) {
    if (function.shader_kind > 15) {
      AddError(info, DxilValidationCategory::RuntimeData,
               "invalid-rdat-shader-kind",
               "RDAT function '" + function.name +
                   "' has an unknown shader kind");
    }
    if (function.has_shader_info &&
        !runtime_data->findShaderInfo(function.shader_info_table_type,
                                      function.shader_info_index)) {
      AddError(info, DxilValidationCategory::RuntimeData,
               "missing-rdat-shader-info",
               "RDAT function '" + function.name +
                   "' references a missing shader info record");
    }
    if (function.minimum_expected_wave_lane_count &&
        function.maximum_expected_wave_lane_count &&
        function.minimum_expected_wave_lane_count >
            function.maximum_expected_wave_lane_count) {
      AddError(info, DxilValidationCategory::RuntimeData,
               "invalid-wave-lane-range",
               "RDAT function '" + function.name +
                   "' has an invalid expected wave lane range");
    }
  }

  for (size_t i = 0; i < runtime_data->resources.size(); i++) {
    const auto &lhs = runtime_data->resources[i];
    if (lhs.upper_bound < lhs.lower_bound &&
        lhs.upper_bound != std::numeric_limits<uint32_t>::max()) {
      AddError(info, DxilValidationCategory::RuntimeData,
               "invalid-rdat-resource-range",
               "RDAT resource '" + lhs.name + "' has an invalid binding range");
    }
    for (size_t j = i + 1; j < runtime_data->resources.size(); j++) {
      const auto &rhs = runtime_data->resources[j];
      if (lhs.resource_class != rhs.resource_class || lhs.space != rhs.space)
        continue;
      const auto lhs_upper = lhs.upper_bound;
      const auto rhs_upper = rhs.upper_bound;
      if (lhs.lower_bound <= rhs_upper && rhs.lower_bound <= lhs_upper) {
        AddWarning(info, DxilValidationCategory::RuntimeData,
                   "overlapping-rdat-resources",
                   "RDAT resources '" + lhs.name + "' and '" + rhs.name +
                       "' overlap at " +
                       FormatBinding(lhs.space,
                                     std::max(lhs.lower_bound, rhs.lower_bound),
                                     std::min(lhs_upper, rhs_upper)));
      }
    }
  }
}

bool
ThreadGroupSizePresent(const ShaderReflectionInfo &info) {
  return info.num_threads_x || info.num_threads_y || info.num_threads_z;
}

void
ValidatePipelineStateValidationInfo(const Parser &parser,
                                    DxilValidationInfo &info) {
  const auto &psv = parser.pipelineStateValidation();
  if (!psv)
    return;

  if (!psv->has_runtime_info_1) {
    AddWarning(info, DxilValidationCategory::PipelineStateValidation,
               "old-psv-runtime-info",
               "PSV0 runtime info is older than runtime-info-1");
    return;
  }

  if (psv->shader_stage > 15) {
    AddError(info, DxilValidationCategory::PipelineStateValidation,
             "invalid-psv-shader-stage",
             "PSV0 contains an unknown shader stage");
  }

  if (const auto &dxil_program = parser.dxilProgram()) {
    if (psv->shader_stage != dxil_program->shader_kind()) {
      AddError(info, DxilValidationCategory::PipelineStateValidation,
               "psv-stage-mismatch",
               "PSV0 shader stage " +
                   std::string(PsvShaderKindName(psv->shader_stage)) +
                   " does not match DXIL header stage " +
                   PsvShaderKindName(uint8_t(dxil_program->shader_kind())));
    }
  }

  if (psv->has_runtime_info_2 &&
      (psv->shader_stage == 5 || psv->shader_stage == 13 ||
       psv->shader_stage == 14 || psv->shader_stage == 15) &&
      (!psv->num_threads_x || !psv->num_threads_y || !psv->num_threads_z)) {
    AddError(info, DxilValidationCategory::PipelineStateValidation,
             "invalid-thread-group-size",
             "PSV0 thread group dimensions must be non-zero");
  }

  for (const auto &resource : psv->resources) {
    if (resource.upper_bound < resource.lower_bound &&
        resource.upper_bound != std::numeric_limits<uint32_t>::max()) {
      AddError(info, DxilValidationCategory::PipelineStateValidation,
               "invalid-psv-resource-range",
               "PSV0 resource has an invalid binding range");
    }
  }
}

bool
SameResourceBinding(const ShaderReflectionResourceInfo &lhs,
                    const ShaderReflectionResourceInfo &rhs) {
  return lhs.space == rhs.space &&
         lhs.lower_bound == rhs.lower_bound &&
         lhs.upper_bound == rhs.upper_bound &&
         (!lhs.resource_kind || !rhs.resource_kind ||
          lhs.resource_kind == rhs.resource_kind) &&
         (!lhs.resource_type || !rhs.resource_type ||
         lhs.resource_type == rhs.resource_type);
}

bool
ResourceRangesEqual(uint32_t lhs_space, uint32_t lhs_lower, uint32_t lhs_upper,
                    uint32_t rhs_space, uint32_t rhs_lower, uint32_t rhs_upper) {
  return lhs_space == rhs_space &&
         lhs_lower == rhs_lower &&
         lhs_upper == rhs_upper;
}

void
ValidateCrossSourceResources(const Parser &parser, DxilValidationInfo &info) {
  const auto &module = parser.llvmModule();
  if (!module)
    return;

  if (const auto &runtime_data = parser.runtimeData()) {
    for (const auto &metadata_resource : module->resources) {
      auto match = std::find_if(
          runtime_data->resources.begin(), runtime_data->resources.end(),
          [&](const RdatResourceInfo &resource) {
            return resource.id == metadata_resource.id;
          });
      if (match == runtime_data->resources.end())
        continue;
      if (!ResourceRangesEqual(metadata_resource.space,
                               metadata_resource.lower_bound,
                               metadata_resource.upper_bound,
                               match->space, match->lower_bound,
                               match->upper_bound)) {
        AddWarning(info, DxilValidationCategory::Reflection,
                   "metadata-rdat-resource-mismatch",
                   "DXIL metadata resource '" + metadata_resource.name +
                       "' has a different binding range than RDAT");
      }
    }
  }

  if (const auto &resource_def = parser.resourceDef()) {
    for (const auto &metadata_resource : module->resources) {
      auto match = std::find_if(
          resource_def->resources.begin(), resource_def->resources.end(),
          [&](const ResourceBindingInfo &resource) {
            return resource.space == metadata_resource.space &&
                   resource.bind_point == metadata_resource.lower_bound;
          });
      if (match == resource_def->resources.end())
        continue;
      const auto upper_bound = match->bind_count
                                   ? match->bind_point + match->bind_count - 1
                                   : match->bind_point;
      if (upper_bound != metadata_resource.upper_bound) {
        AddWarning(info, DxilValidationCategory::Reflection,
                   "metadata-rdef-resource-mismatch",
                   "DXIL metadata resource '" + metadata_resource.name +
                       "' has a different binding range than RDEF");
      }
    }
  }
}

void
ValidateReflectionConsistency(const Parser &parser, DxilValidationInfo &info) {
  const auto &reflection = parser.shaderReflection();
  if (!reflection)
    return;

  if (!reflection->has_runtime_data) {
    AddWarning(info, DxilValidationCategory::Reflection,
               "missing-rdat",
               "RDAT is missing; PSO integration will have reduced reflection data");
  }
  if (!reflection->has_pipeline_state_validation) {
    AddWarning(info, DxilValidationCategory::Reflection,
               "missing-psv",
               "PSV0 is missing; pipeline-state validation data is unavailable");
  }

  ValidateCrossSourceResources(parser, info);

  if (const auto &runtime_data = parser.runtimeData()) {
    if (const auto *rdat_function = FindReflectionRdatFunction(
            *runtime_data, reflection->entry_point_name,
            reflection->function_name)) {
      if (rdat_function->shader_kind != reflection->shader_kind) {
        AddError(info, DxilValidationCategory::Reflection,
                 "reflection-stage-mismatch",
                 "Reflected shader stage does not match RDAT function stage");
      }
    }
  }

  if (const auto &psv = parser.pipelineStateValidation()) {
    if (psv->has_runtime_info_1 &&
        psv->shader_stage != reflection->shader_kind) {
      AddError(info, DxilValidationCategory::Reflection,
               "reflection-psv-stage-mismatch",
               "Reflected shader stage does not match PSV0 shader stage");
    }
    if (psv->has_runtime_info_2 && ThreadGroupSizePresent(*reflection) &&
        (psv->num_threads_x || psv->num_threads_y || psv->num_threads_z) &&
        (psv->num_threads_x != reflection->num_threads_x ||
         psv->num_threads_y != reflection->num_threads_y ||
         psv->num_threads_z != reflection->num_threads_z)) {
      AddError(info, DxilValidationCategory::Reflection,
               "thread-group-size-mismatch",
               "Reflected thread group size does not match PSV0");
    }
  }

  for (const auto &resource : reflection->resources) {
    if (!resource.from_psv)
      continue;
    auto match = std::find_if(
        reflection->resources.begin(), reflection->resources.end(),
        [&](const ShaderReflectionResourceInfo &candidate) {
          return &candidate != &resource && !candidate.from_psv &&
                 SameResourceBinding(resource, candidate);
        });
    if (match == reflection->resources.end() &&
        (reflection->has_runtime_data || reflection->has_resource_def)) {
      AddWarning(info, DxilValidationCategory::Reflection,
                 "unmatched-psv-resource",
                 "PSV0 resource " +
                     FormatBinding(resource.space, resource.lower_bound,
                                   resource.upper_bound) +
                     " has no matching RDAT/RDEF resource");
    }
  }
}

} // namespace

ParseStatus
BuildShaderReflection(const Parser &parser, ShaderReflectionInfo &info) {
  info = {};
  info.valid = parser.dxilProgram().has_value();
  info.has_llvm_module = parser.llvmModule().has_value();
  info.has_runtime_data = parser.runtimeData().has_value();
  info.has_pipeline_state_validation =
      parser.pipelineStateValidation().has_value();
  info.has_resource_def = parser.resourceDef().has_value();
  info.legacy_signatures = parser.signatures();

  if (const auto *root_signature =
          parser.container().findPart(fourcc::RootSignature)) {
    info.has_root_signature = true;
    info.root_signature_offset = root_signature->offset;
    info.root_signature = root_signature->data;
  }

  if (const auto &dxil_program = parser.dxilProgram()) {
    info.shader_kind = dxil_program->shader_kind();
    info.shader_stage_name = PsvShaderKindName(uint8_t(info.shader_kind));
    info.shader_model_major = dxil_program->major_version();
    info.shader_model_minor = dxil_program->minor_version();
    info.dxil_major = dxil_program->dxil_version >> 16;
    info.dxil_minor = dxil_program->dxil_version & 0xffffu;
  }

  if (const auto &feature_info = parser.featureInfo())
    info.feature_flags = feature_info->feature_flags;

  const LlvmFunctionInfo *llvm_function = nullptr;
  const DxilEntryPointInfo *metadata_entry = nullptr;
  if (const auto &module = parser.llvmModule()) {
    if (module->shader_model) {
      info.shader_model_kind = module->shader_model->kind;
      info.shader_model_major = module->shader_model->major;
      info.shader_model_minor = module->shader_model->minor;
    }
    if (module->dxil_version.size() >= 2) {
      info.dxil_major = module->dxil_version[0];
      info.dxil_minor = module->dxil_version[1];
    }
    if (const auto *entry = FindReflectionEntryPoint(*module)) {
      metadata_entry = entry;
      info.entry_point_name = entry->name;
      info.function_name = entry->function_name;
    }
    llvm_function = FindReflectionLlvmFunction(
        *module, info.entry_point_name, info.function_name);
    if (llvm_function && info.function_name.empty())
      info.function_name = llvm_function->name;
    AppendLlvmDxilOperations(info, *module, llvm_function);
  }

  const RdatFunctionInfo *rdat_function = nullptr;
  bool has_reflected_resources = false;
  bool has_reflected_signatures = false;
  if (const auto &runtime_data = parser.runtimeData()) {
    rdat_function = FindReflectionRdatFunction(
        *runtime_data, info.entry_point_name, info.function_name);
    if (rdat_function) {
      if (info.entry_point_name.empty())
        info.entry_point_name = rdat_function->unmangled_name.empty()
                                    ? rdat_function->name
                                    : rdat_function->unmangled_name;
      if (info.function_name.empty())
        info.function_name = rdat_function->name;
      info.shader_kind = rdat_function->shader_kind;
      info.shader_stage_name = PsvShaderKindName(uint8_t(info.shader_kind));
      info.feature_flags = rdat_function->feature_flags();
      info.min_shader_target = rdat_function->min_shader_target;
      info.shader_flags = rdat_function->shader_flags;
      if (const auto *shader_info = runtime_data->findShaderInfo(
              rdat_function->shader_info_table_type,
              rdat_function->shader_info_index)) {
        ApplyRdatShaderInfo(info, *runtime_data, *shader_info);
        has_reflected_signatures =
            !info.input_signature.empty() || !info.output_signature.empty() ||
            !info.patch_constant_signature.empty() ||
            !info.primitive_signature.empty();
      }
    }

    if (rdat_function && !rdat_function->resource_indices.empty()) {
      for (const auto resource_index : rdat_function->resource_indices)
        AppendReflectionRdatResource(
            info, runtime_data->resources[resource_index]);
      has_reflected_resources = true;
    } else {
      for (const auto &resource : runtime_data->resources) {
        AppendReflectionRdatResource(info, resource);
        has_reflected_resources = true;
      }
    }
  }

  if (!has_reflected_signatures && metadata_entry) {
    CopyMetadataSignature(metadata_entry->input_signature, info.input_signature);
    CopyMetadataSignature(metadata_entry->output_signature, info.output_signature);
    CopyMetadataSignature(metadata_entry->patch_constant_signature,
                          info.patch_constant_signature);
  }

  if (!has_reflected_resources && metadata_entry &&
      !metadata_entry->resources.empty()) {
    for (const auto &resource : metadata_entry->resources)
      AppendReflectionMetadataResource(info, resource);
    has_reflected_resources = true;
  }

  if (!has_reflected_resources) {
    if (const auto &module = parser.llvmModule()) {
      for (const auto &resource : module->resources) {
        AppendReflectionMetadataResource(info, resource);
        has_reflected_resources = true;
      }
    }
  }

  if (!has_reflected_resources && parser.resourceDef()) {
    const auto &resource_def = parser.resourceDef();
    for (const auto &resource : resource_def->resources)
      AppendReflectionResourceDef(info, resource);
    has_reflected_resources = !resource_def->resources.empty();
  }

  if (const auto &psv = parser.pipelineStateValidation()) {
    if (info.entry_point_name.empty())
      info.entry_point_name = psv->entry_function_name;
    info.uses_view_id = info.uses_view_id || psv->uses_view_id;
    if (psv->has_runtime_info_1) {
      info.shader_kind = psv->shader_stage;
      info.shader_stage_name = PsvShaderKindName(psv->shader_stage);
    }
    if (psv->has_runtime_info_2 &&
        (psv->num_threads_x || psv->num_threads_y || psv->num_threads_z)) {
      info.num_threads_x = psv->num_threads_x;
      info.num_threads_y = psv->num_threads_y;
      info.num_threads_z = psv->num_threads_z;
    }
    if (psv->has_runtime_info_4)
      info.group_shared_bytes_used = psv->num_bytes_group_shared_memory;

    if (!has_reflected_resources) {
      for (uint32_t i = 0; i < psv->resources.size(); i++)
        AppendReflectionPsvResource(info, psv->resources[i], i);
    }
    info.psv_input_signature = psv->input_signature_elements;
    info.psv_output_signature = psv->output_signature_elements;
    info.psv_patch_constant_or_primitive_signature =
        psv->patch_constant_or_primitive_signature_elements;
  }

  return ParseStatus::Ok;
}

ParseStatus
BuildDxilTranslationInfo(const Parser &parser, DxilTranslationInfo &info) {
  info = DxilTranslationInfo();

  const auto &reflection = parser.shaderReflection();
  if (!reflection)
    return ParseStatus::Ok;

  info.valid = reflection->valid;
  info.entry_point_name = reflection->entry_point_name;
  info.function_name = reflection->function_name;
  info.shader_model_kind = reflection->shader_model_kind;
  info.shader_stage_name = reflection->shader_stage_name;
  info.shader_kind = reflection->shader_kind;
  info.shader_model_major = reflection->shader_model_major;
  info.shader_model_minor = reflection->shader_model_minor;
  info.dxil_major = reflection->dxil_major;
  info.dxil_minor = reflection->dxil_minor;
  info.has_runtime_data = reflection->has_runtime_data;
  info.has_pipeline_state_validation =
      reflection->has_pipeline_state_validation;
  info.has_resource_def = reflection->has_resource_def;
  info.has_root_signature = reflection->has_root_signature;
  info.root_signature_offset = reflection->root_signature_offset;
  info.root_signature = reflection->root_signature;
  info.uses_view_id = reflection->uses_view_id;
  info.num_threads_x = reflection->num_threads_x;
  info.num_threads_y = reflection->num_threads_y;
  info.num_threads_z = reflection->num_threads_z;
  info.group_shared_bytes_used = reflection->group_shared_bytes_used;
  info.feature_flags = reflection->feature_flags;
  info.min_shader_target = reflection->min_shader_target;
  info.shader_flags = reflection->shader_flags;

  const LlvmModuleInfo *module = nullptr;
  const DxilEntryPointInfo *metadata_entry = nullptr;
  if (const auto &llvm_module = parser.llvmModule()) {
    module = &*llvm_module;
    metadata_entry = FindReflectionEntryPoint(*module);
    info.has_metadata = !module->entry_points.empty() ||
                        !module->resources.empty() ||
                        !module->signature_elements.empty();
    AppendTranslationIrInfo(info, *module);
  }

  info.resources.reserve(reflection->resources.size());
  for (const auto &resource : reflection->resources)
    info.resources.push_back(BuildTranslationResource(resource));
  for (auto &resource : info.resources) {
    for (const auto &operation : info.operations)
      ApplyOperationToTranslationResource(resource, operation);
  }

  uint32_t signature_source = DxilTranslationSourceMetadata;
  if (parser.runtimeData()) {
    const auto *rdat_function = FindReflectionRdatFunction(
        *parser.runtimeData(), info.entry_point_name, info.function_name);
    const auto *rdat_shader_info =
        rdat_function ? parser.runtimeData()->findShaderInfo(
                            rdat_function->shader_info_table_type,
                            rdat_function->shader_info_index)
                      : nullptr;
    if (rdat_shader_info &&
        (!rdat_shader_info->input_signature_indices.empty() ||
         !rdat_shader_info->output_signature_indices.empty() ||
         !rdat_shader_info->patch_constant_signature_indices.empty() ||
         !rdat_shader_info->primitive_signature_indices.empty())) {
      signature_source = DxilTranslationSourceRuntimeData;
    }
  }

  const auto empty_metadata =
      std::vector<DxilMetadataSignatureElementInfo>();
  const auto &metadata_inputs =
      metadata_entry ? metadata_entry->input_signature : empty_metadata;
  const auto &metadata_outputs =
      metadata_entry ? metadata_entry->output_signature : empty_metadata;
  const auto &metadata_patch_constants =
      metadata_entry ? metadata_entry->patch_constant_signature : empty_metadata;

  AppendTranslationSignatures(info, DxilTranslationSignatureKind::Input,
                              reflection->input_signature, signature_source,
                              metadata_inputs);
  AppendTranslationSignatures(info, DxilTranslationSignatureKind::Output,
                              reflection->output_signature, signature_source,
                              metadata_outputs);
  AppendTranslationSignatures(info, DxilTranslationSignatureKind::PatchConstant,
                              reflection->patch_constant_signature,
                              signature_source, metadata_patch_constants);
  AppendTranslationSignatures(info, DxilTranslationSignatureKind::Primitive,
                              reflection->primitive_signature,
                              signature_source, empty_metadata);

  if (!HasTranslationSignatureKind(info, DxilTranslationSignatureKind::Input))
    AppendTranslationMetadataSignatures(info, DxilTranslationSignatureKind::Input,
                                        metadata_inputs);
  if (!HasTranslationSignatureKind(info, DxilTranslationSignatureKind::Output))
    AppendTranslationMetadataSignatures(info, DxilTranslationSignatureKind::Output,
                                        metadata_outputs);
  if (!HasTranslationSignatureKind(
          info, DxilTranslationSignatureKind::PatchConstant)) {
    AppendTranslationMetadataSignatures(
        info, DxilTranslationSignatureKind::PatchConstant,
        metadata_patch_constants);
  }

  if (!HasTranslationSignatureKind(info, DxilTranslationSignatureKind::Input)) {
    AppendTranslationPsvSignatures(info, DxilTranslationSignatureKind::Input,
                                   reflection->psv_input_signature);
  }
  if (!HasTranslationSignatureKind(info, DxilTranslationSignatureKind::Output)) {
    AppendTranslationPsvSignatures(info, DxilTranslationSignatureKind::Output,
                                   reflection->psv_output_signature);
  }
  const auto psv_patch_or_primitive_kind =
      info.shader_kind == 13 ? DxilTranslationSignatureKind::Primitive
                             : DxilTranslationSignatureKind::PatchConstant;
  if (!HasTranslationSignatureKind(info, psv_patch_or_primitive_kind)) {
    AppendTranslationPsvSignatures(
        info, psv_patch_or_primitive_kind,
        reflection->psv_patch_constant_or_primitive_signature);
  }

  for (const auto &signature : reflection->legacy_signatures) {
    const auto kind = LegacySignatureKind(signature.part_fourcc);
    if (HasTranslationSignatureKind(info, kind))
      continue;
    for (const auto &element : signature.elements)
      info.signatures.push_back(
          BuildTranslationLegacySignatureElement(kind, element));
  }

  return ParseStatus::Ok;
}

ParseStatus
ValidateDxil(const Parser &parser, DxilValidationInfo &info) {
  info = {};
  ValidateContainerInfo(parser, info);
  ValidateProgramInfo(parser, info);
  ValidateModuleMetadata(parser, info);
  ValidateLlvmFlow(parser, info);
  ValidateDxilOperations(parser, info);
  ValidateRuntimeDataInfo(parser, info);
  ValidatePipelineStateValidationInfo(parser, info);
  ValidateReflectionConsistency(parser, info);
  info.valid = !info.has_errors();
  return ParseStatus::Ok;
}

const char *
DxilValidationSeverityName(DxilValidationSeverity severity) {
  switch (severity) {
  case DxilValidationSeverity::Info:
    return "info";
  case DxilValidationSeverity::Warning:
    return "warning";
  case DxilValidationSeverity::Error:
    return "error";
  default:
    return "unknown";
  }
}

const char *
DxilValidationCategoryName(DxilValidationCategory category) {
  switch (category) {
  case DxilValidationCategory::Container:
    return "container";
  case DxilValidationCategory::Program:
    return "program";
  case DxilValidationCategory::Metadata:
    return "metadata";
  case DxilValidationCategory::RuntimeData:
    return "runtime-data";
  case DxilValidationCategory::PipelineStateValidation:
    return "pipeline-state-validation";
  case DxilValidationCategory::Instruction:
    return "instruction";
  case DxilValidationCategory::Reflection:
    return "reflection";
  default:
    return "unknown";
  }
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
