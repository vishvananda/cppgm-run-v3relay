#pragma once

#include <cstddef>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

#include "posttoken_types.h"

typedef std::size_t TypeId;

enum TypeKind
{
  TYPE_INVALID,
  TYPE_FUNDAMENTAL,
  TYPE_CV,
  TYPE_POINTER,
  TYPE_REFERENCE,
  TYPE_ARRAY,
  TYPE_FUNCTION,
  TYPE_CLASS,
  TYPE_ENUM,
  TYPE_TEMPLATE_PARAM
};
struct TypeNode
{
  TypeKind kind;
  EFundamentalType fundamental;
  TypeId base;
  TypeId result;
  std::vector<TypeId> parameters;
  std::size_t array_bound;
  bool is_const;
  bool is_volatile;
  bool lvalue_reference;
  bool variadic;
  std::string name;
  std::string class_key;

  TypeNode();
};

class TypeTable
{
public:
  TypeTable();

  TypeId Fundamental(EFundamentalType type);
  TypeId Cv(TypeId base, bool is_const, bool is_volatile = false);
  TypeId Pointer(TypeId base);
  TypeId Reference(TypeId base, bool lvalue = true);
  TypeId Array(TypeId element, std::size_t bound);
  TypeId Function(TypeId result, const std::vector<TypeId>& parameters,
                  bool variadic = false);
  TypeId Class(const std::string& name, const std::string& class_key);
  TypeId Enum(const std::string& name, bool scoped, TypeId underlying = 0);
  TypeId TemplateParam(const std::string& name, const std::string& keyword);

  const TypeNode& At(TypeId id) const;
  TypeKind Kind(TypeId id) const;
  std::string Spell(TypeId id) const;
  std::size_t SizeOf(TypeId id) const;
  std::size_t AlignOf(TypeId id) const;

private:
  TypeId Add(const TypeNode& node);
  TypeId Derived(const std::string& key, const TypeNode& node);
  std::string SpellParameters(const TypeNode& node) const;

  std::vector<TypeNode> nodes_;
  std::map<EFundamentalType, TypeId> fundamentals_;
  std::map<std::string, TypeId> derived_;
};
