// Special-member declaration ownership and deferred body analysis.
#include "sema/scope_builder.h"

#include <algorithm>
#include <stdexcept>

// A qualified special-member definition is parsed in its enclosing
// namespace, but its function entity, parameters, and body scope belong to
// the completed class named by the declarator.
void ScopeBuilder::BuildSpecialMember(AstId node, ScopeId scope)
{
  const AstId declarator = FindChild(node, AST_DECLARATOR);
  if (declarator == 0)
    throw std::runtime_error("special member has no declarator");
  const AstId member_specifiers = FindChild(node, AST_MEMBER_SPECIFIERS);
  const bool explicit_constructor = member_specifiers != 0 &&
      SequenceHasKeyword(member_specifiers, KW_EXPLICIT);
  const AstId identifier = FindIdentifier(declarator);
  if (identifier == 0)
    throw std::runtime_error("special member has no name");
  const AstNode& identifier_node = arena_.At(identifier);
  const std::size_t first = identifier_node.first;
  const std::size_t last = identifier_node.last;
  std::size_t tilde = last;
  for (std::size_t token = first; token < last && token < tokens_.size();
       ++token)
    if (tokens_[token].IsSimple(OP_COMPL))
    {
      tilde = token;
      break;
    }
  const bool destructor = tilde != last;
  std::string name;
  ScopeId target_scope = scope;
  if (destructor)
  {
    if (tilde + 1 >= last || tilde + 1 >= tokens_.size() ||
        !tokens_[tilde + 1].IsIdentifier())
      throw std::runtime_error("special member has no destructor name");
    name = "~" + tokens_[tilde + 1].spelling;
    std::size_t prefix_last = tilde;
    if (prefix_last > first &&
        tokens_[prefix_last - 1].IsSimple(OP_COLON2))
      --prefix_last;
    if (prefix_last != first)
    {
      const QualifiedName prefix = ReadQualifiedName(
          tokens_, first, prefix_last);
      target_scope = ResolveQualifierScope(scope, prefix);
    }
  }
  else
  {
    const QualifiedName qualified_name = ReadQualifiedName(
        tokens_, first, last);
    if (qualified_name.Empty())
      throw std::runtime_error("special member has no constructor name");
    name = qualified_name.Last();
    if (qualified_name.Qualified())
      target_scope = ResolveQualifierScope(scope, qualified_name.Prefix());
  }
  ClassEntityId member_class = 0;
  if (!model_.ClassForScope(target_scope, member_class))
    throw std::runtime_error("special member does not name a class");
  const std::string class_name = model_.ScopeAt(target_scope).name;
  if ((destructor ? name.substr(1) : name) != class_name)
    throw std::runtime_error("special member name does not match its class");
  const AstId clause = FindChild(declarator, AST_PARAMETER_CLAUSE);
  std::vector<ParameterInfo> parameters;
  bool variadic = false;
  if (clause != 0)
    BuildParameters(clause, target_scope, parameters, variadic);
  if (destructor && (!parameters.empty() || variadic))
    throw std::runtime_error("destructor has parameters");
  const std::vector<TypeId> parameter_types = [&]() {
    std::vector<TypeId> result;
    for (std::size_t i = 0; i < parameters.size(); ++i)
      result.push_back(parameters[i].type);
    return result;
  }();
  const bool member_const = HasConstFunctionQualifier(declarator);
  const bool member_volatile = HasVolatileFunctionQualifier(declarator);
  const TypeId declared_type = types_.Function(
      types_.Fundamental(FT_VOID), parameter_types, variadic, member_const);
  std::vector<AstId> defaults;
  for (std::size_t i = 0; i < parameters.size(); ++i)
    defaults.push_back(parameters[i].default_initializer);
  BindingId binding = 0;
  const AstId initializer = FindChild(node, AST_INITIALIZER);
  const AstId special_initializer = FindChild(initializer,
                                               AST_SPECIAL_INITIALIZER);
  const bool defaulted = special_initializer != 0 &&
      arena_.At(special_initializer).text == "default";
  const bool deleted = special_initializer != 0 &&
      arena_.At(special_initializer).text == "delete";
  const bool definition = arena_.At(node).kind ==
      AST_SPECIAL_MEMBER_DEFINITION || defaulted;
  const FunctionEntityId function = DeclareFunction(
      target_scope, name, declared_type, definition, binding, member_const,
      member_volatile,
      false, IsNoThrowDeclarator(declarator, target_scope), defaults,
      explicit_constructor);
  if (binding == 0)
    throw std::runtime_error("special member has no binding");

  FunctionEntity& entity = model_.FunctionAt(function);
  entity.special_member = destructor ? SPECIAL_MEMBER_DESTRUCTOR :
      SPECIAL_MEMBER_CONSTRUCTOR;
  entity.parameter_names.clear();
  for (std::size_t i = 0; i < parameters.size(); ++i)
    entity.parameter_names.push_back(parameters[i].name);
  entity.body = FindChild(node, AST_COMPOUND_STATEMENT);
  entity.ctor_initializer = FindChild(node, AST_CTOR_INITIALIZER);
  // The declaration may be in the class while its definition is qualified
  // outside it.  Only the former (or an explicitly inline latter) has weak
  // ODR linkage in LowIR; the entity bit must not be inferred from the
  // declaration scope alone.
  entity.in_class_definition = entity.in_class_definition ||
      (definition && scope == target_scope) ||
      (member_specifiers != 0 &&
       SequenceHasKeyword(member_specifiers, KW_INLINE));
  entity.defaulted = entity.defaulted || defaulted;
  entity.deleted = entity.deleted || deleted;
  if (destructor)
    model_.ClassAt(member_class).destructor = function;
  else
  {
    ClassEntity& owner = model_.ClassAt(member_class);
    if (std::find(owner.constructors.begin(), owner.constructors.end(),
                  function) == owner.constructors.end())
      owner.constructors.push_back(function);
  }

  if (tree_ == 0)
    return;
  const SemaKind semantic_kind = definition ? SEMA_FUNCTION_DEFINITION :
      SEMA_FUNCTION_DECLARATION;
  const bool deferred_definition = scope == target_scope;
  const SemaId function_node = deferred_definition ?
      MakeDetachedSemantic(semantic_kind, target_scope,
                            model_.FunctionAt(function).type, binding,
                            function) :
      MakeSemantic(semantic_kind, target_scope, SemanticParent(target_scope),
                   model_.FunctionAt(function).type, binding, function);
  if (deferred_definition)
    DeferSemantic(function_node);
  if (!definition)
    return;

  const ScopeId function_scope = model_.CreateScope(
      SCOPE_FUNCTION, name, target_scope);
  model_.ScopeAt(function_scope).function_entity = function;
  MapSemanticScope(function_scope, function_node);
  const TypeNode& canonical = types_.At(
      types_.Unqualified(model_.FunctionAt(function).type));
  const BindingId this_binding = model_.AddBinding(
      function_scope, "this", BINDING_PARAMETER, canonical.parameters[0]);
  const SemaId this_parameter = tree_->Make(SEMA_PARAMETER);
  SemaNode& this_node = tree_->At(this_parameter);
  this_node.scope = function_scope;
  this_node.type = canonical.parameters[0];
  this_node.binding = this_binding;
  tree_->Append(function_node, this_parameter);
  for (std::size_t i = 0; i < parameters.size(); ++i)
  {
    const BindingId parameter = model_.AddBinding(
        function_scope, parameters[i].name, BINDING_PARAMETER,
        canonical.parameters[i + 1]);
    MakeSemantic(SEMA_PARAMETER, function_scope, function_node,
                 canonical.parameters[i + 1], parameter);
  }
  // Keep a semantic copy of each constructor default for implicit base and
  // member initialization.  Explicit call sites are analyzed below as
  // usual; these detached nodes cover the separate lowering path used when a
  // subobject is omitted from the mem-initializer list.
  std::vector<SemaId> default_semantic_arguments(
      canonical.parameters.size(), 0);
  for (std::size_t i = 0; i < parameters.size(); ++i)
    if (parameters[i].default_initializer != 0)
      default_semantic_arguments[i + 1] = expression_.AnalyzeInitializer(
          parameters[i].default_initializer, function_scope,
          canonical.parameters[i + 1]);
  model_.FunctionAt(function).default_semantic_arguments.swap(
      default_semantic_arguments);
  labels_.clear();
  gotos_.clear();
  initialized_locals_.clear();
  jump_sequence_ = 0;
  if (deferred_definition)
    deferred_member_bodies_.push_back(DeferredMemberBody(
        model_.FunctionAt(function).body, function_scope, function,
        function_node));
  else
  {
    if (model_.FunctionAt(function).body != 0)
      (void)BuildCompound(model_.FunctionAt(function).body, function_scope,
                           function, 0, 0, function_node);
    else
      (void)MakeSemantic(SEMA_COMPOUND_STATEMENT, function_scope,
                         function_node);
    for (std::size_t i = 0; i < gotos_.size(); ++i)
    {
      const std::map<std::string, LabelRecord>::const_iterator label =
          labels_.find(gotos_[i].name);
      if (label == labels_.end())
        throw std::runtime_error("goto target does not name a label");
      if (gotos_[i].node != 0)
      {
        tree_->At(gotos_[i].node).has_value = true;
        tree_->At(gotos_[i].node).value = label->second.ordinal;
      }
      CheckJumpTarget(gotos_[i].sequence, gotos_[i].node != 0 ?
                          tree_->At(gotos_[i].node).scope : function_scope,
                      label->second.sequence, label->second.scope);
    }
    labels_.clear();
    gotos_.clear();
    initialized_locals_.clear();
  }
}
