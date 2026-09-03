// Type classification, LowIR type rendering, and the PA14 ABI type adapter.
#include "lower/lowir_lowering.h"

#include <algorithm>
#include <stdexcept>

namespace lowir_lowering {

namespace {

lowir_model::LowType MakeLowType(const std::string& text)
{
  lowir_model::LowType result;
  result.text = text;
  return result;
}

lowir_model::Operand NamedOperand(lowir_model::Operand::Kind kind,
                                  const std::string& text)
{
  lowir_model::Operand result;
  result.kind = kind;
  result.text = text;
  return result;
}

LowInfo KindInfo(LowInfo::Kind kind)
{
  LowInfo result;
  result.kind = kind;
  return result;
}

LowInfo IntegerInfo(unsigned bits, bool is_unsigned)
{
  LowInfo result = KindInfo(LowInfo::LK_INTEGER);
  result.bits = bits;
  result.is_unsigned = is_unsigned;
  return result;
}

LowInfo FloatInfo(unsigned bits)
{
  LowInfo result = KindInfo(LowInfo::LK_FLOAT);
  result.bits = bits;
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

bool SameStorage(const LowInfo& left, const LowInfo& right)
{
  if (left.kind != right.kind)
    return false;
  switch (left.kind) {
  case LowInfo::LK_INTEGER:
    return left.bits == right.bits &&
        (left.bits == 64 || left.is_unsigned == right.is_unsigned);
  case LowInfo::LK_FLOAT:
    return left.bits == right.bits;
  case LowInfo::LK_OBJECT:
    return left.bytes == right.bytes && left.alignment == right.alignment;
  default:
    return true;
  }
}

lowir_model::LowType RenderLowType(const LowInfo& info)
{
  switch (info.kind) {
  case LowInfo::LK_VOID:
    return MakeLowType("void");
  case LowInfo::LK_INTEGER:
    return MakeLowType((info.is_unsigned && info.bits < 64 ? "u" : "i") +
                       std::to_string(info.bits));
  case LowInfo::LK_FLOAT:
    return MakeLowType("f" + std::to_string(info.bits));
  case LowInfo::LK_POINTER:
    return MakeLowType("ptr");
  case LowInfo::LK_OBJECT:
    return MakeLowType("obj<" + std::to_string(info.bytes) + "x" +
                       std::to_string(info.alignment) + ">");
  }
  return lowir_model::LowType();
}

lowir_model::LowType PtrType() { return MakeLowType("ptr"); }
lowir_model::LowType I64Type() { return MakeLowType("i64"); }
lowir_model::LowType I8Type() { return MakeLowType("i8"); }
lowir_model::LowType VoidType() { return MakeLowType("void"); }

lowir_model::Operand TempOperand(const std::string& name)
{
  return NamedOperand(lowir_model::Operand::OP_TEMP, name);
}

lowir_model::Operand SlotOperand(const std::string& name)
{
  return NamedOperand(lowir_model::Operand::OP_SLOT, name);
}

lowir_model::Operand GlobalOperand(const std::string& name)
{
  return NamedOperand(lowir_model::Operand::OP_GLOBAL, name);
}

lowir_model::Operand LabelOperand(const std::string& name)
{
  return NamedOperand(lowir_model::Operand::OP_LABEL, name);
}

lowir_model::Operand Immediate(long long value)
{
  lowir_model::Operand result;
  result.kind = lowir_model::Operand::OP_INTEGER;
  result.int_value = value;
  result.text = std::to_string(value);
  return result;
}

// The null pointer is the one immediate LowIR spells as a word.
lowir_model::Operand NullptrImmediate()
{
  lowir_model::Operand result = Immediate(0);
  result.text = "nullptr";
  return result;
}

lowir_model::Operand FloatImmediate(const std::string& spelling,
                                    long double value,
                                    const lowir_model::LowType& type)
{
  lowir_model::Operand result;
  result.kind = lowir_model::Operand::OP_FLOAT;
  result.text = spelling;
  result.float_value = value;
  result.literal_type = type;
  return result;
}

bool IsLogicalOperator(ETokenType op)
{
  return op == OP_LAND || op == OP_LOR;
}

bool IsScalarBinaryOperator(ETokenType op)
{
  switch (op) {
  case OP_PLUS: case OP_MINUS: case OP_STAR: case OP_DIV: case OP_MOD:
  case OP_AMP: case OP_BOR: case OP_XOR: case OP_LSHIFT: case OP_RSHIFT:
    return true;
  default:
    return false;
  }
}

bool IsKnownIntegralLiteral(const SemaNode& node, const TypeTable& types)
{
  if (node.kind != SEMA_LITERAL || !node.has_value)
    return false;
  const TypeId type = types.Unqualified(node.type);
  return types.Kind(type) == TYPE_FUNDAMENTAL &&
      !IsFloatingType(types, type);
}

bool IsFloatingType(const TypeTable& types, TypeId type)
{
  type = types.Unqualified(type);
  if (types.Kind(type) != TYPE_FUNDAMENTAL)
    return false;
  const EFundamentalType fundamental = types.At(type).fundamental;
  return fundamental == FT_FLOAT || fundamental == FT_DOUBLE ||
      fundamental == FT_LONG_DOUBLE;
}

bool IsVoidType(const TypeTable& types, TypeId type)
{
  type = types.Unqualified(type);
  return types.Kind(type) == TYPE_FUNDAMENTAL &&
      types.At(type).fundamental == FT_VOID;
}

bool IsBoolType(const TypeTable& types, TypeId type)
{
  type = types.Unqualified(type);
  return types.Kind(type) == TYPE_FUNDAMENTAL &&
      types.At(type).fundamental == FT_BOOL;
}

LowInfo Lowerer::LowInfoOf(TypeId type) const
{
  if (type == 0)
    Unsupported("an invalid type");
  const TypeNode& node = types_.At(type);
  switch (node.kind) {
  case TYPE_CV:
  case TYPE_ENUM: // the underlying type owns the storage
    return LowInfoOf(node.base);
  case TYPE_REFERENCE:
  case TYPE_POINTER:
  case TYPE_MEMBER_POINTER:
    return KindInfo(LowInfo::LK_POINTER);
  case TYPE_ARRAY: {
    LowInfo result = KindInfo(LowInfo::LK_OBJECT);
    result.bytes = types_.SizeOf(type);
    result.alignment = types_.AlignOf(type);
    return result;
  }
  case TYPE_CLASS: {
    LowInfo result = KindInfo(LowInfo::LK_OBJECT);
    result.bytes = types_.SizeOf(type);
    result.alignment = types_.AlignOf(type);
    return result;
  }
  case TYPE_FUNDAMENTAL:
    switch (node.fundamental) {
    case FT_VOID: return KindInfo(LowInfo::LK_VOID);
    case FT_BOOL: return IntegerInfo(8, true);
    case FT_CHAR: case FT_SIGNED_CHAR: return IntegerInfo(8, false);
    case FT_UNSIGNED_CHAR: return IntegerInfo(8, true);
    case FT_SHORT_INT: return IntegerInfo(16, false);
    case FT_UNSIGNED_SHORT_INT: case FT_CHAR16_T: return IntegerInfo(16, true);
    case FT_INT: case FT_WCHAR_T: return IntegerInfo(32, false);
    case FT_UNSIGNED_INT: case FT_CHAR32_T: return IntegerInfo(32, true);
    case FT_LONG_INT: case FT_LONG_LONG_INT: return IntegerInfo(64, false);
    case FT_UNSIGNED_LONG_INT: case FT_UNSIGNED_LONG_LONG_INT:
      return IntegerInfo(64, true);
    case FT_FLOAT: return FloatInfo(32);
    case FT_DOUBLE: return FloatInfo(64);
    case FT_LONG_DOUBLE: return FloatInfo(80);
    // `nullptr_t` has pointer semantics in overload resolution and ABI
    // mangling, but LowIR carries its null value as the target's 64-bit
    // integer word.  Keeping that representation distinct from `ptr` also
    // preserves the callable signature of a function taking nullptr_t.
    case FT_NULLPTR_T: return IntegerInfo(64, false);
    }
    break;
  default:
    break;
  }
  Unsupported("a non-scalar object type");
  return LowInfo();
}

lowir_model::LowType Lowerer::LowTypeOf(TypeId type) const
{
  return RenderLowType(LowInfoOf(type));
}

ClassBoundaryMode Lowerer::ClassBoundary(TypeId type, bool result) const
{
  TypeId class_type = types_.Unqualified(type);
  if (types_.Kind(class_type) != TYPE_CLASS)
    return CBM_DIRECT_OBJECT;
  const ClassEntity& entity = model_.ClassAt(types_.At(class_type).entity);
  if (!entity.trivially_copyable)
    return result ? CBM_INDIRECT_RESULT : CBM_BY_ADDRESS;
  if (result && entity.size > 8)
    return CBM_INDIRECT_RESULT;
  return CBM_DIRECT_OBJECT;
}

bool Lowerer::IsUnsigned(TypeId type) const
{
  return LowInfoOf(type).is_unsigned;
}

unsigned Lowerer::TypeBits(TypeId type) const
{
  const LowInfo info = LowInfoOf(type);
  return info.Integer() ? info.bits : 0;
}

abi_mangle::AbiType Lowerer::AbiTypeOf(TypeId type) const
{
  if (type == 0)
    Unsupported("an invalid ABI type");
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
    case FT_UNSIGNED_LONG_LONG_INT:
      return Builtin(abi_mangle::ABI_BUILTIN_ULONGLONG);
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
    result.kind = node.lvalue_reference ?
        abi_mangle::ABI_TYPE_LVALUE_REFERENCE :
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
    result.kind = node.kind == TYPE_CLASS ?
        abi_mangle::ABI_TYPE_NAME_OR_REFERENCE : abi_mangle::ABI_TYPE_NAMED;
    result.name = QualifiedTypeName(type);
    return result;
  }
  Unsupported("an ABI type");
  return abi_mangle::AbiType();
}

std::string Lowerer::QualifiedTypeName(TypeId type) const
{
  const TypeNode& node = types_.At(types_.Unqualified(type));
  if (node.name.empty())
    return "<anonymous>";

  ScopeId scope = 0;
  if (node.kind == TYPE_CLASS && node.entity != 0)
    scope = model_.ClassAt(node.entity).class_scope;
  else if (node.kind == TYPE_ENUM && node.entity != 0)
    scope = model_.EnumAt(node.entity).enum_scope;
  if (scope == 0)
    return node.name;

  // The type node owns the leaf spelling (including template arguments);
  // declaration scopes own only its enclosing namespace/class path.
  const std::vector<std::string> pieces =
      NamespacePieces(model_.ScopeAt(scope).parent);
  std::string result;
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    if (!result.empty())
      result += "::";
    result += pieces[i];
  }
  if (!result.empty())
    result += "::";
  result += node.name;
  return result;
}

}  // namespace lowir_lowering
