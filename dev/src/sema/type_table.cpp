#include "sema/type_table.h"

#include <ostream>
#include <sstream>
#include <stdexcept>

TypeNode::TypeNode()
    : kind(TYPE_INVALID), fundamental(FT_INT), base(0), result(0),
      array_bound(0), is_const(false), is_volatile(false),
      lvalue_reference(true), variadic(false), scoped(false),
      keyword(TK_NONE), entity(0)
{
}

bool IsFundamentalTypeKeyword(ETokenType token)
{
  switch (token)
  {
  case KW_CHAR: case KW_CHAR16_T: case KW_CHAR32_T: case KW_WCHAR_T:
  case KW_BOOL: case KW_SHORT: case KW_INT: case KW_LONG: case KW_SIGNED:
  case KW_UNSIGNED: case KW_FLOAT: case KW_DOUBLE: case KW_VOID:
    return true;
  default:
    return false;
  }
}

bool FundamentalIsIntegral(EFundamentalType type)
{
  switch (type)
  {
  case FT_SIGNED_CHAR: case FT_SHORT_INT: case FT_INT: case FT_LONG_INT:
  case FT_LONG_LONG_INT: case FT_UNSIGNED_CHAR: case FT_UNSIGNED_SHORT_INT:
  case FT_UNSIGNED_INT: case FT_UNSIGNED_LONG_INT:
  case FT_UNSIGNED_LONG_LONG_INT: case FT_WCHAR_T: case FT_CHAR:
  case FT_CHAR16_T: case FT_CHAR32_T: case FT_BOOL:
    return true;
  case FT_FLOAT: case FT_DOUBLE: case FT_LONG_DOUBLE: case FT_VOID:
  case FT_NULLPTR_T:
    return false;
  }
  return false;
}

bool FundamentalIsUnsigned(EFundamentalType type)
{
  switch (type)
  {
  case FT_UNSIGNED_CHAR: case FT_UNSIGNED_SHORT_INT: case FT_UNSIGNED_INT:
  case FT_UNSIGNED_LONG_INT: case FT_UNSIGNED_LONG_LONG_INT:
  case FT_CHAR16_T: case FT_CHAR32_T: case FT_BOOL:
    return true;
  default:
    return false; // char and wchar_t are signed on this target
  }
}

std::size_t FundamentalSize(EFundamentalType type)
{
  switch (type)
  {
  case FT_SIGNED_CHAR: case FT_UNSIGNED_CHAR: case FT_CHAR: case FT_BOOL:
    return 1;
  case FT_SHORT_INT: case FT_UNSIGNED_SHORT_INT: case FT_CHAR16_T:
    return 2;
  case FT_INT: case FT_UNSIGNED_INT: case FT_WCHAR_T: case FT_CHAR32_T:
  case FT_FLOAT:
    return 4;
  case FT_LONG_INT: case FT_LONG_LONG_INT: case FT_UNSIGNED_LONG_INT:
  case FT_UNSIGNED_LONG_LONG_INT: case FT_DOUBLE:
    return 8;
  case FT_LONG_DOUBLE:
    return 16;
  case FT_VOID: case FT_NULLPTR_T:
    break;
  }
  return 0;
}

bool TypeTable::FunctionKey::operator<(const FunctionKey& other) const
{
  if (result != other.result)
    return result < other.result;
  if (variadic != other.variadic)
    return variadic < other.variadic;
  return parameters < other.parameters;
}

TypeTable::TypeTable()
    : nodes_(1)
{
}

TypeId TypeTable::Add(const TypeNode& node)
{
  nodes_.push_back(node);
  return nodes_.size() - 1;
}

TypeId TypeTable::Fundamental(EFundamentalType type)
{
  const std::map<EFundamentalType, TypeId>::const_iterator found =
      fundamentals_.find(type);
  if (found != fundamentals_.end())
    return found->second;
  TypeNode node;
  node.kind = TYPE_FUNDAMENTAL;
  node.fundamental = type;
  const TypeId id = Add(node);
  fundamentals_[type] = id;
  return id;
}

TypeId TypeTable::FundamentalFromKeywords(
    const std::vector<ETokenType>& keywords)
{
  bool is_unsigned = false;
  bool is_signed = false;
  bool is_short = false;
  unsigned long_count = 0;
  ETokenType named = KW_INT;
  for (std::size_t i = 0; i < keywords.size(); ++i)
  {
    switch (keywords[i])
    {
    case KW_UNSIGNED: is_unsigned = true; break;
    case KW_SIGNED: is_signed = true; break;
    case KW_SHORT: is_short = true; break;
    case KW_LONG: ++long_count; break;
    case KW_INT: break;
    default:
      if (!IsFundamentalTypeKeyword(keywords[i]))
        throw std::runtime_error("invalid fundamental type specifier");
      named = keywords[i];
      break;
    }
  }
  EFundamentalType result = FT_INT;
  switch (named)
  {
  case KW_CHAR:
    result = is_unsigned ? FT_UNSIGNED_CHAR : is_signed ? FT_SIGNED_CHAR : FT_CHAR;
    break;
  case KW_CHAR16_T: result = FT_CHAR16_T; break;
  case KW_CHAR32_T: result = FT_CHAR32_T; break;
  case KW_WCHAR_T: result = FT_WCHAR_T; break;
  case KW_BOOL: result = FT_BOOL; break;
  case KW_FLOAT: result = FT_FLOAT; break;
  case KW_DOUBLE: result = long_count == 0 ? FT_DOUBLE : FT_LONG_DOUBLE; break;
  case KW_VOID: result = FT_VOID; break;
  default:
    if (is_short)
      result = is_unsigned ? FT_UNSIGNED_SHORT_INT : FT_SHORT_INT;
    else if (long_count >= 2)
      result = is_unsigned ? FT_UNSIGNED_LONG_LONG_INT : FT_LONG_LONG_INT;
    else if (long_count == 1)
      result = is_unsigned ? FT_UNSIGNED_LONG_INT : FT_LONG_INT;
    else if (is_unsigned)
      result = FT_UNSIGNED_INT;
    break;
  }
  return Fundamental(result);
}

TypeId TypeTable::Cv(TypeId base, bool is_const, bool is_volatile)
{
  if (base == 0 || (!is_const && !is_volatile))
    return base;
  const TypeNode& node = At(base);
  switch (node.kind)
  {
  case TYPE_REFERENCE: case TYPE_FUNCTION:
    return base;
  case TYPE_ARRAY:
    return Array(Cv(node.base, is_const, is_volatile), node.array_bound);
  case TYPE_CV:
    return Cv(node.base, node.is_const || is_const,
              node.is_volatile || is_volatile);
  default:
    break;
  }
  const std::pair<TypeId, unsigned> key(
      base, (is_const ? 1u : 0u) | (is_volatile ? 2u : 0u));
  const std::map<std::pair<TypeId, unsigned>, TypeId>::const_iterator found =
      cv_.find(key);
  if (found != cv_.end())
    return found->second;
  TypeNode result;
  result.kind = TYPE_CV;
  result.base = base;
  result.is_const = is_const;
  result.is_volatile = is_volatile;
  return cv_[key] = Add(result);
}

TypeId TypeTable::Pointer(TypeId base)
{
  if (base == 0)
    throw std::runtime_error("pointer has no pointee type");
  if (Kind(base) == TYPE_REFERENCE)
    throw std::runtime_error("pointer to reference is ill-formed");
  const std::map<TypeId, TypeId>::const_iterator found = pointers_.find(base);
  if (found != pointers_.end())
    return found->second;
  TypeNode node;
  node.kind = TYPE_POINTER;
  node.base = base;
  return pointers_[base] = Add(node);
}

TypeId TypeTable::Reference(TypeId base, bool lvalue)
{
  if (base == 0)
    throw std::runtime_error("reference has no referred type");
  if (Kind(base) == TYPE_REFERENCE)
    throw std::runtime_error("reference to reference is ill-formed");
  const std::pair<TypeId, bool> key(base, lvalue);
  const std::map<std::pair<TypeId, bool>, TypeId>::const_iterator found =
      references_.find(key);
  if (found != references_.end())
    return found->second;
  TypeNode node;
  node.kind = TYPE_REFERENCE;
  node.base = base;
  node.lvalue_reference = lvalue;
  return references_[key] = Add(node);
}

TypeId TypeTable::Array(TypeId element, std::size_t bound)
{
  if (element == 0 || bound == 0)
    throw std::runtime_error("array bound must be positive");
  const TypeKind kind = Kind(element);
  const TypeNode& unqualified = At(Unqualified(element));
  if (kind == TYPE_FUNCTION || kind == TYPE_REFERENCE ||
      (unqualified.kind == TYPE_FUNDAMENTAL &&
       unqualified.fundamental == FT_VOID))
    throw std::runtime_error("array element type is not an object type");
  const std::pair<TypeId, std::size_t> key(element, bound);
  const std::map<std::pair<TypeId, std::size_t>, TypeId>::const_iterator
      found = arrays_.find(key);
  if (found != arrays_.end())
    return found->second;
  TypeNode node;
  node.kind = TYPE_ARRAY;
  node.base = element;
  node.array_bound = bound;
  return arrays_[key] = Add(node);
}

TypeId TypeTable::Function(TypeId result, const std::vector<TypeId>& parameters,
                           bool variadic)
{
  if (result == 0)
    throw std::runtime_error("function has no return type");
  const TypeKind result_kind = Kind(result);
  if (result_kind == TYPE_ARRAY || result_kind == TYPE_FUNCTION)
    throw std::runtime_error("function returning array or function");
  FunctionKey key;
  key.result = result;
  key.variadic = variadic;
  key.parameters = parameters;
  const std::map<FunctionKey, TypeId>::const_iterator found =
      functions_.find(key);
  if (found != functions_.end())
    return found->second;
  TypeNode node;
  node.kind = TYPE_FUNCTION;
  node.result = result;
  node.parameters = parameters;
  node.variadic = variadic;
  return functions_[key] = Add(node);
}

TypeId TypeTable::Class(EntityId entity, TypeKeyword key,
                        const std::string& name)
{
  TypeNode node;
  node.kind = TYPE_CLASS;
  node.entity = entity;
  node.keyword = key;
  node.name = name;
  return Add(node);
}

TypeId TypeTable::Enum(EntityId entity, bool scoped, TypeId underlying,
                       const std::string& name)
{
  TypeNode node;
  node.kind = TYPE_ENUM;
  node.entity = entity;
  node.scoped = scoped;
  node.base = underlying;
  node.name = name;
  return Add(node);
}

TypeId TypeTable::TemplateParam(TypeKeyword key, const std::string& name)
{
  TypeNode node;
  node.kind = TYPE_TEMPLATE_PARAM;
  node.keyword = key;
  node.name = name;
  return Add(node);
}

const TypeNode& TypeTable::At(TypeId id) const
{
  if (id == 0 || id >= nodes_.size())
    throw std::out_of_range("invalid type id");
  return nodes_[id];
}

TypeKind TypeTable::Kind(TypeId id) const
{
  return At(id).kind;
}

TypeId TypeTable::Unqualified(TypeId id) const
{
  while (id != 0 && At(id).kind == TYPE_CV)
    id = At(id).base;
  return id;
}

static const char* KeywordSpelling(TypeKeyword key)
{
  switch (key)
  {
  case TK_STRUCT: return "struct";
  case TK_CLASS: return "class";
  case TK_UNION: return "union";
  case TK_TYPENAME: return "typename";
  case TK_TEMPLATE_PARAMETER: return "template-parameter";
  case TK_NONE: break;
  }
  return "";
}

void TypeTable::Spell(std::ostream& out, TypeId id) const
{
  const TypeNode& node = At(id);
  switch (node.kind)
  {
  case TYPE_FUNDAMENTAL:
    out << FundamentalTypeToStringMap.at(node.fundamental);
    return;
  case TYPE_CV:
    out << (node.is_const ? "const " : "") << (node.is_volatile ? "volatile " : "");
    Spell(out, node.base);
    return;
  case TYPE_POINTER:
    out << "pointer to ";
    Spell(out, node.base);
    return;
  case TYPE_REFERENCE:
    out << (node.lvalue_reference ? "lvalue" : "rvalue") << "-reference to ";
    Spell(out, node.base);
    return;
  case TYPE_ARRAY:
    out << "array of " << node.array_bound << ' ';
    Spell(out, node.base);
    return;
  case TYPE_FUNCTION:
    out << "function of (";
    for (std::size_t i = 0; i < node.parameters.size(); ++i)
    {
      if (i != 0)
        out << ", ";
      Spell(out, node.parameters[i]);
    }
    if (node.variadic)
      out << (node.parameters.empty() ? "..." : ", ...");
    out << ") returning ";
    Spell(out, node.result);
    return;
  case TYPE_CLASS: case TYPE_TEMPLATE_PARAM:
    out << KeywordSpelling(node.keyword) << ' ' << node.name;
    return;
  case TYPE_ENUM:
    out << (node.scoped ? "enum class " : "enum ") << node.name;
    return;
  case TYPE_INVALID:
    break;
  }
  throw std::runtime_error("cannot spell invalid type");
}

std::string TypeTable::Spell(TypeId id) const
{
  std::ostringstream out;
  Spell(out, id);
  return out.str();
}

std::size_t TypeTable::SizeOf(TypeId id) const
{
  const TypeNode& node = At(id);
  switch (node.kind)
  {
  case TYPE_FUNDAMENTAL:
  {
    const std::size_t size = FundamentalSize(node.fundamental);
    if (size != 0)
      return size;
    break;
  }
  case TYPE_CV: case TYPE_REFERENCE: case TYPE_ENUM:
    return SizeOf(node.base);
  case TYPE_POINTER:
    return 8;
  case TYPE_ARRAY:
    return SizeOf(node.base) * node.array_bound;
  case TYPE_CLASS: case TYPE_FUNCTION: case TYPE_TEMPLATE_PARAM:
  case TYPE_INVALID:
    break;
  }
  throw std::runtime_error("sizeof incomplete or non-object type");
}

std::size_t TypeTable::AlignOf(TypeId id) const
{
  const TypeNode& node = At(id);
  if (node.kind == TYPE_ARRAY || node.kind == TYPE_CV ||
      node.kind == TYPE_REFERENCE)
    return AlignOf(node.base);
  return SizeOf(id);
}
