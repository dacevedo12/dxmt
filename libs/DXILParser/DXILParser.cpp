// Copyright (c) 2026 GameSir Labs and contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "DXILParser/DXILParser.hpp"

#if DXILPARSER_USE_LLVM
#include <llvm/ADT/StringRef.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
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
    function_info.is_declaration = function.isDeclaration();
    function_info.is_dx_intrinsic = function.getName().startswith("dx.op.");
    function_info.argument_types.reserve(function.arg_size());
    for (const auto &argument : function.args())
      function_info.argument_types.push_back(TypeString(argument.getType()));

    if (!function.isDeclaration()) {
      for (const auto &block : function)
        function_info.instruction_count += uint32_t(block.size());
    }

    info.functions.push_back(std::move(function_info));
  }

  info.globals.reserve(module->global_size());
  for (const auto &global : module->globals()) {
    info.globals.push_back({
        .name = global.getName().str(),
        .value_type = TypeString(global.getValueType()),
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
