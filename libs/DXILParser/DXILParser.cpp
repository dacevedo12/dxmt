// Copyright (c) 2026 GameSir Labs and contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "DXILParser/DXILParser.hpp"
#include "DXILParser/DXILOpcodeInfo.hpp"

#if DXILPARSER_USE_LLVM
#include <llvm/ADT/StringRef.h>
#include <llvm/Bitcode/BitcodeReader.h>
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
#include <limits>
#include <sstream>
#include <utility>

namespace dxmt::dxil {

namespace {

constexpr size_t kContainerHeaderSize = 32;
constexpr size_t kPartHeaderSize = 8;
constexpr size_t kDxilProgramHeaderSize = 24;
constexpr size_t kDxilBitcodeHeaderOffset = 8;
constexpr size_t kDxilSignatureHeaderSize = 8;
constexpr size_t kDxilSignatureElementSize = 32;
constexpr size_t kFeatureInfoSize = 8;
constexpr size_t kShaderHashSize = 20;
constexpr size_t kCompilerVersionHeaderSize = 16;
constexpr size_t kShaderDebugNameHeaderSize = 4;
constexpr size_t kSourceInfoHeaderSize = 8;
constexpr size_t kSourceInfoSectionHeaderSize = 8;
constexpr size_t kShaderPdbInfoHeaderSize = 12;
constexpr size_t kResourceDefHeaderSize = 28;
constexpr size_t kResourceDefConstantBufferSize = 24;
constexpr size_t kResourceDefResourceBindingSize = 32;
constexpr size_t kResourceDefResourceBindingExtendedSize = 40;
constexpr size_t kRuntimeDataHeaderSize = 8;
constexpr size_t kRuntimeDataPartHeaderSize = 8;
constexpr size_t kRuntimeDataTableHeaderSize = 8;
constexpr uint32_t kRdatNullRef = 0xffffffffu;
constexpr size_t kRdatResourceRecordSize = 32;
constexpr size_t kRdatFunctionRecordSize = 44;
constexpr size_t kRdatFunctionRecord2Size = 52;
constexpr size_t kRdatSubobjectRecordSize = 24;
constexpr size_t kRdatNodeIdRecordSize = 8;
constexpr size_t kRdatNodeShaderFuncAttribRecordSize = 8;
constexpr size_t kRdatNodeShaderIoAttribRecordSize = 8;
constexpr size_t kRdatIoNodeRecordSize = 8;
constexpr size_t kRdatNodeShaderInfoRecordSize = 20;
constexpr size_t kRdatPdbInfoSourceRecordSize = 8;
constexpr size_t kRdatPdbInfoLibraryRecordSize = 12;
constexpr size_t kRdatPdbInfoRecordSize = 44;
constexpr size_t kRdatSignatureElementRecordPayloadSize = 14;
constexpr size_t kRdatVSInfoRecordSize = 16;
constexpr size_t kRdatPSInfoRecordSize = 8;
constexpr size_t kRdatHSInfoRecordSize = 48;
constexpr size_t kRdatDSInfoRecordSize = 38;
constexpr size_t kRdatGSInfoRecordSize = 28;
constexpr size_t kRdatCSInfoRecordSize = 8;
constexpr size_t kRdatMSInfoRecordSize = 45;
constexpr size_t kRdatASInfoRecordSize = 12;
constexpr size_t kPsvRuntimeInfo1Size = 36;
constexpr size_t kPsvRuntimeInfo2Size = 48;
constexpr size_t kPsvRuntimeInfo3Size = 52;
constexpr size_t kPsvRuntimeInfo4Size = 56;
constexpr size_t kPsvResourceBindInfo0Size = 16;
constexpr size_t kPsvResourceBindInfo1Size = 24;
constexpr size_t kPsvSignatureElement0Size = 16;
constexpr uint32_t kDxilMagicValue = MakeFourCC('D', 'X', 'I', 'L');
constexpr uint32_t kBitcodeMagicValue = uint32_t('B') | (uint32_t('C') << 8) |
                                        (uint32_t(0xc0) << 16) |
                                        (uint32_t(0xde) << 24);
constexpr uint32_t kBitcodeWrapperMagicValue = 0x0b17c0de;
constexpr size_t kBitcodeWrapperHeaderSize = 20;

namespace bitc {
constexpr uint32_t EndBlock = 0;
constexpr uint32_t EnterSubblock = 1;
constexpr uint32_t DefineAbbrev = 2;
constexpr uint32_t UnabbrevRecord = 3;
constexpr uint32_t FirstApplicationAbbrev = 4;
constexpr uint32_t BlockInfoBlockId = 0;
constexpr uint32_t BlockInfoSetBid = 1;
} // namespace bitc

namespace rdat {
constexpr uint32_t StringBuffer = 1;
constexpr uint32_t IndexArrays = 2;
constexpr uint32_t ResourceTable = 3;
constexpr uint32_t FunctionTable = 4;
constexpr uint32_t RawBytes = 5;
constexpr uint32_t SubobjectTable = 6;
constexpr uint32_t NodeIDTable = 7;
constexpr uint32_t NodeShaderIOAttribTable = 8;
constexpr uint32_t NodeShaderFuncAttribTable = 9;
constexpr uint32_t IONodeTable = 10;
constexpr uint32_t NodeShaderInfoTable = 11;
constexpr uint32_t SignatureElementTable = 13;
constexpr uint32_t VSInfoTable = 14;
constexpr uint32_t PSInfoTable = 15;
constexpr uint32_t HSInfoTable = 16;
constexpr uint32_t DSInfoTable = 17;
constexpr uint32_t GSInfoTable = 18;
constexpr uint32_t CSInfoTable = 19;
constexpr uint32_t MSInfoTable = 20;
constexpr uint32_t ASInfoTable = 21;
constexpr uint32_t DxilPdbInfoTable = 0x10001;
constexpr uint32_t DxilPdbInfoSourceTable = 0x10002;
constexpr uint32_t DxilPdbInfoLibraryTable = 0x10003;
} // namespace rdat

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
  if (const auto *called = call->getCalledFunction())
    info.called_function = called->getName().str();
  else
    info.called_function = ValueOperandText(call->getCalledOperand());

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
        }
      }
      if (entry->getNumOperands() > 3) {
        if (const auto *resources = llvm::dyn_cast_or_null<llvm::MDNode>(
                entry->getOperand(3).get())) {
          entry_info.has_resources = true;
          entry_info.resource_operand_count = resources->getNumOperands();
        }
      }
      if (entry->getNumOperands() > 4) {
        if (const auto *properties = llvm::dyn_cast_or_null<llvm::MDNode>(
                entry->getOperand(4).get())) {
          entry_info.has_properties = true;
          entry_info.property_operand_count = properties->getNumOperands();
          entry_info.properties = MetadataUInt32List(properties);
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

  // Dependency masks/tables are PSV-version and shader-stage dependent. Keep
  // them available as an opaque slice until the PSO validator consumes them.
  info.dependency_payload = std::span<const uint8_t>(data.data() + offset,
                                                    data.size() - offset);
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
              rdat_function->shader_info_index))
        ApplyRdatShaderInfo(info, *runtime_data, *shader_info);
    }

    if (rdat_function && !rdat_function->resource_indices.empty()) {
      for (const auto resource_index : rdat_function->resource_indices)
        AppendReflectionRdatResource(
            info, runtime_data->resources[resource_index]);
    } else {
      for (const auto &resource : runtime_data->resources)
        AppendReflectionRdatResource(info, resource);
    }
  }

  if (const auto &resource_def = parser.resourceDef()) {
    for (const auto &resource : resource_def->resources)
      AppendReflectionResourceDef(info, resource);
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

    for (uint32_t i = 0; i < psv->resources.size(); i++)
      AppendReflectionPsvResource(info, psv->resources[i], i);
    info.psv_input_signature = psv->input_signature_elements;
    info.psv_output_signature = psv->output_signature_elements;
    info.psv_patch_constant_or_primitive_signature =
        psv->patch_constant_or_primitive_signature_elements;
  }

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
