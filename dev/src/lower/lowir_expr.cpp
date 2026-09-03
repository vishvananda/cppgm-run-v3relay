// Expression lowering: values, lvalues, conversions, operators, and calls.
#include "lower/lowir_lowering.h"

#include <cstdlib>
#include <stdexcept>

#include <algorithm>

#include "sema/overload.h"

namespace lowir_lowering {

namespace {

bool FindBasePath(const SemaModel& model, ClassEntityId from,
                  ClassEntityId target, std::vector<ClassBase>& path,
                  std::vector<ClassEntityId>& visited)
{
  if (from == target)
    return true;
  if (from == 0 ||
      std::find(visited.begin(), visited.end(), from) != visited.end())
    return false;
  visited.push_back(from);
  const ClassEntity& value = model.ClassAt(from);
  for (std::size_t i = 0; i < value.bases.size(); ++i)
  {
    path.push_back(value.bases[i]);
    if (FindBasePath(model, value.bases[i].entity, target, path, visited))
      return true;
    path.pop_back();
  }
  return false;
}

bool ClassEntityForType(const TypeTable& types, TypeId type,
                        ClassEntityId& entity)
{
  if (type == 0)
    return false;
  if (types.Kind(type) == TYPE_REFERENCE)
    type = types.Referent(type);
  type = types.Unqualified(type);
  if (types.Kind(type) != TYPE_CLASS)
    return false;
  entity = static_cast<ClassEntityId>(types.At(type).entity);
  return entity != 0;
}

bool FitsSmallIntegerLiteral(long long value, const LowInfo& destination)
{
  if (!destination.Integer() || destination.bits == 0 ||
      destination.bits >= 32)
    return false;
  if (destination.is_unsigned)
    return value >= 0 && static_cast<unsigned long long>(value) <=
        ((1ULL << destination.bits) - 1ULL);
  const long long minimum = -(1LL << (destination.bits - 1));
  const long long maximum = (1LL << (destination.bits - 1)) - 1LL;
  return value >= minimum && value <= maximum;
}

} // namespace

bool Lowerer::FindBitField(SemaId node, ClassField& field) const
{
  if (node == 0 || tree_.At(node).kind != SEMA_MEMBER)
    return false;
  const ClassField* record = model_.FieldFor(tree_.At(node).binding);
  if (record == 0 || record->bit_width == 0)
    return false;
  field = *record;
  return true;
}

TypeId Lowerer::BitFieldValueType(const ClassField& field) const
{
  TypeId type = types_.Unqualified(field.type);
  if (types_.Kind(type) == TYPE_ENUM)
    type = types_.Unqualified(types_.At(type).base);
  if (types_.Kind(type) != TYPE_FUNDAMENTAL)
    return type;
  const EFundamentalType fundamental = types_.At(type).fundamental;
  // Integral promotion of an int bit-field is based on the value width, not
  // the storage type's ordinary expression promotion.  This is why an
  // unsigned int : 1 is read as i32 while its allocation unit is u32.
  if (fundamental == FT_INT || fundamental == FT_UNSIGNED_INT ||
      fundamental == FT_BOOL || fundamental == FT_CHAR ||
      fundamental == FT_SIGNED_CHAR || fundamental == FT_UNSIGNED_CHAR ||
      fundamental == FT_SHORT_INT || fundamental == FT_UNSIGNED_SHORT_INT ||
      fundamental == FT_WCHAR_T || fundamental == FT_CHAR16_T ||
      fundamental == FT_CHAR32_T)
  {
    const unsigned int_bits = 8 * FundamentalSize(FT_INT);
    const bool fits_int =
        FundamentalIsUnsigned(fundamental) ? field.bit_width < int_bits :
        field.bit_width <= int_bits;
    if (fits_int)
      return types_.Fundamental(FT_INT);
  }
  return type;
}

long long Lowerer::BitFieldMask(const ClassField& field) const
{
  if (field.bit_width == 0)
    return 0;
  const unsigned long long mask = field.bit_width >= 64 ? ~0ULL :
      ((1ULL << field.bit_width) - 1ULL);
  return static_cast<long long>(mask);
}

lowir_model::Operand Lowerer::EncodeBitField(
    const ClassField& field, TypeId value_type,
    const lowir_model::Operand& value, TypeId storage_type, bool value_first)
{
  const TypeId storage = storage_type == 0 ? field.type : storage_type;
  Value converted;
  converted.type = value_type;
  converted.operand = value;
  converted = Convert(converted, storage);
  const lowir_model::LowType low_type = LowTypeOf(storage);
  lowir_model::Instruction mask;
  mask.kind = lowir_model::Instruction::IK_BINARY;
  mask.dest = NewTemp();
  mask.op = "and";
  mask.type = low_type;
  if (value_first) {
    mask.first = converted.operand;
    mask.second = Immediate(BitFieldMask(field));
  } else {
    mask.first = Immediate(BitFieldMask(field));
    mask.second = converted.operand;
  }
  Emit(mask);
  lowir_model::Operand encoded = TempOperand(mask.dest);
  if (field.bit_offset != 0) {
    lowir_model::Instruction shift;
    shift.kind = lowir_model::Instruction::IK_BINARY;
    shift.dest = NewTemp();
    shift.op = "shl";
    shift.type = low_type;
    shift.first = encoded;
    shift.second = Immediate(static_cast<long long>(field.bit_offset));
    Emit(shift);
    encoded = TempOperand(shift.dest);
  }
  return encoded;
}

lowir_model::Operand Lowerer::MergeBitField(
    const ClassField& field, const lowir_model::Operand& destination,
    TypeId value_type, const lowir_model::Operand& value, bool preserve,
    TypeId storage_type, bool encode_first)
{
  const TypeId storage = storage_type == 0 ? field.type : storage_type;
  const lowir_model::LowType low_type = LowTypeOf(storage);
  lowir_model::Operand encoded;
  if (encode_first)
    encoded = EncodeBitField(field, value_type, value, storage, true);
  lowir_model::Operand stored;
  if (preserve) {
    lowir_model::Instruction load;
    load.kind = lowir_model::Instruction::IK_LOAD;
    load.dest = NewTemp();
    load.type = low_type;
    load.first = destination;
    Emit(load);

    unsigned long long shifted = field.bit_width >= 64 ? ~0ULL :
        ((1ULL << field.bit_width) - 1ULL);
    if (field.bit_offset < 64)
      shifted <<= field.bit_offset;
    const unsigned unit_bits = TypeBits(field.type);
    const unsigned long long unit_mask = unit_bits >= 64 ? ~0ULL :
        ((1ULL << unit_bits) - 1ULL);
    const unsigned long long clear_bits = (~shifted) & unit_mask;
    lowir_model::Operand clear_operand = Immediate(
        static_cast<long long>(clear_bits));
    // Keep the allocation-unit mask's unsigned spelling.  LowIR's integer
    // value is a bit pattern, while its textual immediate is used by the
    // checked fixtures and should not turn 0xfffffff9 into -7.
    clear_operand.text = std::to_string(clear_bits);
    lowir_model::Instruction clear;
    clear.kind = lowir_model::Instruction::IK_BINARY;
    clear.dest = NewTemp();
    clear.op = "and";
    clear.type = low_type;
    clear.first = TempOperand(load.dest);
    clear.second = clear_operand;
    Emit(clear);

    if (!encode_first)
      encoded = EncodeBitField(field, value_type, value, storage);
    lowir_model::Instruction combine;
    combine.kind = lowir_model::Instruction::IK_BINARY;
    combine.dest = NewTemp();
    combine.op = "or";
    combine.type = low_type;
    combine.first = TempOperand(clear.dest);
    combine.second = encoded;
    Emit(combine);
    stored = TempOperand(combine.dest);
  } else {
    if (!encode_first)
      encoded = EncodeBitField(field, value_type, value, storage);
    stored = encoded;
  }
  return stored;
}

void Lowerer::StoreBitField(const ClassField& field,
                            const lowir_model::Operand& destination,
                            TypeId value_type,
                            const lowir_model::Operand& value,
                            bool preserve,
                            TypeId storage_type,
                            bool encode_first)
{
  const TypeId storage = storage_type == 0 ? field.type : storage_type;
  EmitStore(LowTypeOf(storage),
            MergeBitField(field, destination, value_type, value, preserve,
                          storage, encode_first),
            destination);
}

Lowerer::Value Lowerer::LoadBitField(SemaId node, const ClassField& field)
{
  return ReadBitField(LowerLValue(node), field);
}

Lowerer::Value Lowerer::ReadBitField(const Value& field_lvalue,
                                     const ClassField& field)
{
  const TypeId promoted = BitFieldValueType(field);
  Value storage = field_lvalue;
  storage.type = promoted;
  Value result = LoadValue(storage);
  const lowir_model::LowType low_type = LowTypeOf(promoted);
  lowir_model::Operand value = result.operand;
  if (field.bit_offset != 0) {
    lowir_model::Instruction shift;
    shift.kind = lowir_model::Instruction::IK_BINARY;
    shift.dest = NewTemp();
    shift.op = IsUnsigned(promoted) ? "ushr" : "shr";
    shift.type = low_type;
    shift.first = value;
    shift.second = Immediate(static_cast<long long>(field.bit_offset));
    Emit(shift);
    value = TempOperand(shift.dest);
  }
  lowir_model::Instruction mask;
  mask.kind = lowir_model::Instruction::IK_BINARY;
  mask.dest = NewTemp();
  mask.op = "and";
  mask.type = low_type;
  mask.first = value;
  mask.second = Immediate(BitFieldMask(field));
  Emit(mask);
  // Keep the declared type at the expression boundary.  The emitted load,
  // shift, and mask use the promoted LowIR width; consumers that need the
  // promoted arithmetic type retag the value explicitly.
  result.type = field.type;
  result.operand = TempOperand(mask.dest);
  result.lvalue = false;
  return result;
}

bool Lowerer::BitFieldUnitInitialized(const ClassField& field) const
{
  const ScopeId scope = field.binding == 0 ? 0 :
      model_.BindingAt(field.binding).scope;
  return initialized_bitfield_units_.count(
      std::make_pair(scope, field.offset)) != 0;
}

void Lowerer::MarkBitFieldUnitInitialized(const ClassField& field)
{
  const ScopeId scope = field.binding == 0 ? 0 :
      model_.BindingAt(field.binding).scope;
  initialized_bitfield_units_.insert(std::make_pair(scope, field.offset));
}

lowir_model::Operand Lowerer::ZeroOperand(TypeId type) const
{
  if (IsFloatingType(types_, type))
    return FloatImmediate("0.0", 0.0L, LowTypeOf(type));
  return Immediate(0);
}

lowir_model::Operand Lowerer::OneOperand(TypeId type) const
{
  if (IsFloatingType(types_, type))
    return FloatImmediate("1.0", 1.0L, LowTypeOf(type));
  return Immediate(1);
}

Lowerer::Value Lowerer::ZeroValue(TypeId type)
{
  Value result;
  result.type = type;
  if (!LowInfoOf(type).Pointer()) {
    result.operand = ZeroOperand(type);
    return result;
  }
  lowir_model::Instruction copy;
  copy.kind = lowir_model::Instruction::IK_COPY;
  copy.dest = NewTemp();
  copy.type = PtrType();
  copy.first = NullptrImmediate();
  Emit(copy);
  result.operand = TempOperand(copy.dest);
  return result;
}

bool Lowerer::IsCharacterLiteral(SemaId node) const
{
  const SemaNode& value = tree_.At(node);
  return value.kind == SEMA_LITERAL && value.HasSpan() &&
      value.first < tokens_.size() &&
      tokens_[value.first].lit_type == FT_CHAR;
}

bool Lowerer::IsNullptrLiteral(SemaId node) const
{
  const SemaNode& value = tree_.At(node);
  return value.kind == SEMA_LITERAL && value.HasSpan() &&
      value.first < tokens_.size() &&
      tokens_[value.first].IsSimple(KW_NULLPTR);
}

bool Lowerer::IsZeroLiteral(SemaId node) const
{
  const SemaNode& value = tree_.At(node);
  return value.kind == SEMA_LITERAL && value.has_value && value.value == 0 &&
      types_.IsIntegral(value.type);
}

// LowIR spells the signedness-sensitive operations separately (lowir.md:
// `div`/`udiv`, `mod`/`umod`, `shr`/`ushr`); the C++ operand type decides.
std::string Lowerer::BinaryName(ETokenType op, TypeId type) const
{
  const bool is_unsigned = IsUnsigned(type);
  switch (op) {
  case OP_PLUS: return "add";
  case OP_MINUS: return "sub";
  case OP_STAR: return "mul";
  case OP_DIV: return is_unsigned ? "udiv" : "div";
  case OP_MOD: return is_unsigned ? "umod" : "mod";
  case OP_AMP: return "and";
  case OP_BOR: return "or";
  case OP_XOR: return "xor";
  case OP_LSHIFT: return "shl";
  case OP_RSHIFT: return is_unsigned ? "ushr" : "shr";
  default: break;
  }
  Unsupported("this binary operator");
  return std::string();
}

std::string Lowerer::CompareName(ETokenType op, TypeId type) const
{
  const bool is_unsigned = !IsFloatingType(types_, type) && IsUnsigned(type);
  switch (op) {
  case OP_EQ: return "eq";
  case OP_NE: return "ne";
  case OP_LT: return is_unsigned ? "ult" : "lt";
  case OP_LE: return is_unsigned ? "ule" : "le";
  case OP_GT: return is_unsigned ? "ugt" : "gt";
  case OP_GE: return is_unsigned ? "uge" : "ge";
  default: break;
  }
  Unsupported("this comparison operator");
  return std::string();
}

std::string Lowerer::ConversionName(TypeId from, TypeId to) const
{
  const LowInfo source = LowInfoOf(from);
  const LowInfo target = LowInfoOf(to);
  if (source.Integer() && target.Integer()) {
    if (target.bits > source.bits)
      return source.is_unsigned ? "zext" : "sext";
    return "trunc";
  }
  if (source.Integer() && target.Floating())
    return source.is_unsigned ? "uitofp" : "sitofp";
  if (source.Floating() && target.Integer())
    return target.is_unsigned ? "fptoui" : "fptosi";
  if (source.Floating() && target.Floating())
    return target.bits > source.bits ? "fpext" : "fptrunc";
  Unsupported("this scalar conversion");
  return std::string();
}

// The one conversion rule set (4.5-4.12, 5p9): shared storage converts
// nothing; conversion to bool tests the value; an integer immediate keeps
// its immediate form when the target has the same width or the same
// signedness, otherwise the widening is explicit (the fixtures pin
// `convert sext i64 i32 <n>` for unsigned 64-bit targets); same-width
// values of the other signedness are re-typed with `copy`; everything else
// is an explicit `convert`.
Lowerer::Value Lowerer::Convert(Value value, TypeId target)
{
  if (target == 0 || value.type == 0)
    return value;
  const TypeId source_unqualified = types_.Unqualified(value.type);
  const TypeId target_unqualified = types_.Unqualified(target);
  if (types_.Kind(source_unqualified) == TYPE_POINTER &&
      types_.Kind(target_unqualified) == TYPE_POINTER)
  {
    const TypeId source_pointee = types_.At(source_unqualified).base;
    const TypeId target_pointee = types_.At(target_unqualified).base;
    ClassEntityId source_class = 0;
    ClassEntityId target_class = 0;
    if (ClassEntityForType(types_, source_pointee, source_class) &&
        ClassEntityForType(types_, target_pointee, target_class) &&
        model_.IsDerivedFrom(source_class, target_class))
    {
      std::vector<ClassBase> path;
      std::vector<ClassEntityId> visited;
      if (!FindBasePath(model_, source_class, target_class, path, visited))
        Unsupported("a derived pointer without a base path");
      std::size_t offset = 0;
      for (std::size_t i = 0; i < path.size(); ++i)
        offset += path[i].offset;
      if (!path.empty())
        value.operand = ProjectField(value.operand, offset,
                                     lowir_model::IPK_BASE_SUBOBJECT);
      value.type = target;
      value.lvalue = false;
      return value;
    }
  }
  const LowInfo source = LowInfoOf(value.type);
  const LowInfo destination = LowInfoOf(target);
  const bool immediate = value.operand.kind == lowir_model::Operand::OP_INTEGER;
  if (IsBoolType(types_, target) && !IsBoolType(types_, value.type)) {
    if (immediate) {
      value.operand = Immediate(value.operand.int_value != 0 ? 1 : 0);
    } else {
      lowir_model::Instruction compare;
      compare.kind = lowir_model::Instruction::IK_CMP;
      compare.dest = NewTemp();
      compare.op = "ne";
      compare.type = LowTypeOf(value.type);
      compare.first = value.operand;
      compare.second = ZeroOperand(value.type);
      Emit(compare);
      value.operand = TempOperand(compare.dest);
    }
    value.type = target;
    value.lvalue = false;
    return value;
  }
  if (SameStorage(source, destination)) {
    value.type = target;
    return value;
  }
  if (source.Integer() && destination.Integer()) {
    if (immediate && (source.bits == destination.bits ||
                      source.is_unsigned == destination.is_unsigned)) {
      value.type = target;
      value.lvalue = false;
      return value;
    }
    if (!immediate && source.bits == destination.bits) {
      lowir_model::Instruction copy;
      copy.kind = lowir_model::Instruction::IK_COPY;
      copy.dest = NewTemp();
      copy.type = LowTypeOf(target);
      copy.first = value.operand;
      Emit(copy);
      value.operand = TempOperand(copy.dest);
      value.type = target;
      value.lvalue = false;
      return value;
    }
  }
  if (immediate && source.Integer() && destination.Pointer()) {
    // An integer constant converted to a pointer is a typed value
    // operation, not a retag of the source bits.
    lowir_model::Instruction copy;
    copy.kind = lowir_model::Instruction::IK_COPY;
    copy.dest = NewTemp();
    copy.type = PtrType();
    copy.first = value.operand;
    Emit(copy);
    value.operand = TempOperand(copy.dest);
    value.type = target;
    value.lvalue = false;
    return value;
  }
  lowir_model::Instruction instruction;
  instruction.kind = lowir_model::Instruction::IK_CONVERT;
  instruction.dest = NewTemp();
  instruction.type = LowTypeOf(target);
  instruction.source_type = LowTypeOf(value.type);
  instruction.op = ConversionName(value.type, target);
  instruction.first = value.operand;
  Emit(instruction);
  value.operand = TempOperand(instruction.dest);
  value.type = target;
  value.lvalue = false;
  return value;
}

Lowerer::Value Lowerer::LowerLiteral(SemaId node_id, const SemaNode& node,
                                     TypeId expected)
{
  Value result;
  result.type = node.type;
  if (node.binding != 0 &&
      model_.BindingAt(node.binding).kind == BINDING_ENUMERATOR) {
    result.operand = Immediate(node.value);
  } else if (node.HasSpan() && node.first < tokens_.size()) {
    const Pa6Token& token = tokens_[node.first];
    if (token.kind == PA6_LITERAL_TOKEN && !token.lit_scalar) {
      lowir_model::Instruction address;
      address.kind = lowir_model::Instruction::IK_ADDR;
      address.dest = NewTemp();
      address.type = PtrType();
      address.first = GlobalOperand(RegisterStringLiteral(node_id, node));
      Emit(address);
      Value string_value;
      string_value.type = types_.Decay(node.type);
      string_value.operand = TempOperand(address.dest);
      return Convert(string_value, expected);
    }
    if (token.lit_type == FT_FLOAT || token.lit_type == FT_DOUBLE ||
        token.lit_type == FT_LONG_DOUBLE) {
      // Floating operands keep the source spelling.
      char* end = 0;
      const long double parsed = std::strtold(token.spelling.c_str(), &end);
      if (end == token.spelling.c_str())
        Unsupported("this floating literal");
      result.operand = FloatImmediate(token.spelling, parsed,
                                      LowTypeOf(node.type));
    } else if (token.IsSimple(KW_TRUE) || token.IsSimple(KW_FALSE)) {
      result.operand = Immediate(token.IsSimple(KW_TRUE) ? 1 : 0);
    } else {
      // The token holds the decoded bits; the node's type gives the width
      // used to read them back as a signed value.
      const unsigned long long raw = token.lit_value;
      const unsigned bits = TypeBits(node.type);
      long long value = static_cast<long long>(raw);
      if (!IsUnsigned(node.type) && bits != 0 && bits < 64 &&
          (raw & (1ULL << (bits - 1))) != 0)
        value = static_cast<long long>(raw | (~0ULL << bits));
      result.operand = Immediate(value);
    }
    if (token.IsSimple(KW_NULLPTR) && expected != 0 &&
        types_.IsPointer(expected)) {
      lowir_model::Instruction copy;
      copy.kind = lowir_model::Instruction::IK_COPY;
      copy.dest = NewTemp();
      copy.type = LowTypeOf(expected);
      copy.first = NullptrImmediate();
      Emit(copy);
      result.operand = TempOperand(copy.dest);
      result.type = expected;
    }
  } else if (node.has_value) {
    result.operand = Immediate(node.value);
  } else {
    Unsupported("a synthesized literal");
  }
  if (expected != 0 && result.operand.kind == lowir_model::Operand::OP_INTEGER &&
      FitsSmallIntegerLiteral(result.operand.int_value, LowInfoOf(expected))) {
    // A representable literal already has its final bits.  Retagging it at
    // the literal boundary avoids manufacturing a conversion temporary for
    // narrow scalar storage (the same value from a computed expression must
    // still use the ordinary conversion path).
    result.type = expected;
    result.lvalue = false;
    return result;
  }
  return Convert(result, expected);
}

// Each string literal occurrence becomes one internal structured global of
// its decoded code units, numbered in first-use order.
std::string Lowerer::RegisterStringLiteral(SemaId node_id,
                                            const SemaNode& value)
{
  const std::map<SemaId, std::string>::const_iterator existing =
      string_symbols_.find(node_id);
  if (existing != string_symbols_.end())
    return existing->second;
  if (!value.HasSpan() || value.first >= tokens_.size())
    Unsupported("a string literal without a token");
  const Pa6Token& token = tokens_[value.first];
  if (token.lit_bytes.empty())
    Unsupported("a string literal without decoded bytes");
  const std::size_t width = FundamentalSize(token.lit_type);
  if (width == 0 || token.lit_bytes.size() % width != 0)
    Unsupported("a string literal with an invalid code-unit width");
  const std::pair<EFundamentalType, std::string> content_key(
      token.lit_type,
      std::string(token.lit_bytes.begin(), token.lit_bytes.end()));
  const std::map<std::pair<EFundamentalType, std::string>,
                 std::string>::const_iterator pooled =
      string_content_symbols_.find(content_key);
  if (pooled != string_content_symbols_.end()) {
    string_symbols_[node_id] = pooled->second;
    return pooled->second;
  }

  lowir_model::GlobalDefinition global;
  global.name = "@__strlit__" + std::to_string(++shared_.string_literal_counter_);
  global.structured = true;
  global.metadata.binding = lowir_model::SBM_INTERNAL;
  const lowir_model::LowType code_type =
      LowTypeOf(types_.Fundamental(token.lit_type));
  for (std::size_t offset = 0; offset < token.lit_bytes.size();
       offset += width) {
    unsigned long long raw = 0;
    for (std::size_t byte = 0; byte < width; ++byte)
      raw |= static_cast<unsigned long long>(token.lit_bytes[offset + byte])
          << (byte * 8);
    lowir_model::GlobalDefinition::DataItem item;
    item.kind = lowir_model::GlobalDefinition::DataItem::ITEM_INTEGER;
    item.type = code_type;
    item.literal_operand = Immediate(static_cast<long long>(raw));
    item.literal_operand.literal_type = code_type;
    global.data_items.push_back(item);
  }
  program_.globals.push_back(global);
  string_symbols_[node_id] = global.name;
  string_content_symbols_[content_key] = global.name;
  return global.name;
}

TypeId Lowerer::ReferentType(TypeId type) const
{
  if (type != 0 && types_.Kind(type) == TYPE_REFERENCE)
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

// 5.2.2p7: arguments beyond the prototype undergo the default argument
// promotions, with `float` promoted to `double`.
TypeId Lowerer::DefaultArgumentPromotion(TypeId type)
{
  type = types_.Decay(type);
  const TypeId unqualified = types_.Unqualified(type);
  if (types_.Kind(unqualified) == TYPE_FUNDAMENTAL &&
      types_.At(unqualified).fundamental == FT_FLOAT)
    return types_.Fundamental(FT_DOUBLE);
  return types_.Promote(type);
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
  result.operand = TempOperand(load.dest);
  return result;
}

Lowerer::Value Lowerer::AddressValue(const Value& lvalue)
{
  Value result;
  result.type = types_.Pointer(ReferentType(lvalue.type));
  // Subscripts, dereferences, conditional lvalues, and references already
  // carry the object's address in a temporary.
  if (lvalue.operand.kind == lowir_model::Operand::OP_TEMP) {
    result.operand = lvalue.operand;
    return result;
  }
  lowir_model::Instruction address;
  address.kind = lowir_model::Instruction::IK_ADDR;
  address.dest = NewTemp();
  address.type = PtrType();
  address.first = lvalue.operand;
  Emit(address);
  result.operand = TempOperand(address.dest);
  return result;
}

// The implicit object of a member function body: `this` is stored to its
// slot on entry and reloaded at every use, as the fixtures spell it.
lowir_model::Operand Lowerer::LoadThis()
{
  lowir_model::Instruction load;
  load.kind = lowir_model::Instruction::IK_LOAD;
  load.dest = NewTemp();
  load.type = PtrType();
  load.first = SlotOperand("$this");
  Emit(load);
  return TempOperand(load.dest);
}

lowir_model::Operand Lowerer::ProjectField(
    const lowir_model::Operand& base, std::size_t offset,
    lowir_model::IndexProjectionKind kind)
{
  lowir_model::Instruction projection;
  projection.kind = lowir_model::Instruction::IK_INDEX;
  projection.dest = NewTemp();
  projection.type = I8Type();
  projection.index_projection = kind;
  projection.first = base;
  projection.second = Immediate(static_cast<long long>(offset));
  Emit(projection);
  return TempOperand(projection.dest);
}

// Element `index` of the array at `array_address`: the array decays, and a
// class element is reached by a byte offset scaled from its size.
lowir_model::Operand Lowerer::ProjectArrayElement(
    const lowir_model::Operand& array_address, TypeId element,
    std::size_t index)
{
  lowir_model::Instruction decay;
  decay.kind = lowir_model::Instruction::IK_UNARY;
  decay.dest = NewTemp();
  decay.op = "decay";
  decay.type = PtrType();
  decay.first = array_address;
  Emit(decay);
  lowir_model::Operand offset = Immediate(static_cast<long long>(index));
  const std::size_t element_size = types_.SizeOf(element);
  const bool byte_element =
      types_.Kind(types_.Unqualified(element)) == TYPE_CLASS;
  if (byte_element && element_size != 1) {
    lowir_model::Instruction scale;
    scale.kind = lowir_model::Instruction::IK_BINARY;
    scale.dest = NewTemp();
    scale.op = "mul";
    scale.type = I64Type();
    scale.first = offset;
    scale.second = Immediate(static_cast<long long>(element_size));
    Emit(scale);
    offset = TempOperand(scale.dest);
  }
  lowir_model::Instruction projection;
  projection.kind = lowir_model::Instruction::IK_INDEX;
  projection.dest = NewTemp();
  projection.type = byte_element ? I8Type() : LowTypeOf(element);
  projection.index_projection = lowir_model::IPK_ARRAY_ELEMENT;
  projection.first = TempOperand(decay.dest);
  projection.second = offset;
  Emit(projection);
  return TempOperand(projection.dest);
}

// 5.16p4: both arms are lvalues of one type; the selected address is kept
// in a pointer slot and the result is that object.
Lowerer::Value Lowerer::LowerConditionalLValue(SemaId node)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 3)
    Unsupported("this conditional lvalue");
  const std::string slot = NewGeneratedSlot("condaddr", PtrType());
  const std::string then_label = NewBlockLabel("condaddr_then");
  const std::string else_label = NewBlockLabel("condaddr_else");
  const std::string end_label = NewBlockLabel("condaddr_end");
  PrepareConditionLabels(children[0]);
  LowerCondition(children[0], then_label, else_label);
  for (std::size_t arm = 1; arm <= 2; ++arm) {
    StartBlock(arm == 1 ? then_label : else_label);
    Value address = AddressValue(LowerLValue(children[arm]));
    // The semantic conditional type is the common base object.  Preserve
    // that reference binding in LowIR by projecting a derived arm to the
    // selected base subobject before storing its address.
    ProjectDerivedReference(address, tree_.At(children[arm]).type,
                            value.type);
    EmitStore(PtrType(), address.operand, SlotOperand(slot));
    EmitJump(end_label);
  }
  StartBlock(end_label);
  lowir_model::Instruction load;
  load.kind = lowir_model::Instruction::IK_LOAD;
  load.dest = NewTemp();
  load.type = PtrType();
  load.first = SlotOperand(slot);
  Emit(load);
  Value result;
  result.type = ReferentType(value.type);
  result.lvalue = true;
  result.operand = TempOperand(load.dest);
  return result;
}

// 4.2/4.3: an array or function designator becomes a pointer.  Storage
// designators take `addr` first; a reference already holds the address.
Lowerer::Value Lowerer::LowerArrayDecay(SemaId node)
{
  const SemaNode& value = tree_.At(node);
  const TypeId source_type = ReferentType(value.type);
  const TypeKind kind = types_.Kind(types_.Unqualified(source_type));
  if (kind != TYPE_ARRAY && kind != TYPE_FUNCTION)
    return LowerRValue(node);
  if (value.kind == SEMA_LITERAL)
    return LowerRValue(node);
  Value lvalue = LowerLValue(node);
  const bool addressable =
      lvalue.operand.kind == lowir_model::Operand::OP_SLOT ||
      lvalue.operand.kind == lowir_model::Operand::OP_GLOBAL;
  if (addressable)
    lvalue = AddressValue(lvalue);
  if (kind == TYPE_FUNCTION || addressable || kind == TYPE_ARRAY) {
    lowir_model::Instruction decay;
    decay.kind = lowir_model::Instruction::IK_UNARY;
    decay.dest = NewTemp();
    decay.op = "decay";
    decay.type = PtrType();
    decay.first = lvalue.operand;
    Emit(decay);
    lvalue.operand = TempOperand(decay.dest);
  }
  lvalue.type = types_.Decay(source_type);
  lvalue.lvalue = false;
  return lvalue;
}

Lowerer::Value Lowerer::LowerSubscript(SemaId node, bool lvalue)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 2)
    Unsupported("this subscript expression");
  const TypeId base_type = ReferentType(tree_.At(children[0]).type);
  const TypeKind base_kind = types_.Kind(types_.Unqualified(base_type));
  const Value base = base_kind == TYPE_ARRAY || base_kind == TYPE_FUNCTION ?
      LowerArrayDecay(children[0]) : LowerRValue(children[0]);
  Value index = LowerRValue(children[1]);
  const TypeId element = ReferentType(value.type);
  const TypeId element_unqualified = types_.Unqualified(element);
  if (types_.Kind(element_unqualified) == TYPE_CLASS &&
      types_.SizeOf(element_unqualified) != 1) {
    lowir_model::Instruction scale;
    scale.kind = lowir_model::Instruction::IK_BINARY;
    scale.dest = NewTemp();
    scale.op = "mul";
    scale.type = I64Type();
    scale.first = index.operand;
    scale.second = Immediate(static_cast<long long>(
        types_.SizeOf(element_unqualified)));
    Emit(scale);
    index.operand = TempOperand(scale.dest);
  }
  lowir_model::Instruction projection;
  projection.kind = lowir_model::Instruction::IK_INDEX;
  projection.dest = NewTemp();
  projection.type = types_.Kind(element_unqualified) == TYPE_CLASS ?
      I8Type() : LowTypeOf(element);
  projection.index_projection = lowir_model::IPK_ARRAY_ELEMENT;
  projection.first = base.operand;
  projection.second = index.operand;
  Emit(projection);
  Value result;
  result.type = element;
  result.lvalue = lvalue;
  result.operand = TempOperand(projection.dest);
  return result;
}

Lowerer::Value Lowerer::LowerConstructorTemporary(SemaId node)
{
  const SemaNode& value = tree_.At(node);
  if (value.function == 0)
    Unsupported("a constructor temporary without a constructor");
  const FunctionEntity& constructor = model_.FunctionAt(value.function);
  TemporaryObject& temporary = temporaries_[node];
  if (temporary.slot.empty())
    temporary.slot = NewGeneratedSlot(
        ConstructorTemporaryIsObjectExpression(node) ? "tmpobj" : "arg",
                                      LowTypeOf(value.type));

  if (!temporary.constructed) {
    temporary.constructed = true;
    const std::vector<SemaId> action_children = Children(node);
    if (action_children.size() != 1 ||
        tree_.At(action_children[0]).kind != SEMA_CALL)
      Unsupported("this constructor temporary");
    const std::vector<SemaId> call_children = Children(action_children[0]);
    if (call_children.empty() || tree_.At(call_children[0]).kind != SEMA_CALLEE)
      Unsupported("a constructor temporary without a callee");
    const TypeNode& callable = types_.At(types_.Unqualified(constructor.type));
    if (call_children.size() - 1 > callable.parameters.size() - 1)
      Unsupported("a constructor temporary with too many arguments");

    Value object;
    object.type = value.type;
    object.lvalue = true;
    object.operand = SlotOperand(temporary.slot);
    const lowir_model::Operand address = AddressValue(object).operand;
    temporary.address = address;
    lowir_model::Instruction call;
    call.kind = lowir_model::Instruction::IK_CALL;
    call.type = VoidType();
    call.call_return_type = VoidType();
    call.call_returns_void = true;
    call.first = GlobalOperand(FunctionSymbolName(value.function));
    call.args.push_back(address);
    for (std::size_t i = 1; i < call_children.size(); ++i) {
      const TypeId parameter = callable.parameters[i];
      if (types_.Kind(types_.Unqualified(parameter)) == TYPE_REFERENCE)
        call.args.push_back(LowerReferenceArgument(
            call_children[i], parameter).operand);
      else
        call.args.push_back(LowerRValue(call_children[i], parameter).operand);
    }
    Emit(call);
  }

  Value result;
  result.type = value.type;
  result.lvalue = true;
  result.operand = temporary.address;
  return result;
}

bool Lowerer::ConstructorTemporaryIsObjectExpression(SemaId node) const
{
  if (node == 0 || tree_.At(node).kind != SEMA_CONSTRUCTOR_ACTION)
    return false;
  const std::vector<SemaId> action_children = Children(node);
  if (action_children.size() != 1 ||
      tree_.At(action_children[0]).kind != SEMA_CALL)
    return false;
  const std::vector<SemaId> call_children = Children(action_children[0]);
  if (call_children.size() == 1 && tree_.At(node).function != 0) {
    const FunctionEntity& constructor =
        model_.FunctionAt(tree_.At(node).function);
    if (constructor.special_member == SPECIAL_MEMBER_CONSTRUCTOR &&
        constructor.member_class != 0 &&
        model_.ClassAt(constructor.member_class).trivial_default_constructor)
      return true;
  }
  for (std::size_t i = 1; i < call_children.size(); ++i) {
    SemaId argument = call_children[i];
    while (argument != 0 && tree_.At(argument).kind == SEMA_CAST) {
      const std::vector<SemaId> cast_children = Children(argument);
      if (cast_children.size() != 1)
        break;
      argument = cast_children[0];
    }
    if (argument != 0 && (tree_.At(argument).kind == SEMA_MEMBER ||
                          tree_.At(argument).kind == SEMA_CALL))
      return true;
  }
  return false;
}

Lowerer::Value Lowerer::LowerNew(SemaId node, TypeId expected)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.empty() || children.size() > 2)
    Unsupported("this new-expression");

  const TypeId object_type = PointerElementType(value.type);
  if (object_type == 0)
    Unsupported("a new-expression without an object type");
  const Value allocation = LowerRValue(children[0], value.type);
  Value result;
  result.type = value.type;
  result.operand = allocation.operand;
  if (children.size() == 1)
    return Convert(result, expected);

  const SemaId initialization = children[1];
  const TypeId object_unqualified = types_.Unqualified(object_type);
  Value object;
  object.type = object_type;
  object.lvalue = true;
  object.operand = allocation.operand;
  if (types_.Kind(object_unqualified) == TYPE_CLASS &&
      tree_.At(initialization).kind == SEMA_CONSTRUCTOR_ACTION) {
    const SemaNode& action = tree_.At(initialization);
    if (action.function == 0)
      Unsupported("a new-expression constructor without a function");
    const std::vector<SemaId> action_children = Children(initialization);
    if (action_children.size() != 1 ||
        tree_.At(action_children[0]).kind != SEMA_CALL)
      Unsupported("this new-expression constructor action");
    const std::vector<SemaId> call_children = Children(action_children[0]);
    if (call_children.empty() ||
        tree_.At(call_children[0]).kind != SEMA_CALLEE)
      Unsupported("a new-expression constructor without a callee");
    const FunctionEntity& constructor = model_.FunctionAt(action.function);
    const TypeNode& callable = types_.At(types_.Unqualified(constructor.type));
    if (call_children.size() - 1 > callable.parameters.size() - 1)
      Unsupported("a new-expression constructor with too many arguments");
    const bool trivial_default =
        constructor.special_member == SPECIAL_MEMBER_CONSTRUCTOR &&
        constructor.member_class != 0 &&
        model_.ClassAt(constructor.member_class).trivial_default_constructor;
    // An aggregate constructor synthesized for a non-empty braced
    // initializer is not the class's default construction.  It still has a
    // call-shaped action even when the class itself has trivial default
    // construction, so only elide the zero-argument form.
    if (!trivial_default || call_children.size() != 1) {
      std::vector<lowir_model::Operand> arguments;
      arguments.reserve(call_children.size());
      arguments.push_back(allocation.operand);
      for (std::size_t i = 1; i < call_children.size(); ++i) {
        const TypeId parameter = callable.parameters[i];
        arguments.push_back(types_.Kind(types_.Unqualified(parameter)) ==
            TYPE_REFERENCE ? LowerReferenceArgument(
                call_children[i], parameter).operand :
            LowerRValue(call_children[i], parameter).operand);
      }
      EmitVoidCall(FunctionSymbolName(action.function), arguments);
    }
  } else if (types_.Kind(object_unqualified) == TYPE_CLASS &&
             tree_.At(initialization).kind == SEMA_BRACED_INIT_LIST) {
    initialized_bitfield_units_.clear();
    LowerAggregateObjectInitializer(initialization, object_type, object,
                                    std::vector<std::size_t>());
  } else {
    const Value initialized = LowerRValue(initialization, object_type);
    EmitStore(LowTypeOf(object_type), initialized.operand,
              allocation.operand);
  }
  return Convert(result, expected);
}

Lowerer::Value Lowerer::LowerLValue(SemaId node)
{
  if (node == 0)
    Unsupported("a missing lvalue");
  const SemaNode& value = tree_.At(node);
  if (value.kind == SEMA_CONSTRUCTOR_ACTION)
    return LowerConstructorTemporary(node);
  if (value.kind == SEMA_MEMBER)
  {
    const Binding& binding = model_.BindingAt(value.binding);
    if (binding.kind == BINDING_FUNCTION)
      Unsupported("a static or function member lvalue");
    if (binding.static_member) {
      const GlobalSymbol* global = GlobalFor(value.binding);
      if (global == 0)
        Unsupported("a static member without global storage");
      Value result;
      result.type = ReferentType(value.type);
      result.lvalue = true;
      if (types_.Kind(types_.Unqualified(binding.type)) == TYPE_REFERENCE) {
        lowir_model::Instruction load;
        load.kind = lowir_model::Instruction::IK_LOAD;
        load.dest = NewTemp();
        load.type = PtrType();
        load.first = GlobalOperand(global->name);
        Emit(load);
        result.operand = TempOperand(load.dest);
      } else {
        result.operand = GlobalOperand(global->name);
      }
      return result;
    }
    const std::vector<SemaId> children = Children(node);
    if (children.size() != 1)
      Unsupported("a member without one object");
    const SemaId object_node = children[0];
    TypeId object_type = tree_.At(object_node).type;
    if (types_.Kind(object_type) == TYPE_REFERENCE)
      object_type = types_.Referent(object_type);
    bool pointer_object = types_.IsPointer(object_type);
    TypeId class_type = pointer_object ?
        types_.At(types_.Unqualified(object_type)).base : object_type;
    class_type = types_.Unqualified(class_type);
    if (types_.Kind(class_type) != TYPE_CLASS)
      Unsupported("a member object that is not a class");

    Value address;
    if (pointer_object || value.op == OP_ARROW || value.op == KW_AUTO)
      address = LowerRValue(object_node);
    else
      address = AddressValue(LowerLValue(object_node));
    const ClassEntityId object_entity = types_.At(class_type).entity;
    ClassEntityId owner_entity = binding.declaring_class;
    if (owner_entity == 0 && model_.ScopeAt(binding.scope).kind == SCOPE_CLASS)
      owner_entity = model_.ScopeAt(binding.scope).class_entity;
    if (owner_entity == 0)
      Unsupported("a member without a class owner");

    std::vector<ClassBase> path;
    std::vector<ClassEntityId> visited;
    if (!FindBasePath(model_, object_entity, owner_entity, path, visited))
      Unsupported("a member whose class is unrelated to its object");
    std::size_t offset = 0;
    for (std::size_t i = 0; i < path.size(); ++i)
      offset += path[i].offset;
    if (!path.empty())
      address.operand = ProjectField(address.operand, offset,
                                     lowir_model::IPK_BASE_SUBOBJECT);
    const ClassField* field = model_.FieldFor(value.binding);
    if (field == 0)
      Unsupported("a member without layout metadata");
    const bool reference_member =
        types_.Kind(types_.Unqualified(binding.type)) == TYPE_REFERENCE;
    const lowir_model::Operand member = ProjectField(
        address.operand, field->offset,
        reference_member ? lowir_model::IPK_REFERENCE_FIELD :
                           lowir_model::IPK_FIELD);
    if (reference_member) {
      lowir_model::Instruction referent;
      referent.kind = lowir_model::Instruction::IK_LOAD;
      referent.dest = NewTemp();
      referent.type = PtrType();
      referent.first = member;
      Emit(referent);
      Value result;
      result.type = types_.Referent(binding.type);
      result.lvalue = true;
      result.operand = TempOperand(referent.dest);
      return result;
    }
    Value result;
    result.type = ReferentType(value.type);
    result.lvalue = true;
    result.operand = member;
    return result;
  }
  if (value.kind == SEMA_ID_EXPRESSION) {
    const Binding& binding = model_.BindingAt(value.binding);
    if (binding.kind == BINDING_VARIABLE || binding.kind == BINDING_PARAMETER) {
      Value result;
      result.type = ReferentType(value.type);
      result.lvalue = true;
      const std::map<BindingId, lowir_model::Operand>::const_iterator address =
          parameter_addresses_.find(value.binding);
      if (address != parameter_addresses_.end()) {
        result.operand = address->second;
        return result;
      }
      const GlobalSymbol* global = GlobalFor(value.binding);
      const lowir_model::Operand storage = global != 0 ?
          GlobalOperand(global->name) : SlotOperand(SlotFor(value.binding));
      if (types_.Kind(types_.Unqualified(binding.type)) != TYPE_REFERENCE) {
        result.operand = storage;
        return result;
      }
      // A reference's storage holds the referent's address.
      lowir_model::Instruction load;
      load.kind = lowir_model::Instruction::IK_LOAD;
      load.dest = NewTemp();
      load.type = PtrType();
      load.first = storage;
      Emit(load);
      result.operand = TempOperand(load.dest);
      return result;
    }
    if (binding.kind == BINDING_FUNCTION) {
      Value result;
      result.type = value.type;
      result.lvalue = true;
      result.operand = GlobalOperand(FunctionSymbolName(
          value.function != 0 ? value.function : binding.function));
      return result;
    }
  }
  if (value.kind == SEMA_BINARY && value.op == OP_COMMA) {
    const std::vector<SemaId> children = Children(node);
    if (children.size() != 2)
      Unsupported("this comma expression");
    LowerDiscard(children[0]);
    return LowerLValue(children[1]);
  }
  if (value.kind == SEMA_UNARY && value.op == OP_STAR) {
    const std::vector<SemaId> children = Children(node);
    if (children.size() != 1)
      Unsupported("this indirection expression");
    const Value pointer = LowerRValue(children[0]);
    Value result;
    result.type = ReferentType(value.type);
    result.lvalue = true;
    result.operand = pointer.operand;
    return result;
  }
  if (value.kind == SEMA_SUBSCRIPT)
    return LowerSubscript(node, true);
  if (value.kind == SEMA_CALL && types_.Kind(value.type) == TYPE_REFERENCE) {
    Value result = LowerCall(node, 0);
    // The ABI result is already the address of the referent.  Keep that
    // address for lvalue consumers such as `first(&item).x`; an rvalue
    // consumer performs the object load at its own boundary below.
    result.type = types_.Referent(value.type);
    result.lvalue = true;
    return result;
  }
  if (value.kind == SEMA_CALL &&
      types_.Kind(types_.Unqualified(value.type)) == TYPE_CLASS) {
    Value result = LowerCall(node, 0);
    if (result.lvalue)
      return result;
    // A direct class result is an object value.  Member access needs a
    // stable address, so materialize it once in an object-expression slot.
    Value storage;
    storage.type = value.type;
    storage.lvalue = true;
    storage.operand = SlotOperand(
        NewGeneratedSlot("tmpobj", LowTypeOf(value.type)));
    const lowir_model::Operand destination = AddressValue(storage).operand;
    EmitCopyObject(value.type, result.operand, destination);
    return storage;
  }
  if (value.kind == SEMA_CONDITIONAL && value.category == VC_LVALUE)
    return LowerConditionalLValue(node);
  if (value.kind == SEMA_ASSIGNMENT) {
    Value assigned;
    (void)LowerAssignment(node, &assigned);
    return assigned;
  }
  if (value.kind == SEMA_UNARY && (value.op == OP_INC || value.op == OP_DEC))
    return LowerUnary(node, false, 0, true);
  Unsupported("a non-scalar lvalue");
  return Value();
}

Lowerer::Value Lowerer::LowerRValue(SemaId node, TypeId expected)
{
  if (node == 0)
    Unsupported("a missing expression");
  const SemaNode& value = tree_.At(node);
  switch (value.kind) {
  case SEMA_LITERAL:
    return LowerLiteral(node, value, expected);
  case SEMA_NEW_EXPRESSION:
    return LowerNew(node, expected);
  case SEMA_CONSTRUCTOR_ACTION:
    return LowerConstructorTemporary(node);
  case SEMA_ID_EXPRESSION: {
    const Binding& binding = model_.BindingAt(value.binding);
    if (binding.kind == BINDING_ENUMERATOR)
      return LowerLiteral(node, value, expected);
    if (binding.kind == BINDING_VARIABLE || binding.kind == BINDING_PARAMETER) {
      const TypeKind kind =
          types_.Kind(types_.Unqualified(ReferentType(value.type)));
      if (kind == TYPE_ARRAY || kind == TYPE_FUNCTION)
        return Convert(LowerArrayDecay(node), expected);
      return Convert(LoadValue(LowerLValue(node)), expected);
    }
    if (binding.kind == BINDING_FUNCTION)
      return Convert(LowerArrayDecay(node), expected);
    Unsupported("this named value");
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
      Unsupported("this cast expression");
    if (IsVoidType(types_, value.type)) {
      LowerDiscard(children[0]);
      Value result;
      result.type = value.type;
      return result;
    }
    return Convert(LowerRValue(children[0], value.type), expected);
  }
  case SEMA_SIZEOF: {
    lowir_model::Instruction constant;
    constant.kind = lowir_model::Instruction::IK_CONST;
    constant.dest = NewTemp();
    constant.type = LowTypeOf(value.type);
    constant.first = Immediate(value.has_value ? value.value : 0);
    Emit(constant);
    Value result;
    result.type = value.type;
    result.operand = TempOperand(constant.dest);
    return Convert(result, expected);
  }
  case SEMA_BRACED_INIT_LIST: {
    // 8.5.4p3: `{}` value-initializes; `{e}` initializes from e.
    const std::vector<SemaId> children = Children(node);
    if (!children.empty())
      return LowerRValue(children[0], expected);
    Value zero;
    zero.type = expected == 0 ? value.type : expected;
    zero.operand = (types_.IsNullPointerType(zero.type) ||
                    types_.IsPointer(zero.type)) ?
        NullptrImmediate() : Immediate(0);
    return zero;
  }
  case SEMA_CALL:
  {
    Value result = LowerCall(node, 0);
    if (types_.Kind(types_.Unqualified(value.type)) == TYPE_CLASS &&
        result.lvalue)
      result = LoadValue(result);
    if (types_.Kind(value.type) == TYPE_REFERENCE) {
      // A reference-returning call is an lvalue designating the object at
      // the returned address.  Loading that lvalue is what turns `T&` into a
      // value, including the pointer value in `Derived*&`.
      result.type = types_.Referent(value.type);
      result.lvalue = true;
      result = LoadValue(result);
    }
    return Convert(result, expected);
  }
  case SEMA_SUBSCRIPT:
    return Convert(LoadValue(LowerSubscript(node, false)), expected);
  case SEMA_PSEUDO_DESTRUCTOR:
  {
    const std::vector<SemaId> children = Children(node);
    if (children.size() != 1)
      Unsupported("a pseudo-destructor without one object");
    if (value.op == OP_ARROW)
      (void)LowerRValue(children[0]);
    else
      (void)AddressValue(LowerLValue(children[0]));
    Value result;
    result.type = value.type;
    return result;
  }
  case SEMA_CALLEE:
    Unsupported("a callee outside a call");
    break;
  case SEMA_MEMBER:
  {
    const Binding& binding = model_.BindingAt(value.binding);
    if (binding.kind == BINDING_ENUMERATOR && binding.has_const_value) {
      Value result;
      result.type = binding.type;
      result.operand = Immediate(binding.const_value);
      return Convert(result, expected);
    }
    if (binding.static_member && binding.has_const_value)
    {
      if (value.type == 0)
        Unsupported("a static member without a type");
      Value result;
      result.type = binding.type;
      result.operand = Immediate(binding.const_value);
      return Convert(result, expected);
    }
    ClassField bit_field;
    if (FindBitField(node, bit_field))
      return Convert(LoadBitField(node, bit_field), expected);
    return Convert(LoadValue(LowerLValue(node)), expected);
  }
  default:
    Unsupported("this expression");
  }
  return Value();
}

Lowerer::Value Lowerer::LowerUnary(SemaId node, bool postfix, TypeId expected,
                                   bool as_lvalue)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 1)
    Unsupported("this unary expression");
  const SemaId operand_node = children[0];
  switch (value.op) {
  case OP_AMP: {
    Value result = AddressValue(LowerLValue(operand_node));
    result.type = value.type;
    return Convert(result, expected);
  }
  case OP_STAR: {
    Value lvalue = LowerLValue(node);
    const TypeId object_type = ReferentType(value.type);
    lvalue.type = object_type;
    const TypeKind kind = types_.Kind(types_.Unqualified(object_type));
    if (kind == TYPE_ARRAY || kind == TYPE_FUNCTION) {
      lvalue.type = types_.Decay(object_type);
      lvalue.lvalue = false;
      return Convert(lvalue, expected);
    }
    return Convert(LoadValue(lvalue), expected);
  }
  case OP_LNOT: {
    const Value operand = LowerRValue(operand_node);
    lowir_model::Instruction compare;
    compare.kind = lowir_model::Instruction::IK_CMP;
    compare.dest = NewTemp();
    compare.op = "eq";
    compare.type = LowTypeOf(operand.type);
    compare.first = operand.operand;
    compare.second = ZeroOperand(operand.type);
    Emit(compare);
    Value result;
    result.type = value.type;
    result.operand = TempOperand(compare.dest);
    return Convert(result, expected);
  }
  case OP_PLUS: {
    const TypeKind operand_kind = types_.Kind(
        types_.Unqualified(ReferentType(tree_.At(operand_node).type)));
    const Value operand =
        operand_kind == TYPE_ARRAY || operand_kind == TYPE_FUNCTION ?
        LowerArrayDecay(operand_node) : LowerRValue(operand_node, value.type);
    return Convert(operand, expected);
  }
  case OP_MINUS:
  case OP_COMPL: {
    const Value operand = LowerRValue(operand_node, value.type);
    lowir_model::Instruction unary;
    unary.kind = lowir_model::Instruction::IK_UNARY;
    unary.dest = NewTemp();
    unary.op = value.op == OP_MINUS ? "neg" : "not";
    unary.type = LowTypeOf(value.type);
    unary.first = operand.operand;
    Emit(unary);
    Value result;
    result.type = value.type;
    result.operand = TempOperand(unary.dest);
    return Convert(result, expected);
  }
  case OP_INC:
  case OP_DEC:
    return LowerIncrement(node, operand_node, postfix, expected, as_lvalue);
  default:
    break;
  }
  Unsupported("this unary operator");
  return Value();
}

// Prefix and postfix `++`/`--`: load, adjust, store; the prefix result is
// the new value (or the object itself when an lvalue is wanted), the postfix
// result is the old value.
Lowerer::Value Lowerer::LowerIncrement(SemaId node, SemaId operand_node,
                                       bool postfix, TypeId expected,
                                       bool as_lvalue)
{
  const SemaNode& value = tree_.At(node);
  const bool decrement = value.op == OP_DEC;
  ClassField bit_field;
  if (FindBitField(operand_node, bit_field)) {
    const TypeId arithmetic_type = BitFieldValueType(bit_field);
    const Value initial_lvalue = LowerLValue(operand_node);
    Value old = ReadBitField(initial_lvalue, bit_field);
    old.type = arithmetic_type;
    lowir_model::Instruction binary;
    binary.kind = lowir_model::Instruction::IK_BINARY;
    binary.dest = NewTemp();
    binary.op = decrement ? "sub" : "add";
    binary.type = LowTypeOf(arithmetic_type);
    binary.first = old.operand;
    binary.second = OneOperand(arithmetic_type);
    Emit(binary);
    Value updated;
    updated.type = arithmetic_type;
    updated.operand = TempOperand(binary.dest);

    // The read and the write are separate member accesses.  In particular,
    // the write must read-modify-write the allocation unit so neighboring
    // fields survive an increment.
    const Value destination = postfix ? initial_lvalue :
        LowerLValue(operand_node);
    StoreBitField(bit_field, destination.operand, arithmetic_type,
                  updated.operand, true, arithmetic_type, true);
    if (as_lvalue) {
      Value result = destination;
      result.lvalue = true;
      return result;
    }
    return Convert(postfix ? old : updated, expected);
  }
  Value lvalue = LowerLValue(operand_node);
  const Value old = LoadValue(lvalue);
  Value updated;
  if (types_.IsPointer(lvalue.type)) {
    const std::size_t element_size =
        types_.SizeOf(PointerElementType(lvalue.type));
    lowir_model::Operand offset = Immediate(1);
    if (element_size != 1) {
      lowir_model::Instruction scale;
      scale.kind = lowir_model::Instruction::IK_BINARY;
      scale.dest = NewTemp();
      scale.op = "mul";
      scale.type = I64Type();
      scale.first = Immediate(1);
      scale.second = Immediate(static_cast<long long>(element_size));
      Emit(scale);
      offset = TempOperand(scale.dest);
    }
    if (decrement) {
      lowir_model::Instruction negate;
      negate.kind = lowir_model::Instruction::IK_BINARY;
      negate.dest = NewTemp();
      negate.op = "sub";
      negate.type = I64Type();
      negate.first = Immediate(0);
      negate.second = offset;
      Emit(negate);
      offset = TempOperand(negate.dest);
    }
    lowir_model::Instruction projection;
    projection.kind = lowir_model::Instruction::IK_INDEX;
    projection.dest = NewTemp();
    projection.type = I8Type();
    projection.first = old.operand;
    projection.second = offset;
    Emit(projection);
    updated.type = lvalue.type;
    updated.operand = TempOperand(projection.dest);
    EmitStore(PtrType(), updated.operand, lvalue.operand);
  } else {
    const TypeId arithmetic_type = value.type == 0 ? lvalue.type : value.type;
    const Value current = Convert(old, arithmetic_type);
    lowir_model::Instruction binary;
    binary.kind = lowir_model::Instruction::IK_BINARY;
    binary.dest = NewTemp();
    binary.op = decrement ? "sub" : "add";
    binary.type = LowTypeOf(arithmetic_type);
    binary.first = current.operand;
    binary.second = OneOperand(arithmetic_type);
    Emit(binary);
    updated.type = arithmetic_type;
    updated.operand = TempOperand(binary.dest);
    EmitStore(LowTypeOf(lvalue.type), Convert(updated, lvalue.type).operand,
              lvalue.operand);
  }
  if (as_lvalue) {
    lvalue.lvalue = true;
    return lvalue;
  }
  return Convert(postfix ? old : updated, expected);
}

// 5.7p5: pointer plus or minus an integer scales by the element size; the
// index is converted to `long` first, the product may be negated, and the
// byte offset is applied with a byte-typed `index`.
Lowerer::Value Lowerer::LowerPointerOffset(Value pointer, SemaId index_node,
                                           bool subtract, TypeId result_type)
{
  const Value index = LowerRValue(index_node, types_.Fundamental(FT_LONG_INT));
  const std::size_t element_size =
      types_.SizeOf(PointerElementType(pointer.type));
  lowir_model::Operand offset = index.operand;
  if (element_size != 1) {
    lowir_model::Instruction scale;
    scale.kind = lowir_model::Instruction::IK_BINARY;
    scale.dest = NewTemp();
    scale.op = "mul";
    scale.type = I64Type();
    scale.first = index.operand;
    scale.second = Immediate(static_cast<long long>(element_size));
    Emit(scale);
    offset = TempOperand(scale.dest);
  }
  if (subtract) {
    lowir_model::Instruction negate;
    negate.kind = lowir_model::Instruction::IK_BINARY;
    negate.dest = NewTemp();
    negate.op = "sub";
    negate.type = I64Type();
    negate.first = Immediate(0);
    negate.second = offset;
    Emit(negate);
    offset = TempOperand(negate.dest);
  }
  lowir_model::Instruction projection;
  projection.kind = lowir_model::Instruction::IK_INDEX;
  projection.dest = NewTemp();
  projection.type = I8Type();
  projection.first = pointer.operand;
  projection.second = offset;
  Emit(projection);
  Value result;
  result.type = result_type;
  result.operand = TempOperand(projection.dest);
  return result;
}

Lowerer::Value Lowerer::LowerBinary(SemaId node, TypeId expected)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 2)
    Unsupported("this binary expression");

  // A left-leaning chain of scalar operators (`a + b + c + …`) is reduced
  // iteratively along its left spine so expression depth never becomes
  // call-stack depth.
  std::vector<SemaId> scalar_chain;
  SemaId leaf = node;
  while (tree_.At(leaf).kind == SEMA_BINARY &&
         IsScalarBinaryOperator(tree_.At(leaf).op)) {
    const std::vector<SemaId> current = Children(leaf);
    if (current.size() != 2)
      Unsupported("this binary expression");
    if (types_.IsPointer(types_.Decay(tree_.At(current[0]).type)) ||
        types_.IsPointer(types_.Decay(tree_.At(current[1]).type)) ||
        types_.IsPointer(types_.Decay(tree_.At(leaf).type)))
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
  if (IsLogicalOperator(value.op))
    return Convert(LowerLogicalValue(node), expected);

  const TypeId left_source = types_.Decay(tree_.At(children[0]).type);
  const TypeId right_source = types_.Decay(tree_.At(children[1]).type);
  const bool left_pointer = types_.IsPointer(left_source);
  const bool right_pointer = types_.IsPointer(right_source);
  if ((value.op == OP_PLUS || value.op == OP_MINUS) &&
      ((left_pointer && types_.IsIntegral(right_source)) ||
       (value.op == OP_PLUS && right_pointer &&
        types_.IsIntegral(left_source)))) {
    const SemaId pointer_node = left_pointer ? children[0] : children[1];
    const SemaId index_node = left_pointer ? children[1] : children[0];
    const Value pointer = LowerRValue(pointer_node);
    return Convert(LowerPointerOffset(pointer, index_node,
                                      value.op == OP_MINUS && left_pointer,
                                      value.type), expected);
  }
  if (value.op == OP_MINUS && left_pointer && right_pointer) {
    // 5.7p6: the byte difference divided by the element size.
    const Value left = LowerRValue(children[0]);
    const Value right = LowerRValue(children[1]);
    lowir_model::Instruction difference;
    difference.kind = lowir_model::Instruction::IK_BINARY;
    difference.dest = NewTemp();
    difference.op = "sub";
    difference.type = PtrType();
    difference.first = left.operand;
    difference.second = right.operand;
    Emit(difference);
    lowir_model::Instruction count;
    count.kind = lowir_model::Instruction::IK_BINARY;
    count.dest = NewTemp();
    count.op = "div";
    count.type = I64Type();
    count.first = TempOperand(difference.dest);
    count.second = Immediate(static_cast<long long>(
        types_.SizeOf(PointerElementType(left.type))));
    Emit(count);
    Value result;
    result.type = value.type;
    result.operand = TempOperand(count.dest);
    return Convert(result, expected);
  }
  if (value.op == OP_EQ || value.op == OP_NE || value.op == OP_LT ||
      value.op == OP_LE || value.op == OP_GT || value.op == OP_GE)
    return LowerComparison(node, children, expected);
  return LowerScalarBinary(node, LowerRValue(children[0]), expected);
}

// 5.9/5.10: operands meet at the composite pointer type, the common
// enumeration, or the usual arithmetic type; a null pointer constant or
// literal zero against a pointer stays an immediate; the result is the
// canonical i64 truth temporary typed as the expression's bool.
Lowerer::Value Lowerer::LowerComparison(SemaId node,
                                        const std::vector<SemaId>& children,
                                        TypeId expected)
{
  const SemaNode& value = tree_.At(node);
  const TypeId left_type = tree_.At(children[0]).type;
  const TypeId right_type = tree_.At(children[1]).type;
  const TypeId left_source = types_.Decay(left_type);
  const TypeId right_source = types_.Decay(right_type);
  const bool left_pointer = types_.IsPointer(left_source);
  const bool right_pointer = types_.IsPointer(right_source);
  TypeId common = 0;
  if (left_pointer && right_pointer) {
    bool ok = false;
    common = CompositePointer(model_, types_, left_source, right_source, ok);
    if (!ok)
      Unsupported("this pointer comparison");
  } else if (left_pointer && (types_.IsNullPointerType(right_source) ||
                              IsZeroLiteral(children[1]))) {
    common = left_source;
  } else if (right_pointer && (types_.IsNullPointerType(left_source) ||
                               IsZeroLiteral(children[0]))) {
    common = right_source;
  } else if (types_.Unqualified(left_type) == types_.Unqualified(right_type) &&
             types_.Kind(types_.Unqualified(left_type)) == TYPE_ENUM) {
    common = left_type;
  } else {
    common = types_.UsualArithmetic(left_type, right_type);
  }
  const bool pointer_common = types_.IsPointer(common);
  Value left = IsNullptrLiteral(children[0]) && pointer_common ?
      LowerRValue(children[0], common) : LowerRValue(children[0]);
  Value right = IsNullptrLiteral(children[1]) && pointer_common ?
      LowerRValue(children[1], common) : LowerRValue(children[1]);
  if (IsCharacterLiteral(children[0]) ||
      (pointer_common && IsZeroLiteral(children[0])))
    left.type = common;
  else
    left = Convert(left, common);
  if (IsCharacterLiteral(children[1]) ||
      (pointer_common && IsZeroLiteral(children[1])))
    right.type = common;
  else
    right = Convert(right, common);
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
  result.operand = TempOperand(compare.dest);
  return Convert(result, expected);
}

// Arithmetic, bitwise, and shift operators over one operation type: the
// expression type, or the promoted left operand for shifts (5.8p1).
// Character literals stay immediates of the operation type.
Lowerer::Value Lowerer::LowerScalarBinary(SemaId node, Value left,
                                          TypeId expected)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 2)
    Unsupported("this binary expression");
  const TypeId common = value.op == OP_LSHIFT || value.op == OP_RSHIFT ?
      tree_.At(children[0]).type : value.type;
  Value right = LowerRValue(children[1]);
  if (IsCharacterLiteral(children[0]))
    left.type = common;
  else
    left = Convert(left, common);
  if (IsCharacterLiteral(children[1]))
    right.type = common;
  else
    right = Convert(right, common);
  lowir_model::Instruction binary;
  binary.kind = lowir_model::Instruction::IK_BINARY;
  binary.dest = NewTemp();
  binary.type = LowTypeOf(common);
  binary.op = BinaryName(value.op, common);
  binary.first = left.operand;
  binary.second = right.operand;
  Emit(binary);
  Value result;
  result.type = value.type;
  result.operand = TempOperand(binary.dest);
  return Convert(result, expected);
}

// Simple assignment evaluates the right operand, then the left lvalue, then
// stores; compound assignment reads the object once, applies the operation
// in the object's type, and stores.  The result is the stored value, or the
// object when an lvalue is wanted.
Lowerer::Value Lowerer::LowerAssignment(SemaId node, Value* assigned_lvalue)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 2)
    Unsupported("this assignment expression");
  Value lhs;
  Value rhs;
  if (value.op == OP_ASS) {
    const TypeId target = ReferentType(tree_.At(children[0]).type);
    rhs = tree_.At(children[1]).kind == SEMA_BRACED_INIT_LIST ?
        LowerRValue(children[1], target) : LowerRValue(children[1]);
    // Apply the assignment conversion while the right operand is still the
    // only evaluated side.  Derived-to-base pointer projection (and any
    // future conversion with observable lowering) therefore precedes the
    // left-lvalue evaluation required by the LowIR fixture order.
    rhs = Convert(rhs, target);
    lhs = LowerLValue(children[0]);
    rhs = Convert(rhs, lhs.type);
  } else {
    lhs = LowerLValue(children[0]);
    const TypeId target = lhs.type;
    if ((value.op == OP_PLUSASS || value.op == OP_MINUSASS) &&
        types_.IsPointer(target)) {
      rhs = LowerPointerOffset(LoadValue(lhs), children[1],
                               value.op == OP_MINUSASS, target);
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
      default: Unsupported("this compound assignment operator");
      }
      const Value left = LoadValue(lhs);
      const Value right = LowerRValue(children[1], target);
      lowir_model::Instruction binary;
      binary.kind = lowir_model::Instruction::IK_BINARY;
      binary.dest = NewTemp();
      binary.type = LowTypeOf(target);
      binary.op = BinaryName(binary_op, target);
      binary.first = left.operand;
      binary.second = right.operand;
      Emit(binary);
      rhs.type = target;
      rhs.operand = TempOperand(binary.dest);
    }
  }
  ClassField bit_field;
  if (FindBitField(children[0], bit_field)) {
    const TypeId storage_type = BitFieldValueType(bit_field);
    rhs = Convert(rhs, storage_type);
    StoreBitField(bit_field, lhs.operand, rhs.type, rhs.operand, true,
                  storage_type);
    if (assigned_lvalue != 0) {
      *assigned_lvalue = lhs;
      assigned_lvalue->lvalue = true;
    }
    rhs.lvalue = false;
    return rhs;
  }
  EmitStore(LowTypeOf(lhs.type), rhs.operand, lhs.operand);
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
  if (tree_.At(node).kind == SEMA_CALL &&
      types_.Kind(types_.Unqualified(tree_.At(node).type)) == TYPE_CLASS) {
    Value discard;
    discard.type = tree_.At(node).type;
    discard.lvalue = true;
    discard.operand = SlotOperand(
        NewGeneratedSlot("discard", LowTypeOf(discard.type)));
    const lowir_model::Operand destination = AddressValue(discard).operand;
    if (ClassBoundary(discard.type, true) == CBM_INDIRECT_RESULT)
      (void)LowerCall(node, 0, &destination);
    else {
      const Value result = LowerCall(node, 0);
      EmitCopyObject(discard.type, result.operand, destination);
    }
  }
  else if (tree_.At(node).kind == SEMA_CALL)
    (void)LowerCall(node, 0);
  else if (tree_.At(node).category == VC_LVALUE &&
           types_.Kind(types_.Unqualified(ReferentType(
               tree_.At(node).type))) == TYPE_CLASS)
    // A discarded class glvalue is evaluated for its designation; loading
    // the opaque class value would invent a scalar read and loses the
    // object's canonical address.
    (void)AddressValue(LowerLValue(node));
  else
    (void)LowerRValue(node, 0);
}

// Value-context `&&`/`||`: the truth of the right operand, or the
// short-circuit constant, lands in an i64 slot that the join block reloads.
Lowerer::Value Lowerer::LowerLogicalValue(SemaId node)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 2)
    Unsupported("this logical expression");
  const bool disjunction = value.op == OP_LOR;
  const std::string stem = disjunction ? "lor" : "land";
  const std::string slot = NewGeneratedSlot(stem, I64Type());
  const std::string rhs_label = NewBlockLabel(stem + "_rhs");
  const std::string short_label = NewBlockLabel(stem + "_short");
  const std::string end_label = NewBlockLabel(stem + "_end");
  const std::string& on_true = disjunction ? short_label : rhs_label;
  const std::string& on_false = disjunction ? rhs_label : short_label;
  if (tree_.At(children[0]).kind == SEMA_BINARY &&
      IsLogicalOperator(tree_.At(children[0]).op)) {
    EmitBranch(LowerLogicalValue(children[0]).operand, on_true, on_false);
  } else {
    PrepareConditionLabels(children[0]);
    LowerCondition(children[0], on_true, on_false);
  }

  StartBlock(rhs_label);
  const Value rhs = LowerRValue(children[1]);
  lowir_model::Instruction compare;
  compare.kind = lowir_model::Instruction::IK_CMP;
  compare.dest = NewTemp();
  compare.op = "ne";
  // Integral right operands compare as the canonical i64 (fixture-pinned);
  // floating and pointer operands compare in their own type.
  const LowInfo rhs_info = LowInfoOf(rhs.type);
  compare.type = rhs_info.Integer() ? I64Type() : LowTypeOf(rhs.type);
  compare.first = rhs.operand;
  compare.second = ZeroOperand(rhs.type);
  Emit(compare);
  EmitStore(I64Type(), TempOperand(compare.dest), SlotOperand(slot));
  EmitJump(end_label);

  StartBlock(short_label);
  EmitStore(I64Type(), Immediate(disjunction ? 1 : 0), SlotOperand(slot));
  EmitJump(end_label);

  StartBlock(end_label);
  lowir_model::Instruction load;
  load.kind = lowir_model::Instruction::IK_LOAD;
  load.dest = NewTemp();
  load.type = I64Type();
  load.first = SlotOperand(slot);
  Emit(load);
  Value result;
  result.type = value.type;
  result.operand = TempOperand(load.dest);
  return result;
}

// 5.16: arms that are array/function designators or nested lvalue
// conditionals of pointer type select an address; other arms store the
// converted value into a slot of the expression type; void arms are only
// evaluated.
Lowerer::Value Lowerer::LowerConditional(SemaId node, TypeId expected)
{
  const SemaNode& value = tree_.At(node);
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 3)
    Unsupported("this conditional expression");
  bool addressable_arms = types_.IsPointer(value.type);
  for (std::size_t i = 1; i < children.size() && addressable_arms; ++i) {
    const SemaNode& arm = tree_.At(children[i]);
    const TypeKind kind = types_.Kind(types_.Unqualified(
        ReferentType(arm.type)));
    const bool designator = kind == TYPE_ARRAY || kind == TYPE_FUNCTION;
    const bool selected_lvalue = arm.kind == SEMA_CONDITIONAL &&
        (arm.category == VC_LVALUE || arm.category == VC_XVALUE) &&
        types_.IsPointer(arm.type);
    addressable_arms = designator || selected_lvalue;
  }
  if (addressable_arms) {
    Value lvalue = LowerConditionalLValue(node);
    if (expected == 0 || types_.IsPointer(expected)) {
      lvalue.type = expected == 0 ? value.type : expected;
      lvalue.lvalue = false;
      return Convert(lvalue, expected);
    }
    return Convert(LoadValue(lvalue), expected);
  }
  const bool is_void = IsVoidType(types_, value.type);
  const std::string slot = is_void ? std::string() :
      NewGeneratedSlot("cond", LowTypeOf(value.type));
  const std::string stem = is_void ? "discard_cond" : "cond";
  const std::string then_label = NewBlockLabel(stem + "_then");
  const std::string else_label = NewBlockLabel(stem + "_else");
  const std::string end_label = NewBlockLabel(stem + "_end");
  const SemaNode& condition = tree_.At(children[0]);
  bool materialize_logical = false;
  if (condition.kind == SEMA_BINARY && IsLogicalOperator(condition.op)) {
    // A `?:` condition materializes its logical value unless a literal
    // left operand already decides it.
    const std::vector<SemaId> condition_children = Children(children[0]);
    if (condition_children.size() != 2)
      Unsupported("this logical condition");
    const SemaNode& left = tree_.At(condition_children[0]);
    materialize_logical = !(IsKnownIntegralLiteral(left, types_) &&
        ((condition.op == OP_LOR && left.value != 0) ||
         (condition.op == OP_LAND && left.value == 0)));
  }
  if (materialize_logical) {
    EmitBranch(LowerLogicalValue(children[0]).operand, then_label, else_label);
  } else {
    PrepareConditionLabels(children[0]);
    LowerCondition(children[0], then_label, else_label);
  }
  for (std::size_t arm = 1; arm <= 2; ++arm) {
    StartBlock(arm == 1 ? then_label : else_label);
    if (is_void) {
      LowerDiscard(children[arm]);
    } else {
      const Value selected = LowerRValue(children[arm], value.type);
      EmitStore(LowTypeOf(value.type), selected.operand, SlotOperand(slot));
    }
    EmitJump(end_label);
  }
  StartBlock(end_label);
  Value result;
  result.type = value.type;
  if (is_void)
    return result;
  lowir_model::Instruction load;
  load.kind = lowir_model::Instruction::IK_LOAD;
  load.dest = NewTemp();
  load.type = LowTypeOf(value.type);
  load.first = SlotOperand(slot);
  Emit(load);
  result.operand = TempOperand(load.dest);
  return Convert(result, expected);
}

// 5.2.2p4: passing a class object by value materializes the parameter
// object before evaluating the source object.  The current LowIR object
// boundary carries the opaque object slot itself; the source is still
// evaluated as an lvalue so member access, comma expressions, and other
// side effects retain their source-order semantics.
Lowerer::Value Lowerer::LowerClassArgument(SemaId node, TypeId parameter)
{
  const TypeId object_type = types_.Unqualified(parameter);
  if (types_.Kind(object_type) != TYPE_CLASS)
    Unsupported("a non-class object argument");
  const ClassBoundaryMode boundary = ClassBoundary(parameter, false);
  const std::string slot = NewGeneratedSlot(
      boundary == CBM_BY_ADDRESS ? "arg" : "argobj", LowTypeOf(parameter));
  Value destination;
  destination.type = parameter;
  destination.lvalue = true;
  destination.operand = SlotOperand(slot);
  const Value destination_address = AddressValue(destination);

  const SemaNode& source = tree_.At(node);
  if (boundary == CBM_BY_ADDRESS) {
    // A constructor action owns the selected copy/move/converting
    // constructor and is lowered directly into the argument slot.  This is
    // the address-passed counterpart of a direct object copy.
    if (source.kind == SEMA_CONSTRUCTOR_ACTION) {
      LowerAggregateConstructor(node, parameter,
                                destination_address.operand);
    } else if (source.kind == SEMA_CALL &&
               types_.Kind(types_.Unqualified(source.type)) == TYPE_CLASS &&
               ClassBoundary(source.type, true) == CBM_INDIRECT_RESULT) {
      (void)LowerCall(node, parameter, &destination_address.operand);
    } else if (source.category == VC_LVALUE || source.category == VC_XVALUE) {
      EmitCopyObject(object_type, AddressValue(LowerLValue(node)).operand,
                     destination_address.operand);
    } else {
      EmitCopyObject(object_type, LowerRValue(node, parameter).operand,
                     destination_address.operand);
    }

    Value result;
    result.type = parameter;
    result.lvalue = true;
    result.operand = destination_address.operand;
    return result;
  }

  // Direct object parameters retain the opaque object value at the call
  // boundary.  The callee's entry sequence copies it into its named slot.
  const Value copied = source.category == VC_LVALUE ||
      source.category == VC_XVALUE ?
      AddressValue(LowerLValue(node)) : LowerRValue(node, parameter);
  if (!model_.ClassAt(types_.At(object_type).entity).empty)
    EmitCopyObject(object_type, copied.operand, destination_address.operand);

  Value result;
  result.type = parameter;
  result.operand = SlotOperand(slot);
  return result;
}

// 8.5.3: a reference parameter receives the address of an lvalue, of the
// object a comma expression selects, or of a slot holding the converted
// temporary.
Lowerer::Value Lowerer::LowerReferenceArgument(SemaId node, TypeId parameter)
{
  const TypeId referent = types_.Referent(parameter);
  if (referent == 0)
    Unsupported("a reference parameter without a referent");
  const SemaNode& source = tree_.At(node);
  Value address;
  if (source.category == VC_LVALUE || source.category == VC_XVALUE ||
      source.kind == SEMA_CALL || source.kind == SEMA_SUBSCRIPT ||
      source.kind == SEMA_CONDITIONAL ||
      source.kind == SEMA_CONSTRUCTOR_ACTION ||
      (source.kind == SEMA_UNARY && source.op == OP_STAR)) {
    address = AddressValue(LowerLValue(node));
  } else if (source.kind == SEMA_BINARY && source.op == OP_COMMA) {
    Value result = LowerRValue(node);
    result.type = referent;
    return result;
  } else {
    const Value materialized = LowerRValue(node, referent);
    Value storage;
    storage.type = referent;
    storage.operand = SlotOperand(
        NewGeneratedSlot("refarg", LowTypeOf(referent)));
    EmitStore(LowTypeOf(referent), materialized.operand, storage.operand);
    address = AddressValue(storage);
  }
  ProjectDerivedReference(address, source.type, referent);
  address.type = referent;
  address.lvalue = false;
  return address;
}

void Lowerer::ProjectDerivedReference(Value& value, TypeId source,
                                      TypeId target)
{
  const TypeId source_object = ReferentType(source);
  const TypeId target_object = ReferentType(target);
  ClassEntityId source_class = 0;
  ClassEntityId target_class = 0;
  if (ClassEntityForType(types_, source_object, source_class) &&
      ClassEntityForType(types_, target_object, target_class) &&
      model_.IsDerivedFrom(source_class, target_class))
  {
    std::vector<ClassBase> path;
    std::vector<ClassEntityId> visited;
    if (!FindBasePath(model_, source_class, target_class, path, visited))
      Unsupported("a derived reference without a base path");
    std::size_t offset = 0;
    for (std::size_t i = 0; i < path.size(); ++i)
      offset += path[i].offset;
    if (!path.empty())
      value.operand = ProjectField(value.operand, offset,
                                   lowir_model::IPK_BASE_SUBOBJECT);
  }
}

// Arguments are lowered in order before an indirect callee is evaluated;
// a direct callee names its symbol and thereby requests its declaration.
Lowerer::Value Lowerer::LowerCall(
    SemaId node, TypeId expected,
    const lowir_model::Operand* indirect_destination,
    const std::string& indirect_stem)
{
  const std::vector<SemaId> children = Children(node);
  if (children.empty())
    Unsupported("a call without a callee");
  const SemaNode& callee = tree_.At(children[0]);
  const bool direct = callee.kind == SEMA_CALLEE && callee.function != 0;
  TypeId function_type = 0;
  if (direct)
    function_type = model_.FunctionAt(callee.function).type;
  else if (!CallableFunctionType(types_, callee.type, function_type))
    Unsupported("an indirect callee that is not a function");
  const TypeNode& type = types_.At(types_.Unqualified(function_type));
  const std::size_t arguments = children.size() - 1;
  if (arguments < type.parameters.size() ||
      (!type.variadic && arguments > type.parameters.size()))
    Unsupported("a call whose argument count does not match");

  const ClassBoundaryMode result_boundary = ClassBoundary(type.result, true);
  const bool indirect_result = result_boundary == CBM_INDIRECT_RESULT;
  lowir_model::Operand result_destination;
  if (indirect_result) {
    if (indirect_destination != 0) {
      result_destination = *indirect_destination;
    } else {
      Value storage;
      storage.type = type.result;
      storage.lvalue = true;
      storage.operand = SlotOperand(NewGeneratedSlot(
          indirect_stem.empty() ? "arg" : indirect_stem,
          LowTypeOf(type.result)));
      result_destination = AddressValue(storage).operand;
    }
  }

  lowir_model::Instruction call;
  call.kind = lowir_model::Instruction::IK_CALL;
  call.type = indirect_result ? VoidType() : LowTypeOf(type.result);
  call.call_return_type = call.type;
  call.call_returns_void = indirect_result || IsVoidType(types_, type.result);
  if (indirect_result)
    call.args.push_back(result_destination);
  if (direct) {
    call.first = GlobalOperand(FunctionSymbolName(callee.function));
  } else {
    call.has_call_signature = true;
    call.call_boundary.arity = type.variadic ? lowir_model::CAM_VARIADIC :
        lowir_model::CAM_FIXED;
    if (indirect_result) {
      lowir_model::Parameter result_parameter;
      result_parameter.name = "%ret";
      result_parameter.type = PtrType();
      result_parameter.metadata.passing = lowir_model::PPM_INDIRECT_RESULT;
      call.call_params.push_back(result_parameter);
    }
    for (std::size_t i = 0; i < type.parameters.size(); ++i) {
      lowir_model::Parameter parameter;
      parameter.name = "%arg" + std::to_string(i);
      parameter.type = LowTypeOf(type.parameters[i]);
      if (types_.Kind(types_.Unqualified(type.parameters[i])) ==
          TYPE_REFERENCE)
        parameter.metadata.passing = lowir_model::PPM_REFERENCE;
      if (ClassBoundary(type.parameters[i], false) == CBM_BY_ADDRESS) {
        parameter.type = PtrType();
        parameter.metadata.passing = lowir_model::PPM_BY_ADDRESS;
      }
      call.call_params.push_back(parameter);
    }
  }
  for (std::size_t i = 1; i < children.size(); ++i) {
    const TypeId parameter_type = i - 1 < type.parameters.size() ?
        type.parameters[i - 1] :
        DefaultArgumentPromotion(tree_.At(children[i]).type);
    if (types_.Kind(types_.Unqualified(parameter_type)) == TYPE_REFERENCE)
      call.args.push_back(
          LowerReferenceArgument(children[i], parameter_type).operand);
    else if (types_.Kind(types_.Unqualified(parameter_type)) == TYPE_CLASS)
      call.args.push_back(LowerClassArgument(children[i], parameter_type).operand);
    else
      call.args.push_back(LowerRValue(children[i], parameter_type).operand);
  }
  if (!direct)
    call.first = LowerRValue(children[0]).operand;
  if (!call.call_returns_void)
    call.dest = NewTemp();
  Emit(call);
  Value result;
  result.type = type.result;
  if (indirect_result) {
    result.operand = result_destination;
    result.lvalue = true;
  } else if (!call.call_returns_void)
    result.operand = TempOperand(call.dest);
  if (indirect_result)
    return result;
  return Convert(result, expected);
}

}  // namespace lowir_lowering
