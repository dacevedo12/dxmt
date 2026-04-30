// Copyright (c) 2026 GameSir Labs and contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DXILParser/DXILParser.hpp"

#include <cstddef>
#include <cstdint>

namespace dxmt::dxil {

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

} // namespace dxmt::dxil
