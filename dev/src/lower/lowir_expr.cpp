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
  const lowir_model::LowTypeInfo source_info =
      lowir_model::describe_low_type(source_type);
  const lowir_model::LowTypeInfo target_info =
      lowir_model::describe_low_type(target_type);
  // An integer constant converted to a pointer is a typed value operation,
  // not a retag of the source bits.  Keeping that distinction is important
  // for enum-to-pointer reinterpretation and for the backend's pointer
  // validation.
  if (value.operand.kind == lowir_model::Operand::OP_INTEGER &&
      source_info.integer() && target_info.pointer()) {
    lowir_model::Instruction copy;
    copy.kind = lowir_model::Instruction::IK_COPY;
    copy.dest = NewTemp();
    copy.type = target_type;
    copy.first = value.operand;
    Emit(copy);
    value.operand = NamedOperand(lowir_model::Operand::OP_TEMP, copy.dest);
    value.operand.literal_type = target_type;
    value.type = target;
    value.lvalue = false;
    return value;
  }
  if (source_info.integer() && target_info.integer() &&
      source_info.bits == target_info.bits &&
      value.operand.kind != lowir_model::Operand::OP_INTEGER) {
    lowir_model::Instruction copy;
    copy.kind = lowir_model::Instruction::IK_COPY;
    copy.dest = NewTemp();
    copy.type = target_type;
    copy.first = value.operand;
    Emit(copy);
    value.operand = NamedOperand(lowir_model::Operand::OP_TEMP,
                                 copy.dest);
    value.operand.literal_type = target_type;
    value.type = target;
    value.lvalue = false;
    return value;
  }
  // Integer literal bits are already exact, so integer conversions can stay
  // immediate.  Floating conversions must remain explicit even for an
  // immediate (for example double-to-float initialization), because the
  // target format changes the value.
  if (value.operand.kind == lowir_model::Operand::OP_INTEGER &&
      source_info.integer() && target_info.integer()) {
    if (source_info.bits == target_info.bits ||
        IsUnsigned(value.type) == IsUnsigned(target)) {
      value.type = target;
      value.lvalue = false;
      return value;
    }
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
      (source_info.bits == target_info.bits ||
       IsUnsigned(value.type) == IsUnsigned(target)))
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

Lowerer::Value Lowerer::LowerLiteral(SemaId node_id, const SemaNode& node,
                                     TypeId expected)
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
      const std::string symbol = RegisterStringLiteral(node_id, node);
      lowir_model::Instruction address;
      address.kind = lowir_model::Instruction::IK_ADDR;
      address.dest = NewTemp();
      address.type.text = "ptr";
      address.first.kind = lowir_model::Operand::OP_GLOBAL;
      address.first.text = symbol;
      Emit(address);
      Value string_value;
      string_value.type = const_cast<TypeTable&>(types_).Decay(node.type);
      string_value.operand = NamedOperand(lowir_model::Operand::OP_TEMP,
                                          address.dest);
      return Convert(string_value, expected);
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
    if (token.IsSimple(KW_NULLPTR) && expected != 0 &&
        types_.IsPointer(expected)) {
      lowir_model::Instruction copy;
      copy.kind = lowir_model::Instruction::IK_COPY;
      copy.dest = NewTemp();
      copy.type = LowTypeOf(expected);
      copy.first.kind = lowir_model::Operand::OP_INTEGER;
      copy.first.text = "nullptr";
      copy.first.int_value = 0;
      Emit(copy);
      result.operand = NamedOperand(lowir_model::Operand::OP_TEMP,
                                    copy.dest);
      result.type = expected;
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

std::string Lowerer::RegisterStringLiteral(SemaId node_id,
                                            const SemaNode& value)
{
  const std::map<SemaId, std::string>::const_iterator existing =
      string_symbols_.find(node_id);
  if (existing != string_symbols_.end())
    return existing->second;
  if (!value.HasSpan() || value.first >= tokens_.size())
    Unsupported("string literal without a token");
  const Pa6Token& token = tokens_[value.first];
  if (token.lit_bytes.empty())
    Unsupported("string literal without decoded bytes");

  const std::string name = "@__strlit__" +
      std::to_string(++string_literal_counter_);
  lowir_model::GlobalDefinition global;
  global.name = name;
  global.structured = true;
  global.metadata.binding = lowir_model::SBM_INTERNAL;
  const std::size_t width = FundamentalSize(token.lit_type);
  if (width == 0 || token.lit_bytes.size() % width != 0)
    Unsupported("string literal with invalid code-unit width");
  const TypeId code_type = const_cast<TypeTable&>(types_).Fundamental(
      token.lit_type);
  for (std::size_t offset = 0; offset < token.lit_bytes.size();
       offset += width) {
    unsigned long long raw = 0;
    for (std::size_t byte = 0; byte < width; ++byte)
      raw |= static_cast<unsigned long long>(token.lit_bytes[offset + byte]) <<
          (byte * 8);
    lowir_model::GlobalDefinition::DataItem item;
    item.kind = lowir_model::GlobalDefinition::DataItem::ITEM_INTEGER;
    item.type = LowTypeOf(code_type);
    item.literal_operand.kind = lowir_model::Operand::OP_INTEGER;
    item.literal_operand.int_value = static_cast<long long>(raw);
    item.literal_operand.text = std::to_string(raw);
    item.literal_operand.literal_type = item.type;
    global.data_items.push_back(item);
  }
  program_.globals.push_back(global);
  string_symbols_[node_id] = name;
  return name;
}

TypeId Lowerer::ReferentType(TypeId type) const
{
  if (type == 0)
    return 0;
  if (types_.Kind(type) == TYPE_REFERENCE)
    return types_.Referent(type);
  return type;
}

TypeId Lowerer::PointerElementType(TypeId type) const
{
  type = ReferentType(type);
  if (type == 0 || !types_.IsPointer(type))
    return 0;
  return types_.At(types_.Unqualified(type)).base;
}

TypeId Lowerer::DefaultArgumentPromotion(TypeId type) const
{
  TypeTable& mutable_types = const_cast<TypeTable&>(types_);
  type = mutable_types.Decay(type);
  const TypeId unqualified = types_.Unqualified(type);
  if (types_.Kind(unqualified) == TYPE_FUNDAMENTAL &&
      types_.At(unqualified).fundamental == FT_FLOAT)
    return mutable_types.Fundamental(FT_DOUBLE);
  return mutable_types.Promote(type);
}

lowir_model::Operand Lowerer::ByteOffset(std::size_t bytes) const
{
  return IntegerOperand(static_cast<long long>(bytes));
}

Lowerer::Value Lowerer::LoadValue(const Value& lvalue)
{
  lowir_model::Instruction load;
  load.kind = lowir_model::Instruction::IK_LOAD;
  load.dest = NewTemp();
  load.type = LowTypeOf(lvalue.type);
  load.first = lvalue.operand;
  Emit(load);
  Value result;
  result.type = lvalue.type;
  result.operand = NamedOperand(lowir_model::Operand::OP_TEMP, load.dest);
  result.lvalue = false;
  return result;
}

Lowerer::Value Lowerer::AddressValue(const Value& lvalue)
{
  Value result;
  result.type = const_cast<TypeTable&>(types_).Pointer(ReferentType(lvalue.type));
  result.lvalue = false;
  if (lvalue.operand.kind == lowir_model::Operand::OP_TEMP) {
    // Subscripts, dereferences, conditional lvalues, and references already
    // carry the address of the object.  Taking address again would create a
    // pointer-to-temporary rather than the source object.
    result.operand = lvalue.operand;
    return result;
  }
  lowir_model::Instruction address;
  address.kind = lowir_model::Instruction::IK_ADDR;
  address.dest = NewTemp();
  address.type.text = "ptr";
  address.first = lvalue.operand;
  Emit(address);
  result.operand = NamedOperand(lowir_model::Operand::OP_TEMP,
                                address.dest);
  return result;
}

Lowerer::Value Lowerer::LowerConditionalLValue(SemaId node)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 3)
    Unsupported("conditional lvalue arity");
  const std::string slot = NewGeneratedSlot(
      "condaddr__" + std::to_string(++generated_slot_counter_),
      lowir_model::LowType());
  function_.slots.back().second.text = "ptr";
  const std::string then_label = NewBlockLabel("condaddr_then");
  const std::string else_label = NewBlockLabel("condaddr_else");
  const std::string end_label = NewBlockLabel("condaddr_end");
  PrepareConditionLabels(children[0]);
  LowerCondition(children[0], then_label, else_label);

  AddBlock(then_label);
  SetCurrent(then_label);
  Value then_value = AddressValue(LowerLValue(children[1]));
  lowir_model::Instruction store;
  store.kind = lowir_model::Instruction::IK_STORE;
  store.type.text = "ptr";
  store.first = then_value.operand;
  store.second = NamedOperand(lowir_model::Operand::OP_SLOT, slot);
  Emit(store);
  EmitJump(end_label);

  AddBlock(else_label);
  SetCurrent(else_label);
  Value else_value = AddressValue(LowerLValue(children[2]));
  store = lowir_model::Instruction();
  store.kind = lowir_model::Instruction::IK_STORE;
  store.type.text = "ptr";
  store.first = else_value.operand;
  store.second = NamedOperand(lowir_model::Operand::OP_SLOT, slot);
  Emit(store);
  EmitJump(end_label);

  AddBlock(end_label);
  SetCurrent(end_label);
  lowir_model::Instruction load;
  load.kind = lowir_model::Instruction::IK_LOAD;
  load.dest = NewTemp();
  load.type.text = "ptr";
  load.first = NamedOperand(lowir_model::Operand::OP_SLOT, slot);
  Emit(load);
  Value result;
  result.type = ReferentType(value.type);
  result.lvalue = true;
  result.operand = NamedOperand(lowir_model::Operand::OP_TEMP, load.dest);
  return result;
}

Lowerer::Value Lowerer::LowerArrayDecay(SemaId node)
{
  const SemaNode& value = tree_.At(node);
  TypeId source_type = ReferentType(value.type);
  if (types_.Kind(types_.Unqualified(source_type)) != TYPE_ARRAY &&
      types_.Kind(types_.Unqualified(source_type)) != TYPE_FUNCTION)
    return LowerRValue(node);

  if (value.kind == SEMA_LITERAL) {
    Value literal = LowerRValue(node);
    return literal;
  }
  Value lvalue = LowerLValue(node);
  const bool function_value =
      types_.Kind(types_.Unqualified(source_type)) == TYPE_FUNCTION;
  const bool addressable =
      lvalue.operand.kind == lowir_model::Operand::OP_SLOT ||
      lvalue.operand.kind == lowir_model::Operand::OP_GLOBAL;
  if (addressable) {
    lvalue = AddressValue(lvalue);
  }
  if (function_value || addressable) {
    lowir_model::Instruction decay;
    decay.kind = lowir_model::Instruction::IK_UNARY;
    decay.dest = NewTemp();
    decay.op = "decay";
    decay.type.text = "ptr";
    decay.first = lvalue.operand;
    Emit(decay);
    lvalue.operand = NamedOperand(lowir_model::Operand::OP_TEMP, decay.dest);
  }
  lvalue.type = const_cast<TypeTable&>(types_).Decay(source_type);
  lvalue.lvalue = false;
  return lvalue;
}

Lowerer::Value Lowerer::LowerSubscript(SemaId node, bool lvalue)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 2)
    Unsupported("subscript expression arity");
  const TypeId base_type = ReferentType(tree_.At(children[0]).type);
  Value base;
  if (types_.Kind(types_.Unqualified(base_type)) == TYPE_ARRAY ||
      types_.Kind(types_.Unqualified(base_type)) == TYPE_FUNCTION)
    base = LowerArrayDecay(children[0]);
  else
    base = LowerRValue(children[0]);
  Value index = LowerRValue(children[1]);
  const TypeId element = ReferentType(value.type);
  lowir_model::Instruction projection;
  projection.kind = lowir_model::Instruction::IK_INDEX;
  projection.dest = NewTemp();
  projection.type = LowTypeOf(element);
  projection.index_projection = lowir_model::IPK_ARRAY_ELEMENT;
  projection.first = base.operand;
  projection.second = index.operand;
  Emit(projection);
  Value result;
  result.type = element;
  result.lvalue = lvalue;
  result.operand = NamedOperand(lowir_model::Operand::OP_TEMP,
                                projection.dest);
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
      Value result;
      result.type = ReferentType(value.type);
      result.binding = value.binding;
      result.lvalue = true;
      const std::map<BindingId, std::string>::const_iterator global =
          global_names_.find(value.binding);
      if (global != global_names_.end() &&
          types_.Kind(types_.Unqualified(binding.type)) == TYPE_REFERENCE) {
        lowir_model::Instruction load;
        load.kind = lowir_model::Instruction::IK_LOAD;
        load.dest = NewTemp();
        load.type.text = "ptr";
        load.first = NamedOperand(lowir_model::Operand::OP_GLOBAL,
                                  global->second);
        Emit(load);
        result.operand = NamedOperand(lowir_model::Operand::OP_TEMP,
                                      load.dest);
      } else if (global != global_names_.end()) {
        result.operand = NamedOperand(lowir_model::Operand::OP_GLOBAL,
                                      global->second);
      } else if (types_.Kind(types_.Unqualified(binding.type)) == TYPE_REFERENCE) {
        lowir_model::Instruction load;
        load.kind = lowir_model::Instruction::IK_LOAD;
        load.dest = NewTemp();
        load.type.text = "ptr";
        load.first = NamedOperand(lowir_model::Operand::OP_SLOT,
                                  SlotFor(value.binding));
        Emit(load);
        result.operand = NamedOperand(lowir_model::Operand::OP_TEMP,
                                      load.dest);
      } else {
        result.operand = NamedOperand(lowir_model::Operand::OP_SLOT,
                                      SlotFor(value.binding));
      }
      return result;
    }
    if (binding.kind == BINDING_FUNCTION) {
      Value result;
      result.type = value.type;
      result.binding = value.binding;
      result.lvalue = true;
      const FunctionEntityId function = value.function != 0 ? value.function :
          binding.function;
      result.operand = NamedOperand(lowir_model::Operand::OP_GLOBAL,
                                    function_names_[function]);
      return result;
    }
  }
  if (value.kind == SEMA_BINARY && value.op == OP_COMMA) {
    const std::vector<SemaId> children = Children(node);
    if (children.size() != 2)
      Unsupported("comma expression arity");
    LowerDiscard(children[0]);
    return LowerLValue(children[1]);
  }
  if (value.kind == SEMA_UNARY && value.op == OP_STAR) {
    const std::vector<SemaId> children = Children(node);
    if (children.size() != 1)
      Unsupported("indirection expression");
    Value pointer = LowerRValue(children[0]);
    Value result;
    result.type = ReferentType(value.type);
    result.lvalue = true;
    result.operand = pointer.operand;
    return result;
  }
  if (value.kind == SEMA_SUBSCRIPT)
    return LowerSubscript(node, true);
  if (value.kind == SEMA_CALL &&
      types_.Kind(value.type) == TYPE_REFERENCE) {
    Value result = LowerCall(node, 0);
    result.type = types_.Referent(value.type);
    result.lvalue = true;
    return result;
  }
  if (value.kind == SEMA_CONDITIONAL && value.category == VC_LVALUE)
    return LowerConditionalLValue(node);
  if (value.kind == SEMA_ASSIGNMENT) {
    Value assigned;
    (void)LowerAssignment(node, &assigned);
    return assigned;
  }
  if (value.kind == SEMA_UNARY &&
      (value.op == OP_INC || value.op == OP_DEC))
    return LowerUnary(node, false, 0, true);
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
    return LowerLiteral(node, value, expected);
  case SEMA_ID_EXPRESSION: {
    const Binding& binding = model_.BindingAt(value.binding);
    if (binding.kind == BINDING_ENUMERATOR)
      return LowerLiteral(node, value, expected);
    if (binding.kind == BINDING_VARIABLE || binding.kind == BINDING_PARAMETER) {
      const TypeId source_type = ReferentType(value.type);
      const TypeKind kind = types_.Kind(types_.Unqualified(source_type));
      if (kind == TYPE_ARRAY || kind == TYPE_FUNCTION) {
        Value result = LowerArrayDecay(node);
        return Convert(result, expected);
      }
      Value result = LoadValue(LowerLValue(node));
      return Convert(result, expected);
    }
    if (binding.kind == BINDING_FUNCTION) {
      Value result = LowerArrayDecay(node);
      return Convert(result, expected);
    }
    Unsupported("unsupported named value");
    break;
  }
  case SEMA_UNARY:
    return LowerUnary(node, false, expected);
  case SEMA_POSTFIX:
    return LowerUnary(node, true, expected);
  case SEMA_BINARY:
    return LowerBinary(node, expected);
  case SEMA_ASSIGNMENT:
    return LowerAssignment(node, 0);
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
    Value zero;
    zero.type = expected == 0 ? value.type : expected;
    zero.operand = IntegerOperand(0);
    return zero;
  }
  case SEMA_CALL:
    return LowerCall(node, expected);
  case SEMA_CALLEE:
    Unsupported("callee outside a call");
    break;
  case SEMA_SUBSCRIPT: {
    Value result = LowerSubscript(node, false);
    return Convert(LoadValue(result), expected);
  }
  case SEMA_MEMBER:
    Unsupported("member expressions");
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

Lowerer::Value Lowerer::LowerUnary(SemaId node, bool postfix, TypeId expected,
                                   bool as_lvalue)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 1)
    Unsupported("unary expression arity");
  const SemaId operand_node = children[0];
  if (value.op == OP_AMP) {
    Value lvalue = LowerLValue(operand_node);
    Value result = AddressValue(lvalue);
    result.type = value.type;
    return Convert(result, expected);
  }
  if (value.op == OP_STAR) {
    Value lvalue = LowerLValue(node);
    const TypeId object_type = ReferentType(value.type);
    lvalue.type = object_type;
    const TypeKind kind = types_.Kind(types_.Unqualified(object_type));
    if (kind == TYPE_ARRAY || kind == TYPE_FUNCTION) {
      lvalue.type = const_cast<TypeTable&>(types_).Decay(object_type);
      lvalue.lvalue = false;
      return Convert(lvalue, expected);
    }
    return Convert(LoadValue(lvalue), expected);
  }
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
  if (value.op == OP_PLUS) {
    const TypeKind operand_kind = types_.Kind(
        types_.Unqualified(ReferentType(tree_.At(operand_node).type)));
    Value operand = operand_kind == TYPE_ARRAY || operand_kind == TYPE_FUNCTION ?
        LowerArrayDecay(operand_node) : LowerRValue(operand_node, value.type);
    return Convert(operand, expected);
  }
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
  if (types_.IsPointer(lvalue.type)) {
    const std::size_t element_size = types_.SizeOf(PointerElementType(lvalue.type));
    lowir_model::Operand offset = IntegerOperand(1);
    if (element_size != 1) {
      lowir_model::Instruction scale;
      scale.kind = lowir_model::Instruction::IK_BINARY;
      scale.dest = NewTemp();
      scale.op = "mul";
      scale.type.text = "i64";
      scale.first = IntegerOperand(1);
      scale.second = ByteOffset(element_size);
      Emit(scale);
      offset = NamedOperand(lowir_model::Operand::OP_TEMP, scale.dest);
    }
    if (value.op == OP_DEC) {
      lowir_model::Instruction negate;
      negate.kind = lowir_model::Instruction::IK_BINARY;
      negate.dest = NewTemp();
      negate.type.text = "i64";
      negate.op = "sub";
      negate.first = IntegerOperand(0);
      negate.second = offset;
      Emit(negate);
      offset = NamedOperand(lowir_model::Operand::OP_TEMP, negate.dest);
    }
    lowir_model::Instruction pointer_index;
    pointer_index.kind = lowir_model::Instruction::IK_INDEX;
    pointer_index.dest = NewTemp();
    pointer_index.type.text = "i8";
    pointer_index.first = old.operand;
    pointer_index.second = offset;
    Emit(pointer_index);
    Value updated_pointer;
    updated_pointer.type = lvalue.type;
    updated_pointer.operand = NamedOperand(lowir_model::Operand::OP_TEMP,
                                           pointer_index.dest);
    lowir_model::Instruction pointer_store;
    pointer_store.kind = lowir_model::Instruction::IK_STORE;
    pointer_store.type.text = "ptr";
    pointer_store.first = updated_pointer.operand;
    pointer_store.second = lvalue.operand;
    Emit(pointer_store);
    if (as_lvalue) {
      Value result = lvalue;
      result.lvalue = true;
      return result;
    }
    return Convert(postfix ? old : updated_pointer, expected);
  }
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
  if (as_lvalue) {
    Value result = lvalue;
    result.lvalue = true;
    return result;
  }
  return Convert(postfix ? old : updated, expected);
}

Lowerer::Value Lowerer::LowerBinary(SemaId node, TypeId expected)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 2)
    Unsupported("binary expression arity");
  TypeTable& mutable_types = const_cast<TypeTable&>(types_);
  std::vector<SemaId> scalar_chain;
  SemaId leaf = node;
  while (tree_.At(leaf).kind == SEMA_BINARY &&
         IsScalarBinaryOperator(tree_.At(leaf).op)) {
    const std::vector<SemaId> current = Children(leaf);
    if (current.size() != 2)
      Unsupported("binary expression arity");
    const TypeId left_type = mutable_types.Decay(tree_.At(current[0]).type);
    const TypeId right_type = mutable_types.Decay(tree_.At(current[1]).type);
    const TypeId result_type = mutable_types.Decay(tree_.At(leaf).type);
    if (types_.IsPointer(left_type) || types_.IsPointer(right_type) ||
        types_.IsPointer(result_type))
      break;
    scalar_chain.push_back(leaf);
    leaf = current[0];
  }
  if (scalar_chain.size() > 1) {
    Value result = LowerRValue(leaf);
    for (std::vector<SemaId>::const_reverse_iterator it =
             scalar_chain.rbegin(); it != scalar_chain.rend(); ++it)
      result = LowerScalarBinary(*it, result, 0);
    return Convert(result, expected);
  }
  if (value.op == OP_COMMA) {
    LowerDiscard(children[0]);
    return LowerRValue(children[1], expected);
  }
  if (value.op == OP_LAND || value.op == OP_LOR) {
    return Convert(LowerLogicalValue(node), expected);
  }
  const TypeId left_source = mutable_types.Decay(tree_.At(children[0]).type);
  const TypeId right_source = mutable_types.Decay(tree_.At(children[1]).type);
  const bool left_pointer = types_.IsPointer(left_source);
  const bool right_pointer = types_.IsPointer(right_source);
  if ((value.op == OP_PLUS || value.op == OP_MINUS) &&
      ((left_pointer && types_.IsIntegral(right_source)) ||
       (value.op == OP_PLUS && right_pointer &&
        types_.IsIntegral(left_source)))) {
    const bool pointer_on_left = left_pointer;
    const SemaId pointer_node = pointer_on_left ? children[0] : children[1];
    const SemaId index_node = pointer_on_left ? children[1] : children[0];
    Value pointer = LowerRValue(pointer_node);
    TypeTable& mutable_types = const_cast<TypeTable&>(types_);
    Value index = LowerRValue(index_node,
                              mutable_types.Fundamental(FT_LONG_INT));
    const std::size_t element_size = types_.SizeOf(PointerElementType(
        pointer.type));
    lowir_model::Instruction scale;
    scale.kind = lowir_model::Instruction::IK_BINARY;
    scale.dest = NewTemp();
    scale.op = "mul";
    scale.type.text = "i64";
    scale.first = index.operand;
    scale.second = ByteOffset(element_size);
    Emit(scale);
    lowir_model::Operand offset =
        NamedOperand(lowir_model::Operand::OP_TEMP, scale.dest);
    if (value.op == OP_MINUS && pointer_on_left) {
      lowir_model::Instruction negate;
      negate.kind = lowir_model::Instruction::IK_BINARY;
      negate.dest = NewTemp();
      negate.op = "sub";
      negate.type.text = "i64";
      negate.first = IntegerOperand(0);
      negate.second = offset;
      Emit(negate);
      offset = NamedOperand(lowir_model::Operand::OP_TEMP, negate.dest);
    }
    lowir_model::Instruction projection;
    projection.kind = lowir_model::Instruction::IK_INDEX;
    projection.dest = NewTemp();
    projection.type.text = "i8";
    projection.first = pointer.operand;
    projection.second = offset;
    Emit(projection);
    Value result;
    result.type = value.type;
    result.operand = NamedOperand(lowir_model::Operand::OP_TEMP,
                                  projection.dest);
    return Convert(result, expected);
  }
  if (value.op == OP_MINUS && left_pointer && right_pointer) {
    Value left = LowerRValue(children[0]);
    Value right = LowerRValue(children[1]);
    lowir_model::Instruction difference;
    difference.kind = lowir_model::Instruction::IK_BINARY;
    difference.dest = NewTemp();
    difference.op = "sub";
    difference.type.text = "ptr";
    difference.first = left.operand;
    difference.second = right.operand;
    Emit(difference);
    const std::size_t element_size = types_.SizeOf(PointerElementType(
        left.type));
    lowir_model::Instruction count;
    count.kind = lowir_model::Instruction::IK_BINARY;
    count.dest = NewTemp();
    count.op = "div";
    count.type.text = "i64";
    count.first = NamedOperand(lowir_model::Operand::OP_TEMP,
                               difference.dest);
    count.second = ByteOffset(element_size);
    Emit(count);
    Value result;
    result.type = value.type;
    result.operand = NamedOperand(lowir_model::Operand::OP_TEMP, count.dest);
    return Convert(result, expected);
  }
  if (value.op == OP_EQ || value.op == OP_NE || value.op == OP_LT ||
      value.op == OP_LE || value.op == OP_GT || value.op == OP_GE) {
    const auto is_zero_constant = [this](SemaId child) {
      const SemaNode& candidate = tree_.At(child);
      return candidate.kind == SEMA_LITERAL && candidate.has_value &&
          candidate.value == 0 && types_.IsIntegral(candidate.type);
    };
    TypeId common = 0;
    if (left_pointer && right_pointer) {
      bool ok = false;
      common = mutable_types.CompositePointer(left_source, right_source, ok);
      if (!ok)
        Unsupported("incompatible pointer comparison");
    } else if (left_pointer && (types_.IsNullPointerType(right_source) ||
                               is_zero_constant(children[1]))) {
      common = left_source;
    } else if (right_pointer && (types_.IsNullPointerType(left_source) ||
                                is_zero_constant(children[0]))) {
      common = right_source;
    } else if (types_.Unqualified(tree_.At(children[0]).type) ==
                   types_.Unqualified(tree_.At(children[1]).type) &&
               types_.Kind(types_.Unqualified(tree_.At(children[0]).type)) ==
                   TYPE_ENUM) {
      common = tree_.At(children[0]).type;
    } else {
      common = mutable_types.UsualArithmetic(
          tree_.At(children[0]).type, tree_.At(children[1]).type);
    }
    const auto is_null_literal = [this](SemaId child) {
      return tree_.At(child).kind == SEMA_LITERAL &&
          tree_.At(child).HasSpan() &&
          tree_.At(child).first < tokens_.size() &&
          tokens_[tree_.At(child).first].IsSimple(KW_NULLPTR);
    };
    const bool left_zero = is_zero_constant(children[0]) &&
        types_.IsPointer(common);
    const bool right_zero = is_zero_constant(children[1]) &&
        types_.IsPointer(common);
    Value left = is_null_literal(children[0]) && types_.IsPointer(common) ?
        LowerRValue(children[0], common) : LowerRValue(children[0]);
    Value right = is_null_literal(children[1]) && types_.IsPointer(common) ?
        LowerRValue(children[1], common) : LowerRValue(children[1]);
    const bool left_character = tree_.At(children[0]).kind == SEMA_LITERAL &&
        tree_.At(children[0]).HasSpan() &&
        tree_.At(children[0]).first < tokens_.size() &&
        tokens_[tree_.At(children[0]).first].lit_type == FT_CHAR;
    const bool right_character = tree_.At(children[1]).kind == SEMA_LITERAL &&
        tree_.At(children[1]).HasSpan() &&
        tree_.At(children[1]).first < tokens_.size() &&
        tokens_[tree_.At(children[1]).first].lit_type == FT_CHAR;
    if (left_character || left_zero)
      left.type = common;
    else
      left = ConvertExpression(left, common);
    if (right_character || right_zero)
      right.type = common;
    else
      right = ConvertExpression(right, common);
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
  Value left = LowerRValue(children[0]);
  return LowerScalarBinary(node, left, expected);
}

Lowerer::Value Lowerer::LowerScalarBinary(SemaId node, Value left,
                                          TypeId expected)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 2)
    Unsupported("binary expression arity");
  TypeId common = value.type;
  if (value.op == OP_LSHIFT || value.op == OP_RSHIFT)
    common = tree_.At(children[0]).type;
  Value right = LowerRValue(children[1]);
  const bool left_character = tree_.At(children[0]).kind == SEMA_LITERAL &&
      tree_.At(children[0]).HasSpan() &&
      tree_.At(children[0]).first < tokens_.size() &&
      tokens_[tree_.At(children[0]).first].lit_type == FT_CHAR;
  const bool right_character = tree_.At(children[1]).kind == SEMA_LITERAL &&
      tree_.At(children[1]).HasSpan() &&
      tree_.At(children[1]).first < tokens_.size() &&
      tokens_[tree_.At(children[1]).first].lit_type == FT_CHAR;
  if (left_character)
    left.type = common;
  else
    left = ConvertExpression(left, common);
  if (right_character)
    right.type = common;
  else
    right = ConvertExpression(right, common);
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

Lowerer::Value Lowerer::LowerAssignment(SemaId node, Value* assigned_lvalue)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 2)
    Unsupported("assignment expression arity");
  Value lhs;
  TypeId target = 0;
  Value rhs;
  if (value.op != OP_ASS) {
    lhs = LowerLValue(children[0]);
    target = lhs.type;
  }
  if (value.op == OP_ASS) {
    target = ReferentType(tree_.At(children[0]).type);
    const bool needs_initializer_context =
        tree_.At(children[1]).kind == SEMA_BRACED_INIT_LIST;
    rhs = needs_initializer_context ?
        LowerRValue(children[1], target) : LowerRValue(children[1]);
    lhs = LowerLValue(children[0]);
    target = lhs.type;
    // The expression's declared type may carry reference/CV structure that
    // is not the storage type.  Reconcile after the authoritative lvalue is
    // resolved, while retaining the required right-before-left evaluation
    // order above.
    rhs = Convert(rhs, target);
  } else if ((value.op == OP_PLUSASS || value.op == OP_MINUSASS) &&
             types_.IsPointer(target)) {
    lowir_model::Instruction load;
    load.kind = lowir_model::Instruction::IK_LOAD;
    load.dest = NewTemp();
    load.type.text = "ptr";
    load.first = lhs.operand;
    Emit(load);
    TypeTable& mutable_types = const_cast<TypeTable&>(types_);
    Value index = LowerRValue(children[1],
                              mutable_types.Fundamental(FT_LONG_INT));
    const std::size_t element_size = types_.SizeOf(PointerElementType(target));
    lowir_model::Instruction scale;
    scale.kind = lowir_model::Instruction::IK_BINARY;
    scale.dest = NewTemp();
    scale.op = "mul";
    scale.type.text = "i64";
    scale.first = index.operand;
    scale.second = ByteOffset(element_size);
    Emit(scale);
    lowir_model::Operand offset =
        NamedOperand(lowir_model::Operand::OP_TEMP, scale.dest);
    if (value.op == OP_MINUSASS) {
      lowir_model::Instruction negate;
      negate.kind = lowir_model::Instruction::IK_BINARY;
      negate.dest = NewTemp();
      negate.op = "sub";
      negate.type.text = "i64";
      negate.first = IntegerOperand(0);
      negate.second = offset;
      Emit(negate);
      offset = NamedOperand(lowir_model::Operand::OP_TEMP, negate.dest);
    }
    lowir_model::Instruction projection;
    projection.kind = lowir_model::Instruction::IK_INDEX;
    projection.dest = NewTemp();
    projection.type.text = "i8";
    projection.first = NamedOperand(lowir_model::Operand::OP_TEMP, load.dest);
    projection.second = offset;
    Emit(projection);
    rhs.type = target;
    rhs.operand = NamedOperand(lowir_model::Operand::OP_TEMP,
                               projection.dest);
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
  if (assigned_lvalue != 0) {
    *assigned_lvalue = lhs;
    assigned_lvalue->lvalue = true;
  }
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

  if (tree_.At(children[0]).kind == SEMA_BINARY &&
      IsLogical(tree_.At(children[0]).op)) {
    Value left = LowerLogicalValue(children[0]);
    EmitBranch(left.operand, disjunction ? short_label : rhs_label,
               disjunction ? rhs_label : short_label);
  } else {
    PrepareConditionLabels(children[0]);
    LowerCondition(children[0], disjunction ? short_label : rhs_label,
                   disjunction ? rhs_label : short_label);
  }
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
  bool addressable_arms = types_.IsPointer(value.type);
  if (addressable_arms) {
    for (std::size_t i = 1; i < children.size(); ++i) {
      const SemaNode& arm = tree_.At(children[i]);
      const TypeId arm_type = ReferentType(arm.type);
      const TypeKind kind = types_.Kind(types_.Unqualified(arm_type));
      const bool direct_array_or_function = kind == TYPE_ARRAY ||
          kind == TYPE_FUNCTION;
      const bool selected_lvalue_conditional =
          arm.kind == SEMA_CONDITIONAL &&
          (arm.category == VC_LVALUE || arm.category == VC_XVALUE) &&
          types_.IsPointer(arm.type);
      if (!direct_array_or_function && !selected_lvalue_conditional) {
        addressable_arms = false;
        break;
      }
    }
  }
  if (addressable_arms) {
    Value lvalue = LowerConditionalLValue(node);
    if (expected == 0 || types_.IsPointer(expected)) {
      lvalue.type = expected;
      if (expected == 0)
        lvalue.type = value.type;
      lvalue.lvalue = false;
      return Convert(lvalue, expected);
    }
    return Convert(LoadValue(lvalue), expected);
  }
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

Lowerer::Value Lowerer::LowerReferenceArgument(SemaId node, TypeId parameter)
{
  const TypeId referent = types_.Referent(parameter);
  if (referent == 0)
    Unsupported("reference parameter without a referent");
  const SemaNode& source = tree_.At(node);
  Value address;
  if (source.category == VC_LVALUE || source.category == VC_XVALUE ||
      source.kind == SEMA_CALL || source.kind == SEMA_SUBSCRIPT ||
      source.kind == SEMA_CONDITIONAL ||
      (source.kind == SEMA_UNARY && source.op == OP_STAR)) {
    address = AddressValue(LowerLValue(node));
  } else if (source.kind == SEMA_BINARY && source.op == OP_COMMA) {
    // A comma expression whose result is a reference already carries the
    // selected object's address through its right operand.
    Value result = LowerRValue(node);
    result.type = referent;
    return result;
  } else {
    Value materialized = LowerRValue(node, referent);
    const std::string slot = NewGeneratedSlot(
        "refarg__" + std::to_string(++generated_slot_counter_),
        LowTypeOf(referent));
    lowir_model::Instruction store;
    store.kind = lowir_model::Instruction::IK_STORE;
    store.type = LowTypeOf(referent);
    store.first = materialized.operand;
    store.second = NamedOperand(lowir_model::Operand::OP_SLOT, slot);
    Emit(store);
    Value storage;
    storage.type = referent;
    storage.operand = NamedOperand(lowir_model::Operand::OP_SLOT, slot);
    address = AddressValue(storage);
  }
  address.type = referent;
  address.lvalue = false;
  return address;
}

Lowerer::Value Lowerer::LowerCall(SemaId node, TypeId expected)
{
  const std::vector<SemaId> children = Children(node);
  if (children.empty())
    Unsupported("call without a callee");
  const SemaNode& callee = tree_.At(children[0]);
  const bool direct = callee.kind == SEMA_CALLEE && callee.function != 0;
  TypeId function_type = 0;
  FunctionEntityId direct_function = 0;
  Value indirect;
  if (direct) {
    direct_function = callee.function;
    emitted_function_uses_.insert(direct_function);
    function_type = model_.FunctionAt(direct_function).type;
  } else {
    TypeTable& mutable_types = const_cast<TypeTable&>(types_);
    if (!CallableFunctionType(mutable_types, callee.type, function_type))
      Unsupported("indirect callee is not a function");
  }
  const TypeNode& type = types_.At(types_.Unqualified(function_type));
  if (children.size() - 1 < type.parameters.size() && !type.variadic)
    Unsupported("call with missing arguments");
  if (!type.variadic && children.size() - 1 > type.parameters.size())
    Unsupported("call with too many arguments");
  lowir_model::Instruction call;
  call.kind = lowir_model::Instruction::IK_CALL;
  call.type = LowTypeOf(type.result);
  call.call_return_type = call.type;
  call.call_returns_void = call.type.text == "void";
  if (direct) {
    call.first.kind = lowir_model::Operand::OP_GLOBAL;
    const std::map<FunctionEntityId, std::string>::const_iterator name =
        function_names_.find(direct_function);
    if (name == function_names_.end())
      Unsupported("call target is not a known function");
    call.first.text = name->second;
  } else {
    call.first = indirect.operand;
    call.has_call_signature = true;
    call.call_boundary.arity = type.variadic ? lowir_model::CAM_VARIADIC :
        lowir_model::CAM_FIXED;
    for (std::size_t i = 0; i < type.parameters.size(); ++i) {
      lowir_model::Parameter parameter;
      parameter.name = "%arg" + std::to_string(i);
      parameter.type = LowTypeOf(type.parameters[i]);
      if (types_.Kind(types_.Unqualified(type.parameters[i])) == TYPE_REFERENCE)
        parameter.metadata.passing = lowir_model::PPM_REFERENCE;
      call.call_params.push_back(parameter);
    }
  }
  for (std::size_t i = 1; i < children.size(); ++i) {
    const TypeId parameter_type = i - 1 < type.parameters.size() ?
        type.parameters[i - 1] :
        DefaultArgumentPromotion(tree_.At(children[i]).type);
    if (types_.Kind(types_.Unqualified(parameter_type)) == TYPE_REFERENCE) {
      call.args.push_back(LowerReferenceArgument(children[i], parameter_type).
                          operand);
    } else {
      Value argument = LowerRValue(children[i], parameter_type);
      // Default argument promotions apply only to arguments beyond a
      // prototype's fixed parameter list; LowerRValue performs the explicit
      // f32-to-f64 conversion while retaining integer immediates.
      call.args.push_back(argument.operand);
    }
  }
  if (!direct) {
    indirect = LowerRValue(children[0]);
    call.first = indirect.operand;
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
