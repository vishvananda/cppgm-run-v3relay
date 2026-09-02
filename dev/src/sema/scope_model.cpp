#include "sema/scope_model.h"

#include <algorithm>
#include <stdexcept>

Binding::Binding()
    : kind(BINDING_VARIABLE), type(0), namespace_scope(0),
      has_const_value(false), const_value(0)
{
}

Scope::Scope()
    : kind(SCOPE_NAMESPACE), parent(0), inline_namespace(false)
{
}

ClassEntity::ClassEntity()
    : class_scope(0), is_union(false), defined(false)
{
}

EnumEntity::EnumEntity()
    : enum_scope(0), underlying(0), scoped(false), defined(false)
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

BindingId SemaModel::AddBinding(ScopeId scope, const std::string& name,
                                BindingKind kind, TypeId type,
                                ScopeId namespace_scope)
{
  if (scope >= scopes_.size())
    throw std::out_of_range("invalid binding scope");
  if (name.empty() && kind != BINDING_PARAMETER)
    throw std::runtime_error("declaration has no name");

  // 3.3.1p4: a namespace name and any other entity of that name cannot share
  // a scope; every other redeclaration is a fresh binding (the dump prints
  // each declaration).
  const BindingId previous = DirectBinding(scope, name);
  if (previous != 0 &&
      (bindings_[previous].kind == BINDING_NAMESPACE) !=
          (kind == BINDING_NAMESPACE))
    throw std::runtime_error("namespace and ordinary name conflict");

  Binding binding;
  binding.name = name;
  binding.kind = kind;
  binding.type = type;
  binding.namespace_scope = namespace_scope;
  bindings_.push_back(binding);
  const BindingId id = bindings_.size() - 1;

  Scope& owner = scopes_[scope];
  owner.bindings.push_back(id);
  if (!owner.index.empty())
    owner.index[name].push_back(id);
  else if (owner.bindings.size() > kSmallScope)
    for (std::size_t i = 0; i < owner.bindings.size(); ++i)
      owner.index[bindings_[owner.bindings[i]].name].push_back(
          owner.bindings[i]);
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

bool SemaModel::Matches(const Binding& binding, unsigned filter)
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

BindingId SemaModel::DirectBinding(ScopeId scope, const std::string& name,
                                   unsigned filter) const
{
  if (scope >= scopes_.size())
    return 0;
  const Scope& owner = scopes_[scope];
  const std::vector<BindingId>* candidates = &owner.bindings;
  if (!owner.index.empty())
  {
    const std::unordered_map<std::string, std::vector<BindingId> >::
        const_iterator found = owner.index.find(name);
    if (found == owner.index.end())
      return 0;
    candidates = &found->second;
  }
  for (std::size_t i = candidates->size(); i != 0; --i)
  {
    const Binding& binding = bindings_[(*candidates)[i - 1]];
    if (binding.name == name && Matches(binding, filter))
      return (*candidates)[i - 1];
  }
  return 0;
}

BindingId SemaModel::SearchUsingDirectives(ScopeId scope,
                                           const std::string& name,
                                           unsigned filter,
                                           std::vector<ScopeId>& visited) const
{
  const std::vector<ScopeId>& directives = scopes_[scope].using_directives;
  for (std::size_t i = directives.size(); i != 0; --i)
  {
    const BindingId found = SearchNamespace(directives[i - 1], name, filter,
                                            visited);
    if (found != 0)
      return found;
  }
  return 0;
}

// 3.4.3.2p2: a namespace's own names, then its inline namespaces (7.3.1p8),
// then the namespaces it nominates; each namespace is searched once.
BindingId SemaModel::SearchNamespace(ScopeId scope, const std::string& name,
                                     unsigned filter,
                                     std::vector<ScopeId>& visited) const
{
  if (std::find(visited.begin(), visited.end(), scope) != visited.end())
    return 0;
  visited.push_back(scope);

  const BindingId direct = DirectBinding(scope, name, filter);
  if (direct != 0)
    return direct;
  const Scope& value = scopes_[scope];
  for (std::size_t i = 0; i < value.children.size(); ++i)
  {
    const Scope& child = scopes_[value.children[i]];
    if (child.kind == SCOPE_NAMESPACE && child.inline_namespace)
    {
      const BindingId found = SearchNamespace(value.children[i], name, filter,
                                              visited);
      if (found != 0)
        return found;
    }
  }
  return SearchUsingDirectives(scope, name, filter, visited);
}

BindingId SemaModel::SearchScope(ScopeId scope, const std::string& name,
                                 unsigned filter,
                                 std::vector<ScopeId>& visited) const
{
  if (scopes_[scope].kind == SCOPE_NAMESPACE)
    return SearchNamespace(scope, name, filter, visited);
  const BindingId direct = DirectBinding(scope, name, filter);
  if (direct != 0)
    return direct;
  return SearchUsingDirectives(scope, name, filter, visited);
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
    const BindingId direct = DirectBinding(current, name);
    if (direct != 0)
    {
      const BindingKind kind = bindings_[direct].kind;
      if (kind == BINDING_TYPE || kind == BINDING_TYPE_ALIAS)
        return direct;
      if (kind != BINDING_NAMESPACE)
        return 0;
    }
    const BindingId found = SearchScope(current, name, LOOKUP_TYPES, visited);
    if (found != 0)
      return found;
    if (current == GlobalScope())
      break;
  }
  return 0;
}

bool SemaModel::ScopeOfType(TypeId type, ScopeId& scope) const
{
  if (type == 0)
    return false;
  const TypeNode& node = types_.At(types_.Unqualified(type));
  if (node.kind == TYPE_CLASS && classes_[node.entity].defined)
  {
    scope = classes_[node.entity].class_scope;
    return true;
  }
  if (node.kind == TYPE_ENUM)
  {
    // A scoped enumeration owns its scope from its first declaration; the
    // enumerators of an unscoped one live in its declaring scope once defined.
    const EnumEntity& entity = enums_[node.entity];
    if (entity.scoped ? entity.enum_scope == 0 : !entity.defined)
      return false;
    scope = entity.enum_scope;
    return true;
  }
  return false;
}

bool SemaModel::NominatedScope(BindingId binding, ScopeId& scope) const
{
  const Binding& value = BindingAt(binding);
  if (value.kind == BINDING_NAMESPACE)
  {
    scope = value.namespace_scope;
    return true;
  }
  if (value.kind == BINDING_TYPE || value.kind == BINDING_TYPE_ALIAS)
    return ScopeOfType(value.type, scope);
  return false;
}

// A member of a class or enum scope, or an enumerator of an unscoped
// enumeration found among its declaring scope's names.
BindingId SemaModel::SearchMember(ScopeId scope, EntityId unscoped_enum,
                                  const std::string& name,
                                  unsigned filter) const
{
  if (unscoped_enum == 0)
    return DirectBinding(scope, name, filter);
  const BindingId found = DirectBinding(scope, name,
                                        filter & LOOKUP_ENUMERATORS);
  if (found == 0 || bindings_[found].kind != BINDING_ENUMERATOR ||
      types_.At(bindings_[found].type).entity != unscoped_enum)
    return 0;
  return found;
}

BindingId SemaModel::LookupQualified(ScopeId scope, const QualifiedName& name,
                                     unsigned filter) const
{
  if (name.components.empty())
    return 0;
  ScopeId current = GlobalScope();
  EntityId unscoped_enum = 0;
  std::size_t next = 0;
  BindingId binding = 0;
  if (!name.global)
  {
    binding = LookupUnqualified(scope, name.components[0], LOOKUP_QUALIFIER);
    if (binding == 0)
      return 0;
    next = 1;
  }
  std::vector<ScopeId> visited;
  for (; next < name.components.size(); ++next)
  {
    if (next != 0)
    {
      // Step into the scope the previous component denotes.
      if (!NominatedScope(binding, current))
        return 0;
      const Binding& prefix = bindings_[binding];
      const TypeNode* type = prefix.kind == BINDING_NAMESPACE ? 0 :
          &types_.At(types_.Unqualified(prefix.type));
      unscoped_enum = type != 0 && type->kind == TYPE_ENUM && !type->scoped ?
          type->entity : 0;
      visited.clear();
    }
    const unsigned step_filter = next + 1 == name.components.size() ?
        filter : LOOKUP_QUALIFIER;
    binding = scopes_[current].kind == SCOPE_NAMESPACE ?
        SearchNamespace(current, name.components[next], step_filter, visited) :
        SearchMember(current, unscoped_enum, name.components[next],
                     step_filter);
    if (binding == 0)
      return 0;
  }
  return Matches(bindings_[binding], filter) ? binding : 0;
}

BindingId SemaModel::Lookup(ScopeId scope, const QualifiedName& name,
                            unsigned filter) const
{
  if (name.components.empty())
    return 0;
  if (!name.Qualified())
    return LookupUnqualified(scope, name.components[0], filter);
  return LookupQualified(scope, name, filter);
}

ClassEntityId SemaModel::CreateClass(bool is_union)
{
  ClassEntity entity;
  entity.is_union = is_union;
  classes_.push_back(entity);
  return classes_.size() - 1;
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

EnumEntityId SemaModel::CreateEnum(bool scoped, TypeId underlying)
{
  EnumEntity entity;
  entity.scoped = scoped;
  entity.underlying = underlying;
  enums_.push_back(entity);
  return enums_.size() - 1;
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
