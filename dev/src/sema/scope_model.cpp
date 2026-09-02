#include "sema/scope_model.h"

#include <algorithm>
#include <stdexcept>

Binding::Binding()
    : kind(BINDING_VARIABLE), type(0), scope(0), namespace_scope(0),
      function(0), object_binding(0),
      has_const_value(false), const_value(0)
{
}

Scope::Scope()
    : kind(SCOPE_NAMESPACE), parent(0), inline_namespace(false),
      unnamed_namespace(false)
{
}

ClassEntity::ClassEntity()
    : class_scope(0), type(0), default_constructor(0),
      anonymous_storage(0),
      is_union(false), defined(false)
{
}

EnumEntity::EnumEntity()
    : enum_scope(0), underlying(0), scoped(false), defined(false)
{
}

FunctionEntity::FunctionEntity()
    : scope(0), type(0), member_type(0), member_pointer_type(0),
      member_class(0), is_member(false), member_const(false),
      is_constructor(false), is_template(false), defined(false)
{
}

SemaModel::SemaModel(TypeTable& types)
    : types_(types), scopes_(1), bindings_(1), classes_(1), enums_(1),
      functions_(1)
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
  binding.scope = scope;
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
  // The directive participates in unqualified lookup at the nearest
  // namespace that encloses both the directive and its nominated namespace.
  // Storing that boundary keeps lookup independent of source-position scans.
  ScopeId apply_at = scope;
  while (scopes_[apply_at].kind != SCOPE_NAMESPACE)
    apply_at = scopes_[apply_at].parent;
  for (ScopeId candidate = apply_at;;
       candidate = scopes_[candidate].parent)
  {
    ScopeId nominated = target;
    while (nominated != candidate && nominated != GlobalScope())
      nominated = scopes_[nominated].parent;
    if (nominated == candidate)
    {
      apply_at = candidate;
      break;
    }
    if (candidate == GlobalScope())
    {
      apply_at = GlobalScope();
      break;
    }
  }
  scopes_[scope].using_directives.push_back(
      Scope::UsingDirective(target, apply_at));
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
  const std::vector<Scope::UsingDirective>& directives =
      scopes_[scope].using_directives;
  for (std::size_t i = directives.size(); i != 0; --i)
  {
    const BindingId found = SearchNamespace(directives[i - 1].nominated, name, filter,
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

void SemaModel::AppendUnique(std::vector<BindingId>& result,
                             BindingId binding)
{
  if (binding == 0)
    return;
  for (std::size_t i = 0; i < result.size(); ++i)
    if (result[i] == binding)
      return;
  result.push_back(binding);
}

void SemaModel::CollectDirect(ScopeId scope, const std::string& name,
                              unsigned filter,
                              std::vector<BindingId>& result) const
{
  if (scope >= scopes_.size())
    return;
  const Scope& owner = scopes_[scope];
  const std::vector<BindingId>* candidates = &owner.bindings;
  if (!owner.index.empty())
  {
    const std::unordered_map<std::string, std::vector<BindingId> >::
        const_iterator found = owner.index.find(name);
    if (found == owner.index.end())
      return;
    candidates = &found->second;
  }
  for (std::size_t i = 0; i < candidates->size(); ++i)
  {
    const BindingId binding = (*candidates)[i];
    if (bindings_[binding].name == name && Matches(bindings_[binding], filter))
      AppendUnique(result, binding);
  }
}

void SemaModel::CollectNamespace(ScopeId scope, const std::string& name,
                                 unsigned filter,
                                 std::vector<ScopeId>& visited,
                                 std::vector<BindingId>& result) const
{
  if (scope >= scopes_.size() ||
      std::find(visited.begin(), visited.end(), scope) != visited.end())
    return;
  visited.push_back(scope);

  std::vector<BindingId> direct;
  CollectDirect(scope, name, filter, direct);
  if (!direct.empty())
  {
    for (std::size_t i = 0; i < direct.size(); ++i)
      AppendUnique(result, direct[i]);
    return;
  }

  const Scope& owner = scopes_[scope];
  for (std::size_t i = 0; i < owner.children.size(); ++i)
  {
    const ScopeId child_id = owner.children[i];
    const Scope& child = scopes_[child_id];
    if (child.kind == SCOPE_NAMESPACE && child.inline_namespace)
      CollectNamespace(child_id, name, filter, visited, result);
  }
  if (!result.empty())
    return;
  for (std::size_t i = 0; i < owner.using_directives.size(); ++i)
    CollectNamespace(owner.using_directives[i].nominated, name, filter,
                     visited, result);
}

void SemaModel::LookupSet(ScopeId scope, const std::string& name,
                          unsigned filter,
                          std::vector<BindingId>& result) const
{
  result.clear();
  if (scope >= scopes_.size())
    return;
  std::vector<ScopeId> visited;
  for (ScopeId current = scope;; current = scopes_[current].parent)
  {
    std::vector<BindingId> found;
    if (scopes_[current].kind == SCOPE_NAMESPACE)
      CollectNamespace(current, name, filter, visited, found);
    else
      CollectDirect(current, name, filter, found);
    if (found.empty())
    {
      // A using-directive declared in a block is applied at its recorded
      // namespace boundary.  The ordinary current-scope search remains for
      // compatibility with the PA11 lookup API; this branch gives overload
      // lookup the complete set at the same boundary.
      const Scope& owner = scopes_[current];
      for (std::size_t i = 0; i < owner.using_directives.size(); ++i)
        if (owner.using_directives[i].apply_at == current)
          CollectNamespace(owner.using_directives[i].nominated, name, filter,
                           visited, found);
    }
    if (!found.empty())
    {
      result.swap(found);
      return;
    }
    if (current == GlobalScope())
      break;
  }
}

void SemaModel::LookupQualifiedSet(ScopeId scope, const QualifiedName& name,
                                   unsigned filter,
                                   std::vector<BindingId>& result) const
{
  result.clear();
  if (name.components.empty())
    return;

  ScopeId current = GlobalScope();
  BindingId prefix_binding = 0;
  std::size_t next = 0;
  if (!name.global)
  {
    std::vector<BindingId> prefixes;
    LookupSet(scope, name.components[0], LOOKUP_QUALIFIER, prefixes);
    if (prefixes.empty())
      return;
    prefix_binding = prefixes.back();
    next = 1;
  }
  for (; next < name.components.size(); ++next)
  {
    if (next != 0)
    {
      if (!NominatedScope(prefix_binding, current))
        return;
    }
    std::vector<BindingId> found;
    if (next + 1 == name.components.size())
    {
      std::vector<ScopeId> visited;
      if (scopes_[current].kind == SCOPE_NAMESPACE)
        CollectNamespace(current, name.components[next], filter, visited,
                         found);
      else
      {
        const TypeNode& prefix_type =
            types_.At(types_.Unqualified(bindings_[prefix_binding].type));
        const EntityId unscoped = prefix_type.kind == TYPE_ENUM &&
            !prefix_type.scoped ? prefix_type.entity : 0;
        const BindingId member = SearchMember(current, unscoped,
                                              name.components[next], filter);
        AppendUnique(found, member);
      }
    }
    else
    {
      std::vector<ScopeId> visited;
      if (scopes_[current].kind == SCOPE_NAMESPACE)
        CollectNamespace(current, name.components[next], LOOKUP_QUALIFIER,
                         visited, found);
      else
      {
        const BindingId member = SearchMember(current, 0,
                                              name.components[next],
                                              LOOKUP_QUALIFIER);
        AppendUnique(found, member);
      }
    }
    if (found.empty())
      return;
    prefix_binding = found.back();
  }
  result.clear();
  // The loop leaves prefix_binding at the final declaration.  Re-run the
  // final lookup at its containing scope so all overloads, rather than only
  // the qualifier used to reach it, are returned.
  if (name.components.size() == 1 && !name.global)
  {
    result.push_back(prefix_binding);
    return;
  }

  // Resolve the prefix immediately before the final component again.
  current = GlobalScope();
  next = 0;
  if (!name.global)
  {
    std::vector<BindingId> prefixes;
    LookupSet(scope, name.components[0], LOOKUP_QUALIFIER, prefixes);
    if (prefixes.empty())
      return;
    prefix_binding = prefixes.back();
    next = 1;
  }
  for (; next + 1 < name.components.size(); ++next)
  {
    if (next != 0 && !NominatedScope(prefix_binding, current))
      return;
    std::vector<BindingId> found;
    std::vector<ScopeId> visited;
    if (scopes_[current].kind == SCOPE_NAMESPACE)
      CollectNamespace(current, name.components[next], LOOKUP_QUALIFIER,
                       visited, found);
    else
      AppendUnique(found, SearchMember(current, 0,
                                       name.components[next],
                                       LOOKUP_QUALIFIER));
    if (found.empty())
      return;
    prefix_binding = found.back();
  }
  if (name.components.size() == 1 && name.global)
    current = GlobalScope();
  else if (name.components.size() > 1 &&
           !NominatedScope(prefix_binding, current))
    return;
  std::vector<ScopeId> visited;
  if (scopes_[current].kind == SCOPE_NAMESPACE)
    CollectNamespace(current, name.components.back(), filter, visited, result);
  else
  {
    const TypeNode& prefix_type =
        types_.At(types_.Unqualified(bindings_[prefix_binding].type));
    const EntityId unscoped = prefix_type.kind == TYPE_ENUM &&
        !prefix_type.scoped ? prefix_type.entity : 0;
    if (unscoped != 0)
      AppendUnique(result, SearchMember(current, unscoped,
                                        name.components.back(), filter));
    else
      CollectDirect(current, name.components.back(), filter, result);
  }
}

BindingId SemaModel::LookupUnqualified(ScopeId scope, const std::string& name,
                                       unsigned filter) const
{
  // A qualifier is resolved lexically before namespace using-directives are
  // considered.  This matters when a block says `using namespace imported`
  // but its enclosing namespace already owns a nearer `detail` namespace:
  // `detail::name` must continue down the lexical namespace path.  Ordinary
  // value lookup below retains the usual using-directive search behavior.
  if (filter == LOOKUP_QUALIFIER)
    for (ScopeId current = scope;; current = scopes_[current].parent)
    {
      const BindingId direct = DirectBinding(current, name, filter);
      if (direct != 0)
        return direct;
      if (current == GlobalScope())
        break;
    }
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

bool SemaModel::ClassForScope(ScopeId scope, ClassEntityId& entity) const
{
  if (scope >= scopes_.size())
    return false;
  for (ScopeId current = scope;; current = scopes_[current].parent)
  {
    for (ClassEntityId candidate = 1; candidate < classes_.size();
         ++candidate)
      if (classes_[candidate].class_scope != 0 &&
          classes_[candidate].class_scope == current)
      {
        entity = candidate;
        return true;
      }
    if (current == GlobalScope())
      break;
  }
  return false;
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

FunctionEntityId SemaModel::CreateFunction(ScopeId scope,
                                           const std::string& name,
                                           TypeId type)
{
  if (scope >= scopes_.size() || type == 0)
    throw std::out_of_range("invalid function entity");
  FunctionEntity entity;
  entity.scope = scope;
  entity.name = name;
  entity.type = type;
  functions_.push_back(entity);
  return functions_.size() - 1;
}

FunctionEntity& SemaModel::FunctionAt(FunctionEntityId id)
{
  if (id == 0 || id >= functions_.size())
    throw std::out_of_range("invalid function entity");
  return functions_[id];
}

const FunctionEntity& SemaModel::FunctionAt(FunctionEntityId id) const
{
  if (id == 0 || id >= functions_.size())
    throw std::out_of_range("invalid function entity");
  return functions_[id];
}

TypeTable& SemaModel::Types()
{
  return types_;
}

const TypeTable& SemaModel::Types() const
{
  return types_;
}
