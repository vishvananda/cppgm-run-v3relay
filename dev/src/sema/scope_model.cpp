#include "sema/scope_model.h"

#include <sstream>
#include <stdexcept>

Binding::Binding()
    : kind(BINDING_VARIABLE), type(0), target_scope(0), class_entity(0),
      print(true)
{
}

Scope::Scope()
    : kind(SCOPE_NAMESPACE), parent(0), inline_namespace(false)
{
}

ClassEntity::ClassEntity()
    : parent_scope(0), current_type(0), class_scope(0), defined(false)
{
}

EnumEntity::EnumEntity()
    : parent_scope(0), type(0)
{
}

SemaModel::SemaModel(TypeTable& types)
    : types_(types), scopes_(1), bindings_(1), classes_(1), enums_(1)
{
  scopes_[0].kind = SCOPE_NAMESPACE;
  scopes_[0].name = "<global>";
}

ScopeId SemaModel::GlobalScope() const
{
  return 0;
}

ScopeId SemaModel::CreateScope(ScopeKind kind, const std::string& name,
                               ScopeId parent, bool inline_namespace)
{
  if (parent >= scopes_.size())
    throw std::out_of_range("invalid scope parent");
  Scope scope;
  scope.kind = kind;
  scope.name = name;
  scope.parent = parent;
  scope.inline_namespace = inline_namespace;
  scopes_.push_back(scope);
  const ScopeId id = scopes_.size() - 1;
  scopes_[parent].children.push_back(id);
  return id;
}

Scope& SemaModel::ScopeAt(ScopeId id)
{
  if (id >= scopes_.size())
    throw std::out_of_range("invalid scope id");
  return scopes_[id];
}

const Scope& SemaModel::ScopeAt(ScopeId id) const
{
  if (id >= scopes_.size())
    throw std::out_of_range("invalid scope id");
  return scopes_[id];
}

static bool IsNamespaceBinding(BindingKind kind)
{
  return kind == BINDING_NAMESPACE;
}

static bool IsOrdinaryObjectBinding(BindingKind kind)
{
  return kind == BINDING_VARIABLE || kind == BINDING_FUNCTION ||
      kind == BINDING_ENUMERATOR || kind == BINDING_PARAMETER;
}

BindingId SemaModel::AddBinding(ScopeId scope, const std::string& name,
                                BindingKind kind, TypeId type,
                                ScopeId target_scope, bool print,
                                ClassEntityId class_entity)
{
  if (scope >= scopes_.size())
    throw std::out_of_range("invalid binding scope");
  if (name.empty() && kind != BINDING_PARAMETER)
    throw std::runtime_error("declaration has no name");

  const BindingId previous = DirectBinding(scope, name);
  if (previous != 0)
  {
    const BindingKind old_kind = bindings_[previous].kind;
    if ((IsNamespaceBinding(kind) && !IsNamespaceBinding(old_kind)) ||
        (!IsNamespaceBinding(kind) && IsNamespaceBinding(old_kind)))
      throw std::runtime_error("namespace and ordinary name conflict");
  }

  Binding binding;
  binding.name = name;
  binding.kind = kind;
  binding.type = type;
  binding.target_scope = target_scope;
  binding.class_entity = class_entity;
  binding.print = print;
  bindings_.push_back(binding);
  const BindingId id = bindings_.size() - 1;
  scopes_[scope].bindings.push_back(id);
  return id;
}

void SemaModel::AddUsingDirective(ScopeId scope, ScopeId target)
{
  if (scope >= scopes_.size() || target >= scopes_.size())
    throw std::out_of_range("invalid using-directive scope");
  scopes_[scope].using_directives.push_back(target);
}

Binding& SemaModel::BindingAt(BindingId id)
{
  if (id == 0 || id >= bindings_.size())
    throw std::out_of_range("invalid binding id");
  return bindings_[id];
}

const Binding& SemaModel::BindingAt(BindingId id) const
{
  if (id == 0 || id >= bindings_.size())
    throw std::out_of_range("invalid binding id");
  return bindings_[id];
}

bool SemaModel::Matches(const Binding& binding, unsigned filter) const
{
  switch (binding.kind)
  {
  case BINDING_TYPE: case BINDING_TYPE_ALIAS:
    return (filter & LOOKUP_TYPES) != 0;
  case BINDING_VARIABLE: case BINDING_PARAMETER:
    return (filter & LOOKUP_VALUES) != 0;
  case BINDING_FUNCTION:
    return (filter & (LOOKUP_VALUES | LOOKUP_FUNCTIONS)) != 0;
  case BINDING_NAMESPACE:
    return (filter & LOOKUP_NAMESPACES) != 0;
  case BINDING_ENUMERATOR:
    return (filter & (LOOKUP_VALUES | LOOKUP_ENUMERATORS)) != 0;
  }
  return false;
}

BindingId SemaModel::SearchNamespace(ScopeId scope, const std::string& name,
                                     unsigned filter,
                                     std::vector<ScopeId>& visited) const
{
  for (std::size_t i = 0; i < visited.size(); ++i)
    if (visited[i] == scope)
      return 0;
  visited.push_back(scope);

  const BindingId direct = DirectBinding(scope, name, filter);
  if (direct != 0)
    return direct;

  const Scope& value = scopes_[scope];
  for (std::size_t i = 0; i < value.children.size(); ++i)
  {
    const ScopeId child = value.children[i];
    if (scopes_[child].kind == SCOPE_NAMESPACE &&
        scopes_[child].inline_namespace)
    {
      const BindingId found = SearchNamespace(child, name, filter, visited);
      if (found != 0)
        return found;
    }
  }
  for (std::size_t i = value.using_directives.size(); i != 0; --i)
  {
    const BindingId found = SearchNamespace(value.using_directives[i - 1],
                                             name, filter, visited);
    if (found != 0)
      return found;
  }
  return 0;
}

BindingId SemaModel::SearchScope(ScopeId scope, const std::string& name,
                                 unsigned filter,
                                 std::vector<ScopeId>& visited) const
{
  const BindingId direct = DirectBinding(scope, name, filter);
  if (direct != 0)
    return direct;
  if (scopes_[scope].kind == SCOPE_NAMESPACE)
    return SearchNamespace(scope, name, filter, visited);
  for (std::size_t i = scopes_[scope].using_directives.size(); i != 0; --i)
  {
    const BindingId found = SearchNamespace(
        scopes_[scope].using_directives[i - 1], name, filter, visited);
    if (found != 0)
      return found;
  }
  return 0;
}

BindingId SemaModel::LookupUnqualified(ScopeId scope, const std::string& name,
                                       unsigned filter) const
{
  std::vector<ScopeId> visited;
  for (ScopeId current = scope;; current = scopes_[current].parent)
  {
    const BindingId found = SearchScope(current, name, filter, visited);
    if (found != 0)
      return found;
    if (current == GlobalScope())
      break;
  }
  return 0;
}

BindingId SemaModel::LookupTypeName(ScopeId scope, const std::string& name) const
{
  std::vector<ScopeId> visited;
  for (ScopeId current = scope;; current = scopes_[current].parent)
  {
    const Scope& value = scopes_[current];
    for (std::size_t i = value.bindings.size(); i != 0; --i)
    {
      const Binding& binding = bindings_[value.bindings[i - 1]];
      if (binding.name != name)
        continue;
      if (binding.kind == BINDING_TYPE || binding.kind == BINDING_TYPE_ALIAS)
        return value.bindings[i - 1];
      if (IsOrdinaryObjectBinding(binding.kind))
        return 0;
    }
    const BindingId using_found = SearchScope(current, name, LOOKUP_TYPES,
                                              visited);
    if (using_found != 0)
      return using_found;
    if (current == GlobalScope())
      break;
  }
  return 0;
}

BindingId SemaModel::DirectBinding(ScopeId scope, const std::string& name,
                                   unsigned filter) const
{
  if (scope >= scopes_.size())
    return 0;
  const std::vector<BindingId>& values = scopes_[scope].bindings;
  for (std::size_t i = values.size(); i != 0; --i)
  {
    const Binding& binding = bindings_[values[i - 1]];
    if (binding.name == name && Matches(binding, filter))
      return values[i - 1];
  }
  return 0;
}

std::string SemaModel::EntityKey(ScopeId scope, const std::string& name) const
{
  std::ostringstream out;
  out << scope << ':' << name;
  return out.str();
}

ClassEntityId SemaModel::GetOrCreateClass(ScopeId parent,
                                          const std::string& name)
{
  const std::string key = EntityKey(parent, name);
  const std::map<std::string, ClassEntityId>::const_iterator found =
      class_by_name_.find(key);
  if (found != class_by_name_.end())
    return found->second;
  ClassEntity entity;
  entity.parent_scope = parent;
  entity.name = name;
  classes_.push_back(entity);
  const ClassEntityId id = classes_.size() - 1;
  class_by_name_[key] = id;
  return id;
}

ClassEntity& SemaModel::ClassAt(ClassEntityId id)
{
  if (id == 0 || id >= classes_.size())
    throw std::out_of_range("invalid class entity");
  return classes_[id];
}

const ClassEntity& SemaModel::ClassAt(ClassEntityId id) const
{
  if (id == 0 || id >= classes_.size())
    throw std::out_of_range("invalid class entity");
  return classes_[id];
}

ScopeId SemaModel::TargetScopeForType(TypeId type) const
{
  for (std::size_t i = 1; i < classes_.size(); ++i)
    if (classes_[i].current_type == type && classes_[i].class_scope != 0)
      return classes_[i].class_scope;
  return 0;
}

EnumEntityId SemaModel::GetOrCreateEnum(ScopeId parent, const std::string& name)
{
  const std::string key = EntityKey(parent, name);
  const std::map<std::string, EnumEntityId>::const_iterator found =
      enum_by_name_.find(key);
  if (found != enum_by_name_.end())
    return found->second;
  EnumEntity entity;
  entity.parent_scope = parent;
  entity.name = name;
  enums_.push_back(entity);
  const EnumEntityId id = enums_.size() - 1;
  enum_by_name_[key] = id;
  return id;
}

EnumEntity& SemaModel::EnumAt(EnumEntityId id)
{
  if (id == 0 || id >= enums_.size())
    throw std::out_of_range("invalid enum entity");
  return enums_[id];
}

const EnumEntity& SemaModel::EnumAt(EnumEntityId id) const
{
  if (id == 0 || id >= enums_.size())
    throw std::out_of_range("invalid enum entity");
  return enums_[id];
}

TypeTable& SemaModel::Types()
{
  return types_;
}

const TypeTable& SemaModel::Types() const
{
  return types_;
}

BindingId SemaModel::LookupQualified(
    ScopeId scope, const std::vector<std::string>& components,
    unsigned filter) const
{
  if (components.empty())
    return 0;
  BindingId binding = LookupUnqualified(scope, components[0],
                                       LOOKUP_QUALIFIER);
  if (binding == 0)
    return 0;
  std::vector<ScopeId> visited;
  for (std::size_t i = 1; i < components.size(); ++i)
  {
    const Binding& prefix = bindings_[binding];
    if (prefix.target_scope == 0)
      return 0;
    binding = SearchNamespace(prefix.target_scope, components[i],
                              i + 1 == components.size() ? filter :
                              LOOKUP_QUALIFIER, visited);
    if (binding == 0)
      return 0;
  }
  return Matches(bindings_[binding], filter) ? binding : 0;
}
