#include "sema/scope_model.h"

#include <algorithm>
#include <stdexcept>

namespace
{

bool SameMemberParameterList(const TypeTable& types,
                             const FunctionEntity& left,
                             const FunctionEntity& right)
{
  if (left.member_type == 0 || right.member_type == 0)
    return false;
  const TypeNode& left_type = types.At(types.Unqualified(left.member_type));
  const TypeNode& right_type = types.At(types.Unqualified(right.member_type));
  return left_type.variadic == right_type.variadic &&
      left_type.parameters == right_type.parameters &&
      left.member_const == right.member_const &&
      left.member_volatile == right.member_volatile;
}

} // namespace

Binding::Binding()
    : kind(BINDING_VARIABLE), type(0), scope(0), declaring_class(0),
      namespace_scope(0),
      function(0), object_binding(0),
      internal_linkage(false), c_linkage(false), thread_local_storage(false),
      extern_declaration(false),
      hidden_friend(false), noexcept_qualifier(false), access(ACCESS_PUBLIC),
      static_member(false), field_index(kNoFieldIndex),
      redeclared_binding(0),
      has_const_value(false), const_value(0)
{
}

Scope::Scope()
    : kind(SCOPE_NAMESPACE), parent(0), class_entity(0), function_entity(0),
      inline_namespace(false), unnamed_namespace(false)
{
}

ClassEntity::ClassEntity()
    : class_scope(0), type(0), default_constructor(0),
      size(0), alignment(1), requested_alignment(0), pack_alignment(0),
      destructor(0), inheriting_constructor_base(0), layout_complete(false),
      trivial_default_constructor(true), trivial_destructor(true),
      empty(false), aggregate(true), is_union(false), defined(false)
{
}

EnumEntity::EnumEntity()
    : enum_scope(0), underlying(0), scoped(false), defined(false)
{
}

FunctionEntity::FunctionEntity()
    : scope(0), type(0), member_type(0), member_pointer_type(0),
      member_class(0), is_member(false), member_const(false),
      member_volatile(false),
      is_template(false), internal_linkage(false), c_linkage(false),
      noexcept_qualifier(false), special_member(SPECIAL_MEMBER_NONE),
      body(0), ctor_initializer(0), parameter_names(), defaulted(false),
      deleted(false), explicit_constructor(false),
      static_member(false), in_class_definition(false), synthesized(false),
      default_semantic_arguments(), default_member_initializers(),
      defined(false)
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
  if (binding.hidden_friend && (filter & LOOKUP_HIDDEN_FRIENDS) == 0)
    return false;
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
    if (owner.kind == SCOPE_CLASS && owner.class_entity != 0)
    {
      // 3.4.1p8: a name used in a member of class X is looked up in X and
      // its bases before any enclosing scope is considered.
      std::vector<ClassEntityId> visited_classes;
      if (hide_types)
      {
        std::vector<BindingId> members;
        CollectClassMember(owner.class_entity, name, LOOKUP_ANY,
                           visited_classes, members);
        if (!members.empty())
        {
          const BindingId direct = InjectedClassName(members.back(), name);
          if (!Matches(bindings_[direct], filter))
            return kNoScope;
          result.push_back(direct);
          return current;
        }
      }
      else
        CollectClassMember(owner.class_entity, name, filter, visited_classes,
                           result);
    }
    else
    {
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
    }
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

// A class scope binds its constructors under the class's own name and holds
// no type binding for itself, so a constructor found by a type lookup stands
// for the injected-class-name (9p2): the class binding in its declaring
// scope.  Any other binding is returned unchanged.
BindingId SemaModel::InjectedClassName(BindingId binding,
                                       const std::string& name) const
{
  const Binding& value = bindings_[binding];
  if (value.kind != BINDING_FUNCTION || value.function == 0 ||
      value.function >= functions_.size())
    return binding;
  const FunctionEntity& function = functions_[value.function];
  if (function.special_member != SPECIAL_MEMBER_CONSTRUCTOR ||
      function.member_class == 0 || function.member_class >= classes_.size())
    return binding;
  const ScopeId class_scope = classes_[function.member_class].class_scope;
  if (class_scope == 0 || class_scope >= scopes_.size())
    return binding;
  const BindingId type = DirectBinding(scopes_[class_scope].parent, name,
                                       LOOKUP_TYPES);
  return type != 0 ? type : binding;
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

const ClassField* SemaModel::FieldFor(BindingId binding) const
{
  if (binding == 0 || binding >= bindings_.size())
    return 0;
  const Binding& value = bindings_[binding];
  if (value.field_index == kNoFieldIndex || value.scope >= scopes_.size())
    return 0;
  const Scope& owner_scope = scopes_[value.scope];
  ClassEntityId owner = value.declaring_class;
  if (owner == 0) {
    if (owner_scope.kind != SCOPE_CLASS || owner_scope.class_entity == 0)
      return 0;
    owner = owner_scope.class_entity;
  }
  if (owner >= classes_.size())
    return 0;
  const ClassEntity& entity = classes_[owner];
  return value.field_index < entity.fields.size() ?
      &entity.fields[value.field_index] : 0;
}

bool SemaModel::IsDerivedFrom(ClassEntityId derived, ClassEntityId base) const
{
  if (derived == 0 || base == 0 || derived == base ||
      derived >= classes_.size() || base >= classes_.size())
    return false;
  std::vector<ClassEntityId> pending(1, derived);
  std::vector<ClassEntityId> visited;
  while (!pending.empty())
  {
    const ClassEntityId current = pending.back();
    pending.pop_back();
    if (std::find(visited.begin(), visited.end(), current) != visited.end())
      continue;
    visited.push_back(current);
    const ClassEntity& owner = classes_[current];
    for (std::size_t i = 0; i < owner.bases.size(); ++i)
    {
      if (owner.bases[i].entity == base)
        return true;
      pending.push_back(owner.bases[i].entity);
    }
  }
  return false;
}

ClassEntityId SemaModel::DeclaringClass(BindingId binding) const
{
  if (binding == 0 || binding >= bindings_.size())
    return 0;
  const Binding& value = bindings_[binding];
  if (value.declaring_class != 0)
    return value.declaring_class;
  if (value.kind == BINDING_FUNCTION && value.function != 0 &&
      value.function < functions_.size())
    return functions_[value.function].member_class;
  ScopeId scope = value.scope;
  while (scope < scopes_.size())
  {
    const Scope& owner = scopes_[scope];
    if (owner.kind == SCOPE_CLASS && owner.class_entity != 0)
      return owner.class_entity;
    if (scope == GlobalScope())
      break;
    scope = owner.parent;
  }
  return 0;
}

FunctionEntityId SemaModel::ContextFunction(ScopeId scope) const
{
  while (scope < scopes_.size())
  {
    const Scope& owner = scopes_[scope];
    if (owner.kind == SCOPE_FUNCTION && owner.function_entity != 0)
      return owner.function_entity;
    if (scope == GlobalScope())
      break;
    scope = owner.parent;
  }
  return 0;
}

void SemaModel::ContextClasses(
    ScopeId scope, std::vector<ClassEntityId>& result) const
{
  result.clear();
  while (scope < scopes_.size())
  {
    const Scope& owner = scopes_[scope];
    if (owner.kind == SCOPE_CLASS && owner.class_entity != 0 &&
        std::find(result.begin(), result.end(), owner.class_entity) ==
            result.end())
      result.push_back(owner.class_entity);
    if (scope == GlobalScope())
      break;
    scope = owner.parent;
  }
}

// 11.3: a friend of the owner shares the owner's access; 11.2p5 further
// lets a friend or member of a class derived from the owner use the
// owner's protected members.
bool SemaModel::FriendGrantsAccess(ClassEntityId granting,
                                   ClassEntityId owner,
                                   AccessKind access) const
{
  return granting == owner ||
      (access == ACCESS_PROTECTED && IsDerivedFrom(granting, owner));
}

bool SemaModel::ContextCanAccess(ClassEntityId owner, AccessKind access,
                                 ScopeId context) const
{
  if (owner == 0 || owner >= classes_.size())
    return false;
  // Every class enclosing the context is a candidate: a nested class is a
  // member of its enclosing class (11.7), and a friend declaration naming
  // an enclosing class covers the code nested inside it (11.3p2).
  std::vector<ClassEntityId> classes;
  ContextClasses(context, classes);
  for (std::size_t i = 0; i < classes.size(); ++i)
  {
    const ClassEntityId candidate = classes[i];
    if (candidate == owner ||
        (access == ACCESS_PROTECTED && IsDerivedFrom(candidate, owner)))
      return true;
    const std::vector<ClassEntityId>& granting =
        classes_[candidate].friend_of;
    for (std::size_t j = 0; j < granting.size(); ++j)
      if (FriendGrantsAccess(granting[j], owner, access))
        return true;
  }
  const FunctionEntityId function = ContextFunction(context);
  if (function != 0 && function < functions_.size())
  {
    const std::vector<ClassEntityId>& granting =
        functions_[function].friend_of;
    for (std::size_t j = 0; j < granting.size(); ++j)
      if (FriendGrantsAccess(granting[j], owner, access))
        return true;
  }
  return false;
}

bool SemaModel::IsAccessible(BindingId binding, ScopeId context) const
{
  if (binding == 0 || binding >= bindings_.size())
    return false;
  const Binding& value = bindings_[binding];
  if (value.access == ACCESS_PUBLIC)
    return true;
  const ClassEntityId owner = DeclaringClass(binding);
  return owner != 0 && ContextCanAccess(owner, value.access, context);
}

bool SemaModel::IsBaseEdgeAccessible(ClassEntityId owner, AccessKind access,
                                     ScopeId context) const
{
  return access == ACCESS_PUBLIC || ContextCanAccess(owner, access, context);
}

bool SemaModel::FindAccessibleBasePath(
    ClassEntityId current, ClassEntityId target, ScopeId context,
    std::vector<ClassEntityId>& visited) const
{
  if (current == target)
    return true;
  if (current == 0 || current >= classes_.size() ||
      std::find(visited.begin(), visited.end(), current) != visited.end())
    return false;
  visited.push_back(current);
  const ClassEntity& owner = classes_[current];
  for (std::size_t i = 0; i < owner.bases.size(); ++i)
  {
    const ClassBase& base = owner.bases[i];
    if (!IsBaseEdgeAccessible(current, base.access, context))
      continue;
    if (FindAccessibleBasePath(base.entity, target, context, visited))
      return true;
  }
  return false;
}

bool SemaModel::IsBaseAccessible(ClassEntityId derived, ClassEntityId base,
                                  ScopeId context) const
{
  if (derived == 0 || base == 0 || derived >= classes_.size() ||
      base >= classes_.size())
    return false;
  if (derived == base)
    return true;
  std::vector<ClassEntityId> visited;
  return FindAccessibleBasePath(derived, base, context, visited);
}

void SemaModel::CollectClassMember(
    ClassEntityId entity, const std::string& name, unsigned filter,
    std::vector<ClassEntityId>& visited,
    std::vector<BindingId>& result) const
{
  if (entity == 0 || entity >= classes_.size() ||
      std::find(visited.begin(), visited.end(), entity) != visited.end())
    return;
  visited.push_back(entity);
  const ClassEntity& value = classes_[entity];
  const std::size_t before = result.size();
  if (value.class_scope != 0)
    DirectBindings(value.class_scope, name, filter, result);
  // 9p2: the injected-class-name is a member of its class scope even though
  // the canonical type binding lives in the enclosing scope.  Materialize
  // that existing binding during lookup so a derived qualifier can find an
  // inherited `Base` in `Derived::Base` without copying type ownership.
  if (result.size() == before && (filter & LOOKUP_TYPES) != 0 &&
      value.class_scope != 0 &&
      scopes_[value.class_scope].name == name)
  {
    const BindingId injected = DirectBinding(
        scopes_[value.class_scope].parent, name, LOOKUP_TYPES);
    if (injected != 0)
      result.push_back(injected);
  }
  if (result.size() != before)
  {
    // A using-declaration re-exposes base overloads in the derived scope.
    // A declaration in that same scope with an identical member parameter
    // list hides the re-exposed base declaration, while distinct base
    // overloads remain candidates.
    std::vector<BindingId> local_functions;
    for (std::size_t i = before; i < result.size(); ++i)
    {
      const Binding& binding = bindings_[result[i]];
      if (binding.kind == BINDING_FUNCTION && binding.function != 0 &&
          functions_[binding.function].member_class == entity)
        local_functions.push_back(result[i]);
    }
    if (!local_functions.empty())
    {
      std::vector<BindingId> kept;
      kept.reserve(result.size() - before);
      for (std::size_t i = before; i < result.size(); ++i)
      {
        const Binding& binding = bindings_[result[i]];
        bool hidden = false;
        if (binding.kind == BINDING_FUNCTION && binding.function != 0 &&
            functions_[binding.function].member_class != entity)
        {
          const FunctionEntity& candidate = functions_[binding.function];
          for (std::size_t local = 0; local < local_functions.size(); ++local)
          {
            const FunctionEntity& declared = functions_[
                bindings_[local_functions[local]].function];
            if (SameMemberParameterList(types_, candidate, declared))
            {
              hidden = true;
              break;
            }
          }
        }
        if (!hidden)
          kept.push_back(result[i]);
      }
      result.resize(before);
      result.insert(result.end(), kept.begin(), kept.end());
    }
    // A declaration in the derived class hides every base declaration of
    // the same name, including declarations that are not viable overloads.
    return;
  }
  for (std::size_t i = 0; i < value.bases.size(); ++i)
    CollectClassMember(value.bases[i].entity, name, filter, visited, result);
}

void SemaModel::LookupMember(ClassEntityId entity, const std::string& name,
                             unsigned filter,
                             std::vector<BindingId>& result) const
{
  result.clear();
  std::vector<ClassEntityId> visited;
  CollectClassMember(entity, name, filter, visited, result);
}

void SemaModel::AddAssociatedNamespace(
    ScopeId scope, std::vector<ScopeId>& namespaces) const
{
  if (scope >= scopes_.size())
    return;
  ScopeId current = scope;
  // The associated namespace of a class or enumeration is its innermost
  // enclosing namespace.  Do not climb namespace parents: ADL deliberately
  // does not turn `a::b::T` into an argument associated with all of `a`.
  while (current != GlobalScope())
  {
    const Scope& owner = scopes_[current];
    if (owner.kind == SCOPE_NAMESPACE)
    {
      if (std::find(namespaces.begin(), namespaces.end(), current) ==
          namespaces.end())
        namespaces.push_back(current);
      return;
    }
    current = owner.parent;
  }
  if (std::find(namespaces.begin(), namespaces.end(), GlobalScope()) ==
      namespaces.end())
    namespaces.push_back(GlobalScope());
}

void SemaModel::CollectAssociatedClass(
    ClassEntityId entity, std::vector<ScopeId>& namespaces,
    std::vector<ClassEntityId>& classes) const
{
  if (entity == 0 || entity >= classes_.size() ||
      std::find(classes.begin(), classes.end(), entity) != classes.end())
    return;
  classes.push_back(entity);
  const ClassEntity& owner = classes_[entity];
  if (owner.class_scope != 0)
  {
    ScopeId current = owner.class_scope;
    while (current != GlobalScope())
    {
      const Scope& scope = scopes_[current];
      if (scope.kind == SCOPE_CLASS && scope.class_entity != 0)
        CollectAssociatedClass(scope.class_entity, namespaces, classes);
      current = scope.parent;
    }
    AddAssociatedNamespace(owner.class_scope, namespaces);
  }
  for (std::size_t i = 0; i < owner.bases.size(); ++i)
    CollectAssociatedClass(owner.bases[i].entity, namespaces, classes);
}

void SemaModel::CollectAssociated(
    TypeId type, std::vector<ScopeId>& namespaces,
    std::vector<ClassEntityId>& classes,
    std::vector<TypeId>& visited_types) const
{
  if (type == 0 ||
      std::find(visited_types.begin(), visited_types.end(), type) !=
          visited_types.end())
    return;
  visited_types.push_back(type);
  const TypeNode& node = types_.At(type);
  switch (node.kind)
  {
  case TYPE_CV: case TYPE_REFERENCE: case TYPE_ARRAY:
    CollectAssociated(node.base, namespaces, classes, visited_types);
    return;
  case TYPE_POINTER:
    // Pointers associate the pointed-to type as well as their own built-in
    // representation (which contributes no namespace of its own).
    CollectAssociated(node.base, namespaces, classes, visited_types);
    return;
  case TYPE_FUNCTION:
    CollectAssociated(node.result, namespaces, classes, visited_types);
    for (std::size_t i = 0; i < node.parameters.size(); ++i)
      CollectAssociated(node.parameters[i], namespaces, classes,
                        visited_types);
    return;
  case TYPE_MEMBER_POINTER:
    CollectAssociated(node.member_class, namespaces, classes, visited_types);
    CollectAssociated(node.base, namespaces, classes, visited_types);
    return;
  case TYPE_CLASS:
    CollectAssociatedClass(static_cast<ClassEntityId>(node.entity),
                           namespaces, classes);
    return;
  case TYPE_ENUM:
    if (node.entity == 0 || node.entity >= enums_.size())
      return;
    {
      const EnumEntity& enum_entity = enums_[node.entity];
      if (enum_entity.enum_scope != 0)
      {
        const Scope& enum_scope = scopes_[enum_entity.enum_scope];
        if (enum_scope.kind == SCOPE_ENUM &&
            enum_scope.parent < scopes_.size() &&
            scopes_[enum_scope.parent].kind == SCOPE_CLASS &&
            scopes_[enum_scope.parent].class_entity != 0)
          CollectAssociatedClass(scopes_[enum_scope.parent].class_entity,
                                 namespaces, classes);
        else if (enum_scope.kind == SCOPE_CLASS &&
                 enum_scope.class_entity != 0)
          CollectAssociatedClass(enum_scope.class_entity, namespaces, classes);
        else
          AddAssociatedNamespace(enum_entity.enum_scope, namespaces);
      }
    }
    return;
  case TYPE_FUNDAMENTAL: case TYPE_TEMPLATE_PARAM: case TYPE_INVALID:
    return;
  }
}

void SemaModel::CollectAssociatedNamespace(
    ScopeId scope, const std::string& name, unsigned filter,
    std::vector<ScopeId>& visited,
    std::vector<BindingId>& result) const
{
  if (scope >= scopes_.size() ||
      std::find(visited.begin(), visited.end(), scope) != visited.end())
    return;
  visited.push_back(scope);
  DirectBindings(scope, name, filter, result);
  const Scope& owner = scopes_[scope];
  if (owner.inline_namespace && owner.parent < scopes_.size() &&
      scopes_[owner.parent].kind == SCOPE_NAMESPACE)
    CollectAssociatedNamespace(owner.parent, name, filter, visited, result);
  for (std::size_t i = 0; i < owner.inline_namespaces.size(); ++i)
    CollectAssociatedNamespace(owner.inline_namespaces[i], name, filter,
                               visited, result);
}

void SemaModel::LookupCallSet(
    ScopeId scope, const std::string& name,
    const std::vector<TypeId>& argument_types,
    std::vector<BindingId>& result) const
{
  result.clear();
  std::vector<BindingId> ordinary;
  LookupSet(scope, name, LOOKUP_FUNCTIONS, ordinary);
  result.insert(result.end(), ordinary.begin(), ordinary.end());

  std::vector<ScopeId> namespaces;
  std::vector<ClassEntityId> classes;
  std::vector<TypeId> visited_types;
  for (std::size_t i = 0; i < argument_types.size(); ++i)
    CollectAssociated(argument_types[i], namespaces, classes, visited_types);

  // ADL searches declarations in associated namespaces.  Using-directives
  // are deliberately not followed here: a using-declaration creates a
  // direct binding in the associated namespace, while a using-directive is
  // ordinary lookup state and must not be imported into ADL.
  std::vector<ScopeId> visited_namespaces;
  for (std::size_t i = 0; i < namespaces.size(); ++i)
  {
    std::vector<BindingId> found;
    CollectAssociatedNamespace(namespaces[i], name, LOOKUP_FUNCTIONS,
                               visited_namespaces, found);
    for (std::size_t j = 0; j < found.size(); ++j)
      if (std::find(result.begin(), result.end(), found[j]) == result.end())
        result.push_back(found[j]);
  }
  for (std::size_t i = 0; i < classes.size(); ++i)
  {
    const ClassEntity& owner = classes_[classes[i]];
    for (std::size_t j = 0; j < owner.hidden_friends.size(); ++j)
    {
      const BindingId binding = owner.hidden_friends[j];
      const Binding& value = bindings_[binding];
      if (value.name != name || value.kind != BINDING_FUNCTION ||
          !value.hidden_friend ||
          std::find(result.begin(), result.end(), binding) != result.end())
        continue;
      result.push_back(binding);
    }
  }
}

void SemaModel::LookupOperatorSet(
    ScopeId scope, const std::string& name,
    const std::vector<TypeId>& argument_types,
    std::vector<BindingId>& result) const
{
  LookupCallSet(scope, name, argument_types, result);
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
  if (scopes_[scope].kind == SCOPE_CLASS &&
      scopes_[scope].class_entity != 0)
  {
    std::vector<BindingId> members;
    LookupMember(scopes_[scope].class_entity, name, filter, members);
    return members.empty() ? 0 : members.back();
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
  else if (scopes_[scope].kind == SCOPE_CLASS &&
           scopes_[scope].class_entity != 0)
    LookupMember(scopes_[scope].class_entity, name, filter, result);
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
