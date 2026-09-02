#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "sema/qualified_name.h"
#include "sema/type_table.h"

// The PA11 scope tree: scopes own bindings in declaration order and child
// scopes in creation order (both are dump order); a per-scope name index
// serves lookup once a scope grows past a handful of names.  Class and enum
// entities carry the facts shared by all declarations of one type: the member
// scope and whether a definition has been seen.  Every scope reachable through
// a binding is derived on demand from the binding's kind and type, so no
// binding holds a copy of a scope that may not exist yet.
typedef std::size_t ScopeId;
typedef std::size_t BindingId;
typedef std::size_t ClassEntityId;
typedef std::size_t EnumEntityId;
typedef std::size_t FunctionEntityId;

enum ScopeKind
{
  SCOPE_NAMESPACE,
  SCOPE_CLASS,
  SCOPE_FUNCTION,
  SCOPE_BLOCK,
  SCOPE_ENUM,
  SCOPE_TEMPLATE_PARAMETERS
};

enum BindingKind
{
  BINDING_TYPE,
  BINDING_TYPE_ALIAS,
  BINDING_VARIABLE,
  BINDING_FUNCTION,
  BINDING_NAMESPACE,
  BINDING_ENUMERATOR,
  BINDING_PARAMETER
};

enum LookupFilter
{
  LOOKUP_TYPES = 1u << 0,
  LOOKUP_VALUES = 1u << 1,
  LOOKUP_FUNCTIONS = 1u << 2,
  LOOKUP_NAMESPACES = 1u << 3,
  LOOKUP_ENUMERATORS = 1u << 4,
  LOOKUP_ANY = LOOKUP_TYPES | LOOKUP_VALUES | LOOKUP_FUNCTIONS |
      LOOKUP_NAMESPACES | LOOKUP_ENUMERATORS,
  // 3.4.3p1: the name before `::` ignores objects, functions and enumerators.
  LOOKUP_QUALIFIER = LOOKUP_TYPES | LOOKUP_NAMESPACES
};

struct Binding
{
  // Printed spelling.  A qualified out-of-class enum definition keeps its
  // qualified spelling as the dump requires; lookup never reads that form.
  std::string name;
  BindingKind kind;
  TypeId type;
  ScopeId scope; // owning declaration scope
  ScopeId namespace_scope; // BINDING_NAMESPACE: the nominated namespace
  FunctionEntityId function; // BINDING_FUNCTION: canonical function entity
  bool has_const_value;
  long long const_value;

  Binding();
};

struct Scope
{
  ScopeKind kind;
  std::string name; // printed after the scope kind; empty for blocks and template scopes
  ScopeId parent;
  bool inline_namespace;
  bool unnamed_namespace;
  std::vector<BindingId> bindings;
  std::vector<ScopeId> children;
  struct UsingDirective
  {
    ScopeId nominated;
    ScopeId apply_at;

    UsingDirective(ScopeId nominated = 0, ScopeId apply_at = 0)
        : nominated(nominated), apply_at(apply_at) {}
  };
  std::vector<UsingDirective> using_directives;
  // name -> bindings of that name in declaration order; built once the scope
  // holds more than kSmallScope bindings, empty before that.
  std::unordered_map<std::string, std::vector<BindingId> > index;

  Scope();
};

struct ClassEntity
{
  ScopeId class_scope; // 0 until defined
  bool is_union;
  bool defined;

  ClassEntity();
};

struct EnumEntity
{
  // Scope holding the enumerators: the enum scope of a scoped enumeration or
  // the declaring scope of a defined unscoped one; 0 before that exists.
  ScopeId enum_scope;
  TypeId underlying;
  bool scoped;
  bool defined;

  EnumEntity();
};

struct FunctionEntity
{
  ScopeId scope; // declaration scope, not the function body's block scope
  std::string name;
  TypeId type; // canonical function type with adjusted parameters
  bool defined;

  FunctionEntity();
};

class SemaModel
{
public:
  explicit SemaModel(TypeTable& types);

  ScopeId GlobalScope() const;
  ScopeId CreateScope(ScopeKind kind, const std::string& name,
                      ScopeId parent, bool inline_namespace = false);
  Scope& ScopeAt(ScopeId id);
  const Scope& ScopeAt(ScopeId id) const;

  BindingId AddBinding(ScopeId scope, const std::string& name,
                       BindingKind kind, TypeId type = 0,
                       ScopeId namespace_scope = 0);
  void AddUsingDirective(ScopeId scope, ScopeId target);
  Binding& BindingAt(BindingId id);
  const Binding& BindingAt(BindingId id) const;

  // Latest binding of `name` declared directly in `scope` that passes the
  // filter; 0 when none.
  BindingId DirectBinding(ScopeId scope, const std::string& name,
                          unsigned filter = LOOKUP_ANY) const;
  // 3.4.1 unqualified lookup restricted to the filter (elaborated lookup
  // uses LOOKUP_TYPES, the qualifier position LOOKUP_QUALIFIER).
  BindingId LookupUnqualified(ScopeId scope, const std::string& name,
                              unsigned filter) const;
  // Overload-aware forms.  The result is the complete set from the first
  // lookup level that contains a match; a non-function in that set is kept so
  // callers can enforce ordinary-name hiding before attempting a call.
  void LookupSet(ScopeId scope, const std::string& name, unsigned filter,
                 std::vector<BindingId>& result) const;
  void LookupQualifiedSet(ScopeId scope, const QualifiedName& name,
                          unsigned filter,
                          std::vector<BindingId>& result) const;
  // Ordinary lookup of a type name: a later object, function, enumerator or
  // parameter of the same name hides the type (3.3.10p2).
  BindingId LookupTypeName(ScopeId scope, const std::string& name) const;
  // 3.4.3 qualified lookup through namespaces, class scopes and enums.
  BindingId LookupQualified(ScopeId scope, const QualifiedName& name,
                            unsigned filter) const;
  // Dispatches on the name form: global, qualified, or unqualified.
  BindingId Lookup(ScopeId scope, const QualifiedName& name,
                   unsigned filter) const;
  // Scope a binding denotes when used as a qualifier: the nominated
  // namespace, the member scope of a class, or the enumerator scope of an
  // enumeration (also through a type alias).  False when it has none yet.
  bool NominatedScope(BindingId binding, ScopeId& scope) const;
  // Member scope of a class type or enumerator scope of an enum type.
  bool ScopeOfType(TypeId type, ScopeId& scope) const;

  ClassEntityId CreateClass(bool is_union);
  ClassEntity& ClassAt(ClassEntityId id);
  const ClassEntity& ClassAt(ClassEntityId id) const;
  EnumEntityId CreateEnum(bool scoped, TypeId underlying);
  EnumEntity& EnumAt(EnumEntityId id);
  const EnumEntity& EnumAt(EnumEntityId id) const;
  FunctionEntityId CreateFunction(ScopeId scope, const std::string& name,
                                  TypeId type);
  FunctionEntity& FunctionAt(FunctionEntityId id);
  const FunctionEntity& FunctionAt(FunctionEntityId id) const;
  TypeTable& Types();
  const TypeTable& Types() const;

private:
  static const std::size_t kSmallScope = 8;

  static bool Matches(const Binding& binding, unsigned filter);
  BindingId SearchScope(ScopeId scope, const std::string& name,
                        unsigned filter, std::vector<ScopeId>& visited) const;
  BindingId SearchNamespace(ScopeId scope, const std::string& name,
                            unsigned filter, std::vector<ScopeId>& visited) const;
  BindingId SearchUsingDirectives(ScopeId scope, const std::string& name,
                                  unsigned filter,
                                  std::vector<ScopeId>& visited) const;
  void CollectDirect(ScopeId scope, const std::string& name,
                     unsigned filter, std::vector<BindingId>& result) const;
  void CollectNamespace(ScopeId scope, const std::string& name,
                        unsigned filter, std::vector<ScopeId>& visited,
                        std::vector<BindingId>& result) const;
  static void AppendUnique(std::vector<BindingId>& result,
                           BindingId binding);
  BindingId SearchMember(ScopeId scope, EntityId unscoped_enum,
                         const std::string& name, unsigned filter) const;

  TypeTable& types_;
  std::vector<Scope> scopes_;
  std::vector<Binding> bindings_;
  std::vector<ClassEntity> classes_;
  std::vector<EnumEntity> enums_;
  std::vector<FunctionEntity> functions_;
};
