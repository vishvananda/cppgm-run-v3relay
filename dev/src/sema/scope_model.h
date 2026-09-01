#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "sema/type_table.h"

typedef std::size_t ScopeId;
typedef std::size_t BindingId;
typedef std::size_t ClassEntityId;
typedef std::size_t EnumEntityId;

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
  LOOKUP_QUALIFIER = LOOKUP_TYPES | LOOKUP_NAMESPACES
};

struct Binding
{
  std::string name;
  BindingKind kind;
  TypeId type;
  ScopeId target_scope;
  ClassEntityId class_entity;
  bool print;

  Binding();
};

struct Scope
{
  ScopeKind kind;
  std::string name;
  ScopeId parent;
  bool inline_namespace;
  std::vector<BindingId> bindings;
  std::vector<ScopeId> children;
  std::vector<ScopeId> using_directives;

  Scope();
};

struct ClassEntity
{
  ScopeId parent_scope;
  std::string name;
  std::string class_key;
  TypeId current_type;
  ScopeId class_scope;
  bool defined;

  ClassEntity();
};

struct EnumEntity
{
  ScopeId parent_scope;
  std::string name;
  TypeId type;

  EnumEntity();
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
                       ScopeId target_scope = 0, bool print = true,
                       ClassEntityId class_entity = 0);
  void AddUsingDirective(ScopeId scope, ScopeId target);
  Binding& BindingAt(BindingId id);
  const Binding& BindingAt(BindingId id) const;

  BindingId LookupUnqualified(ScopeId scope, const std::string& name,
                              unsigned filter) const;
  BindingId LookupTypeName(ScopeId scope, const std::string& name) const;
  BindingId LookupQualified(ScopeId scope,
                            const std::vector<std::string>& components,
                            unsigned filter) const;

  BindingId DirectBinding(ScopeId scope, const std::string& name,
                          unsigned filter = LOOKUP_ANY) const;
  ClassEntityId GetOrCreateClass(ScopeId parent, const std::string& name);
  ClassEntity& ClassAt(ClassEntityId id);
  const ClassEntity& ClassAt(ClassEntityId id) const;
  ScopeId TargetScopeForType(TypeId type) const;
  EnumEntityId GetOrCreateEnum(ScopeId parent, const std::string& name);
  EnumEntity& EnumAt(EnumEntityId id);
  const EnumEntity& EnumAt(EnumEntityId id) const;
  TypeTable& Types();
  const TypeTable& Types() const;

private:
  bool Matches(const Binding& binding, unsigned filter) const;
  BindingId SearchScope(ScopeId scope, const std::string& name,
                        unsigned filter, std::vector<ScopeId>& visited) const;
  BindingId SearchNamespace(ScopeId scope, const std::string& name,
                            unsigned filter, std::vector<ScopeId>& visited) const;
  std::string EntityKey(ScopeId scope, const std::string& name) const;

  TypeTable& types_;
  std::vector<Scope> scopes_;
  std::vector<Binding> bindings_;
  std::vector<ClassEntity> classes_;
  std::vector<EnumEntity> enums_;
  std::map<std::string, ClassEntityId> class_by_name_;
  std::map<std::string, EnumEntityId> enum_by_name_;
};
