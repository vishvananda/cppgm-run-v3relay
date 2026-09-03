#pragma once

#include <cstddef>
#include <deque>
#include <iosfwd>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "posttoken_types.h"

// Canonical PA11 types.  Derived types (cv, pointer, reference, array,
// function) are interned on typed keys so equal types share one id and
// equality is `==`.  Class, enum, and template-parameter types carry the
// entity they denote plus the spelling of the declaration that introduced
// them; the dump prints every declaration with its own spelling, so two type
// ids may name one entity and semantic identity is the entity, not the id.
typedef std::size_t TypeId;   // 0 is the null type
typedef std::size_t EntityId; // class or enum entity in the SemaModel

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
  TYPE_TEMPLATE_PARAM,
  TYPE_MEMBER_POINTER
};

// Keyword spelled before a named type: the class-key of a class type or the
// declaring keyword of a template type parameter.
enum TypeKeyword
{
  TK_NONE,
  TK_STRUCT,
  TK_CLASS,
  TK_UNION,
  TK_TYPENAME,
  TK_TEMPLATE_PARAMETER
};

struct TypeNode
{
  TypeKind kind;
  EFundamentalType fundamental;   // TYPE_FUNDAMENTAL
  TypeId base;                    // cv/pointer/reference/array operand, enum underlying type
  TypeId result;                  // function return type
  std::vector<TypeId> parameters; // function parameter types
  std::size_t array_bound;
  bool is_const;
  bool is_volatile;
  bool lvalue_reference;
  bool variadic;
  bool function_const;            // TYPE_FUNCTION: member-function cv
  bool scoped;                    // TYPE_ENUM
  TypeKeyword keyword;            // TYPE_CLASS, TYPE_TEMPLATE_PARAM
  EntityId entity;                // TYPE_CLASS, TYPE_ENUM
  TypeId member_class;             // TYPE_MEMBER_POINTER
  std::string name;               // declared spelling of a named type

  TypeNode();
};

// Fundamental-type facts for the Linux x86-64 target.
bool IsFundamentalTypeKeyword(ETokenType token);
bool FundamentalIsIntegral(EFundamentalType type);
bool FundamentalIsUnsigned(EFundamentalType type);
std::size_t FundamentalSize(EFundamentalType type); // 0 for void and nullptr_t

class TypeTable
{
public:
  TypeTable();

  TypeId Fundamental(EFundamentalType type);
  // 3.9.1 simple-type-specifier combination such as {unsigned, long, long}.
  TypeId FundamentalFromKeywords(const std::vector<ETokenType>& keywords);
  // Adds qualifiers to a type: merges with an existing cv layer, applies to
  // the element type of an array (3.9.3p5), and is ignored on reference and
  // function types (8.3.2p1, 8.3.5p6).
  TypeId Cv(TypeId base, bool is_const, bool is_volatile = false);
  TypeId Pointer(TypeId base);
  TypeId Reference(TypeId base, bool lvalue = true);
  TypeId Array(TypeId element, std::size_t bound);
  TypeId IncompleteArray(TypeId element);
  TypeId Function(TypeId result, const std::vector<TypeId>& parameters,
                  bool variadic = false, bool function_const = false);
  TypeId Class(EntityId entity, TypeKeyword key, const std::string& name);
  TypeId Enum(EntityId entity, bool scoped, TypeId underlying,
              const std::string& name);
  TypeId TemplateParam(TypeKeyword key, const std::string& name);
  TypeId MemberPointer(TypeId member_class, TypeId member,
                       bool member_const = false);

  const TypeNode& At(TypeId id) const;
  TypeKind Kind(TypeId id) const;
  TypeId Unqualified(TypeId id) const; // strips top-level cv
  // Standard conversion helpers shared by expression analysis and overload
  // selection.  These intentionally operate on interned type identities so
  // callers never need to reconstruct a derived type by spelling it.
  TypeId Decay(TypeId id);
  TypeId AdjustParameter(TypeId id);
  TypeId Referent(TypeId id) const;
  bool IsIntegral(TypeId id) const;
  bool IsArithmetic(TypeId id) const;
  bool IsScalar(TypeId id) const;
  bool IsPointer(TypeId id) const;
  bool IsNullPointerType(TypeId id) const;
  TypeId Promote(TypeId id);
  TypeId UsualArithmetic(TypeId left, TypeId right);
  TypeId CompositePointer(TypeId left, TypeId right, bool& ok);
  void Spell(std::ostream& out, TypeId id) const;
  std::string Spell(TypeId id) const;
  std::size_t SizeOf(TypeId id) const;
  std::size_t AlignOf(TypeId id) const;
  // Class layout is completed by the scope builder once all direct members
  // and bases have been collected.  TypeTable owns the immutable lookup used
  // by sizeof, arrays and lowering thereafter.
  void SetClassLayout(EntityId entity, std::size_t size,
                      std::size_t alignment);

private:
  struct FunctionKey
  {
    TypeId result;
    bool variadic;
    bool function_const;
    std::vector<TypeId> parameters;
    bool operator<(const FunctionKey& other) const;
  };

  TypeId Add(const TypeNode& node);

  // A deque so that a TypeNode reference from At() stays valid while later
  // types are interned: callers hold one across conversions and lowering.
  std::deque<TypeNode> nodes_;
  std::map<EFundamentalType, TypeId> fundamentals_;
  std::map<std::pair<TypeId, unsigned>, TypeId> cv_;
  std::map<TypeId, TypeId> pointers_;
  std::map<std::pair<TypeId, bool>, TypeId> references_;
  std::map<std::pair<TypeId, std::size_t>, TypeId> arrays_;
  std::map<FunctionKey, TypeId> functions_;
  std::map<std::pair<EntityId, std::pair<TypeKeyword, std::string> >, TypeId>
      class_types_;
  std::map<std::pair<TypeId, TypeId>, TypeId> member_pointers_;
  std::map<EntityId, std::pair<std::size_t, std::size_t> > class_layouts_;
};
