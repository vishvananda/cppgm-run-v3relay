#include "sema/scope_builder.h"

#include <algorithm>
#include <stdexcept>

namespace
{

TypeId ObjectElementType(TypeTable& types, TypeId type)
{
  TypeId result = types.Unqualified(type);
  while (result != 0 && types.Kind(result) == TYPE_ARRAY)
    result = types.Unqualified(types.At(result).base);
  return result;
}

bool HasConstObject(TypeTable& types, TypeId type)
{
  if (type == 0)
    return false;
  const TypeKind kind = types.Kind(type);
  if (kind == TYPE_CV)
    return types.At(type).is_const || HasConstObject(types,
                                                     types.At(type).base);
  if (kind == TYPE_ARRAY)
    return HasConstObject(types, types.At(type).base);
  return false;
}

bool UserDeclared(const SemaModel& model, FunctionEntityId function)
{
  return function != 0 && !model.FunctionAt(function).synthesized;
}

}  // namespace

// The parser must choose a declaration before semantic lookup has populated
// inherited members.  Reclassify only the bounded zero-argument parameter
// shape used by an expression initializer; ordinary function declarators
// remain owned by the normal type builder.
bool ScopeBuilder::FindAmbiguousDirectInitializer(
    AstId declarator, ScopeId scope, std::vector<AstId>& arguments) const
{
  arguments.clear();
  if (declarator == 0)
    return false;
  const AstNode& value = arena_.At(declarator);
  const AstId clause = FindChild(declarator, AST_PARAMETER_CLAUSE);
  if (clause == 0 || arena_.At(clause).children.empty())
    return false;
  for (std::size_t i = 0; i < value.children.size(); ++i)
  {
    const AstId child = value.children[i];
    if (child != clause && arena_.At(child).kind != AST_IDENTIFIER)
      return false;
  }

  bool has_expression_item = false;
  const std::vector<AstId>& parameters = arena_.At(clause).children;
  for (std::size_t i = 0; i < parameters.size(); ++i)
  {
    const AstId parameter = parameters[i];
    if (arena_.At(parameter).kind != AST_PARAMETER_DECLARATION)
      return false;
    const AstId specifiers = FindChild(parameter, AST_DECL_SPECIFIER_SEQ);
    const AstId parameter_declarator =
        FindChild(parameter, AST_DECLARATOR);
    if (specifiers == 0 || parameter_declarator == 0)
      return false;
    const AstId parameter_clause =
        FindChild(parameter_declarator, AST_PARAMETER_CLAUSE);
    if (parameter_clause == 0 ||
        !arena_.At(parameter_clause).children.empty() ||
        arena_.At(parameter_declarator).children.size() != 1)
      return false;
    const std::vector<AstId>& specifier_nodes =
        arena_.At(specifiers).children;
    if (specifier_nodes.size() != 1)
      return false;
    const AstNode& specifier = arena_.At(specifier_nodes[0]);
    bool type_like = specifier.first < tokens_.size() &&
        specifier.last == specifier.first + 1 &&
        tokens_[specifier.first].kind == PA6_SIMPLE_TOKEN &&
        IsFundamentalTypeKeyword(tokens_[specifier.first].simple_type);
    const QualifiedName name = NodeName(specifier_nodes[0]);
    if (name.Empty())
      return false;
    if (!type_like)
    {
      const BindingId type = name.Qualified() ?
          model_.LookupQualified(scope, name, LOOKUP_TYPES) :
          model_.LookupTypeName(scope, name.Last());
      type_like = type != 0;
    }
    if (!type_like)
    {
      std::vector<BindingId> functions;
      if (name.Qualified())
        model_.LookupQualifiedSet(scope, name, LOOKUP_FUNCTIONS, functions);
      else
        model_.LookupSet(scope, name.Last(), LOOKUP_FUNCTIONS, functions);
      if (functions.empty())
        return false;
      has_expression_item = true;
    }
    arguments.push_back(parameter);
  }
  return has_expression_item;
}

bool ScopeBuilder::BuildAmbiguousDirectInitializer(
    TypeId type, BindingId binding, SemaId variable, ScopeId scope,
    bool static_member, const std::vector<AstId>& arguments)
{
  if (type == 0)
    return false;
  if (variable != 0)
  {
    EnsureDestructor(type);
    AddConstructorActionWithArguments(variable, scope, type, binding,
                                      arguments);
  }
  if (model_.ScopeAt(scope).kind == SCOPE_BLOCK && !static_member)
    RecordInitializedLocal(scope);
  return true;
}

// Namespace redeclarations and out-of-class member definitions share the
// same direct-binding search, but keep each declaration as a separate dump
// binding while recording its canonical entity here.
void ScopeBuilder::LinkRedeclaration(BindingId binding, ScopeId scope,
                                     const std::string& name, TypeId type)
{
  std::vector<BindingId> priors;
  model_.DirectBindings(scope, name, LOOKUP_VALUES, priors);
  for (std::size_t i = 0; i < priors.size(); ++i)
  {
    if (priors[i] == binding)
      continue;
    const Binding& prior = model_.BindingAt(priors[i]);
    if (prior.kind != BINDING_VARIABLE)
      continue;
    if (!CompatibleRedeclaration(prior.type, type))
      throw std::runtime_error("object redeclared with a different type");
    if (prior.thread_local_storage !=
        model_.BindingAt(binding).thread_local_storage)
      throw std::runtime_error(
          "thread_local does not agree across redeclarations");
    model_.BindingAt(binding).redeclared_binding =
        prior.redeclared_binding != 0 ? prior.redeclared_binding : priors[i];
    return;
  }
}

// 3.5p6-7: a block-scope `extern` variable declaration names the entity with
// linkage that a visible declaration inside the innermost enclosing namespace
// already declares; otherwise it declares a new member of that namespace.
// Either way the block binding stands for the namespace entity through
// redeclared_binding, so lowering gives it no slot and no lifetime.
void ScopeBuilder::LinkBlockScopeExtern(BindingId binding, ScopeId scope,
                                        const std::string& name, TypeId type)
{
  const ScopeId namespace_scope = EnclosingNamespace(scope);
  for (ScopeId level = scope;; level = model_.ScopeAt(level).parent)
  {
    const ScopeKind kind = model_.ScopeAt(level).kind;
    if (kind == SCOPE_NAMESPACE || kind == SCOPE_BLOCK ||
        kind == SCOPE_FUNCTION)
    {
      std::vector<BindingId> priors;
      model_.DirectBindings(level, name, LOOKUP_VALUES, priors);
      for (std::size_t i = 0; i < priors.size(); ++i)
      {
        if (priors[i] == binding)
          continue;
        const Binding& prior = model_.BindingAt(priors[i]);
        if (prior.kind != BINDING_VARIABLE)
          continue;
        if (kind != SCOPE_NAMESPACE && !prior.extern_declaration)
          throw std::runtime_error(
              "extern declaration follows a declaration with no linkage");
        if (!CompatibleRedeclaration(prior.type, type))
          throw std::runtime_error(
              "object redeclared with a different type");
        model_.BindingAt(binding).redeclared_binding =
            prior.redeclared_binding != 0 ? prior.redeclared_binding :
                priors[i];
        return;
      }
    }
    if (level == namespace_scope)
      break;
  }
  const BindingId member = model_.AddBinding(namespace_scope, name,
                                             BINDING_VARIABLE, type);
  Binding& value = model_.BindingAt(member);
  value.access = ACCESS_PUBLIC;
  value.extern_declaration = true;
  value.internal_linkage = model_.InUnnamedNamespace(namespace_scope);
  value.c_linkage = model_.BindingAt(binding).c_linkage;
  value.thread_local_storage = model_.BindingAt(binding).thread_local_storage;
  model_.BindingAt(binding).redeclared_binding = member;
}

bool ScopeBuilder::CompatibleRedeclaration(TypeId prior, TypeId current) const
{
  if (prior == current)
    return true;
  // 8.3.4p3: an array of unknown bound is completed by a later declaration.
  if (types_.Kind(prior) != TYPE_ARRAY || types_.Kind(current) != TYPE_ARRAY)
    return false;
  const TypeNode& first = types_.At(prior);
  const TypeNode& second = types_.At(current);
  return first.base == second.base &&
      (first.array_bound == 0 || second.array_bound == 0 ||
       first.array_bound == second.array_bound);
}

BindingId ScopeBuilder::FindStaticMemberVariable(
    ScopeId scope, const std::string& name, TypeId type) const
{
  std::vector<BindingId> prior_members;
  model_.DirectBindings(scope, name, LOOKUP_VALUES, prior_members);
  for (std::size_t i = prior_members.size(); i != 0; --i)
  {
    const BindingId prior_id = prior_members[i - 1];
    const Binding& prior = model_.BindingAt(prior_id);
    if (prior.kind == BINDING_VARIABLE && prior.static_member &&
        CompatibleRedeclaration(prior.type, type))
      return prior.redeclared_binding != 0 ?
          prior.redeclared_binding : prior_id;
  }
  return 0;
}

bool ScopeBuilder::HasStaticMemberFunction(
    ScopeId scope, const std::string& name, TypeId member_type) const
{
  std::vector<BindingId> prior_members;
  model_.DirectBindings(scope, name,
                        LOOKUP_FUNCTIONS | LOOKUP_HIDDEN_FRIENDS,
                        prior_members);
  for (std::size_t i = prior_members.size(); i != 0; --i)
  {
    const Binding& prior = model_.BindingAt(prior_members[i - 1]);
    if (prior.kind != BINDING_FUNCTION || prior.function == 0)
      continue;
    const FunctionEntity& entity = model_.FunctionAt(prior.function);
    if (entity.is_member && entity.static_member &&
        entity.member_type == member_type)
      return true;
  }
  return false;
}

ScopeId ScopeBuilder::TypeScopeForDeclaration(ScopeId scope, AstId list) const
{
  if (list == 0 || arena_.At(list).children.empty())
    return scope;
  const AstId first = arena_.At(list).children[0];
  const AstId identifier = FindIdentifier(FindChild(first, AST_DECLARATOR));
  const QualifiedName name = NodeName(identifier);
  return name.Qualified() ? ResolveQualifierScope(scope, name.Prefix()) :
      scope;
}

// 11.3: a friend declaration in `granting` names the entity that owns
// `friend_of`.  A class may repeat the declaration; the relation is one entry.
void ScopeBuilder::RecordFriend(ClassEntityId granting,
                                std::vector<ClassEntityId>& friend_of)
{
  if (std::find(friend_of.begin(), friend_of.end(), granting) ==
      friend_of.end())
    friend_of.push_back(granting);
}

// Ordinary declarations carry operator= through the same canonical function
// path as every other member function.  Record its copy/move category there,
// so overload resolution, deletion checks, and LowIR all consume one entity
// fact instead of rediscovering the parameter spelling.
void ScopeBuilder::RecordAssignmentMember(FunctionEntityId function)
{
  if (function == 0)
    return;
  FunctionEntity& entity = model_.FunctionAt(function);
  if (!entity.is_member || entity.static_member || entity.name != "operator=" ||
      entity.member_type == 0)
    return;
  const TypeNode& member_type =
      types_.At(types_.Unqualified(entity.member_type));
  if (member_type.parameters.size() != 1 || entity.member_class == 0)
    return;
  TypeId parameter = member_type.parameters[0];
  bool copy = false;
  bool move = false;
  if (types_.Kind(types_.Unqualified(parameter)) == TYPE_REFERENCE)
  {
    const bool lvalue_reference =
        types_.At(types_.Unqualified(parameter)).lvalue_reference;
    parameter = types_.Unqualified(types_.Referent(parameter));
    const bool same_class = types_.Kind(parameter) == TYPE_CLASS &&
        types_.At(parameter).entity == entity.member_class;
    copy = same_class && lvalue_reference;
    move = same_class && !lvalue_reference;
  }
  else
  {
    parameter = ObjectElementType(types_, parameter);
    copy = types_.Kind(parameter) == TYPE_CLASS &&
        types_.At(parameter).entity == entity.member_class;
  }
  entity.copy_assignment = copy;
  entity.move_assignment = move;
  ClassEntity& owner = model_.ClassAt(entity.member_class);
  if (copy && owner.copy_assignment == 0)
    owner.copy_assignment = function;
  if (move && owner.move_assignment == 0)
    owner.move_assignment = function;
}

// A defaulted operator= is parsed as an ordinary member declaration.  In-class
// definitions wait for class completion; an out-of-class definition is already
// in a complete-class context and can be attached immediately.
void ScopeBuilder::BuildDefaultedMemberDefinition(
    FunctionEntityId function, BindingId binding, ScopeId target_scope,
    const std::string& name, const std::vector<ParameterInfo>& parameters,
    bool defer_definition)
{
  if (!EmitsSemantics())
    return;
  FunctionEntity& entity = model_.FunctionAt(function);
  entity.defined = true;
  const SemaId function_node = defer_definition ? MakeDetachedSemantic(
      SEMA_FUNCTION_DEFINITION, target_scope, entity.type, binding, function) :
      MakeSemantic(SEMA_FUNCTION_DEFINITION, target_scope,
                   SemanticParent(target_scope), entity.type, binding,
                   function);
  if (defer_definition)
    DeferSemantic(function_node);
  const ScopeId function_scope = model_.CreateScope(
      SCOPE_FUNCTION, name, target_scope);
  model_.ScopeAt(function_scope).function_entity = function;
  MapSemanticScope(function_scope, function_node);
  const TypeNode& canonical = types_.At(types_.Unqualified(entity.type));
  if (canonical.parameters.empty())
    throw std::runtime_error("defaulted member has no this parameter");
  const BindingId this_binding = model_.AddBinding(
      function_scope, "this", BINDING_PARAMETER, canonical.parameters[0]);
  MakeSemantic(SEMA_PARAMETER, function_scope, function_node,
               canonical.parameters[0], this_binding);
  for (std::size_t i = 0; i < parameters.size(); ++i)
  {
    const BindingId parameter = model_.AddBinding(
        function_scope, parameters[i].name, BINDING_PARAMETER,
        canonical.parameters[i + 1]);
    MakeSemantic(SEMA_PARAMETER, function_scope, function_node,
                 canonical.parameters[i + 1], parameter);
  }
  if (defer_definition)
    deferred_member_bodies_.push_back(DeferredMemberBody(
        0, function_scope, function, function_node));
  else
    BuildFunctionBody(0, function_scope, function, function_node);
}

// Build an implicit/defaulted special member as a normal function entity and
// a detached semantic definition.  Keeping this construction here makes the
// class model the sole owner of copy/move ids; expression analysis only asks
// for the operation it needs.
void ScopeBuilder::EnsureCopyMoveMembers(TypeId type, bool constructors,
                                         bool assignments)
{
  const TypeId class_type = types_.Unqualified(type);
  if (types_.Kind(class_type) != TYPE_CLASS)
    return;
  const ClassEntityId class_entity =
      static_cast<ClassEntityId>(types_.At(class_type).entity);
  ClassEntity& owner = model_.ClassAt(class_entity);
  if (!owner.layout_complete || owner.class_scope == 0)
    return;

  const bool user_copy_constructor = UserDeclared(
      model_, owner.copy_constructor);
  const bool user_move_constructor = UserDeclared(
      model_, owner.move_constructor);
  const bool user_copy_assignment = UserDeclared(
      model_, owner.copy_assignment);
  const bool user_move_assignment = UserDeclared(
      model_, owner.move_assignment);
  const bool user_destructor = owner.destructor != 0 &&
      UserDeclared(model_, owner.destructor);

  // A trivial class subobject is covered by the surrounding byte range.  A
  // nontrivial one must have the corresponding canonical operation available;
  // an xvalue first tries the move member, then falls back to copy only when
  // no move member was declared.
  const auto constructor_viable = [&](ClassEntityId sub,
                                      bool move) -> bool {
    const ClassEntity& subowner = model_.ClassAt(sub);
    if (subowner.trivially_copyable)
      return true;
    EnsureCopyMoveMembers(subowner.type, true, false);
    const ClassEntity& refreshed = model_.ClassAt(sub);
    FunctionEntityId operation = move ? refreshed.move_constructor :
        refreshed.copy_constructor;
    if (move && operation == 0)
      operation = refreshed.copy_constructor;
    return operation != 0 && !model_.FunctionAt(operation).deleted;
  };
  const auto assignment_viable = [&](TypeId subobject, bool move) -> bool {
    if (types_.Kind(types_.Unqualified(subobject)) == TYPE_REFERENCE ||
        HasConstObject(types_, subobject))
      return false;
    const TypeId sub = ObjectElementType(types_, subobject);
    if (types_.Kind(sub) != TYPE_CLASS)
      return true;
    const ClassEntityId sub_entity =
        static_cast<ClassEntityId>(types_.At(sub).entity);
    const ClassEntity& subowner = model_.ClassAt(sub_entity);
    const bool has_requested_operation = move ?
        (subowner.move_assignment != 0 || subowner.copy_assignment != 0) :
        subowner.copy_assignment != 0;
    if (subowner.trivially_copyable && !has_requested_operation)
      return true;
    EnsureCopyMoveMembers(subowner.type, false, true);
    const ClassEntity& refreshed = model_.ClassAt(sub_entity);
    FunctionEntityId operation = move ? refreshed.move_assignment :
        refreshed.copy_assignment;
    if (move && operation == 0)
      operation = refreshed.copy_assignment;
    return operation != 0 && !model_.FunctionAt(operation).deleted;
  };
  const auto subobjects_viable = [&](bool constructor,
                                     bool move) -> bool {
    for (std::size_t i = 0; i < owner.bases.size(); ++i)
      if (constructor ? !constructor_viable(owner.bases[i].entity, move) :
                        !assignment_viable(model_.ClassAt(
                            owner.bases[i].entity).type, move))
        return false;
    for (std::size_t i = 0; i < owner.fields.size(); ++i)
    {
      const ClassField& field = owner.fields[i];
      if (field.static_member)
        continue;
      if (constructor) {
        const TypeId sub = ObjectElementType(types_, field.type);
        if (types_.Kind(sub) == TYPE_CLASS &&
            !constructor_viable(static_cast<ClassEntityId>(
                types_.At(sub).entity), move))
          return false;
      } else if (!assignment_viable(field.type, move))
        return false;
    }
    return true;
  };

  const auto create_member = [&](const std::string& name, bool constructor,
                                 bool move, bool deleted) -> FunctionEntityId {
    const TypeId void_type = types_.Fundamental(FT_VOID);
    const TypeId source_base = move ? class_type :
        types_.Cv(class_type, true);
    const TypeId source = types_.Reference(source_base, !move);
    const TypeId result = constructor ? void_type :
        types_.Reference(class_type);
    const std::vector<TypeId> member_parameters(1, source);
    const TypeId member_type = types_.Function(result, member_parameters);
    std::vector<TypeId> canonical_parameters;
    canonical_parameters.push_back(types_.Pointer(class_type));
    canonical_parameters.push_back(source);
    const TypeId function_type = types_.Function(
        result, canonical_parameters);
    const FunctionEntityId id = model_.CreateFunction(
        owner.class_scope, name, function_type);
    FunctionEntity& function = model_.FunctionAt(id);
    function.member_type = member_type;
    function.member_pointer_type = types_.MemberPointer(class_type,
                                                         member_type);
    function.member_class = class_entity;
    function.is_member = true;
    function.special_member = constructor ? SPECIAL_MEMBER_CONSTRUCTOR :
        SPECIAL_MEMBER_NONE;
    function.in_class_definition = true;
    function.synthesized = true;
    function.defaulted = true;
    function.deleted = deleted;
    function.defined = !deleted;
    function.copy_constructor = constructor && !move;
    function.move_constructor = constructor && move;
    function.copy_assignment = !constructor && !move;
    function.move_assignment = !constructor && move;
    function.parameter_names.push_back("other");
    function.default_arguments.assign(canonical_parameters.size(), 0);
    const BindingId binding = model_.AddBinding(
        owner.class_scope, name, BINDING_FUNCTION, member_type);
    model_.BindingAt(binding).function = id;
    model_.BindingAt(binding).declaring_class = class_entity;
    model_.BindingAt(binding).access = ACCESS_PUBLIC;
    if (!deleted && EmitsSemantics())
    {
      const SemaId function_node = MakeDetachedSemantic(
          SEMA_FUNCTION_DEFINITION, owner.class_scope, function_type,
          binding, id);
      DeferSemantic(function_node);
      const ScopeId function_scope = model_.CreateScope(
          SCOPE_FUNCTION, name, owner.class_scope);
      model_.ScopeAt(function_scope).function_entity = id;
      MapSemanticScope(function_scope, function_node);
      const BindingId this_binding = model_.AddBinding(
          function_scope, "this", BINDING_PARAMETER,
          canonical_parameters[0]);
      MakeSemantic(SEMA_PARAMETER, function_scope, function_node,
                   canonical_parameters[0], this_binding);
      const BindingId source_binding = model_.AddBinding(
          function_scope, "other", BINDING_PARAMETER,
          canonical_parameters[1]);
      MakeSemantic(SEMA_PARAMETER, function_scope, function_node,
                   canonical_parameters[1], source_binding);
      MakeSemantic(SEMA_COMPOUND_STATEMENT, function_scope, function_node);
    }
    return id;
  };

  if (constructors)
  {
    if (owner.copy_constructor == 0) {
      const bool deleted = user_move_constructor || user_move_assignment ||
          !subobjects_viable(true, false);
      owner.copy_constructor = create_member(
          model_.ScopeAt(owner.class_scope).name, true, false, deleted);
      ClassEntity& refreshed = model_.ClassAt(class_entity);
      if (refreshed.copy_constructor != 0 &&
          model_.FunctionAt(refreshed.copy_constructor).deleted)
        model_.FunctionAt(refreshed.copy_constructor).defined = false;
      refreshed.constructors.push_back(refreshed.copy_constructor);
    }
    if (owner.move_constructor == 0 && !user_copy_constructor &&
        !user_copy_assignment && !user_move_assignment && !user_destructor) {
      const bool deleted = !subobjects_viable(true, true);
      owner.move_constructor = create_member(
          model_.ScopeAt(owner.class_scope).name, true, true, deleted);
      ClassEntity& refreshed = model_.ClassAt(class_entity);
      refreshed.constructors.push_back(refreshed.move_constructor);
    }
    ClassEntity& refreshed = model_.ClassAt(class_entity);
    if (refreshed.copy_constructor != 0 &&
        model_.FunctionAt(refreshed.copy_constructor).defaulted &&
        !model_.FunctionAt(refreshed.copy_constructor).deleted &&
        !subobjects_viable(true, false))
      model_.FunctionAt(refreshed.copy_constructor).deleted = true;
    if (refreshed.move_constructor != 0 &&
        model_.FunctionAt(refreshed.move_constructor).defaulted &&
        !model_.FunctionAt(refreshed.move_constructor).deleted &&
        !subobjects_viable(true, true))
      model_.FunctionAt(refreshed.move_constructor).deleted = true;
  }

  if (assignments)
  {
    if (owner.copy_assignment == 0) {
      const bool deleted = user_move_constructor || user_move_assignment ||
          !subobjects_viable(false, false);
      owner.copy_assignment = create_member("operator=", false, false,
                                            deleted);
    }
    if (owner.move_assignment == 0 && !user_copy_constructor &&
        !user_move_constructor && !user_copy_assignment &&
        !user_destructor) {
      const bool deleted = !subobjects_viable(false, true);
      owner.move_assignment = create_member("operator=", false, true,
                                            deleted);
    }
    ClassEntity& refreshed = model_.ClassAt(class_entity);
    if (refreshed.copy_assignment != 0 &&
        model_.FunctionAt(refreshed.copy_assignment).defaulted &&
        !model_.FunctionAt(refreshed.copy_assignment).deleted &&
        !subobjects_viable(false, false))
      model_.FunctionAt(refreshed.copy_assignment).deleted = true;
    if (refreshed.move_assignment != 0 &&
        model_.FunctionAt(refreshed.move_assignment).defaulted &&
        !model_.FunctionAt(refreshed.move_assignment).deleted &&
        !subobjects_viable(false, true))
      model_.FunctionAt(refreshed.move_assignment).deleted = true;
  }
}
