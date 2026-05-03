#include "dxil_air_converter.hpp"

#include "air_signature.hpp"
#include "air_type.hpp"
#include "air_operations.hpp"
#include "airconv_context.hpp"
#include "airconv_error.hpp"
#include "dxbc_converter.hpp"
#include "metallib_writer.hpp"
#include "nt/air_builder.hpp"
#include "shader_common.hpp"

#include "llvm/ADT/APFloat.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dxmt::airconv {
namespace {

using namespace llvm;

enum class DxilStage {
  Pixel = 0,
  Vertex = 1,
  Geometry = 2,
  Hull = 3,
  Domain = 4,
  Compute = 5,
};

struct DxilResourceHandle {
  dxil::DxilTranslationResourceClass resource_class =
      dxil::DxilTranslationResourceClass::Unknown;
  uint32_t range_id = 0;
  Value *index = nullptr;
  bool non_uniform = false;
};

struct DxilAirContext {
  LLVMContext &llvm;
  Module &module;
  IRBuilder<> &builder;
  llvm::air::AIRBuilder &air;
  air::AirType &types;
  const dxil::DxilTranslationInfo &translation;
  const Function *source_function = nullptr;
  Function *function = nullptr;

  std::unordered_map<const Value *, Value *> values;
  std::unordered_map<const GlobalVariable *, GlobalVariable *> globals;
  std::unordered_map<const BasicBlock *, BasicBlock *> blocks;
  std::unordered_map<const Value *, DxilResourceHandle> handles;
  std::map<uint32_t, uint32_t> input_args;
  std::map<uint32_t, uint32_t> output_indices;
  std::map<uint32_t, const dxil::DxilTranslationSignatureElementInfo *> inputs;
  std::map<uint32_t, const dxil::DxilTranslationSignatureElementInfo *> outputs;
  std::map<uint32_t, SM50_IA_INPUT_ELEMENT> vertex_inputs;
  dxbc::ShaderInfo shader_info;
  dxbc::io_binding_map resources;
  uint32_t vertex_buffer_table_arg = UINT32_MAX;
  uint32_t vertex_slot_mask = 0;
  uint32_t vertex_id_arg = UINT32_MAX;
  uint32_t base_vertex_arg = UINT32_MAX;
  uint32_t instance_id_arg = UINT32_MAX;
  uint32_t base_instance_arg = UINT32_MAX;
  AllocaInst *return_value = nullptr;
  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
};

llvm::Error
Unsupported(std::string message) {
  return llvm::make_error<UnsupportedFeature>(message);
}

const char *
ResourceClassName(dxil::DxilTranslationResourceClass resource_class) {
  switch (resource_class) {
  case dxil::DxilTranslationResourceClass::Srv:
    return "SRV";
  case dxil::DxilTranslationResourceClass::Uav:
    return "UAV";
  case dxil::DxilTranslationResourceClass::Cbv:
    return "CBV";
  case dxil::DxilTranslationResourceClass::Sampler:
    return "Sampler";
  case dxil::DxilTranslationResourceClass::Unknown:
  default:
    return "Unknown";
  }
}

dxil::DxilTranslationResourceClass
ResourceClassFromDxilValue(uint32_t resource_class) {
  switch (resource_class) {
  case 0:
    return dxil::DxilTranslationResourceClass::Srv;
  case 1:
    return dxil::DxilTranslationResourceClass::Uav;
  case 2:
    return dxil::DxilTranslationResourceClass::Cbv;
  case 3:
    return dxil::DxilTranslationResourceClass::Sampler;
  default:
    return dxil::DxilTranslationResourceClass::Unknown;
  }
}

const char *
StageName(uint32_t kind) {
  switch (kind) {
  case uint32_t(DxilStage::Vertex):
    return "vertex";
  case uint32_t(DxilStage::Pixel):
    return "pixel";
  case uint32_t(DxilStage::Compute):
    return "compute";
  case uint32_t(DxilStage::Hull):
    return "hull";
  case uint32_t(DxilStage::Domain):
    return "domain";
  case uint32_t(DxilStage::Geometry):
    return "geometry";
  default:
    return "unknown";
  }
}

std::string
BindingPrefix(dxil::DxilTranslationResourceClass resource_class) {
  switch (resource_class) {
  case dxil::DxilTranslationResourceClass::Cbv:
    return "b";
  case dxil::DxilTranslationResourceClass::Sampler:
    return "s";
  case dxil::DxilTranslationResourceClass::Uav:
    return "u";
  case dxil::DxilTranslationResourceClass::Srv:
    return "t";
  case dxil::DxilTranslationResourceClass::Unknown:
  default:
    return "?";
  }
}

std::string
Lower(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (char ch : value)
    out.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
  return out;
}

std::string
SemanticUserName(const dxil::DxilTranslationSignatureElementInfo &sig) {
  if (!sig.semantic_key.empty())
    return Lower(sig.semantic_key);
  if (!sig.semantic_name.empty())
    return Lower(sig.semantic_name) + std::to_string(sig.semantic_index);
  return "sig" + std::to_string(sig.element_id);
}

uint32_t
SignatureMask(const dxil::DxilTranslationSignatureElementInfo &sig) {
  if (sig.component_mask)
    return sig.component_mask;
  if (sig.component_count)
    return ((1u << sig.component_count) - 1u) << sig.component_start;
  if (sig.cols)
    return (1u << std::min<uint8_t>(sig.cols, 4u)) - 1u;
  return 0xfu;
}

air::InputAttributeComponentType
ToAirInputComponent(uint32_t type) {
  switch (type) {
  case 1:
    return air::InputAttributeComponentType::Uint;
  case 2:
    return air::InputAttributeComponentType::Int;
  case 3:
    return air::InputAttributeComponentType::Float;
  default:
    return air::InputAttributeComponentType::Unknown;
  }
}

air::MSLScalerOrVectorType
ToAirSignatureType(uint32_t component_type, uint32_t mask) {
  const auto components = std::max(1u, std::min(4u, uint32_t(std::popcount(mask & 0xfu))));
  const bool is_int = component_type == 1 || component_type == 2;
  const bool is_signed = component_type == 2;
  if (components == 1)
    return is_int ? (is_signed ? air::MSLScalerOrVectorType(air::msl_int)
                               : air::MSLScalerOrVectorType(air::msl_uint))
                  : air::MSLScalerOrVectorType(air::msl_float);
  air::MSLScalerType scalar =
      is_int ? (is_signed ? air::MSLScalerType(air::msl_int)
                          : air::MSLScalerType(air::msl_uint))
             : air::MSLScalerType(air::msl_float);
  return air::MSLVector{components, scalar};
}

air::Interpolation
ToAirInterpolation(uint32_t mode) {
  switch (mode) {
  case 1:
    return air::Interpolation::flat;
  case 3:
    return air::Interpolation::center_no_perspective;
  case 4:
    return air::Interpolation::centroid_perspective;
  case 5:
    return air::Interpolation::centroid_no_perspective;
  case 6:
    return air::Interpolation::sample_perspective;
  case 7:
    return air::Interpolation::sample_no_perspective;
  case 2:
  default:
    return air::Interpolation::center_perspective;
  }
}

shader::common::ScalerDataType
ToScalerDataType(uint32_t return_type) {
  switch (return_type) {
  case 2:
    return shader::common::ScalerDataType::Uint;
  case 3:
    return shader::common::ScalerDataType::Int;
  default:
    return shader::common::ScalerDataType::Float;
  }
}

shader::common::ResourceType
ToResourceType(uint32_t resource_kind, uint32_t dimension) {
  switch (resource_kind) {
  case 1:
    return shader::common::ResourceType::Texture1D;
  case 2:
    return shader::common::ResourceType::Texture2D;
  case 3:
    return shader::common::ResourceType::Texture2DMultisampled;
  case 4:
    return shader::common::ResourceType::Texture3D;
  case 5:
    return shader::common::ResourceType::TextureCube;
  case 6:
    return shader::common::ResourceType::Texture1DArray;
  case 7:
    return shader::common::ResourceType::Texture2DArray;
  case 8:
    return shader::common::ResourceType::Texture2DMultisampledArray;
  case 9:
    return shader::common::ResourceType::TextureCubeArray;
  case 10:
    return shader::common::ResourceType::TextureBuffer;
  case 11:
  case 12:
  case 13:
  case 14:
  case 15:
  case 16:
  case 17:
  case 18:
    return shader::common::ResourceType::NonApplicable;
  default:
    break;
  }

  switch (dimension) {
  case 1:
    return shader::common::ResourceType::TextureBuffer;
  case 2:
    return shader::common::ResourceType::Texture1D;
  case 3:
    return shader::common::ResourceType::Texture2D;
  case 4:
    return shader::common::ResourceType::Texture2DMultisampled;
  case 5:
    return shader::common::ResourceType::Texture3D;
  case 6:
    return shader::common::ResourceType::TextureCube;
  case 7:
    return shader::common::ResourceType::Texture1DArray;
  case 8:
    return shader::common::ResourceType::Texture2DArray;
  case 9:
    return shader::common::ResourceType::Texture2DMultisampledArray;
  case 10:
    return shader::common::ResourceType::TextureCubeArray;
  default:
    return shader::common::ResourceType::NonApplicable;
  }
}

bool
IsSystemSemantic(const dxil::DxilTranslationSignatureElementInfo &sig,
                 std::string_view name) {
  return Lower(sig.semantic_key) == Lower(name) ||
         Lower(sig.semantic_name) == Lower(name);
}

Type *
MapType(Type *source, DxilAirContext &ctx);

Value *
MapConstant(const Constant *constant, DxilAirContext &ctx) {
  if (const auto *global = dyn_cast<GlobalVariable>(constant)) {
    auto found = ctx.globals.find(global);
    if (found != ctx.globals.end())
      return found->second;
  }
  if (const auto *integer = dyn_cast<ConstantInt>(constant))
    return ConstantInt::get(MapType(integer->getType(), ctx),
                            integer->getValue());
  if (const auto *fp = dyn_cast<ConstantFP>(constant))
    return ConstantFP::get(MapType(fp->getType(), ctx), fp->getValueAPF());
  if (isa<UndefValue>(constant))
    return UndefValue::get(MapType(constant->getType(), ctx));
  if (const auto *aggregate = dyn_cast<ConstantAggregateZero>(constant))
    return ConstantAggregateZero::get(MapType(aggregate->getType(), ctx));
  if (const auto *vector = dyn_cast<ConstantDataVector>(constant)) {
    std::vector<Constant *> elements;
    for (uint32_t i = 0; i < vector->getNumElements(); i++)
      elements.push_back(cast<Constant>(MapConstant(vector->getElementAsConstant(i), ctx)));
    return ConstantVector::get(elements);
  }
  if (const auto *constant_vector = dyn_cast<ConstantVector>(constant)) {
    std::vector<Constant *> elements;
    for (uint32_t i = 0; i < constant_vector->getNumOperands(); i++)
      elements.push_back(cast<Constant>(MapConstant(constant_vector->getOperand(i), ctx)));
    return ConstantVector::get(elements);
  }
  if (const auto *constant_expr = dyn_cast<ConstantExpr>(constant)) {
    if (constant_expr->getOpcode() == Instruction::GetElementPtr) {
      SmallVector<Value *, 8> indices;
      for (uint32_t i = 1; i < constant_expr->getNumOperands(); i++)
        indices.push_back(MapConstant(constant_expr->getOperand(i), ctx));
      auto *base = MapConstant(constant_expr->getOperand(0), ctx);
      auto *base_type = cast<PointerType>(base->getType())
                            ->getNonOpaquePointerElementType();
      return ctx.builder.CreateGEP(
          base_type, base, indices);
    }
    if (constant_expr->isCast())
      return ctx.builder.CreateCast(
          static_cast<Instruction::CastOps>(constant_expr->getOpcode()),
          MapConstant(constant_expr->getOperand(0), ctx),
          MapType(constant_expr->getType(), ctx));
  }
  return UndefValue::get(MapType(constant->getType(), ctx));
}

Type *
MapType(Type *source, DxilAirContext &ctx) {
  if (source->isVoidTy())
    return Type::getVoidTy(ctx.llvm);
  if (source->isIntegerTy())
    return IntegerType::get(ctx.llvm, source->getIntegerBitWidth());
  if (source->isHalfTy())
    return Type::getHalfTy(ctx.llvm);
  if (source->isFloatTy())
    return Type::getFloatTy(ctx.llvm);
  if (source->isDoubleTy())
    return Type::getDoubleTy(ctx.llvm);
  if (const auto *vector = dyn_cast<FixedVectorType>(source))
    return FixedVectorType::get(MapType(vector->getElementType(), ctx),
                                vector->getNumElements());
  if (const auto *array = dyn_cast<ArrayType>(source))
    return ArrayType::get(MapType(array->getElementType(), ctx),
                          array->getNumElements());
  if (const auto *structure = dyn_cast<StructType>(source)) {
    if (structure->isOpaque())
      return StructType::create(ctx.llvm, structure->getName());
    std::vector<Type *> elements;
    for (auto *element : structure->elements())
      elements.push_back(MapType(element, ctx));
    return StructType::get(ctx.llvm, elements, structure->isPacked());
  }
  if (const auto *pointer = dyn_cast<PointerType>(source)) {
    if (pointer->isOpaque())
      return PointerType::get(ctx.llvm, pointer->getAddressSpace());
    return MapType(pointer->getNonOpaquePointerElementType(), ctx)
        ->getPointerTo(pointer->getAddressSpace());
  }
  return Type::getVoidTy(ctx.llvm);
}

Value *
ZeroValue(Type *type) {
  if (type->isVoidTy())
    return nullptr;
  return Constant::getNullValue(type);
}

Value *
MapValue(const Value *source, DxilAirContext &ctx) {
  if (!source)
    return nullptr;
  if (const auto *global = dyn_cast<GlobalVariable>(source)) {
    auto found = ctx.globals.find(global);
    if (found != ctx.globals.end())
      return found->second;
  }
  if (const auto *constant = dyn_cast<Constant>(source))
    return MapConstant(constant, ctx);
  auto found = ctx.values.find(source);
  if (found != ctx.values.end())
    return found->second;
  return UndefValue::get(MapType(source->getType(), ctx));
}

Value *
CastValue(Value *value, Type *type, DxilAirContext &ctx) {
  if (!value || value->getType() == type)
    return value;
  if (value->getType()->isPointerTy() && type->isPointerTy())
    return ctx.builder.CreatePointerCast(value, type);
  if (value->getType()->isIntegerTy() && type->isIntegerTy())
    return ctx.builder.CreateZExtOrTrunc(value, type);
  if (value->getType()->isFloatingPointTy() && type->isFloatingPointTy())
    return ctx.builder.CreateFPCast(value, type);
  if (value->getType()->isIntegerTy() && type->isFloatingPointTy())
    return ctx.builder.CreateUIToFP(value, type);
  if (value->getType()->isFloatingPointTy() && type->isIntegerTy())
    return ctx.builder.CreateFPToUI(value, type);
  if (value->getType()->isVectorTy() && type->isVectorTy())
    return ctx.builder.CreateBitCast(value, type);
  return ctx.builder.CreateBitCast(value, type);
}

dxil::DxilTranslationResourceClass
DxilResourceClassFromOperand(uint32_t value) {
  switch (value) {
  case 0:
    return dxil::DxilTranslationResourceClass::Srv;
  case 1:
    return dxil::DxilTranslationResourceClass::Uav;
  case 2:
    return dxil::DxilTranslationResourceClass::Cbv;
  case 3:
    return dxil::DxilTranslationResourceClass::Sampler;
  default:
    return dxil::DxilTranslationResourceClass::Unknown;
  }
}

Value *
SplatOrExtract(Value *value, uint32_t component, DxilAirContext &ctx) {
  if (!value)
    return nullptr;
  if (auto *vector = dyn_cast<FixedVectorType>(value->getType())) {
    const auto index = std::min<uint32_t>(component, vector->getNumElements() - 1);
    return ctx.builder.CreateExtractElement(value, index);
  }
  return value;
}

Value *
BuildVector4(std::array<Value *, 4> values, DxilAirContext &ctx) {
  Type *element_type = nullptr;
  for (auto *value : values) {
    if (value) {
      element_type = value->getType();
      break;
    }
  }
  if (!element_type)
    element_type = ctx.types._int;
  Value *out = UndefValue::get(FixedVectorType::get(element_type, 4));
  for (uint32_t i = 0; i < 4; i++) {
    auto *value = values[i] ? CastValue(values[i], element_type, ctx)
                            : Constant::getNullValue(element_type);
    out = ctx.builder.CreateInsertElement(out, value, i);
  }
  return out;
}

Value *
BitcastOrCastValue(Value *value, Type *type, DxilAirContext &ctx) {
  if (!value || value->getType() == type)
    return value;
  if ((value->getType()->isIntegerTy() && type->isFloatingPointTy() &&
       value->getType()->getIntegerBitWidth() ==
           type->getPrimitiveSizeInBits()) ||
      (value->getType()->isFloatingPointTy() && type->isIntegerTy() &&
       value->getType()->getPrimitiveSizeInBits() ==
           type->getIntegerBitWidth()))
    return ctx.builder.CreateBitCast(value, type);
  return CastValue(value, type, ctx);
}

Value *
PackDxilReturn(const CallBase &call, Value *value, DxilAirContext &ctx,
               Value *status = nullptr) {
  auto *return_type = MapType(call.getType(), ctx);
  if (return_type->isVoidTy())
    return nullptr;
  if (!value)
    return ZeroValue(return_type);
  if (return_type == value->getType())
    return value;
  if (auto *structure = dyn_cast<StructType>(return_type)) {
    Value *out = UndefValue::get(structure);
    const uint32_t elements = structure->getNumElements();
    if (auto *vector = dyn_cast<FixedVectorType>(value->getType())) {
      for (uint32_t i = 0; i < std::min<uint32_t>(elements, vector->getNumElements()); i++) {
        auto *component = ctx.builder.CreateExtractElement(value, i);
        out = ctx.builder.CreateInsertValue(
            out, BitcastOrCastValue(component, structure->getElementType(i), ctx), i);
      }
    } else {
      out = ctx.builder.CreateInsertValue(
          out, BitcastOrCastValue(value, structure->getElementType(0), ctx), 0);
    }
    if (status && elements > 4)
      out = ctx.builder.CreateInsertValue(
          out, CastValue(status, structure->getElementType(4), ctx), 4);
    return out;
  }
  return CastValue(value, return_type, ctx);
}

std::optional<uint32_t>
ConstantOperandU32(const CallBase &call, uint32_t index) {
  if (index >= call.arg_size())
    return std::nullopt;
  if (const auto *integer = dyn_cast<ConstantInt>(call.getArgOperand(index)))
    return uint32_t(integer->getZExtValue());
  return std::nullopt;
}

std::optional<uint32_t>
ConstantAggregateElementU32(const Value *value, uint32_t index) {
  const auto *constant = dyn_cast_or_null<Constant>(value);
  if (!constant)
    return std::nullopt;
  const auto *element = constant->getAggregateElement(index);
  const auto *integer = dyn_cast_or_null<ConstantInt>(element);
  if (!integer)
    return std::nullopt;
  return uint32_t(integer->getZExtValue());
}

Value *
OperandValue(const CallBase &call, uint32_t index, DxilAirContext &ctx) {
  if (index >= call.arg_size())
    return nullptr;
  return MapValue(call.getArgOperand(index), ctx);
}

const dxil::DxilTranslationResourceInfo *
FindResource(const dxil::DxilTranslationInfo &translation,
             dxil::DxilTranslationResourceClass resource_class,
             uint32_t id) {
  auto found = std::find_if(
      translation.resources.begin(), translation.resources.end(),
      [&](const dxil::DxilTranslationResourceInfo &resource) {
        return resource.resource_class == resource_class && resource.id == id;
      });
  return found == translation.resources.end() ? nullptr : &*found;
}

const dxil::DxilTranslationResourceInfo *
FindResourceByBinding(const dxil::DxilTranslationInfo &translation,
                      dxil::DxilTranslationResourceClass resource_class,
                      uint32_t space, uint32_t lower_bound) {
  auto found = std::find_if(
      translation.resources.begin(), translation.resources.end(),
      [&](const dxil::DxilTranslationResourceInfo &resource) {
        return resource.resource_class == resource_class &&
               resource.space == space && resource.lower_bound == lower_bound;
      });
  return found == translation.resources.end() ? nullptr : &*found;
}

std::string
DxilCallName(const CallBase &call);

DxilResourceHandle
ResolveHandle(const Value *value, DxilAirContext &ctx) {
  if (!value)
    return {};
  auto found = ctx.handles.find(value);
  if (found != ctx.handles.end())
    return found->second;
  return {};
}

std::string
FormatResourceBinding(const dxil::DxilTranslationResourceInfo &resource) {
  const auto upper_bound =
      resource.unbounded ? std::string("unbounded")
                         : std::to_string(resource.upper_bound);
  return std::string(ResourceClassName(resource.resource_class)) +
         " id=" + std::to_string(resource.id) + " binding=" +
         BindingPrefix(resource.resource_class) +
         std::to_string(resource.lower_bound) + ".." + upper_bound +
         " space" + std::to_string(resource.space);
}

uint32_t
AllocateDxilBindingSlot(std::set<uint32_t> &used_slots, uint32_t preferred_slot) {
  if (used_slots.insert(preferred_slot).second)
    return preferred_slot;

  uint32_t slot = 0;
  while (!used_slots.insert(slot).second)
    slot++;
  return slot;
}

std::string
FormatHandleBinding(const DxilResourceHandle &handle,
                    const DxilAirContext &ctx) {
  const auto *resource =
      FindResource(ctx.translation, handle.resource_class, handle.range_id);
  if (resource)
    return FormatResourceBinding(*resource);
  return std::string(ResourceClassName(handle.resource_class)) +
         " id=" + std::to_string(handle.range_id);
}

llvm::Error
UnsupportedDxilCall(const CallBase &call, std::string_view reason,
                    DxilAirContext &ctx) {
  std::string message = "unsupported DXIL opcode ";
  message += DxilCallName(call);
  message += " stage=";
  message += StageName(ctx.translation.shader_kind);
  if (!reason.empty()) {
    message += ": ";
    message += reason;
  }

  if (call.arg_size() > 1) {
    const auto handle = ResolveHandle(call.getArgOperand(1), ctx);
    if (handle.resource_class != dxil::DxilTranslationResourceClass::Unknown) {
      message += " resource={";
      message += FormatHandleBinding(handle, ctx);
      message += "}";
    }
  }
  return Unsupported(std::move(message));
}

std::string
DxilCallName(const CallBase &call) {
  if (auto opcode = ConstantOperandU32(call, 0))
    return dxil::DxilOpcodeName(*opcode);

  auto *called = call.getCalledFunction();
  if (!called || !called->getName().startswith("dx.op."))
    return {};

  std::string name = called->getName().str().substr(strlen("dx.op."));
  if (auto dot = name.find('.'); dot != std::string::npos)
    name = name.substr(0, dot);
  if (!name.empty())
    name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
  return name;
}

llvm::air::Texture
BuildTexture(const dxil::DxilTranslationResourceInfo &resource,
             air::MemoryAccess access) {
  auto resource_type = ToResourceType(resource.resource_kind, resource.dimension);
  auto texture_kind = air::to_air_resource_type(resource_type);
  auto scaler = ToScalerDataType(resource.return_type);
  llvm::air::Texture::SampleType sample_type = llvm::air::Texture::sample_float;
  if (scaler == shader::common::ScalerDataType::Uint)
    sample_type = llvm::air::Texture::sample_uint;
  else if (scaler == shader::common::ScalerDataType::Int)
    sample_type = llvm::air::Texture::sample_int;
  llvm::air::Texture::MemoryAccess memory_access = llvm::air::Texture::access_read;
  if (access == air::MemoryAccess::sample)
    memory_access = llvm::air::Texture::access_sample;
  else if (access == air::MemoryAccess::write)
    memory_access = llvm::air::Texture::access_write;
  else if (access == air::MemoryAccess::read_write)
    memory_access = llvm::air::Texture::acesss_readwrite;
  return {
      .kind = texture_kind,
      .sample_type = sample_type,
      .memory_access = memory_access,
  };
}

Value *
GetResourceIndex(const DxilResourceHandle &handle, DxilAirContext &ctx) {
  return handle.index ? handle.index : ctx.builder.getInt32(0);
}

Value *
GetCBufferPointer(const DxilResourceHandle &handle, DxilAirContext &ctx) {
  auto range = ctx.resources.cb_range_map.find(handle.range_id);
  if (range == ctx.resources.cb_range_map.end())
    return nullptr;
  return range->second(GetResourceIndex(handle, ctx)).build(
      dxbc::context{ctx.builder, ctx.air, ctx.llvm, ctx.module, ctx.function,
                    ctx.resources, ctx.types, 0xffffffff,
                    microsoft::D3D10_SB_PIXEL_SHADER, SM50_SHADER_METAL_310})
      .get();
}

Value *
GetBufferPointer(const DxilResourceHandle &handle, bool uav, DxilAirContext &ctx) {
  auto &map = uav ? ctx.resources.uav_buf_range_map : ctx.resources.srv_buf_range_map;
  auto range = map.find(handle.range_id);
  if (range == map.end())
    return nullptr;
  return range->second.resource_id(GetResourceIndex(handle, ctx))
      .build(dxbc::context{ctx.builder, ctx.air, ctx.llvm, ctx.module,
                           ctx.function, ctx.resources, ctx.types, 0xffffffff,
                           microsoft::D3D10_SB_PIXEL_SHADER,
                           SM50_SHADER_METAL_310})
      .get();
}

std::pair<Value *, const dxil::DxilTranslationResourceInfo *>
GetTextureHandle(const DxilResourceHandle &handle, DxilAirContext &ctx) {
  const auto *resource =
      FindResource(ctx.translation, handle.resource_class, handle.range_id);
  if (!resource)
    return {nullptr, nullptr};
  auto &map = handle.resource_class == dxil::DxilTranslationResourceClass::Uav
                  ? ctx.resources.uav_range_map
                  : ctx.resources.srv_range_map;
  auto range = map.find(handle.range_id);
  if (range == map.end())
    return {nullptr, resource};
  return {range->second.resource_id(GetResourceIndex(handle, ctx))
              .build(dxbc::context{ctx.builder, ctx.air, ctx.llvm, ctx.module,
                                   ctx.function, ctx.resources, ctx.types,
                                   0xffffffff, microsoft::D3D10_SB_PIXEL_SHADER,
                                   SM50_SHADER_METAL_310})
              .get(),
          resource};
}

Value *
GetSamplerHandle(const DxilResourceHandle &handle, DxilAirContext &ctx) {
  auto range = ctx.resources.sampler_range_map.find(handle.range_id);
  if (range == ctx.resources.sampler_range_map.end())
    return nullptr;
  return range->second.handle(GetResourceIndex(handle, ctx))
      .build(dxbc::context{ctx.builder, ctx.air, ctx.llvm, ctx.module,
                           ctx.function, ctx.resources, ctx.types, 0xffffffff,
                           microsoft::D3D10_SB_PIXEL_SHADER,
                           SM50_SHADER_METAL_310})
      .get();
}

Value *
ComponentFromOperand(const CallBase &call, uint32_t index, DxilAirContext &ctx) {
  auto *value = OperandValue(call, index, ctx);
  return value ? value : ctx.builder.getInt32(0);
}

Value *
VectorFromOperands(const CallBase &call, uint32_t first, uint32_t count,
                   bool floating, DxilAirContext &ctx) {
  std::array<Value *, 4> values = {};
  for (uint32_t i = 0; i < count; i++) {
    values[i] = ComponentFromOperand(call, first + i, ctx);
    if (floating && values[i]->getType()->isIntegerTy())
      values[i] = ctx.builder.CreateUIToFP(values[i], ctx.types._float);
  }
  auto *vec4 = BuildVector4(values, ctx);
  if (count == 1)
    return ctx.builder.CreateExtractElement(vec4, uint64_t(0));
  SmallVector<int, 3> mask;
  for (uint32_t i = 0; i < count; i++)
    mask.push_back(int(i));
  return ctx.builder.CreateShuffleVector(vec4, mask);
}

uint32_t
TextureCoordinateCount(const dxil::DxilTranslationResourceInfo &resource) {
  auto type = ToResourceType(resource.resource_kind, resource.dimension);
  switch (type) {
  case shader::common::ResourceType::Texture1D:
  case shader::common::ResourceType::Texture1DArray:
  case shader::common::ResourceType::TextureBuffer:
    return 1;
  case shader::common::ResourceType::Texture2D:
  case shader::common::ResourceType::Texture2DArray:
  case shader::common::ResourceType::Texture2DMultisampled:
  case shader::common::ResourceType::Texture2DMultisampledArray:
    return 2;
  case shader::common::ResourceType::Texture3D:
  case shader::common::ResourceType::TextureCube:
  case shader::common::ResourceType::TextureCubeArray:
    return 3;
  default:
    return 3;
  }
}

Value *
TextureArrayIndex(const dxil::DxilTranslationResourceInfo &resource,
                  const CallBase &call, uint32_t first, bool floating,
                  DxilAirContext &ctx) {
  auto type = ToResourceType(resource.resource_kind, resource.dimension);
  uint32_t operand = 0;
  switch (type) {
  case shader::common::ResourceType::Texture1DArray:
    operand = first + 1;
    break;
  case shader::common::ResourceType::Texture2DArray:
  case shader::common::ResourceType::Texture2DMultisampledArray:
    operand = first + 2;
    break;
  case shader::common::ResourceType::TextureCubeArray:
    operand = first + 3;
    break;
  default:
    return nullptr;
  }
  auto *value = ComponentFromOperand(call, operand, ctx);
  if (floating && value->getType()->isFloatingPointTy())
    return ctx.builder.CreateFPToUI(value, ctx.types._int);
  return CastValue(value, ctx.types._int, ctx);
}

Value *
TextureCoord(const dxil::DxilTranslationResourceInfo &resource,
             const CallBase &call, uint32_t first, bool floating,
             DxilAirContext &ctx) {
  return VectorFromOperands(call, first, TextureCoordinateCount(resource),
                            floating, ctx);
}

bool
IsStructuredBuffer(const dxil::DxilTranslationResourceInfo &resource) {
  return resource.resource_kind == 12;
}

bool
IsTypedBufferResource(shader::common::ResourceType resource_type) {
  return resource_type == shader::common::ResourceType::TextureBuffer;
}

Value *
BufferWordIndex(const dxil::DxilTranslationResourceInfo &resource,
                const CallBase &call, uint32_t element_operand,
                uint32_t offset_operand, DxilAirContext &ctx) {
  auto *element = OperandValue(call, element_operand, ctx);
  if (!element)
    element = ctx.builder.getInt32(0);

  auto *byte_offset = OperandValue(call, offset_operand, ctx);
  if (!byte_offset)
    byte_offset = ctx.builder.getInt32(0);

  auto *word_offset = ctx.builder.CreateLShr(byte_offset, 2);
  if (!IsStructuredBuffer(resource))
    return ctx.builder.CreateAdd(element, word_offset);

  const auto stride_words = std::max(1u, resource.element_stride >> 2);
  return ctx.builder.CreateAdd(
      ctx.builder.CreateMul(element, ctx.builder.getInt32(stride_words)),
      word_offset);
}

llvm::AtomicRMWInst::BinOp
AtomicOpFromDxil(uint32_t op) {
  switch (op) {
  case 0:
    return llvm::AtomicRMWInst::Add;
  case 1:
    return llvm::AtomicRMWInst::And;
  case 2:
    return llvm::AtomicRMWInst::Or;
  case 3:
    return llvm::AtomicRMWInst::Xor;
  case 4:
    return llvm::AtomicRMWInst::Min;
  case 5:
    return llvm::AtomicRMWInst::Max;
  case 6:
    return llvm::AtomicRMWInst::UMin;
  case 7:
    return llvm::AtomicRMWInst::UMax;
  case 8:
    return llvm::AtomicRMWInst::Xchg;
  default:
    return llvm::AtomicRMWInst::Add;
  }
}

struct DxilBarrierInfo {
  llvm::air::MemFlags mem_flags = llvm::air::MemFlags::None;
  llvm::air::ThreadScope scope = llvm::air::ThreadScope::Threadgroup;
  bool execution_sync = false;
};

static bool
HasFlag(uint32_t value, uint32_t flag) {
  return (value & flag) != 0;
}

static llvm::air::MemFlags
AppendMemFlags(llvm::air::MemFlags lhs, llvm::air::MemFlags rhs) {
  return static_cast<llvm::air::MemFlags>(uint32_t(lhs) | uint32_t(rhs));
}

static llvm::air::ThreadScope
AppendThreadScope(llvm::air::ThreadScope lhs, llvm::air::ThreadScope rhs) {
  return static_cast<llvm::air::ThreadScope>(uint32_t(lhs) | uint32_t(rhs));
}

static bool
SupportsNonExecutionBarrier(const DxilAirContext &ctx) {
  return ctx.metal_version >= SM50_SHADER_METAL_320;
}

DxilBarrierInfo
BarrierInfoFromLegacyMode(uint32_t mode) {
  constexpr uint32_t kSyncThreadGroup = 1u;
  constexpr uint32_t kUavFenceGlobal = 2u;
  constexpr uint32_t kUavFenceThreadGroup = 4u;
  constexpr uint32_t kTgsmFence = 8u;

  DxilBarrierInfo info = {};
  info.execution_sync = HasFlag(mode, kSyncThreadGroup);
  if (HasFlag(mode, kTgsmFence))
    info.mem_flags =
        AppendMemFlags(info.mem_flags, llvm::air::MemFlags::Threadgroup);
  if (HasFlag(mode, kUavFenceGlobal) || HasFlag(mode, kUavFenceThreadGroup)) {
    info.mem_flags =
        AppendMemFlags(info.mem_flags, llvm::air::MemFlags::Device);
    info.mem_flags =
        AppendMemFlags(info.mem_flags, llvm::air::MemFlags::Texture);
  }
  if (HasFlag(mode, kUavFenceGlobal))
    info.scope = AppendThreadScope(info.scope, llvm::air::ThreadScope::Device);
  return info;
}

std::optional<uint32_t>
TranslateMemoryTypeBarrierMode(uint32_t memory_type_flags,
                               uint32_t semantic_flags) {
  constexpr uint32_t kUavMemory = 1u;
  constexpr uint32_t kGroupSharedMemory = 2u;
  constexpr uint32_t kNodeInputMemory = 4u;
  constexpr uint32_t kNodeOutputMemory = 8u;
  constexpr uint32_t kAllMemory = 0xfu;
  constexpr uint32_t kGroupSync = 1u;
  constexpr uint32_t kGroupScope = 2u;
  constexpr uint32_t kDeviceScope = 4u;
  constexpr uint32_t kLegacyMemoryFlags = kUavMemory | kGroupSharedMemory;
  constexpr uint32_t kLegacySemanticFlags =
      kGroupSync | kGroupScope | kDeviceScope;
  constexpr uint32_t kSyncThreadGroup = 1u;
  constexpr uint32_t kUavFenceGlobal = 2u;
  constexpr uint32_t kUavFenceThreadGroup = 4u;
  constexpr uint32_t kTgsmFence = 8u;

  if (semantic_flags & ~kLegacySemanticFlags)
    return std::nullopt;

  if (memory_type_flags == kAllMemory)
    memory_type_flags &= kLegacyMemoryFlags;
  if (memory_type_flags & (kNodeInputMemory | kNodeOutputMemory))
    return std::nullopt;
  if (memory_type_flags & ~kLegacyMemoryFlags)
    return std::nullopt;

  uint32_t mode = 0;
  if (memory_type_flags & kGroupSharedMemory)
    mode |= kTgsmFence;
  if (HasFlag(memory_type_flags, kUavMemory)) {
    if (semantic_flags & kDeviceScope)
      mode |= kUavFenceGlobal;
    else if (semantic_flags & kGroupScope)
      mode |= kUavFenceThreadGroup;
  }
  if (semantic_flags & kGroupSync)
    mode |= kSyncThreadGroup;
  return mode;
}

void
EmitBarrier(const DxilBarrierInfo &info, DxilAirContext &ctx) {
  const bool non_execution_barrier = SupportsNonExecutionBarrier(ctx);
  if (info.mem_flags != llvm::air::MemFlags::None && non_execution_barrier)
    ctx.air.CreateAtomicFence(info.mem_flags, info.scope);
  if (info.execution_sync) {
    auto barrier_flags = info.mem_flags;
    if (non_execution_barrier) {
      barrier_flags = HasFlag(uint32_t(info.mem_flags),
                              uint32_t(llvm::air::MemFlags::Threadgroup))
                          ? llvm::air::MemFlags::Threadgroup
                          : llvm::air::MemFlags::None;
    }
    ctx.air.CreateBarrier(barrier_flags);
  }
}

Value *
LoadSignatureInput(const CallBase &call, DxilAirContext &ctx) {
  const auto element_id = ConstantOperandU32(call, 1).value_or(0);
  const auto column = ConstantOperandU32(call, 3).value_or(0);
  auto vertex_input = ctx.vertex_inputs.find(element_id);
  if (vertex_input != ctx.vertex_inputs.end()) {
    const auto &element = vertex_input->second;
    if (ctx.vertex_buffer_table_arg == UINT32_MAX ||
        ctx.vertex_id_arg == UINT32_MAX)
      return UndefValue::get(MapType(call.getType(), ctx));

    Value *index = ctx.function->getArg(ctx.vertex_id_arg);
    if (element.step_function) {
      if (ctx.instance_id_arg == UINT32_MAX ||
          ctx.base_instance_arg == UINT32_MAX)
        return UndefValue::get(MapType(call.getType(), ctx));
      index = ctx.function->getArg(ctx.base_instance_arg);
      if (element.step_rate) {
        auto *instance = ctx.function->getArg(ctx.instance_id_arg);
        index = ctx.builder.CreateAdd(
            index, ctx.builder.CreateUDiv(instance,
                                          ctx.builder.getInt32(element.step_rate)));
      }
    } else if (ctx.base_vertex_arg != UINT32_MAX) {
      index = ctx.builder.CreateAdd(index,
                                    ctx.function->getArg(ctx.base_vertex_arg));
    }

    auto *table = ctx.builder.CreatePointerCast(
        ctx.function->getArg(ctx.vertex_buffer_table_arg),
        ctx.types._dxmt_vertex_buffer_entry->getPointerTo(
            uint32_t(air::AddressSpace::constant)));
    const unsigned shift = 32u - std::min<uint32_t>(element.slot, 31u);
    const unsigned table_index =
        element.slot ? std::popcount((ctx.vertex_slot_mask << shift) >> shift)
                     : 0;
    auto *entry = ctx.builder.CreateLoad(
        ctx.types._dxmt_vertex_buffer_entry,
        ctx.builder.CreateConstGEP1_32(ctx.types._dxmt_vertex_buffer_entry,
                                       table, table_index));
    auto *base_addr = ctx.builder.CreateExtractValue(entry, {0});
    auto *stride = ctx.builder.CreateExtractValue(entry, {1});
    auto *byte_offset = ctx.builder.CreateAdd(
        ctx.builder.CreateMul(stride, index),
        ctx.builder.getInt32(element.aligned_byte_offset));
    auto pulled = air::pull_vec4_from_addr(
                      air::MTLAttributeFormat(element.format), base_addr,
                      byte_offset)
                      .build(air::AIRBuilderContext{
                          ctx.llvm, ctx.module, ctx.builder, ctx.types,
                          ctx.air});
    if (auto err = pulled.takeError()) {
      consumeError(std::move(err));
      return UndefValue::get(MapType(call.getType(), ctx));
    }
    auto *vec4 = pulled.get();
    return SplatOrExtract(vec4, column, ctx);
  }
  auto arg = ctx.input_args.find(element_id);
  if (arg == ctx.input_args.end())
    return UndefValue::get(MapType(call.getType(), ctx));
  return SplatOrExtract(ctx.function->getArg(arg->second), column, ctx);
}

Value *
StoreSignatureOutput(const CallBase &call, Value *current_return,
                     DxilAirContext &ctx) {
  const auto element_id = ConstantOperandU32(call, 1).value_or(0);
  const auto column = ConstantOperandU32(call, 3).value_or(0);
  auto out = ctx.output_indices.find(element_id);
  if (out == ctx.output_indices.end())
    return current_return;
  auto *value = OperandValue(call, 4, ctx);
  auto *return_type = ctx.function->getReturnType();
  if (!value || return_type->isVoidTy())
    return current_return;
  auto *field_type = return_type->getStructElementType(out->second);
  if (auto *field_vector = dyn_cast<FixedVectorType>(field_type)) {
    auto *field = ctx.builder.CreateExtractValue(current_return, out->second);
    value = CastValue(value, field_vector->getElementType(), ctx);
    field = ctx.builder.CreateInsertElement(field, value, column);
    return ctx.builder.CreateInsertValue(current_return, field, out->second);
  }
  value = CastValue(value, field_type, ctx);
  return ctx.builder.CreateInsertValue(current_return, value, out->second);
}

Value *
BuildSystemValue(const CallBase &call, std::string_view name, DxilAirContext &ctx) {
  const auto component = ConstantOperandU32(call, 1).value_or(0);
  Value *source = nullptr;
  if (name == "ThreadId") {
    auto idx = ctx.input_args[0xff00u];
    source = ctx.function->getArg(idx);
  } else if (name == "GroupId") {
    auto idx = ctx.input_args[0xff01u];
    source = ctx.function->getArg(idx);
  } else if (name == "ThreadIdInGroup") {
    auto idx = ctx.input_args[0xff02u];
    source = ctx.function->getArg(idx);
  } else if (name == "FlattenedThreadIdInGroup") {
    auto idx = ctx.input_args[0xff03u];
    source = ctx.function->getArg(idx);
  } else {
    return ZeroValue(MapType(call.getType(), ctx));
  }
  return SplatOrExtract(source, component, ctx);
}

Value *
BuildDxilMath(const CallBase &call, std::string_view name, DxilAirContext &ctx) {
  auto *a = OperandValue(call, 1, ctx);
  auto *b = OperandValue(call, 2, ctx);
  auto *c = OperandValue(call, 3, ctx);
  if (name == "FAbs")
    return ctx.air.CreateFPUnOp(llvm::air::AIRBuilder::fabs, a);
  if (name == "Sqrt")
    return ctx.air.CreateFPUnOp(llvm::air::AIRBuilder::sqrt, a);
  if (name == "Rsqrt")
    return ctx.air.CreateFPUnOp(llvm::air::AIRBuilder::rsqrt, a);
  if (name == "Sin")
    return ctx.air.CreateFPUnOp(llvm::air::AIRBuilder::sin, a);
  if (name == "Cos")
    return ctx.air.CreateFPUnOp(llvm::air::AIRBuilder::cos, a);
  if (name == "Exp")
    return ctx.air.CreateFPUnOp(llvm::air::AIRBuilder::exp2, a);
  if (name == "Log")
    return ctx.air.CreateFPUnOp(llvm::air::AIRBuilder::log2, a);
  if (name == "Frc")
    return ctx.air.CreateFPUnOp(llvm::air::AIRBuilder::fract, a);
  if (name == "Round_ne")
    return ctx.air.CreateFPUnOp(llvm::air::AIRBuilder::rint, a);
  if (name == "Round_ni")
    return ctx.air.CreateFPUnOp(llvm::air::AIRBuilder::floor, a);
  if (name == "Round_pi")
    return ctx.air.CreateFPUnOp(llvm::air::AIRBuilder::ceil, a);
  if (name == "Round_z")
    return ctx.air.CreateFPUnOp(llvm::air::AIRBuilder::trunc, a);
  if (name == "IsNaN")
    return ctx.builder.CreateSExt(ctx.air.CreateIsNaN(a), MapType(call.getType(), ctx));
  if (name == "Bfrev")
    return ctx.air.CreateIntUnOp(llvm::air::AIRBuilder::reverse_bits, a);
  if (name == "Countbits")
    return ctx.air.CreateIntUnOp(llvm::air::AIRBuilder::popcount, a);
  if (name == "FMax")
    return ctx.air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, a, b);
  if (name == "FMin")
    return ctx.air.CreateFPBinOp(llvm::air::AIRBuilder::fmin, a, b);
  if (name == "IMax")
    return ctx.air.CreateIntBinOp(llvm::air::AIRBuilder::max, a, b, true);
  if (name == "IMin")
    return ctx.air.CreateIntBinOp(llvm::air::AIRBuilder::min, a, b, true);
  if (name == "UMax")
    return ctx.air.CreateIntBinOp(llvm::air::AIRBuilder::max, a, b, false);
  if (name == "UMin")
    return ctx.air.CreateIntBinOp(llvm::air::AIRBuilder::min, a, b, false);
  if (name == "FMad")
    return ctx.air.CreateFMA(a, b, c);
  if (name == "Dot2" || name == "Dot3" || name == "Dot4" ||
      name == "FDot")
    return ctx.air.CreateDotProduct(a, b);
  if (name == "DerivCoarseX" || name == "DerivFineX")
    return ctx.air.CreateDerivative(a, false);
  if (name == "DerivCoarseY" || name == "DerivFineY")
    return ctx.air.CreateDerivative(a, true);
  return nullptr;
}

llvm::Error
LowerDxilCall(const CallBase &call, DxilAirContext &ctx,
              Value *&current_return) {
  auto *called = call.getCalledFunction();
  if (!called || !called->getName().startswith("dx.op."))
    return UnsupportedDxilCall(
        call, "call target is not a direct dx.op function", ctx);

  std::string name = DxilCallName(call);

  if (name == "CreateHandle") {
    DxilResourceHandle handle = {};
    handle.resource_class =
        DxilResourceClassFromOperand(ConstantOperandU32(call, 1).value_or(0));
    handle.range_id = ConstantOperandU32(call, 2).value_or(0);
    handle.index = OperandValue(call, 3, ctx);
    ctx.handles[&call] = handle;
    return Error::success();
  }
  if (name == "CreateHandleFromBinding") {
    const auto lower_bound =
        ConstantAggregateElementU32(call.getArgOperand(1), 0);
    const auto resource_class =
        ConstantAggregateElementU32(call.getArgOperand(1), 3);
    const auto space = ConstantAggregateElementU32(call.getArgOperand(1), 2);
    if (!lower_bound || !resource_class || !space)
      return UnsupportedDxilCall(call, "requires a constant ResBind", ctx);
    const auto *resource =
        FindResourceByBinding(
            ctx.translation, ResourceClassFromDxilValue(*resource_class),
            *space, *lower_bound);
    if (!resource)
      return UnsupportedDxilCall(
          call,
          "unresolved binding space" + std::to_string(*space) + " register" +
              std::to_string(*lower_bound),
          ctx);
    DxilResourceHandle handle = {};
    handle.resource_class = resource->resource_class;
    handle.range_id = resource->id;
    handle.index = OperandValue(call, 2, ctx);
    handle.non_uniform = ConstantOperandU32(call, 3).value_or(0) != 0;
    ctx.handles[&call] = handle;
    return Error::success();
  }
  if (name == "CreateHandleFromHeap") {
    return UnsupportedDxilCall(
        call, "bindless descriptor heap is unsupported", ctx);
  }
  if (name == "AnnotateHandle") {
    ctx.handles[&call] = ResolveHandle(call.getArgOperand(1), ctx);
    return Error::success();
  }
  if (name == "LoadInput" || name == "LoadOutputControlPoint" ||
      name == "LoadPatchConstant") {
    ctx.values[&call] = LoadSignatureInput(call, ctx);
    return Error::success();
  }
  if (name == "StoreOutput" || name == "StorePatchConstant" ||
      name == "StoreVertexOutput" || name == "StorePrimitiveOutput") {
    current_return = StoreSignatureOutput(call, current_return, ctx);
    return Error::success();
  }
  if (name == "ThreadId" || name == "GroupId" || name == "ThreadIdInGroup" ||
      name == "FlattenedThreadIdInGroup") {
    ctx.values[&call] = BuildSystemValue(call, name, ctx);
    return Error::success();
  }
  if (name == "CBufferLoad" || name == "CBufferLoadLegacy") {
    auto handle = ResolveHandle(call.getArgOperand(1), ctx);
    auto *ptr = GetCBufferPointer(handle, ctx);
    auto *index = OperandValue(call, 2, ctx);
    if (!ptr)
      return UnsupportedDxilCall(call, "cbuffer load without resolved CBV handle", ctx);
    auto *pointee = cast<PointerType>(ptr->getType())->getNonOpaquePointerElementType();
    auto *gep = ctx.builder.CreateGEP(pointee, ptr, index ? index : ctx.builder.getInt32(0));
    ctx.values[&call] =
        PackDxilReturn(call, ctx.builder.CreateLoad(pointee, gep), ctx);
    return Error::success();
  }
  if (name == "RawBufferLoad" || name == "BufferLoad") {
    auto handle = ResolveHandle(call.getArgOperand(1), ctx);
    const auto *resource =
        FindResource(ctx.translation, handle.resource_class, handle.range_id);
    if (!resource)
      return UnsupportedDxilCall(call, "buffer load without resolved resource metadata", ctx);
    auto *ptr = GetBufferPointer(handle,
                                 handle.resource_class == dxil::DxilTranslationResourceClass::Uav,
                                 ctx);
    if (!ptr)
      return UnsupportedDxilCall(call, "buffer load without resolved buffer handle", ctx);
    uint32_t mask = 0xfu;
    if (name == "RawBufferLoad") {
      const auto raw_mask = ConstantOperandU32(call, 4);
      if (!raw_mask)
        return UnsupportedDxilCall(call, "raw buffer load mask must be constant", ctx);
      mask = *raw_mask;
    }
    auto *word_index = BufferWordIndex(*resource, call, 2, 3, ctx);
    auto *pointee = cast<PointerType>(ptr->getType())->getNonOpaquePointerElementType();
    std::array<Value *, 4> values = {};
    for (uint32_t i = 0; i < 4; i++) {
      if (!(mask & (1u << i)))
        continue;
      auto *component_index =
          ctx.builder.CreateAdd(word_index, ctx.builder.getInt32(i));
      auto *gep = ctx.builder.CreateGEP(pointee, ptr, component_index);
      values[i] = ctx.builder.CreateLoad(pointee, gep);
    }
    ctx.values[&call] = PackDxilReturn(call, BuildVector4(values, ctx), ctx);
    return Error::success();
  }
  if (name == "RawBufferStore" || name == "BufferStore") {
    auto handle = ResolveHandle(call.getArgOperand(1), ctx);
    const auto *resource =
        FindResource(ctx.translation, handle.resource_class, handle.range_id);
    auto *ptr = GetBufferPointer(handle, true, ctx);
    if (!ptr || !resource)
      return UnsupportedDxilCall(call, "buffer store without resolved UAV handle", ctx);
    const uint32_t value_base = name == "RawBufferStore" ? 4 : 4;
    const uint32_t mask_index = name == "RawBufferStore" ? 8 : 8;
    const uint32_t mask = ConstantOperandU32(call, mask_index).value_or(0xfu);
    auto *base_word_index = BufferWordIndex(*resource, call, 2, 3, ctx);
    auto *pointee = cast<PointerType>(ptr->getType())->getNonOpaquePointerElementType();
    for (uint32_t i = 0; i < 4; i++) {
      if (!(mask & (1u << i)))
        continue;
      auto *word_index =
          ctx.builder.CreateAdd(base_word_index, ctx.builder.getInt32(i));
      auto *gep = ctx.builder.CreateGEP(pointee, ptr, word_index);
      ctx.builder.CreateStore(CastValue(OperandValue(call, value_base + i, ctx),
                                        pointee, ctx),
                              gep);
    }
    return Error::success();
  }
  if (name == "TextureLoad") {
    auto handle = ResolveHandle(call.getArgOperand(1), ctx);
    auto [texture_handle, resource] = GetTextureHandle(handle, ctx);
    if (!texture_handle || !resource)
      return UnsupportedDxilCall(call, "texture load without resolved texture handle", ctx);
    auto texture = BuildTexture(*resource, air::MemoryAccess::read);
    auto *coord = TextureCoord(*resource, call, 3, false, ctx);
    auto *array_index = TextureArrayIndex(*resource, call, 3, false, ctx);
    auto [value, residency] =
        ctx.air.CreateRead(texture, texture_handle, coord, array_index, nullptr,
                           OperandValue(call, 2, ctx));
    ctx.values[&call] = PackDxilReturn(call, value, ctx, residency);
    return Error::success();
  }
  if (name == "TextureStore") {
    auto handle = ResolveHandle(call.getArgOperand(1), ctx);
    auto [texture_handle, resource] = GetTextureHandle(handle, ctx);
    if (!texture_handle || !resource)
      return UnsupportedDxilCall(call, "texture store without resolved texture handle", ctx);
    auto texture = BuildTexture(*resource, air::MemoryAccess::write);
    auto *coord = TextureCoord(*resource, call, 2, false, ctx);
    auto *array_index = TextureArrayIndex(*resource, call, 2, false, ctx);
    auto *value = BuildVector4({OperandValue(call, 5, ctx),
                                OperandValue(call, 6, ctx),
                                OperandValue(call, 7, ctx),
                                OperandValue(call, 8, ctx)},
                               ctx);
    ctx.air.CreateWrite(texture, texture_handle, coord, array_index, nullptr,
                        ctx.builder.getInt32(0), value);
    return Error::success();
  }
  if (name.starts_with("Sample")) {
    auto texture_handle = ResolveHandle(call.getArgOperand(1), ctx);
    auto sampler_handle = ResolveHandle(call.getArgOperand(2), ctx);
    auto [texture, resource] = GetTextureHandle(texture_handle, ctx);
    auto *sampler = GetSamplerHandle(sampler_handle, ctx);
    if (!texture || !sampler || !resource)
      return UnsupportedDxilCall(call, "sample without resolved texture/sampler handle", ctx);
    int32_t offset[3] = {};
    auto air_texture = BuildTexture(*resource, air::MemoryAccess::sample);
    auto *coord = TextureCoord(*resource, call, 3, true, ctx);
    auto *array_index = TextureArrayIndex(*resource, call, 3, true, ctx);
    std::pair<Value *, Value *> result = {nullptr, nullptr};
    if (name == "SampleLevel")
      result = ctx.air.CreateSample(air_texture, texture, sampler, coord,
                                    array_index, offset,
                                    llvm::air::sample_level{OperandValue(call, 8, ctx)});
    else if (name == "SampleBias")
      result = ctx.air.CreateSample(air_texture, texture, sampler, coord,
                                    array_index, offset,
                                    llvm::air::sample_bias{OperandValue(call, 8, ctx)});
    else if (name == "SampleGrad")
      result = ctx.air.CreateSampleGrad(
          air_texture, texture, sampler, coord, array_index,
          VectorFromOperands(call, 8, TextureCoordinateCount(*resource), true, ctx),
          VectorFromOperands(call, 11, TextureCoordinateCount(*resource), true, ctx),
          offset);
    else if (name == "SampleCmp" || name == "SampleCmpLevelZero")
      result = ctx.air.CreateSampleCmp(
          air_texture, texture, sampler, coord, array_index,
          OperandValue(call, 8, ctx), offset,
          llvm::air::sample_level{name == "SampleCmpLevelZero"
                                      ? ctx.air.getFloat(0)
                                      : ctx.air.getFloat(0)});
    else
      result = ctx.air.CreateSample(air_texture, texture, sampler, coord,
                                    array_index, offset);
    ctx.values[&call] = PackDxilReturn(call, result.first, ctx, result.second);
    return Error::success();
  }
  if (name == "TextureGather" || name == "TextureGatherCmp") {
    auto texture_handle = ResolveHandle(call.getArgOperand(1), ctx);
    auto sampler_handle = ResolveHandle(call.getArgOperand(2), ctx);
    auto [texture, resource] = GetTextureHandle(texture_handle, ctx);
    auto *sampler = GetSamplerHandle(sampler_handle, ctx);
    if (!texture || !sampler || !resource)
      return UnsupportedDxilCall(call, "gather without resolved texture/sampler handle", ctx);
    auto air_texture = BuildTexture(*resource, air::MemoryAccess::sample);
    auto *coord = TextureCoord(*resource, call, 3, true, ctx);
    auto *array_index = TextureArrayIndex(*resource, call, 3, true, ctx);
    int32_t offset[3] = {};
    std::pair<Value *, Value *> result = {nullptr, nullptr};
    if (name == "TextureGatherCmp")
      result = ctx.air.CreateGatherCompare(
          air_texture, texture, sampler, coord, array_index,
          OperandValue(call, 8, ctx), offset);
    else
      result = ctx.air.CreateGather(
          air_texture, texture, sampler, coord, array_index, offset,
          ctx.builder.getInt32(ConstantOperandU32(call, 8).value_or(0)));
    ctx.values[&call] = PackDxilReturn(call, result.first, ctx, result.second);
    return Error::success();
  }
  if (name == "AtomicBinOp" || name == "AtomicCompareExchange") {
    auto handle = ResolveHandle(call.getArgOperand(1), ctx);
    auto [texture_handle, resource] = GetTextureHandle(handle, ctx);
    if (texture_handle && resource) {
      auto air_texture = BuildTexture(*resource, air::MemoryAccess::read_write);
      auto *coord = TextureCoord(*resource, call, name == "AtomicBinOp" ? 3 : 2,
                                 false, ctx);
      auto *array_index = TextureArrayIndex(*resource, call,
                                            name == "AtomicBinOp" ? 3 : 2,
                                            false, ctx);
      Value *value = nullptr;
      if (name == "AtomicCompareExchange") {
        auto [old_value, success] = ctx.air.CreateAtomicCmpXchg(
            air_texture, texture_handle, coord,
            BuildVector4({OperandValue(call, 5, ctx), nullptr, nullptr, nullptr}, ctx),
            BuildVector4({OperandValue(call, 6, ctx), nullptr, nullptr, nullptr}, ctx),
            array_index);
        (void)success;
        value = old_value;
      } else {
        value = ctx.air.CreateAtomicRMW(
            air_texture, texture_handle,
            AtomicOpFromDxil(ConstantOperandU32(call, 2).value_or(1)),
            coord,
            BuildVector4({OperandValue(call, 6, ctx), nullptr, nullptr, nullptr}, ctx),
            array_index);
      }
      ctx.values[&call] = PackDxilReturn(call, value, ctx);
      return Error::success();
    }

    auto *ptr = GetBufferPointer(handle, true, ctx);
    const auto *buffer_resource =
        FindResource(ctx.translation, handle.resource_class, handle.range_id);
    if (!ptr || !buffer_resource)
      return UnsupportedDxilCall(call, "atomic without resolved UAV handle", ctx);
    auto *pointee = cast<PointerType>(ptr->getType())->getNonOpaquePointerElementType();
    auto *word_index = name == "AtomicBinOp"
                           ? BufferWordIndex(*buffer_resource, call, 3, 4, ctx)
                           : BufferWordIndex(*buffer_resource, call, 2, 3, ctx);
    auto *gep = ctx.builder.CreateGEP(pointee, ptr, word_index);
    Value *value = nullptr;
    if (name == "AtomicCompareExchange") {
      auto *cmp = CastValue(OperandValue(call, 5, ctx), pointee, ctx);
      auto *new_value = CastValue(OperandValue(call, 6, ctx), pointee, ctx);
      auto *result = ctx.builder.CreateAtomicCmpXchg(
          gep, cmp, new_value, llvm::MaybeAlign(),
          llvm::AtomicOrdering::Monotonic, llvm::AtomicOrdering::Monotonic);
      value = ctx.builder.CreateExtractValue(result, 0);
    } else {
      value = ctx.air.CreateAtomicRMW(
          AtomicOpFromDxil(ConstantOperandU32(call, 2).value_or(1)), gep,
          CastValue(OperandValue(call, 6, ctx), pointee, ctx));
    }
    ctx.values[&call] = PackDxilReturn(call, value, ctx);
    return Error::success();
  }
  if (name == "Discard") {
    ctx.air.CreateDiscard();
    return Error::success();
  }
  if (name == "Barrier") {
    const auto mode = ConstantOperandU32(call, 1);
    if (!mode)
      return UnsupportedDxilCall(call, "barrier mode must be constant", ctx);
    EmitBarrier(BarrierInfoFromLegacyMode(*mode), ctx);
    return Error::success();
  }
  if (name == "BarrierByMemoryType") {
    const auto memory_type_flags = ConstantOperandU32(call, 1);
    const auto semantic_flags = ConstantOperandU32(call, 2);
    if (!memory_type_flags || !semantic_flags)
      return UnsupportedDxilCall(
          call, "memory type barrier flags must be constant", ctx);
    const auto mode =
        TranslateMemoryTypeBarrierMode(*memory_type_flags, *semantic_flags);
    if (!mode)
      return UnsupportedDxilCall(
          call, "node/reorder scoped memory type barrier is unsupported", ctx);
    EmitBarrier(BarrierInfoFromLegacyMode(*mode), ctx);
    return Error::success();
  }
  if (name == "BarrierByMemoryHandle" ||
      name == "BarrierByNodeRecordHandle") {
    return UnsupportedDxilCall(
        call, "handle-scoped barriers are unsupported", ctx);
  }
  if (auto *math = BuildDxilMath(call, name, ctx)) {
    ctx.values[&call] = math;
    return Error::success();
  }

  return UnsupportedDxilCall(call, "lowering is not implemented", ctx);
}

llvm::Error
LowerInstruction(const Instruction &instruction, DxilAirContext &ctx,
                 Value *&current_return) {
  if (const auto *call = dyn_cast<CallBase>(&instruction))
    return LowerDxilCall(*call, ctx, current_return);

  Value *result = nullptr;
  switch (instruction.getOpcode()) {
  case Instruction::Alloca:
    result = ctx.builder.CreateAlloca(
        MapType(cast<AllocaInst>(instruction).getAllocatedType(), ctx));
    break;
  case Instruction::Load: {
    const auto &load = cast<LoadInst>(instruction);
    auto *ptr = MapValue(load.getPointerOperand(), ctx);
    result = ctx.builder.CreateLoad(MapType(load.getType(), ctx), ptr);
    break;
  }
  case Instruction::Store: {
    const auto &store = cast<StoreInst>(instruction);
    ctx.builder.CreateStore(MapValue(store.getValueOperand(), ctx),
                            MapValue(store.getPointerOperand(), ctx));
    break;
  }
  case Instruction::GetElementPtr: {
    const auto &gep = cast<GetElementPtrInst>(instruction);
    SmallVector<Value *, 8> indices;
    for (const auto &index : gep.indices())
      indices.push_back(MapValue(index.get(), ctx));
    result = ctx.builder.CreateGEP(
        MapType(gep.getSourceElementType(), ctx),
        MapValue(gep.getPointerOperand(), ctx), indices);
    break;
  }
  case Instruction::ExtractElement: {
    const auto &extract = cast<ExtractElementInst>(instruction);
    result = ctx.builder.CreateExtractElement(
        MapValue(extract.getVectorOperand(), ctx),
        MapValue(extract.getIndexOperand(), ctx));
    break;
  }
  case Instruction::InsertElement: {
    const auto &insert = cast<InsertElementInst>(instruction);
    result = ctx.builder.CreateInsertElement(
        MapValue(insert.getOperand(0), ctx), MapValue(insert.getOperand(1), ctx),
        MapValue(insert.getOperand(2), ctx));
    break;
  }
  case Instruction::ExtractValue: {
    const auto &extract = cast<ExtractValueInst>(instruction);
    result = ctx.builder.CreateExtractValue(MapValue(extract.getAggregateOperand(), ctx),
                                            extract.getIndices());
    break;
  }
  case Instruction::InsertValue: {
    const auto &insert = cast<InsertValueInst>(instruction);
    result = ctx.builder.CreateInsertValue(MapValue(insert.getAggregateOperand(), ctx),
                                           MapValue(insert.getInsertedValueOperand(), ctx),
                                           insert.getIndices());
    break;
  }
  case Instruction::PHI:
    break;
  case Instruction::Select: {
    const auto &select = cast<SelectInst>(instruction);
    result = ctx.builder.CreateSelect(MapValue(select.getCondition(), ctx),
                                      MapValue(select.getTrueValue(), ctx),
                                      MapValue(select.getFalseValue(), ctx));
    break;
  }
  case Instruction::ICmp: {
    const auto &cmp = cast<ICmpInst>(instruction);
    result = ctx.builder.CreateICmp(cmp.getPredicate(),
                                    MapValue(cmp.getOperand(0), ctx),
                                    MapValue(cmp.getOperand(1), ctx));
    break;
  }
  case Instruction::FCmp: {
    const auto &cmp = cast<FCmpInst>(instruction);
    result = ctx.builder.CreateFCmp(cmp.getPredicate(),
                                    MapValue(cmp.getOperand(0), ctx),
                                    MapValue(cmp.getOperand(1), ctx));
    break;
  }
  case Instruction::Add:
    result = ctx.builder.CreateAdd(MapValue(instruction.getOperand(0), ctx),
                                   MapValue(instruction.getOperand(1), ctx));
    break;
  case Instruction::Sub:
    result = ctx.builder.CreateSub(MapValue(instruction.getOperand(0), ctx),
                                   MapValue(instruction.getOperand(1), ctx));
    break;
  case Instruction::Mul:
    result = ctx.builder.CreateMul(MapValue(instruction.getOperand(0), ctx),
                                   MapValue(instruction.getOperand(1), ctx));
    break;
  case Instruction::UDiv:
    result = ctx.builder.CreateUDiv(MapValue(instruction.getOperand(0), ctx),
                                    MapValue(instruction.getOperand(1), ctx));
    break;
  case Instruction::SDiv:
    result = ctx.builder.CreateSDiv(MapValue(instruction.getOperand(0), ctx),
                                    MapValue(instruction.getOperand(1), ctx));
    break;
  case Instruction::URem:
    result = ctx.builder.CreateURem(MapValue(instruction.getOperand(0), ctx),
                                    MapValue(instruction.getOperand(1), ctx));
    break;
  case Instruction::SRem:
    result = ctx.builder.CreateSRem(MapValue(instruction.getOperand(0), ctx),
                                    MapValue(instruction.getOperand(1), ctx));
    break;
  case Instruction::FAdd:
    result = ctx.builder.CreateFAdd(MapValue(instruction.getOperand(0), ctx),
                                    MapValue(instruction.getOperand(1), ctx));
    break;
  case Instruction::FSub:
    result = ctx.builder.CreateFSub(MapValue(instruction.getOperand(0), ctx),
                                    MapValue(instruction.getOperand(1), ctx));
    break;
  case Instruction::FMul:
    result = ctx.builder.CreateFMul(MapValue(instruction.getOperand(0), ctx),
                                    MapValue(instruction.getOperand(1), ctx));
    break;
  case Instruction::FDiv:
    result = ctx.builder.CreateFDiv(MapValue(instruction.getOperand(0), ctx),
                                    MapValue(instruction.getOperand(1), ctx));
    break;
  case Instruction::And:
    result = ctx.builder.CreateAnd(MapValue(instruction.getOperand(0), ctx),
                                   MapValue(instruction.getOperand(1), ctx));
    break;
  case Instruction::Or:
    result = ctx.builder.CreateOr(MapValue(instruction.getOperand(0), ctx),
                                  MapValue(instruction.getOperand(1), ctx));
    break;
  case Instruction::Xor:
    result = ctx.builder.CreateXor(MapValue(instruction.getOperand(0), ctx),
                                   MapValue(instruction.getOperand(1), ctx));
    break;
  case Instruction::Shl:
    result = ctx.builder.CreateShl(MapValue(instruction.getOperand(0), ctx),
                                   MapValue(instruction.getOperand(1), ctx));
    break;
  case Instruction::LShr:
    result = ctx.builder.CreateLShr(MapValue(instruction.getOperand(0), ctx),
                                    MapValue(instruction.getOperand(1), ctx));
    break;
  case Instruction::AShr:
    result = ctx.builder.CreateAShr(MapValue(instruction.getOperand(0), ctx),
                                    MapValue(instruction.getOperand(1), ctx));
    break;
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::UIToFP:
  case Instruction::SIToFP:
  case Instruction::FPTrunc:
  case Instruction::FPExt:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::BitCast:
  case Instruction::AddrSpaceCast:
    result = ctx.builder.CreateCast(static_cast<Instruction::CastOps>(instruction.getOpcode()),
                                    MapValue(instruction.getOperand(0), ctx),
                                    MapType(instruction.getType(), ctx));
    break;
  default:
    if (!instruction.isTerminator())
      return Unsupported("unsupported LLVM instruction in DXIL: " +
                         std::string(instruction.getOpcodeName()));
    break;
  }

  if (result)
    ctx.values[&instruction] = result;
  return Error::success();
}

llvm::Error
LowerTerminator(const Instruction &terminator, DxilAirContext &ctx,
                Value *current_return) {
  if (const auto *ret = dyn_cast<ReturnInst>(&terminator)) {
    (void)ret;
    if (ctx.function->getReturnType()->isVoidTy())
      ctx.builder.CreateRetVoid();
    else
      ctx.builder.CreateRet(ctx.return_value
                                ? ctx.builder.CreateLoad(
                                      ctx.function->getReturnType(),
                                      ctx.return_value)
                                : current_return);
    return Error::success();
  }
  if (const auto *branch = dyn_cast<BranchInst>(&terminator)) {
    if (branch->isUnconditional()) {
      ctx.builder.CreateBr(ctx.blocks[branch->getSuccessor(0)]);
    } else {
      ctx.builder.CreateCondBr(MapValue(branch->getCondition(), ctx),
                               ctx.blocks[branch->getSuccessor(0)],
                               ctx.blocks[branch->getSuccessor(1)]);
    }
    return Error::success();
  }
  if (const auto *sw = dyn_cast<SwitchInst>(&terminator)) {
    auto *out = ctx.builder.CreateSwitch(MapValue(sw->getCondition(), ctx),
                                         ctx.blocks[sw->getDefaultDest()],
                                         sw->getNumCases());
    for (const auto &case_value : sw->cases())
      out->addCase(const_cast<ConstantInt *>(case_value.getCaseValue()),
                   ctx.blocks[case_value.getCaseSuccessor()]);
    return Error::success();
  }
  if (isa<UnreachableInst>(terminator)) {
    ctx.builder.CreateUnreachable();
    return Error::success();
  }
  return Unsupported("unsupported DXIL terminator: " +
                     std::string(terminator.getOpcodeName()));
}

llvm::Error
BuildPhiNodes(const Function &source, DxilAirContext &ctx) {
  for (const auto &block : source) {
    ctx.builder.SetInsertPoint(ctx.blocks[&block]);
    for (const auto &instruction : block) {
      const auto *phi = dyn_cast<PHINode>(&instruction);
      if (!phi)
        break;
      auto *mapped = ctx.builder.CreatePHI(MapType(phi->getType(), ctx),
                                           phi->getNumIncomingValues());
      ctx.values[phi] = mapped;
    }
  }
  return Error::success();
}

void
PatchPhiNodes(const Function &source, DxilAirContext &ctx) {
  for (const auto &block : source) {
    for (const auto &instruction : block) {
      const auto *phi = dyn_cast<PHINode>(&instruction);
      if (!phi)
        break;
      auto *mapped = cast<PHINode>(ctx.values[phi]);
      for (uint32_t i = 0; i < phi->getNumIncomingValues(); i++)
        mapped->addIncoming(MapValue(phi->getIncomingValue(i), ctx),
                            ctx.blocks[phi->getIncomingBlock(i)]);
    }
  }
}

void
BuildDxilShaderInfoImpl(const dxil::DxilTranslationInfo &translation,
                        dxbc::ShaderInfo &shader_info) {
  using shader::common::ResourceType;
  std::set<uint32_t> cbv_slots;
  std::set<uint32_t> sampler_slots;
  std::set<uint32_t> srv_slots;
  std::set<uint32_t> uav_slots;
  for (const auto &resource : translation.resources) {
    const auto range_id = resource.id;
    if (resource.resource_class == dxil::DxilTranslationResourceClass::Cbv) {
      const auto binding_slot =
          AllocateDxilBindingSlot(cbv_slots, resource.lower_bound);
      auto &cbv = shader_info.cbufferMap[range_id];
      cbv.range = {.range_id = range_id,
                   .binding_slot = binding_slot,
                   .lower_bound = resource.lower_bound,
                   .size = resource.bind_count ? resource.bind_count : 1,
                   .space = resource.space};
      cbv.size_in_vec4 = 4096;
      cbv.arg_index = shader_info.binding_table_cbuffer.DefineBuffer(
          "cb" + std::to_string(range_id), air::AddressSpace::constant,
          air::MemoryAccess::read, air::msl_uint4,
          GetArgumentIndex(SM50BindingType::ConstantBuffer, binding_slot));
      continue;
    }
    if (resource.resource_class == dxil::DxilTranslationResourceClass::Sampler) {
      const auto binding_slot =
          AllocateDxilBindingSlot(sampler_slots, resource.lower_bound);
      auto &sampler = shader_info.samplerMap[range_id];
      sampler.range = {.range_id = range_id,
                       .binding_slot = binding_slot,
                       .lower_bound = resource.lower_bound,
                       .size = resource.bind_count ? resource.bind_count : 1,
                       .space = resource.space};
      auto attr = GetArgumentIndex(SM50BindingType::Sampler, binding_slot);
      sampler.arg_index = shader_info.binding_table.DefineSampler(
          "s" + std::to_string(range_id), attr);
      sampler.arg_cube_index = shader_info.binding_table.DefineSampler(
          "cube_s" + std::to_string(range_id), attr + 1);
      sampler.arg_metadata_index = shader_info.binding_table.DefineInteger64(
          "meta_s" + std::to_string(range_id), attr + 2);
      continue;
    }

    auto resource_type =
        ToResourceType(resource.resource_kind, resource.dimension);
    auto scaler = ToScalerDataType(resource.return_type);
    if (resource.resource_class == dxil::DxilTranslationResourceClass::Srv) {
      const auto binding_slot =
          AllocateDxilBindingSlot(srv_slots, resource.lower_bound);
      auto &srv = shader_info.srvMap[range_id];
      srv.range = {.range_id = range_id,
                   .binding_slot = binding_slot,
                   .lower_bound = resource.lower_bound,
                   .size = resource.bind_count ? resource.bind_count : 1,
                   .space = resource.space};
      srv.resource_type = resource_type;
      srv.scaler_type = scaler;
      srv.read = resource.read;
      srv.sampled = resource.sampled;
      srv.structure_stride = resource.element_stride;
      auto attr = GetArgumentIndex(SM50BindingType::SRV, binding_slot);
      if (IsTypedBufferResource(resource_type)) {
        srv.arg_index = shader_info.binding_table.DefineBuffer(
            "t" + std::to_string(range_id), air::AddressSpace::device,
            air::MemoryAccess::read, air::msl_uint, attr);
      } else if (resource_type != ResourceType::NonApplicable) {
        srv.arg_index = shader_info.binding_table.DefineTexture(
            "t" + std::to_string(range_id),
            air::to_air_resource_type(resource_type),
            resource.sampled ? air::MemoryAccess::sample
                             : air::MemoryAccess::read,
            air::to_air_scaler_type(scaler), attr);
      } else {
        srv.arg_index = shader_info.binding_table.DefineBuffer(
            "t" + std::to_string(range_id), air::AddressSpace::device,
            air::MemoryAccess::read, air::msl_uint, attr);
      }
      srv.arg_metadata_index = shader_info.binding_table.DefineInteger64(
          "meta_t" + std::to_string(range_id), attr + 1);
    } else if (resource.resource_class == dxil::DxilTranslationResourceClass::Uav) {
      const auto binding_slot =
          AllocateDxilBindingSlot(uav_slots, resource.lower_bound);
      auto &uav = shader_info.uavMap[range_id];
      uav.range = {.range_id = range_id,
                   .binding_slot = binding_slot,
                   .lower_bound = resource.lower_bound,
                   .size = resource.bind_count ? resource.bind_count : 1,
                   .space = resource.space};
      uav.resource_type = resource_type;
      uav.scaler_type = scaler;
      uav.read = resource.read;
      uav.written = resource.written;
      uav.structure_stride = resource.element_stride;
      auto attr = GetArgumentIndex(SM50BindingType::UAV, binding_slot);
      const auto access = uav.written ? (uav.read ? air::MemoryAccess::read_write
                                                  : air::MemoryAccess::write)
                                      : air::MemoryAccess::read;
      if (IsTypedBufferResource(resource_type)) {
        uav.arg_index = shader_info.binding_table.DefineBuffer(
            "u" + std::to_string(range_id), air::AddressSpace::device, access,
            air::msl_uint, attr);
      } else if (resource_type != ResourceType::NonApplicable) {
        uav.arg_index = shader_info.binding_table.DefineTexture(
            "u" + std::to_string(range_id),
            air::to_air_resource_type(resource_type), access,
            air::to_air_scaler_type(scaler), attr);
      } else {
        uav.arg_index = shader_info.binding_table.DefineBuffer(
            "u" + std::to_string(range_id), air::AddressSpace::device, access,
            air::msl_uint, attr);
      }
      uav.arg_metadata_index = shader_info.binding_table.DefineInteger64(
          "meta_u" + std::to_string(range_id), attr + 1);
    }
  }
}

void
BuildSignature(const dxil::DxilTranslationInfo &translation,
               air::FunctionSignatureBuilder &signature,
               DxilAirContext &ctx,
               SM50_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout) {
  uint32_t vertex_input_index = 0;
  for (const auto &sig : translation.signatures) {
    const auto mask = SignatureMask(sig);
    if (sig.kind == dxil::DxilTranslationSignatureKind::Input) {
      ctx.inputs[sig.element_id] = &sig;
      if (translation.shader_kind == uint32_t(DxilStage::Vertex)) {
        if (IsSystemSemantic(sig, "SV_VertexID"))
          ctx.input_args[sig.element_id] = signature.DefineInput(air::InputVertexID{});
        else if (IsSystemSemantic(sig, "SV_InstanceID"))
          ctx.input_args[sig.element_id] = signature.DefineInput(air::InputInstanceID{});
        else if (ia_layout && vertex_input_index < ia_layout->num_elements)
          ctx.vertex_inputs[sig.element_id] =
              ia_layout->elements[vertex_input_index++];
        else
          ctx.input_args[sig.element_id] = signature.DefineInput(
              air::InputVertexStageIn{.attribute = sig.element_id,
                                      .type = ToAirInputComponent(sig.component_type),
                                      .name = SemanticUserName(sig)});
      } else if (translation.shader_kind == uint32_t(DxilStage::Pixel)) {
        if (IsSystemSemantic(sig, "SV_Position"))
          ctx.input_args[sig.element_id] =
              signature.DefineInput(air::InputPosition{ToAirInterpolation(sig.interpolation_mode)});
        else if (IsSystemSemantic(sig, "SV_PrimitiveID"))
          ctx.input_args[sig.element_id] = signature.DefineInput(air::InputPrimitiveID{});
        else if (IsSystemSemantic(sig, "SV_Coverage"))
          ctx.input_args[sig.element_id] = signature.DefineInput(air::InputInputCoverage{});
        else if (IsSystemSemantic(sig, "SV_SampleIndex"))
          ctx.input_args[sig.element_id] = signature.DefineInput(air::InputSampleIndex{});
        else
          ctx.input_args[sig.element_id] = signature.DefineInput(
              air::InputFragmentStageIn{.user = SemanticUserName(sig),
                                        .type = ToAirSignatureType(sig.component_type, mask),
                                        .interpolation = ToAirInterpolation(sig.interpolation_mode),
                                        .pull_mode = false});
      }
    } else if (sig.kind == dxil::DxilTranslationSignatureKind::Output) {
      ctx.outputs[sig.element_id] = &sig;
      if (translation.shader_kind == uint32_t(DxilStage::Vertex)) {
        if (IsSystemSemantic(sig, "SV_Position"))
          ctx.output_indices[sig.element_id] =
              signature.DefineOutput(air::OutputPosition{air::msl_float4});
        else
          ctx.output_indices[sig.element_id] = signature.DefineOutput(
              air::OutputVertex{.user = SemanticUserName(sig),
                                .type = ToAirSignatureType(sig.component_type, mask)});
      } else if (translation.shader_kind == uint32_t(DxilStage::Pixel)) {
        if (IsSystemSemantic(sig, "SV_Depth"))
          ctx.output_indices[sig.element_id] =
              signature.DefineOutput(air::OutputDepth{air::DepthArgument::any});
        else if (IsSystemSemantic(sig, "SV_Coverage"))
          ctx.output_indices[sig.element_id] =
              signature.DefineOutput(air::OutputCoverageMask{});
        else
          ctx.output_indices[sig.element_id] = signature.DefineOutput(
              air::OutputRenderTarget{.dual_source_blending = false,
                                      .index = sig.semantic_index,
                                      .type = ToAirSignatureType(sig.component_type, mask)});
      }
    }
  }

  if (translation.shader_kind == uint32_t(DxilStage::Compute)) {
    ctx.input_args[0xff00u] = signature.DefineInput(air::InputThreadPositionInGrid{});
    ctx.input_args[0xff01u] = signature.DefineInput(air::InputThreadgroupPositionInGrid{});
    ctx.input_args[0xff02u] =
        signature.DefineInput(air::InputThreadPositionInThreadgroup{});
    ctx.input_args[0xff03u] =
        signature.DefineInput(air::InputThreadIndexInThreadgroup{});
    signature.UseMaxWorkgroupSize(translation.num_threads_x *
                                  translation.num_threads_y *
                                  translation.num_threads_z);
  }
}

const Function *
FindEntryFunction(const Module &module, const dxil::DxilTranslationInfo &translation) {
  if (!translation.function_name.empty()) {
    if (auto *function = module.getFunction(translation.function_name))
      return function;
  }
  if (!translation.entry_point_name.empty()) {
    if (auto *function = module.getFunction(translation.entry_point_name))
      return function;
  }
  for (const auto &function : module.functions()) {
    if (!function.isDeclaration() && !function.getName().startswith("dx."))
      return &function;
  }
  return nullptr;
}

llvm::Expected<std::unique_ptr<Module>>
ParseInputModule(const dxil::Parser &parser, LLVMContext &context) {
  const auto &program = parser.dxilProgram();
  if (!program)
    return llvm::make_error<UnsupportedFeature>("DXIL container has no DXIL program");
  auto buffer = MemoryBuffer::getMemBufferCopy(
      StringRef(reinterpret_cast<const char *>(program->bitcode.data()),
                program->bitcode.size()));
  return parseBitcodeFile(buffer->getMemBufferRef(), context);
}

llvm::Error
LowerFunction(const Function &source, DxilAirContext &ctx) {
  for (const auto &global : source.getParent()->globals()) {
    const auto address_space = global.getAddressSpace();
    if (address_space != uint32_t(air::AddressSpace::threadgroup))
      continue;
    auto *type = MapType(global.getValueType(), ctx);
    auto *mapped = new GlobalVariable(
        ctx.module, type, false, GlobalValue::InternalLinkage,
        UndefValue::get(type), global.getName(), nullptr,
        GlobalValue::NotThreadLocal, address_space);
    mapped->setAlignment(global.getAlign().valueOrOne());
    ctx.globals[&global] = mapped;
  }

  for (const auto &block : source)
    ctx.blocks[&block] = BasicBlock::Create(ctx.llvm, block.getName(), ctx.function);
  if (auto err = BuildPhiNodes(source, ctx))
    return err;

  if (!ctx.function->getReturnType()->isVoidTy()) {
    ctx.builder.SetInsertPoint(ctx.blocks[&source.getEntryBlock()]);
    ctx.return_value = ctx.builder.CreateAlloca(ctx.function->getReturnType());
    ctx.builder.CreateStore(
        ConstantAggregateZero::get(ctx.function->getReturnType()),
        ctx.return_value);
  }

  for (const auto &block : source) {
    ctx.builder.SetInsertPoint(ctx.blocks[&block]);
    Value *current_return =
        ctx.function->getReturnType()->isVoidTy()
            ? nullptr
            : ctx.builder.CreateLoad(ctx.function->getReturnType(),
                                     ctx.return_value);
    for (const auto &instruction : block) {
      if (isa<PHINode>(&instruction) || instruction.isTerminator())
        continue;
      if (auto err = LowerInstruction(instruction, ctx, current_return))
        return err;
      if (ctx.return_value)
        ctx.builder.CreateStore(current_return, ctx.return_value);
    }
    const auto *terminator = block.getTerminator();
    if (!terminator)
      return Unsupported("DXIL basic block without terminator");
    if (auto err = LowerTerminator(*terminator, ctx, current_return))
      return err;
  }

  PatchPhiNodes(source, ctx);
  return Error::success();
}

SM50_SHADER_METAL_VERSION
GetMetalVersion(SM50_SHADER_COMPILATION_ARGUMENT_DATA *args) {
  SM50_SHADER_COMMON_DATA *common = nullptr;
  if (dxbc::args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(
          args, &common))
    return common->metal_version;
  return SM50_SHADER_METAL_310;
}

} // namespace

void
BuildDxilShaderInfo(const dxil::DxilTranslationInfo &translation,
                    dxbc::ShaderInfo &shader_info) {
  BuildDxilShaderInfoImpl(translation, shader_info);
}

llvm::Error
ConvertDxilToAir(const dxil::Parser &parser, const char *name,
                 LLVMContext &context, Module &module,
                 SM50_SHADER_COMPILATION_ARGUMENT_DATA *args) {
  const auto &translation = parser.dxilTranslation();
  if (!translation || !translation->valid)
    return Unsupported("DXIL translation info is not available");

  if (translation->shader_kind != uint32_t(DxilStage::Vertex) &&
      translation->shader_kind != uint32_t(DxilStage::Pixel) &&
      translation->shader_kind != uint32_t(DxilStage::Compute)) {
    return Unsupported(std::string("DXIL ") + StageName(translation->shader_kind) +
                       " shader translation is not implemented yet");
  }

  auto input = ParseInputModule(parser, context);
  if (!input)
    return input.takeError();
  auto input_module = std::move(*input);
  const auto *source = FindEntryFunction(*input_module, *translation);
  if (!source)
    return Unsupported("DXIL entry function was not found");

  dxbc::ShaderInfo shader_info;
  BuildDxilShaderInfoImpl(*translation, shader_info);

  SM50_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout = nullptr;
  dxbc::args_get_data<SM50_SHADER_IA_INPUT_LAYOUT,
                      SM50_SHADER_IA_INPUT_LAYOUT_DATA>(args, &ia_layout);

  air::FunctionSignatureBuilder signature;
  air::AirType types(context);
  llvm::raw_null_ostream null_debug;
  auto *entry_block = BasicBlock::Create(context, "entry.stub");
  IRBuilder<> builder(entry_block);
  llvm::air::AIRBuilder air_builder(builder, null_debug);
  const auto metal_version = GetMetalVersion(args);

  DxilAirContext dxil_ctx{
      .llvm = context,
      .module = module,
      .builder = builder,
      .air = air_builder,
      .types = types,
      .translation = *translation,
      .source_function = source,
      .shader_info = std::move(shader_info),
      .metal_version = metal_version,
  };
  if (translation->shader_kind == uint32_t(DxilStage::Vertex) && ia_layout) {
    dxil_ctx.vertex_slot_mask = ia_layout->slot_mask;
    dxil_ctx.vertex_id_arg = signature.DefineInput(air::InputVertexID{});
    dxil_ctx.base_vertex_arg = signature.DefineInput(air::InputBaseVertex{});
    dxil_ctx.instance_id_arg = signature.DefineInput(air::InputInstanceID{});
    dxil_ctx.base_instance_arg = signature.DefineInput(air::InputBaseInstance{});
    dxil_ctx.vertex_buffer_table_arg = signature.DefineInput(
        air::ArgumentBindingBuffer{
            .buffer_size = {},
            .location_index = 16,
            .array_size = 0,
            .memory_access = air::MemoryAccess::read,
            .address_space = air::AddressSpace::constant,
            .type = air::msl_uint,
            .arg_name = "vertex_buffers",
            .raster_order_group = {},
        });
  }
  BuildSignature(*translation, signature, dxil_ctx, ia_layout);

  dxbc::setup_binding_table(&dxil_ctx.shader_info, dxil_ctx.resources,
                            signature, module);
  auto [function, metadata] = signature.CreateFunction(
      name ? name : "main0", context, module, 0, false);
  dxil_ctx.function = function;

  if (entry_block->getParent())
    entry_block->removeFromParent();
  delete entry_block;

  if (auto err = LowerFunction(*source, dxil_ctx))
    return err;

  dxbc::setup_metal_version(module, metal_version);
  switch (translation->shader_kind) {
  case uint32_t(DxilStage::Vertex):
    module.getOrInsertNamedMetadata("air.vertex")->addOperand(metadata);
    break;
  case uint32_t(DxilStage::Pixel):
    module.getOrInsertNamedMetadata("air.fragment")->addOperand(metadata);
    break;
  case uint32_t(DxilStage::Compute):
    module.getOrInsertNamedMetadata("air.kernel")->addOperand(metadata);
    break;
  default:
    break;
  }
  return Error::success();
}

void
FillDxilReflection(const dxil::Parser &parser, MTL_SHADER_REFLECTION *reflection) {
  if (!reflection)
    return;

  std::memset(reflection, 0, sizeof(*reflection));
  reflection->ConstanttBufferTableBindIndex = dxbc::kConstantBufferBindIndex;
  reflection->ArgumentBufferBindIndex = dxbc::kArgumentBufferBindIndex;

  const auto &translation = parser.dxilTranslation();
  if (!translation)
    return;

  dxbc::ShaderInfo shader_info;
  BuildDxilShaderInfo(*translation, shader_info);

  uint32_t cbv_count = 0;
  uint32_t argument_count = 0;
  for (const auto &resource : translation->resources) {
    switch (resource.resource_class) {
    case dxil::DxilTranslationResourceClass::Cbv: {
      auto it = shader_info.cbufferMap.find(resource.id);
      if (it == shader_info.cbufferMap.end())
        break;
      const auto binding_slot = it->second.range.binding_slot;
      cbv_count++;
      if (binding_slot < 16)
        reflection->ConstantBufferSlotMask |= uint16_t(1u << binding_slot);
      break;
    }
    case dxil::DxilTranslationResourceClass::Sampler: {
      auto it = shader_info.samplerMap.find(resource.id);
      if (it == shader_info.samplerMap.end())
        break;
      const auto binding_slot = it->second.range.binding_slot;
      argument_count++;
      if (binding_slot < 16)
        reflection->SamplerSlotMask |= uint16_t(1u << binding_slot);
      break;
    }
    case dxil::DxilTranslationResourceClass::Srv: {
      auto it = shader_info.srvMap.find(resource.id);
      if (it == shader_info.srvMap.end())
        break;
      const auto binding_slot = it->second.range.binding_slot;
      argument_count++;
      if (binding_slot < 64)
        reflection->SRVSlotMaskLo |= uint64_t(1) << binding_slot;
      else if (binding_slot < 128)
        reflection->SRVSlotMaskHi |= uint64_t(1) << (binding_slot - 64);
      break;
    }
    case dxil::DxilTranslationResourceClass::Uav: {
      auto it = shader_info.uavMap.find(resource.id);
      if (it == shader_info.uavMap.end())
        break;
      const auto binding_slot = it->second.range.binding_slot;
      argument_count++;
      if (binding_slot < 64)
        reflection->UAVSlotMask |= uint64_t(1) << binding_slot;
      break;
    }
    default:
      break;
    }
  }
  reflection->NumConstantBuffers = cbv_count;
  reflection->NumArguments = argument_count;
  reflection->ArgumentTableQwords = argument_count;
  reflection->ThreadgroupSize[0] = translation->num_threads_x;
  reflection->ThreadgroupSize[1] = translation->num_threads_y;
  reflection->ThreadgroupSize[2] = translation->num_threads_z;

  for (const auto &sig : translation->signatures) {
    if (sig.kind == dxil::DxilTranslationSignatureKind::Output &&
        translation->shader_kind == uint32_t(DxilStage::Pixel)) {
      if (IsSystemSemantic(sig, "SV_Target") && sig.semantic_index < 32)
        reflection->PSValidRenderTargets |= 1u << sig.semantic_index;
    }
  }
}

} // namespace dxmt::airconv
