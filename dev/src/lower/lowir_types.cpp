#include "lower/lowir_lowering.h"

#include <sstream>
#include <stdexcept>

namespace lowir_lowering {

namespace {

lowir_model::LowType MakeLowType(const char* text)
{
  lowir_model::LowType result;
  result.text = text;
  return result;
}

abi_mangle::AbiType Builtin(abi_mangle::AbiBuiltinType type)
{
  abi_mangle::AbiType result;
  result.kind = abi_mangle::ABI_TYPE_BUILTIN;
  result.builtin = type;
  return result;
}

}  // namespace

lowir_model::LowType Lowerer::LowTypeOf(TypeId type) const
{
  if (type == 0)
    throw std::logic_error("unsupported in CP1: invalid type");
  const TypeKind kind = types_.Kind(type);
  if (kind == TYPE_CV)
    return LowTypeOf(types_.At(type).base);
  if (kind == TYPE_REFERENCE || kind == TYPE_POINTER ||
      kind == TYPE_MEMBER_POINTER)
    return MakeLowType("ptr");
  if (kind == TYPE_ARRAY) {
    std::ostringstream out;
    out << "obj<" << types_.SizeOf(type) << "x" << types_.AlignOf(type)
        << ">";
    lowir_model::LowType result;
    result.text = out.str();
    return result;
  }
  if (kind == TYPE_ENUM)
    return LowTypeOf(types_.At(type).base);
  if (kind != TYPE_FUNDAMENTAL)
    throw std::logic_error("unsupported in CP1: non-scalar object type");
  switch (types_.At(type).fundamental) {
  case FT_VOID: return MakeLowType("void");
  case FT_BOOL: return MakeLowType("u8");
  case FT_SIGNED_CHAR: case FT_CHAR: return MakeLowType("i8");
  case FT_UNSIGNED_CHAR: return MakeLowType("u8");
  case FT_SHORT_INT: return MakeLowType("i16");
  case FT_UNSIGNED_SHORT_INT: return MakeLowType("u16");
  case FT_INT: return MakeLowType("i32");
  case FT_UNSIGNED_INT: return MakeLowType("u32");
  case FT_LONG_INT: case FT_LONG_LONG_INT: return MakeLowType("i64");
  case FT_UNSIGNED_LONG_INT: case FT_UNSIGNED_LONG_LONG_INT:
    return MakeLowType("i64");
  case FT_WCHAR_T: return MakeLowType("i32");
  case FT_CHAR16_T: return MakeLowType("u16");
  case FT_CHAR32_T: return MakeLowType("u32");
  case FT_FLOAT: return MakeLowType("f32");
  case FT_DOUBLE: return MakeLowType("f64");
  case FT_LONG_DOUBLE: return MakeLowType("f80");
  case FT_NULLPTR_T: return MakeLowType("ptr");
  }
  throw std::logic_error("unsupported in CP1: fundamental type");
}

lowir_model::LowType Lowerer::LowTypeOfUnqualified(TypeId type) const
{
  return LowTypeOf(types_.Unqualified(type));
}

bool Lowerer::IsUnsigned(TypeId type) const
{
  type = types_.Unqualified(type);
  if (types_.Kind(type) == TYPE_ENUM)
    return IsUnsigned(types_.At(type).base);
  return types_.Kind(type) == TYPE_FUNDAMENTAL &&
      FundamentalIsUnsigned(types_.At(type).fundamental);
}

unsigned Lowerer::TypeBits(TypeId type) const
{
  const lowir_model::LowTypeInfo info =
      lowir_model::describe_low_type(LowTypeOf(type));
  return info.integer() ? info.bits : 0;
}

bool Lowerer::IsScalar(TypeId type) const
{
  return types_.IsScalar(type) || types_.Kind(types_.Unqualified(type)) ==
      TYPE_ENUM;
}

std::string Lowerer::NamedType(TypeId type) const
{
  const TypeNode& node = types_.At(types_.Unqualified(type));
  if (node.name.empty())
    return "<anonymous>";
  return node.name;
}

abi_mangle::AbiType Lowerer::AbiTypeOf(TypeId type) const
{
  if (type == 0)
    throw std::logic_error("unsupported in CP1: invalid ABI type");
  const TypeNode& node = types_.At(type);
  if (node.kind == TYPE_CV) {
    abi_mangle::AbiType result = AbiTypeOf(node.base);
    result.is_const = result.is_const || node.is_const;
    result.is_volatile = result.is_volatile || node.is_volatile;
    return result;
  }
  if (node.kind == TYPE_FUNDAMENTAL) {
    switch (node.fundamental) {
    case FT_VOID: return Builtin(abi_mangle::ABI_BUILTIN_VOID);
    case FT_BOOL: return Builtin(abi_mangle::ABI_BUILTIN_BOOL);
    case FT_CHAR: return Builtin(abi_mangle::ABI_BUILTIN_CHAR);
    case FT_SIGNED_CHAR: return Builtin(abi_mangle::ABI_BUILTIN_SCHAR);
    case FT_UNSIGNED_CHAR: return Builtin(abi_mangle::ABI_BUILTIN_UCHAR);
    case FT_SHORT_INT: return Builtin(abi_mangle::ABI_BUILTIN_SHORT);
    case FT_UNSIGNED_SHORT_INT: return Builtin(abi_mangle::ABI_BUILTIN_USHORT);
    case FT_INT: return Builtin(abi_mangle::ABI_BUILTIN_INT);
    case FT_UNSIGNED_INT: return Builtin(abi_mangle::ABI_BUILTIN_UINT);
    case FT_LONG_INT: return Builtin(abi_mangle::ABI_BUILTIN_LONG);
    case FT_UNSIGNED_LONG_INT: return Builtin(abi_mangle::ABI_BUILTIN_ULONG);
    case FT_LONG_LONG_INT: return Builtin(abi_mangle::ABI_BUILTIN_LONGLONG);
    case FT_UNSIGNED_LONG_LONG_INT: return Builtin(abi_mangle::ABI_BUILTIN_ULONGLONG);
    case FT_WCHAR_T: return Builtin(abi_mangle::ABI_BUILTIN_WCHAR);
    case FT_CHAR16_T: return Builtin(abi_mangle::ABI_BUILTIN_CHAR16);
    case FT_CHAR32_T: return Builtin(abi_mangle::ABI_BUILTIN_CHAR32);
    case FT_FLOAT: return Builtin(abi_mangle::ABI_BUILTIN_FLOAT);
    case FT_DOUBLE: return Builtin(abi_mangle::ABI_BUILTIN_DOUBLE);
    case FT_LONG_DOUBLE: return Builtin(abi_mangle::ABI_BUILTIN_LONGDOUBLE);
    case FT_NULLPTR_T: return Builtin(abi_mangle::ABI_BUILTIN_NULLPTR);
    }
  }
  if (node.kind == TYPE_POINTER) {
    abi_mangle::AbiType result;
    result.kind = abi_mangle::ABI_TYPE_POINTER;
    result.types.push_back(AbiTypeOf(node.base));
    return result;
  }
  if (node.kind == TYPE_REFERENCE) {
    abi_mangle::AbiType result;
    result.kind = node.lvalue_reference ? abi_mangle::ABI_TYPE_LVALUE_REFERENCE :
        abi_mangle::ABI_TYPE_RVALUE_REFERENCE;
    result.types.push_back(AbiTypeOf(node.base));
    return result;
  }
  if (node.kind == TYPE_ARRAY) {
    abi_mangle::AbiType result;
    result.kind = abi_mangle::ABI_TYPE_ARRAY;
    result.array_bound.kind = abi_mangle::ABI_ARRAY_BOUND_VALUE;
    result.array_bound.value = std::to_string(node.array_bound);
    result.types.push_back(AbiTypeOf(node.base));
    return result;
  }
  if (node.kind == TYPE_FUNCTION) {
    abi_mangle::AbiType result;
    result.kind = abi_mangle::ABI_TYPE_FUNCTION;
    result.types.push_back(AbiTypeOf(node.result));
    for (std::size_t i = 0; i < node.parameters.size(); ++i)
      result.types.push_back(AbiTypeOf(node.parameters[i]));
    result.variadic = node.variadic;
    return result;
  }
  if (node.kind == TYPE_ENUM || node.kind == TYPE_CLASS) {
    abi_mangle::AbiType result;
    result.kind = abi_mangle::ABI_TYPE_NAMED;
    result.name = NamedType(type);
    return result;
  }
  throw std::logic_error("unsupported in CP1: ABI type");
}

}  // namespace lowir_lowering
