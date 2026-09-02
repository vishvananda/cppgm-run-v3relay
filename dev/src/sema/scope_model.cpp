#include "sema/scope_model.h"

#include <algorithm>
#include <stdexcept>

Binding::Binding()
    : kind(BINDING_VARIABLE), type(0), scope(0), namespace_scope(0),
      function(0), object_binding(0),
      internal_linkage(false), c_linkage(false), extern_declaration(false),
      noexcept_qualifier(false),
      has_const_value(false), const_value(0)
{
}

Scope::Scope()
    : kind(SCOPE_NAMESPACE), parent(0), class_entity(0),
      inline_namespace(false), unnamed_namespace(false)
{
}

ClassEntity::ClassEntity()
    : class_scope(0), type(0), default_constructor(0),
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
      is_template(false), internal_linkage(false), c_linkage(false),
      noexcept_qualifier(false), defined(false)
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
  if (kind == SCOPE_NAMESPACE && inline_namespace)
    scopes_[parent].inline_namespaces.push_back(id);
  return id;
}

void SemaModel::MarkInlineNamespace(ScopeId scope)
{
  Scope& value = ScopeAt(scope);
  if (value.kind != SCOPE_NAMESPACE || value.inline_namespace)
    return;
  value.inline_namespace = true;
  scopes_[value.parent].inline_namespaces.push_back(scope);
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

void SemaModel::DirectBindings(ScopeId scope, const std::string& name,
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
  // Each binding belongs to one scope and a lookup visits a scope once, so
  // the result never needs deduplication.
  for (std::size_t i = 0; i < candidates->size(); ++i)
  {
    const BindingId binding = (*candidates)[i];
    if (bindings_[binding].name == name && Matches(bindings_[binding], filter))
      result.push_back(binding);
  }
}

BindingId SemaModel::Latest(const std::vector<BindingId>& found,
                            ScopeId level) const
{
  for (std::size_t i = found.size(); i != 0; --i)
    if (bindings_[found[i - 1]].scope == level)
      return found[i - 1];
  return found.empty() ? 0 : found.front();
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

  const std::size_t before = result.size();
  DirectBindings(scope, name, filter, result);
  if (result.size() != before)
    return;

  const Scope& owner = scopes_[scope];
  for (std::size_t i = 0; i < owner.inline_namespaces.size(); ++i)
    CollectNamespace(owner.inline_namespaces[i], name, filter, visited,
                     result);
  if (result.size() != before)
    return;
  for (std::size_t i = 0; i < owner.using_directives.size(); ++i)
    CollectNamespace(owner.using_directives[i].nominated, name, filter,
                     visited, result);
}

ScopeId SemaModel::WalkUnqualified(ScopeId scope, const std::string& name,
                                   unsigned filter, bool hide_types,
                                   std::vector<BindingId>& result) const
{
  result.clear();
  if (scope >= scopes_.size())
    return kNoScope;
  std::vector<ScopeId> visited;
  // Directives passed on the way out; each applies once, at its namespace.
  std::vector<Scope::UsingDirective> pending;
  for (ScopeId current = scope;; current = scopes_[current].parent)
  {
    const Scope& owner = scopes_[current];
    pending.insert(pending.end(), owner.using_directives.begin(),
                   owner.using_directives.end());
    if (hide_types)
    {
      const BindingId direct = DirectBinding(current, name, LOOKUP_ANY);
      if (direct != 0 && bindings_[direct].kind != BINDING_NAMESPACE)
      {
        if (!Matches(bindings_[direct], filter))
          return kNoScope;
        result.push_back(direct);
        return current;
      }
    }
    DirectBindings(current, name, filter, result);
    if (owner.kind == SCOPE_NAMESPACE)
    {
      // 7.3.1p8, 7.3.4p2: members of inline namespaces and of the namespaces
      // whose directives apply here are members of this level.
      visited.push_back(current);
      for (std::size_t i = 0; i < owner.inline_namespaces.size(); ++i)
        CollectNamespace(owner.inline_namespaces[i], name, filter, visited,
                         result);
      for (std::size_t i = 0; i < pending.size(); ++i)
        if (pending[i].apply_at == current)
          CollectNamespace(pending[i].nominated, name, filter, visited,
                           result);
    }
    if (!result.empty())
      return current;
    if (current == GlobalScope())
      break;
  }
  return kNoScope;
}

void SemaModel::LookupSet(ScopeId scope, const std::string& name,
                          unsigned filter,
                          std::vector<BindingId>& result) const
{
  (void)WalkUnqualified(scope, name, filter, false, result);
}

BindingId SemaModel::LookupUnqualified(ScopeId scope, const std::string& name,
                                       unsigned filter) const
{
  std::vector<BindingId> found;
  const ScopeId level = WalkUnqualified(scope, name, filter, false, found);
  return level == kNoScope ? 0 : Latest(found, level);
}

BindingId SemaModel::LookupTypeName(ScopeId scope, const std::string& name) const
{
  std::vector<BindingId> found;
  const ScopeId level = WalkUnqualified(scope, name, LOOKUP_TYPES, true,
                                        found);
  return level == kNoScope ? 0 : Latest(found, level);
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

bool SemaModel::StepInto(BindingId prefix, ScopeId& target,
                         EntityId& unscoped_enum) const
{
  if (!NominatedScope(prefix, target))
    return false;
  unscoped_enum = 0;
  const Binding& binding = bindings_[prefix];
  if (binding.kind != BINDING_NAMESPACE)
  {
    const TypeNode& type = types_.At(types_.Unqualified(binding.type));
    if (type.kind == TYPE_ENUM && !type.scoped)
      unscoped_enum = type.entity;
  }
  return true;
}

// A member of a namespace, class or enum scope, or an enumerator of an
// unscoped enumeration found among its declaring scope's names.
BindingId SemaModel::SearchMember(ScopeId scope, EntityId unscoped_enum,
                                  const std::string& name,
                                  unsigned filter) const
{
  if (scopes_[scope].kind == SCOPE_NAMESPACE)
  {
    std::vector<ScopeId> visited;
    std::vector<BindingId> found;
    CollectNamespace(scope, name, filter, visited, found);
    return Latest(found, scope);
  }
  if (unscoped_enum == 0)
    return DirectBinding(scope, name, filter);
  const BindingId found = DirectBinding(scope, name,
                                        filter & LOOKUP_ENUMERATORS);
  if (found == 0 || bindings_[found].kind != BINDING_ENUMERATOR ||
      types_.At(bindings_[found].type).entity != unscoped_enum)
    return 0;
  return found;
}

void SemaModel::CollectMember(ScopeId scope, EntityId unscoped_enum,
                              const std::string& name, unsigned filter,
                              std::vector<BindingId>& result) const
{
  if (scopes_[scope].kind == SCOPE_NAMESPACE)
  {
    std::vector<ScopeId> visited;
    CollectNamespace(scope, name, filter, visited, result);
  }
  else if (unscoped_enum != 0)
  {
    const BindingId enumerator = SearchMember(scope, unscoped_enum, name,
                                              filter);
    if (enumerator != 0)
      result.push_back(enumerator);
  }
  else
    DirectBindings(scope, name, filter, result);
}

bool SemaModel::ResolveQualifier(ScopeId scope, const QualifiedName& name,
                                 ScopeId& target,
                                 EntityId& unscoped_enum) const
{
  target = GlobalScope();
  unscoped_enum = 0;
  BindingId prefix = 0;
  std::size_t next = 0;
  if (!name.global)
  {
    prefix = LookupUnqualified(scope, name.components[0], LOOKUP_QUALIFIER);
    if (prefix == 0)
      return false;
    next = 1;
  }
  for (; next + 1 < name.components.size(); ++next)
  {
    if (prefix != 0 && !StepInto(prefix, target, unscoped_enum))
      return false;
    prefix = SearchMember(target, unscoped_enum, name.components[next],
                          LOOKUP_QUALIFIER);
    if (prefix == 0)
      return false;
  }
  return prefix == 0 || StepInto(prefix, target, unscoped_enum);
}

BindingId SemaModel::LookupQualified(ScopeId scope, const QualifiedName& name,
                                     unsigned filter) const
{
  if (name.components.empty())
    return 0;
  if (!name.Qualified())
    return LookupUnqualified(scope, name.components[0], filter);
  ScopeId target = 0;
  EntityId unscoped_enum = 0;
  if (!ResolveQualifier(scope, name, target, unscoped_enum))
    return 0;
  return SearchMember(target, unscoped_enum, name.Last(), filter);
}

void SemaModel::LookupQualifiedSet(ScopeId scope, const QualifiedName& name,
                                   unsigned filter,
                                   std::vector<BindingId>& result) const
{
  result.clear();
  if (name.components.empty())
    return;
  if (!name.Qualified())
  {
    LookupSet(scope, name.components[0], filter, result);
    return;
  }
  ScopeId target = 0;
  EntityId unscoped_enum = 0;
  if (ResolveQualifier(scope, name, target, unscoped_enum))
    CollectMember(target, unscoped_enum, name.Last(), filter, result);
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
  ScopeId current = scope;
  while (scopes_[current].kind == SCOPE_TEMPLATE_PARAMETERS &&
         current != GlobalScope())
    current = scopes_[current].parent;
  if (scopes_[current].kind != SCOPE_CLASS || scopes_[current].class_entity == 0)
    return false;
  entity = scopes_[current].class_entity;
  return true;
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
