#include "lower/lowir_lowering.h"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace lowir_lowering {

namespace {

lowir_model::Operand IntegerOperand(long long value)
{
  lowir_model::Operand result;
  result.kind = lowir_model::Operand::OP_INTEGER;
  result.int_value = value;
  result.text = std::to_string(value);
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

lowir_model::Operand FloatOperand(const std::string& spelling,
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

bool IsVoid(const TypeTable& types, TypeId type)
{
  type = types.Unqualified(type);
  return types.Kind(type) == TYPE_FUNDAMENTAL &&
      types.At(type).fundamental == FT_VOID;
}

bool IsFloat(const TypeTable& types, TypeId type)
{
  type = types.Unqualified(type);
  if (types.Kind(type) != TYPE_FUNDAMENTAL)
    return false;
  return types.At(type).fundamental == FT_FLOAT ||
      types.At(type).fundamental == FT_DOUBLE ||
      types.At(type).fundamental == FT_LONG_DOUBLE;
}

bool IsKnownIntegralLiteral(const SemaNode& node, const TypeTable& types)
{
  if (node.kind != SEMA_LITERAL || !node.has_value)
    return false;
  const TypeId type = types.Unqualified(node.type);
  if (types.Kind(type) != TYPE_FUNDAMENTAL)
    return false;
  const EFundamentalType fundamental = types.At(type).fundamental;
  return fundamental != FT_FLOAT && fundamental != FT_DOUBLE &&
      fundamental != FT_LONG_DOUBLE;
}

bool IsLogical(ETokenType op)
{
  return op == OP_LAND || op == OP_LOR;
}

}  // namespace

lowir_model::Operand Lowerer::ZeroOperand(TypeId type) const
{
  if (IsFloat(types_, type)) {
    return FloatOperand("0.0", 0.0L, LowTypeOf(type));
  }
  return IntegerOperand(0);
}

lowir_model::Operand Lowerer::OneOperand(TypeId type) const
{
  if (IsFloat(types_, type))
    return FloatOperand("1.0", 1.0L, LowTypeOf(type));
  return IntegerOperand(1);
}

std::string Lowerer::BinaryName(ETokenType op) const
{
  switch (op) {
  case OP_PLUS: return "add";
  case OP_MINUS: return "sub";
  case OP_STAR: return "mul";
  case OP_DIV: return "div";
  case OP_MOD: return "rem";
  case OP_AMP: return "and";
  case OP_BOR: return "or";
  case OP_XOR: return "xor";
  case OP_LSHIFT: return "shl";
  case OP_RSHIFT: return "ashr";
  default: break;
  }
  throw std::logic_error("unsupported in CP1: binary operator");
}

std::string Lowerer::CompareName(ETokenType op, TypeId type) const
{
  const bool is_float = IsFloat(types_, type);
  switch (op) {
  case OP_EQ: return "eq";
  case OP_NE: return "ne";
  case OP_LT: return is_float ? "lt" : (IsUnsigned(type) ? "ult" : "lt");
  case OP_LE: return is_float ? "le" : (IsUnsigned(type) ? "ule" : "le");
  case OP_GT: return is_float ? "gt" : (IsUnsigned(type) ? "ugt" : "gt");
  case OP_GE: return is_float ? "ge" : (IsUnsigned(type) ? "uge" : "ge");
  default: break;
  }
  throw std::logic_error("unsupported in CP1: comparison operator");
}

std::string Lowerer::ConversionName(TypeId from, TypeId to) const
{
  const lowir_model::LowTypeInfo source =
      lowir_model::describe_low_type(LowTypeOf(from));
  const lowir_model::LowTypeInfo target =
      lowir_model::describe_low_type(LowTypeOf(to));
  if (source.integer() && target.integer()) {
    if (target.bits > source.bits)
      return IsUnsigned(from) ? "zext" : "sext";
    if (target.bits < source.bits)
      return "trunc";
    return "bitcast";
  }
  if (source.integer() && target.floating())
    return IsUnsigned(from) ? "uitofp" : "sitofp";
  if (source.floating() && target.integer())
    return "fptoui";
  if (source.floating() && target.floating())
    return target.bits > source.bits ? "fpext" : "fptrunc";
  if (source.pointer() && target.pointer())
    return "bitcast";
  throw std::logic_error("unsupported in CP1: scalar conversion");
}

Lowerer::Value Lowerer::Convert(Value value, TypeId target)
{
  if (target == 0 || value.type == 0)
    return value;
  const lowir_model::LowType source_type = LowTypeOf(value.type);
  const lowir_model::LowType target_type = LowTypeOf(target);
  if (source_type.text == target_type.text) {
    value.type = target;
    return value;
  }
  // The literal's bits are already known exactly.  Keeping an immediate
  // immediate avoids manufacturing a temporary for the common widening case.
  if (value.operand.kind == lowir_model::Operand::OP_INTEGER ||
      value.operand.kind == lowir_model::Operand::OP_FLOAT) {
    value.type = target;
    return value;
  }
  lowir_model::Instruction instruction;
  instruction.kind = lowir_model::Instruction::IK_CONVERT;
  instruction.dest = NewTemp();
  instruction.type = target_type;
  instruction.source_type = source_type;
  instruction.op = ConversionName(value.type, target);
  instruction.first = value.operand;
  Emit(instruction);
  value.operand.kind = lowir_model::Operand::OP_TEMP;
  value.operand.text = instruction.dest;
  value.operand.literal_type = target_type;
  value.type = target;
  value.lvalue = false;
  return value;
}

Lowerer::Value Lowerer::ConvertExpression(Value value, TypeId target)
{
  if (target == 0 || value.type == 0)
    return value;
  const lowir_model::LowType source_type = LowTypeOf(value.type);
  const lowir_model::LowType target_type = LowTypeOf(target);
  if (source_type.text == target_type.text) {
    value.type = target;
    return value;
  }
  if (value.operand.kind != lowir_model::Operand::OP_INTEGER &&
      value.operand.kind != lowir_model::Operand::OP_FLOAT)
    return Convert(value, target);
  const lowir_model::LowTypeInfo source_info =
      lowir_model::describe_low_type(source_type);
  const lowir_model::LowTypeInfo target_info =
      lowir_model::describe_low_type(target_type);
  if (source_info.integer() && target_info.integer() &&
      source_info.bits == target_info.bits)
    return Convert(value, target);
  lowir_model::Instruction instruction;
  instruction.kind = lowir_model::Instruction::IK_CONVERT;
  instruction.dest = NewTemp();
  instruction.type = target_type;
  instruction.source_type = source_type;
  instruction.op = ConversionName(value.type, target);
  instruction.first = value.operand;
  Emit(instruction);
  value.type = target;
  value.operand = NamedOperand(lowir_model::Operand::OP_TEMP,
                               instruction.dest);
  return value;
}

Lowerer::Value Lowerer::MakeBoolValue(const lowir_model::Operand& operand)
{
  Value result;
  result.operand = operand;
  result.type = 0;
  result.lvalue = false;
  return result;
}

Lowerer::Value Lowerer::LowerLiteral(const SemaNode& node, TypeId expected)
{
  Value result;
  result.type = node.type;
  result.lvalue = false;
  long long value = node.value;
  if (node.binding != 0 &&
      model_.BindingAt(node.binding).kind == BINDING_ENUMERATOR) {
    value = node.value;
    result.operand = IntegerOperand(value);
  } else if (node.HasSpan() && node.first < tokens_.size()) {
    const Pa6Token& token = tokens_[node.first];
    if (token.kind == PA6_LITERAL_TOKEN && !token.lit_scalar) {
      Unsupported("string literals");
    }
    if (token.lit_type == FT_FLOAT || token.lit_type == FT_DOUBLE ||
        token.lit_type == FT_LONG_DOUBLE) {
      char* end = 0;
      const long double parsed = std::strtold(token.spelling.c_str(), &end);
      if (end == token.spelling.c_str())
        throw std::logic_error("invalid floating literal");
      result.operand = FloatOperand(token.spelling, parsed, LowTypeOf(node.type));
    } else if (token.IsSimple(KW_TRUE) || token.IsSimple(KW_FALSE)) {
      result.operand = IntegerOperand(token.IsSimple(KW_TRUE) ? 1 : 0);
    } else {
      unsigned long long raw = token.lit_value;
      const unsigned bits = TypeBits(node.type);
      if (!IsUnsigned(node.type) && bits != 0 && bits < 64 &&
          (raw & (1ULL << (bits - 1))) != 0)
        value = static_cast<long long>(raw | (~0ULL << bits));
      else
        value = static_cast<long long>(raw);
      result.operand = IntegerOperand(value);
    }
  } else if (node.has_value) {
    result.operand = IntegerOperand(node.value);
  } else {
    Unsupported("synthesized literal");
  }
  if (expected != 0)
    return Convert(result, expected);
  return result;
}

Lowerer::Value Lowerer::LowerLValue(SemaId node)
{
  if (node == 0)
    Unsupported("missing lvalue");
  const SemaNode& value = tree_.At(node);
  if (value.kind == SEMA_ID_EXPRESSION) {
    const Binding& binding = model_.BindingAt(value.binding);
    if (binding.kind == BINDING_VARIABLE || binding.kind == BINDING_PARAMETER) {
      if (types_.Kind(types_.Unqualified(binding.type)) == TYPE_REFERENCE)
        Unsupported("reference bindings");
      Value result;
      result.type = value.type;
      result.binding = value.binding;
      result.lvalue = true;
      result.operand.kind = lowir_model::Operand::OP_SLOT;
      result.operand.text = SlotFor(value.binding);
      return result;
    }
  }
  if (value.kind == SEMA_UNARY && value.op == OP_STAR) {
    const std::vector<SemaId> children = Children(node);
    if (children.size() != 1)
      Unsupported("indirection expression");
    Value pointer = LowerRValue(children[0]);
    Value result;
    result.type = value.type;
    result.lvalue = true;
    result.operand = pointer.operand;
    return result;
  }
  Unsupported("non-scalar lvalue");
  return Value();
}

Lowerer::Value Lowerer::LowerRValue(SemaId node, TypeId expected)
{
  if (node == 0)
    Unsupported("missing expression");
  const SemaNode& value = tree_.At(node);
  switch (value.kind) {
  case SEMA_LITERAL:
    return LowerLiteral(value, expected);
  case SEMA_ID_EXPRESSION: {
    const Binding& binding = model_.BindingAt(value.binding);
    if (binding.kind == BINDING_ENUMERATOR)
      return LowerLiteral(value, expected);
    if (binding.kind == BINDING_VARIABLE || binding.kind == BINDING_PARAMETER) {
      if (types_.Kind(types_.Unqualified(binding.type)) == TYPE_REFERENCE)
        Unsupported("reference bindings");
      lowir_model::Instruction load;
      load.kind = lowir_model::Instruction::IK_LOAD;
      load.dest = NewTemp();
      load.type = LowTypeOf(value.type);
      load.first.kind = lowir_model::Operand::OP_SLOT;
      load.first.text = SlotFor(value.binding);
      Emit(load);
      Value result;
      result.type = value.type;
      result.operand.kind = lowir_model::Operand::OP_TEMP;
      result.operand.text = load.dest;
      result.lvalue = false;
      return Convert(result, expected);
    }
    Unsupported("function value");
    break;
  }
  case SEMA_UNARY:
    return LowerUnary(node, false, expected);
  case SEMA_POSTFIX:
    return LowerUnary(node, true, expected);
  case SEMA_BINARY:
    return LowerBinary(node, expected);
  case SEMA_ASSIGNMENT:
    return LowerAssignment(node);
  case SEMA_CONDITIONAL:
    return LowerConditional(node, expected);
  case SEMA_CAST: {
    const std::vector<SemaId> children = Children(node);
    if (children.size() != 1)
      Unsupported("cast expression");
    if (IsVoid(types_, value.type)) {
      LowerDiscard(children[0]);
      Value result;
      result.type = value.type;
      return result;
    }
    Value result = LowerRValue(children[0], value.type);
    result = ConvertExpression(result, value.type);
    return Convert(result, expected);
  }
  case SEMA_SIZEOF: {
    lowir_model::Instruction constant;
    constant.kind = lowir_model::Instruction::IK_CONST;
    constant.dest = NewTemp();
    constant.type = LowTypeOf(value.type);
    constant.first = IntegerOperand(static_cast<long long>(value.has_value ?
        value.value : 0));
    Emit(constant);
    Value result;
    result.type = value.type;
    result.operand.kind = lowir_model::Operand::OP_TEMP;
    result.operand.text = constant.dest;
    return Convert(result, expected);
  }
  case SEMA_BRACED_INIT_LIST: {
    const std::vector<SemaId> children = Children(node);
    if (!children.empty())
      return LowerRValue(children[0], expected);
    TypeTable& mutable_types = const_cast<TypeTable&>(types_);
    Value zero;
    zero.type = mutable_types.Fundamental(FT_INT);
    zero.operand = IntegerOperand(0);
    return Convert(zero, expected == 0 ? value.type : expected);
  }
  case SEMA_CALL:
    return LowerCall(node, expected);
  case SEMA_CALLEE:
    Unsupported("callee outside a call");
    break;
  case SEMA_MEMBER:
  case SEMA_SUBSCRIPT:
    Unsupported(value.kind == SEMA_MEMBER ? "member expressions" : "subscripts");
    break;
  default:
    Unsupported("expression node");
  }
  return Value();
}

Lowerer::Value Lowerer::LowerExpression(SemaId node, TypeId expected)
{
  return LowerRValue(node, expected);
}

Lowerer::Value Lowerer::LowerUnary(SemaId node, bool postfix, TypeId expected)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 1)
    Unsupported("unary expression arity");
  const SemaId operand_node = children[0];
  if (value.op == OP_AMP) {
    Value lvalue = LowerLValue(operand_node);
    lowir_model::Instruction address;
    address.kind = lowir_model::Instruction::IK_ADDR;
    address.dest = NewTemp();
    address.first = lvalue.operand;
    Emit(address);
    Value result;
    result.type = value.type;
    result.operand.kind = lowir_model::Operand::OP_TEMP;
    result.operand.text = address.dest;
    return Convert(result, expected);
  }
  if (value.op == OP_STAR)
    return Convert(LowerRValue(operand_node), expected);
  if (value.op == OP_LNOT) {
    Value operand = LowerRValue(operand_node);
    lowir_model::Instruction compare;
    compare.kind = lowir_model::Instruction::IK_CMP;
    compare.dest = NewTemp();
    compare.op = "eq";
    compare.type = LowTypeOf(operand.type);
    compare.first = operand.operand;
    compare.second = ZeroOperand(operand.type);
    Emit(compare);
    Value result = MakeBoolValue(NamedOperand(lowir_model::Operand::OP_TEMP,
                                               compare.dest));
    result.type = value.type;
    return Convert(result, expected);
  }
  if (value.op == OP_PLUS)
    return Convert(LowerRValue(operand_node, value.type), expected);
  if (value.op == OP_MINUS || value.op == OP_COMPL) {
    Value operand = LowerRValue(operand_node, value.type);
    lowir_model::Instruction unary;
    unary.kind = lowir_model::Instruction::IK_UNARY;
    unary.dest = NewTemp();
    unary.op = value.op == OP_MINUS ? "neg" : "not";
    unary.type = LowTypeOf(value.type);
    unary.first = operand.operand;
    Emit(unary);
    Value result;
    result.type = value.type;
    result.operand.kind = lowir_model::Operand::OP_TEMP;
    result.operand.text = unary.dest;
    return Convert(result, expected);
  }
  if (value.op != OP_INC && value.op != OP_DEC)
    Unsupported("unary operator");

  Value lvalue = LowerLValue(operand_node);
  lowir_model::Instruction load;
  load.kind = lowir_model::Instruction::IK_LOAD;
  load.dest = NewTemp();
  load.type = LowTypeOf(lvalue.type);
  load.first = lvalue.operand;
  Emit(load);
  Value old;
  old.type = lvalue.type;
  old.operand.kind = lowir_model::Operand::OP_TEMP;
  old.operand.text = load.dest;
  const TypeId arithmetic_type = value.type == 0 ? lvalue.type : value.type;
  Value current = Convert(old, arithmetic_type);
  lowir_model::Instruction binary;
  binary.kind = lowir_model::Instruction::IK_BINARY;
  binary.dest = NewTemp();
  binary.op = value.op == OP_INC ? "add" : "sub";
  binary.type = LowTypeOf(arithmetic_type);
  binary.first = current.operand;
  binary.second = OneOperand(arithmetic_type);
  Emit(binary);
  Value updated;
  updated.type = arithmetic_type;
  updated.operand.kind = lowir_model::Operand::OP_TEMP;
  updated.operand.text = binary.dest;
  lowir_model::Instruction store;
  store.kind = lowir_model::Instruction::IK_STORE;
  store.type = LowTypeOf(lvalue.type);
  store.first = Convert(updated, lvalue.type).operand;
  store.second = lvalue.operand;
  Emit(store);
  return Convert(postfix ? old : updated, expected);
}

Lowerer::Value Lowerer::LowerBinary(SemaId node, TypeId expected)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 2)
    Unsupported("binary expression arity");
  if (value.op == OP_COMMA) {
    LowerDiscard(children[0]);
    return LowerRValue(children[1], expected);
  }
  if (value.op == OP_LAND || value.op == OP_LOR) {
    return Convert(LowerLogicalValue(node), expected);
  }
  if (value.op == OP_EQ || value.op == OP_NE || value.op == OP_LT ||
      value.op == OP_LE || value.op == OP_GT || value.op == OP_GE) {
    TypeTable& mutable_types = const_cast<TypeTable&>(types_);
    const TypeId common = mutable_types.UsualArithmetic(
        tree_.At(children[0]).type, tree_.At(children[1]).type);
    Value left = ConvertExpression(LowerRValue(children[0]), common);
    Value right = ConvertExpression(LowerRValue(children[1]), common);
    lowir_model::Instruction compare;
    compare.kind = lowir_model::Instruction::IK_CMP;
    compare.dest = NewTemp();
    compare.op = CompareName(value.op, common);
    compare.type = LowTypeOf(common);
    compare.first = left.operand;
    compare.second = right.operand;
    Emit(compare);
    Value result;
    result.type = value.type;
    result.operand.kind = lowir_model::Operand::OP_TEMP;
    result.operand.text = compare.dest;
    return Convert(result, expected);
  }
  TypeId common = value.type;
  if (value.op == OP_LSHIFT || value.op == OP_RSHIFT)
    common = tree_.At(children[0]).type;
  Value left = Convert(LowerRValue(children[0]), common);
  Value right = Convert(LowerRValue(children[1]), common);
  lowir_model::Instruction binary;
  binary.kind = lowir_model::Instruction::IK_BINARY;
  binary.dest = NewTemp();
  binary.type = LowTypeOf(common);
  if (value.op == OP_RSHIFT)
    binary.op = IsUnsigned(common) ? "lshr" : "ashr";
  else
    binary.op = BinaryName(value.op);
  binary.first = left.operand;
  binary.second = right.operand;
  Emit(binary);
  Value result;
  result.type = value.type;
  result.operand.kind = lowir_model::Operand::OP_TEMP;
  result.operand.text = binary.dest;
  return Convert(result, expected);
}

Lowerer::Value Lowerer::LowerAssignment(SemaId node)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 2)
    Unsupported("assignment expression arity");
  Value lhs = LowerLValue(children[0]);
  const TypeId target = lhs.type;
  Value rhs;
  if (value.op == OP_ASS) {
    rhs = LowerRValue(children[1], target);
  } else {
    ETokenType binary_op = OP_PLUS;
    switch (value.op) {
    case OP_PLUSASS: binary_op = OP_PLUS; break;
    case OP_MINUSASS: binary_op = OP_MINUS; break;
    case OP_STARASS: binary_op = OP_STAR; break;
    case OP_DIVASS: binary_op = OP_DIV; break;
    case OP_MODASS: binary_op = OP_MOD; break;
    case OP_BANDASS: binary_op = OP_AMP; break;
    case OP_BORASS: binary_op = OP_BOR; break;
    case OP_XORASS: binary_op = OP_XOR; break;
    case OP_LSHIFTASS: binary_op = OP_LSHIFT; break;
    case OP_RSHIFTASS: binary_op = OP_RSHIFT; break;
    default: Unsupported("compound assignment operator");
    }
    lowir_model::Instruction load;
    load.kind = lowir_model::Instruction::IK_LOAD;
    load.dest = NewTemp();
    load.type = LowTypeOf(target);
    load.first = lhs.operand;
    Emit(load);
    Value left;
    left.type = target;
    left.operand.kind = lowir_model::Operand::OP_TEMP;
    left.operand.text = load.dest;
    Value right = LowerRValue(children[1], target);
    lowir_model::Instruction binary;
    binary.kind = lowir_model::Instruction::IK_BINARY;
    binary.dest = NewTemp();
    binary.type = LowTypeOf(target);
    binary.op = binary_op == OP_RSHIFT ?
        (IsUnsigned(target) ? "lshr" : "ashr") : BinaryName(binary_op);
    binary.first = left.operand;
    binary.second = right.operand;
    Emit(binary);
    rhs.type = target;
    rhs.operand.kind = lowir_model::Operand::OP_TEMP;
    rhs.operand.text = binary.dest;
  }
  lowir_model::Instruction store;
  store.kind = lowir_model::Instruction::IK_STORE;
  store.type = LowTypeOf(target);
  store.first = rhs.operand;
  store.second = lhs.operand;
  Emit(store);
  rhs.lvalue = false;
  return rhs;
}

void Lowerer::LowerDiscard(SemaId node)
{
  if (node == 0)
    return;
  if (tree_.At(node).kind == SEMA_CALL) {
    (void)LowerCall(node, 0);
    return;
  }
  (void)LowerRValue(node, 0);
}

Lowerer::Value Lowerer::LowerLogicalValue(SemaId node)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 2)
    Unsupported("logical expression arity");
  const bool disjunction = value.op == OP_LOR;
  const std::string stem = disjunction ? "lor" : "land";
  lowir_model::LowType logical_type;
  logical_type.text = "i64";
  const std::string slot = NewGeneratedSlot(stem + "__" +
      std::to_string(++generated_slot_counter_), logical_type);
  // Generated slots use the source-independent base, while the numbering is
  // owned by the function's slot namespace.  Replace the provisional suffix
  // if it made a noncanonical name.
  (void)slot;
  const std::string actual_slot = function_.slots.back().first;
  const std::string rhs_label = NewBlockLabel(stem + "_rhs");
  const std::string short_label = NewBlockLabel(stem + "_short");
  const std::string end_label = NewBlockLabel(stem + "_end");

  PrepareConditionLabels(children[0]);
  LowerCondition(children[0], disjunction ? short_label : rhs_label,
                 disjunction ? rhs_label : short_label);
  SetCurrent(rhs_label);
  Value rhs = LowerRValue(children[1]);
  lowir_model::Operand truth = rhs.operand;
  lowir_model::Instruction compare;
  compare.kind = lowir_model::Instruction::IK_CMP;
  compare.dest = NewTemp();
  compare.op = "ne";
  if (!IsFloat(types_, rhs.type))
    compare.type.text = "i64";
  else
    compare.type = LowTypeOf(rhs.type);
  compare.first = rhs.operand;
  compare.second = ZeroOperand(rhs.type);
  Emit(compare);
  truth = NamedOperand(lowir_model::Operand::OP_TEMP, compare.dest);
  lowir_model::Instruction store;
  store.kind = lowir_model::Instruction::IK_STORE;
  // The LowIR logical temporary is an i64 value even when the source result
  // is bool.  Keep the store's type canonical rather than leaking the source
  // bool width into the materialized value.
  store.type.text = "i64";
  store.first = truth;
  store.second.kind = lowir_model::Operand::OP_SLOT;
  store.second.text = actual_slot;
  Emit(store);
  EmitJump(end_label);

  AddBlock(short_label);
  SetCurrent(short_label);
  store = lowir_model::Instruction();
  store.kind = lowir_model::Instruction::IK_STORE;
  store.type.text = "i64";
  store.first = IntegerOperand(disjunction ? 1 : 0);
  store.second.kind = lowir_model::Operand::OP_SLOT;
  store.second.text = actual_slot;
  Emit(store);
  EmitJump(end_label);

  AddBlock(end_label);
  SetCurrent(end_label);
  lowir_model::Instruction load;
  load.kind = lowir_model::Instruction::IK_LOAD;
  load.dest = NewTemp();
  load.type.text = "i64";
  load.first.kind = lowir_model::Operand::OP_SLOT;
  load.first.text = actual_slot;
  Emit(load);
  Value result;
  result.type = value.type;
  result.operand = NamedOperand(lowir_model::Operand::OP_TEMP, load.dest);
  return result;
}

Lowerer::Value Lowerer::LowerConditional(SemaId node, TypeId expected)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 3)
    Unsupported("conditional expression arity");
  const bool is_void = IsVoid(types_, value.type);
  const std::string slot = is_void ? std::string() :
      NewGeneratedSlot("cond__" + std::to_string(++generated_slot_counter_),
                       LowTypeOf(value.type));
  const std::string stem = is_void ? "discard_cond" : "cond";
  const std::string then_label = NewBlockLabel(stem + "_then");
  const std::string else_label = NewBlockLabel(stem + "_else");
  const std::string end_label = NewBlockLabel(stem + "_end");
  const SemaNode& condition = tree_.At(children[0]);
  bool materialize_logical = false;
  if (condition.kind == SEMA_BINARY &&
      IsLogical(condition.op)) {
    const std::vector<SemaId> condition_children = Children(children[0]);
    if (condition_children.size() != 2)
      Unsupported("logical condition arity");
    const SemaNode& left = tree_.At(condition_children[0]);
    const bool decisive = IsKnownIntegralLiteral(left, types_) &&
        ((condition.op == OP_LOR && left.value != 0) ||
         (condition.op == OP_LAND && left.value == 0));
    materialize_logical = !decisive;
  }
  if (materialize_logical) {
    Value condition_value = LowerLogicalValue(children[0]);
    EmitBranch(condition_value.operand, then_label, else_label);
  } else {
    PrepareConditionLabels(children[0]);
    LowerCondition(children[0], then_label, else_label);
  }

  AddBlock(then_label);
  SetCurrent(then_label);
  if (is_void) {
    LowerDiscard(children[1]);
  } else {
    Value arm = LowerRValue(children[1], value.type);
    lowir_model::Instruction store;
    store.kind = lowir_model::Instruction::IK_STORE;
    store.type = LowTypeOf(value.type);
    store.first = arm.operand;
    store.second.kind = lowir_model::Operand::OP_SLOT;
    store.second.text = slot;
    Emit(store);
  }
  EmitJump(end_label);

  AddBlock(else_label);
  SetCurrent(else_label);
  if (is_void) {
    LowerDiscard(children[2]);
  } else {
    Value arm = LowerRValue(children[2], value.type);
    lowir_model::Instruction store;
    store.kind = lowir_model::Instruction::IK_STORE;
    store.type = LowTypeOf(value.type);
    store.first = arm.operand;
    store.second.kind = lowir_model::Operand::OP_SLOT;
    store.second.text = slot;
    Emit(store);
  }
  EmitJump(end_label);

  AddBlock(end_label);
  SetCurrent(end_label);
  if (is_void) {
    Value result;
    result.type = value.type;
    return result;
  }
  lowir_model::Instruction load;
  load.kind = lowir_model::Instruction::IK_LOAD;
  load.dest = NewTemp();
  load.type = LowTypeOf(value.type);
  load.first.kind = lowir_model::Operand::OP_SLOT;
  load.first.text = slot;
  Emit(load);
  Value result;
  result.type = value.type;
  result.operand = NamedOperand(lowir_model::Operand::OP_TEMP, load.dest);
  return Convert(result, expected);
}

Lowerer::Value Lowerer::LowerCall(SemaId node, TypeId expected)
{
  const std::vector<SemaId> children = Children(node);
  if (children.empty())
    Unsupported("call without a callee");
  const SemaNode& callee = tree_.At(children[0]);
  if (callee.kind != SEMA_CALLEE || callee.function == 0)
    Unsupported("indirect calls");
  emitted_function_uses_.insert(callee.function);
  const FunctionEntity& entity = model_.FunctionAt(callee.function);
  const TypeNode& type = types_.At(types_.Unqualified(entity.type));
  if (children.size() - 1 < type.parameters.size() && !type.variadic)
    Unsupported("call with missing arguments");
  lowir_model::Instruction call;
  call.kind = lowir_model::Instruction::IK_CALL;
  call.type = LowTypeOf(type.result);
  call.call_return_type = call.type;
  call.call_returns_void = call.type.text == "void";
  call.first.kind = lowir_model::Operand::OP_GLOBAL;
  const std::map<FunctionEntityId, std::string>::const_iterator name =
      function_names_.find(callee.function);
  if (name == function_names_.end())
    Unsupported("call target is not a known function");
  call.first.text = name->second;
  for (std::size_t i = 1; i < children.size(); ++i) {
    const TypeId parameter_type = i - 1 < type.parameters.size() ?
        type.parameters[i - 1] : tree_.At(children[i]).type;
    if (types_.Kind(types_.Unqualified(parameter_type)) == TYPE_REFERENCE) {
      Value lvalue = LowerLValue(children[i]);
      lowir_model::Instruction address;
      address.kind = lowir_model::Instruction::IK_ADDR;
      address.dest = NewTemp();
      address.first = lvalue.operand;
      Emit(address);
      call.args.push_back(NamedOperand(lowir_model::Operand::OP_TEMP,
                                       address.dest));
    } else {
      call.args.push_back(LowerRValue(children[i], parameter_type).operand);
    }
  }
  if (!call.call_returns_void)
    call.dest = NewTemp();
  Emit(call);
  Value result;
  result.type = type.result;
  if (!call.call_returns_void)
    result.operand = NamedOperand(lowir_model::Operand::OP_TEMP, call.dest);
  return Convert(result, expected);
}

}  // namespace lowir_lowering
