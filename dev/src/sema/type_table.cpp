#include "sema/type_table.h"

#include <sstream>
#include <stdexcept>

TypeNode::TypeNode()
    : kind(TYPE_INVALID), fundamental(FT_INT), base(0), result(0),
      array_bound(0), is_const(false), is_volatile(false),
      lvalue_reference(true), variadic(false)
{
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

TypeId TypeTable::Derived(const std::string& key, const TypeNode& node)
{
  const std::map<std::string, TypeId>::const_iterator found =
      derived_.find(key);
  if (found != derived_.end())
    return found->second;
  const TypeId id = Add(node);
  derived_[key] = id;
  return id;
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

TypeId TypeTable::Cv(TypeId base, bool is_const, bool is_volatile)
{
  if (base == 0 || (!is_const && !is_volatile))
    return base;
  TypeNode node;
  node.kind = TYPE_CV;
  node.base = base;
  node.is_const = is_const;
  node.is_volatile = is_volatile;
  std::ostringstream key;
  key << "cv:" << base << ':' << (is_const ? 1 : 0) << ':'
      << (is_volatile ? 1 : 0);
  return Derived(key.str(), node);
}

TypeId TypeTable::Pointer(TypeId base)
{
  if (base == 0)
    throw std::runtime_error("pointer has no pointee type");
  if (Kind(base) == TYPE_REFERENCE)
    throw std::runtime_error("pointer to reference is ill-formed");
  TypeNode node;
  node.kind = TYPE_POINTER;
  node.base = base;
  std::ostringstream key;
  key << "ptr:" << base;
  return Derived(key.str(), node);
}

TypeId TypeTable::Reference(TypeId base, bool lvalue)
{
  if (base == 0)
    throw std::runtime_error("reference has no referred type");
  if (Kind(base) == TYPE_REFERENCE)
    throw std::runtime_error("reference to reference is ill-formed");
  TypeNode node;
  node.kind = TYPE_REFERENCE;
  node.base = base;
  node.lvalue_reference = lvalue;
  std::ostringstream key;
  key << "ref:" << base << ':' << (lvalue ? 1 : 0);
  return Derived(key.str(), node);
}

TypeId TypeTable::Array(TypeId element, std::size_t bound)
{
  if (element == 0 || bound == 0)
    throw std::runtime_error("array bound must be positive");
  if (Kind(element) == TYPE_FUNCTION)
    throw std::runtime_error("array of functions is ill-formed");
  TypeNode node;
  node.kind = TYPE_ARRAY;
  node.base = element;
  node.array_bound = bound;
  std::ostringstream key;
  key << "array:" << element << ':' << bound;
  return Derived(key.str(), node);
}

TypeId TypeTable::Function(TypeId result, const std::vector<TypeId>& parameters,
                           bool variadic)
{
  if (result == 0)
    throw std::runtime_error("function has no return type");
  TypeNode node;
  node.kind = TYPE_FUNCTION;
  node.result = result;
  node.parameters = parameters;
  node.variadic = variadic;
  std::ostringstream key;
  key << "function:" << result << ':' << (variadic ? 1 : 0);
  for (std::size_t i = 0; i < parameters.size(); ++i)
    key << ':' << parameters[i];
  return Derived(key.str(), node);
}

TypeId TypeTable::Class(const std::string& name, const std::string& class_key)
{
  TypeNode node;
  node.kind = TYPE_CLASS;
  node.name = name;
  node.class_key = class_key;
  return Add(node);
}

TypeId TypeTable::Enum(const std::string& name, bool scoped)
{
  TypeNode node;
  node.kind = TYPE_ENUM;
  node.name = name;
  node.class_key = scoped ? "class" : "";
  return Add(node);
}

TypeId TypeTable::TemplateParam(const std::string& name,
                               const std::string& keyword)
{
  TypeNode node;
  node.kind = TYPE_TEMPLATE_PARAM;
  node.name = name;
  node.class_key = keyword;
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

std::string TypeTable::SpellParameters(const TypeNode& node) const
{
  std::ostringstream out;
  for (std::size_t i = 0; i < node.parameters.size(); ++i)
  {
    if (i != 0)
      out << ", ";
    out << Spell(node.parameters[i]);
  }
  if (node.variadic)
  {
    if (!node.parameters.empty())
      out << ", ";
    out << "...";
  }
  return out.str();
}

std::string TypeTable::Spell(TypeId id) const
{
  const TypeNode& node = At(id);
  switch (node.kind)
  {
  case TYPE_FUNDAMENTAL:
    switch (node.fundamental)
    {
    case FT_SIGNED_CHAR: return "signed char";
    case FT_SHORT_INT: return "short int";
    case FT_INT: return "int";
    case FT_LONG_INT: return "long int";
    case FT_LONG_LONG_INT: return "long long int";
    case FT_UNSIGNED_CHAR: return "unsigned char";
    case FT_UNSIGNED_SHORT_INT: return "unsigned short int";
    case FT_UNSIGNED_INT: return "unsigned int";
    case FT_UNSIGNED_LONG_INT: return "unsigned long int";
    case FT_UNSIGNED_LONG_LONG_INT: return "unsigned long long int";
    case FT_WCHAR_T: return "wchar_t";
    case FT_CHAR: return "char";
    case FT_CHAR16_T: return "char16_t";
    case FT_CHAR32_T: return "char32_t";
    case FT_BOOL: return "bool";
    case FT_FLOAT: return "float";
    case FT_DOUBLE: return "double";
    case FT_LONG_DOUBLE: return "long double";
    case FT_VOID: return "void";
    case FT_NULLPTR_T: return "nullptr_t";
    }
    break;
  case TYPE_CV:
    return std::string(node.is_const ? "const " : "") +
        (node.is_volatile ? "volatile " : "") + Spell(node.base);
  case TYPE_POINTER:
    return "pointer to " + Spell(node.base);
  case TYPE_REFERENCE:
    return std::string(node.lvalue_reference ? "lvalue" : "rvalue") +
        "-reference to " + Spell(node.base);
  case TYPE_ARRAY:
  {
    std::ostringstream out;
    out << "array of " << node.array_bound << ' ' << Spell(node.base);
    return out.str();
  }
  case TYPE_FUNCTION:
    return "function of (" + SpellParameters(node) + ") returning " +
        Spell(node.result);
  case TYPE_CLASS:
    return node.class_key + " " + node.name;
  case TYPE_ENUM:
    return std::string(node.class_key.empty() ? "enum " : "enum class ") +
        node.name;
  case TYPE_TEMPLATE_PARAM:
    return node.class_key + " " + node.name;
  case TYPE_INVALID:
    break;
  }
  throw std::runtime_error("cannot spell invalid type");
}

std::size_t TypeTable::SizeOf(TypeId id) const
{
  const TypeNode& node = At(id);
  switch (node.kind)
  {
  case TYPE_FUNDAMENTAL:
    switch (node.fundamental)
    {
    case FT_SIGNED_CHAR: case FT_UNSIGNED_CHAR: case FT_CHAR:
    case FT_BOOL: return 1;
    case FT_SHORT_INT: case FT_UNSIGNED_SHORT_INT: return 2;
    case FT_INT: case FT_UNSIGNED_INT: case FT_FLOAT: return 4;
    case FT_LONG_INT: case FT_LONG_LONG_INT: case FT_UNSIGNED_LONG_INT:
    case FT_UNSIGNED_LONG_LONG_INT: case FT_DOUBLE: return 8;
    case FT_LONG_DOUBLE: return 16;
    case FT_WCHAR_T: case FT_CHAR16_T: return 2;
    case FT_CHAR32_T: return 4;
    case FT_VOID: case FT_NULLPTR_T: break;
    }
    break;
  case TYPE_CV: return SizeOf(node.base);
  case TYPE_POINTER: case TYPE_REFERENCE: return 8;
  case TYPE_ARRAY: return SizeOf(node.base) * node.array_bound;
  case TYPE_ENUM: return 4;
  case TYPE_CLASS: case TYPE_FUNCTION: case TYPE_TEMPLATE_PARAM: break;
  case TYPE_INVALID: break;
  }
  throw std::runtime_error("sizeof incomplete or non-object type");
}

std::size_t TypeTable::AlignOf(TypeId id) const
{
  const TypeNode& node = At(id);
  if (node.kind == TYPE_ARRAY)
    return AlignOf(node.base);
  if (node.kind == TYPE_CV)
    return AlignOf(node.base);
  if (node.kind == TYPE_POINTER || node.kind == TYPE_REFERENCE)
    return 8;
  return SizeOf(id);
}
