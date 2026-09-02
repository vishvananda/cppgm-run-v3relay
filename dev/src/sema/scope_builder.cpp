// Declaration collection and scope ownership for the semantic model.
#include "sema/scope_builder.h"

#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

using std::string;
using std::vector;

void ScopeBuilder::Build(AstId root)
{
  if (root == 0)
    throw std::runtime_error("empty AST");
  if (arena_.At(root).kind != AST_TRANSLATION_UNIT)
    throw std::runtime_error("AST root is not a translation unit");
  if (tree_ != 0)
  {
    semantic_root_ = tree_->Make(SEMA_TRANSLATION_UNIT);
    tree_->SetRoot(semantic_root_);
    MapSemanticScope(model_.GlobalScope(), semantic_root_);
  }
  const vector<AstId>& children = arena_.At(root).children;
  for (std::size_t i = 0; i < children.size(); ++i)
    BuildNode(children[i], model_.GlobalScope());
  EmitDeferredSemantics();
  EmitTemplateInstances();
}

void ScopeBuilder::BuildNode(AstId node, ScopeId scope, SemaId semantic_parent)
{
  if (node == 0)
    return;
  switch (arena_.At(node).kind)
  {
  case AST_NAMESPACE_DEFINITION: BuildNamespace(node, scope); return;
  case AST_NAMESPACE_ALIAS_DEFINITION: BuildNamespaceAlias(node, scope); return;
  case AST_USING_DIRECTIVE: BuildUsingDirective(node, scope); return;
  case AST_USING_DECLARATION: BuildUsingDeclaration(node, scope); return;
  case AST_ALIAS_DECLARATION: BuildAlias(node, scope); return;
  case AST_SIMPLE_DECLARATION:
    BuildSimpleDeclaration(node, scope, semantic_parent);
    return;
  case AST_FUNCTION_DEFINITION: BuildFunctionDefinition(node, scope); return;
  case AST_ENUM_SPECIFIER: case AST_ENUM_DECLARATION:
    if (EmitsSemantics() && model_.ScopeAt(scope).kind == SCOPE_BLOCK)
      MakeSemantic(SEMA_SIMPLE_DECLARATION, scope, SemanticParent(scope));
    (void)BuildEnum(node, scope, string());
    return;
  case AST_CLASS_SPECIFIER:
  {
    const TypeId type = BuildClassDefinition(node, scope, string());
    if (EmitsSemantics() && model_.ScopeAt(scope).kind == SCOPE_BLOCK &&
        NodeName(node).Empty() && ClassKey(node) == TK_UNION)
      BuildAnonymousUnionStorage(node, scope, semantic_parent, type);
    return;
  }
  case AST_CLASS_FORWARD_DECLARATION:
    (void)BuildClassForward(node, scope);
    return;
  case AST_LINKAGE_SPECIFICATION: BuildLinkage(node, scope); return;
  case AST_COMPOUND_STATEMENT:
    (void)BuildCompound(node, scope);
    return;
  case AST_STATIC_ASSERT_DECLARATION: BuildStaticAssert(node, scope); return;
  case AST_TEMPLATE_DECLARATION: BuildTemplate(node, scope); return;
  case AST_EMPTY_DECLARATION: case AST_ACCESS_SPECIFIER:
    return;
  default:
    throw std::runtime_error("unsupported pa11 declaration");
  }
}

// 14.1: a template-declaration owns a scope for its parameters; the declared
// entity lands inside it.
void ScopeBuilder::BuildTemplate(AstId node, ScopeId scope)
{
  const AstId clause = FindChild(node, AST_TEMPLATE_PARAMETER_CLAUSE);
  AstId declaration = 0;
  const vector<AstId>& children = arena_.At(node).children;
  for (std::size_t i = 0; i < children.size() && declaration == 0; ++i)
    if (children[i] != clause)
      declaration = children[i];
  if (clause == 0 || declaration == 0)
    throw std::runtime_error("invalid template declaration");

  const ScopeId template_scope = model_.CreateScope(
      SCOPE_TEMPLATE_PARAMETERS, string(), scope);
  const AstId list = FindChild(clause, AST_TEMPLATE_PARAMETER_LIST);
  if (list != 0)
  {
    const vector<AstId>& parameters = arena_.At(list).children;
    for (std::size_t i = 0; i < parameters.size(); ++i)
      BuildTemplateParameter(parameters[i], template_scope);
  }

  const bool previous_suppression = suppress_semantics_;
  suppress_semantics_ = true;
  BuildNode(declaration, template_scope);
  suppress_semantics_ = previous_suppression;

  // A template declaration remains visible from the enclosing declaration
  // scope.  Its parameter scope is an implementation detail used when the
  // generic function type is substituted, not a lookup barrier for the
  // template-name itself.
  std::vector<TypeId> template_parameters;
  const std::vector<BindingId>& parameter_bindings =
      model_.ScopeAt(template_scope).bindings;
  for (std::size_t i = 0; i < parameter_bindings.size(); ++i)
  {
    const Binding& parameter = model_.BindingAt(parameter_bindings[i]);
    if (parameter.kind == BINDING_TYPE)
      template_parameters.push_back(parameter.type);
  }
  const std::vector<BindingId> declarations =
      model_.ScopeAt(template_scope).bindings;
  for (std::size_t i = 0; i < declarations.size(); ++i)
  {
    const Binding& source = model_.BindingAt(declarations[i]);
    if (source.kind != BINDING_FUNCTION || source.function == 0)
      continue;
    const std::string name = source.name;
    const TypeId type = source.type;
    const FunctionEntityId function = source.function;
    FunctionEntity& entity = model_.FunctionAt(function);
    entity.is_template = true;
    entity.template_parameters = template_parameters;
    Binding& exported = model_.BindingAt(model_.AddBinding(
        scope, name, BINDING_FUNCTION, type));
    exported.function = function;
  }
}

// Type parameters become named template-parameter types; the parameter list
// of a template template parameter is not a scope, so its names stay
// invisible.  Non-type parameters are typed but not bound (out of scope).
void ScopeBuilder::BuildTemplateParameter(AstId parameter, ScopeId scope)
{
  const AstNode& value = arena_.At(parameter);
  if (value.kind == AST_TYPE_PARAMETER)
  {
    const AstId identifier = FindChild(parameter, AST_IDENTIFIER);
    if (identifier == 0)
      return;
    const AstId key = FindChild(parameter, AST_PARAMETER_KEY);
    TypeKeyword keyword = TK_TYPENAME;
    if (FindChild(parameter, AST_TEMPLATE_TEMPLATE_PARAMETER) != 0)
      keyword = TK_TEMPLATE_PARAMETER;
    else if (key != 0 && arena_.At(key).first < tokens_.size() &&
             tokens_[arena_.At(key).first].IsSimple(KW_CLASS))
      keyword = TK_CLASS;
    const string name = IdentifierName(identifier);
    model_.AddBinding(scope, name, BINDING_TYPE,
                      types_.TemplateParam(keyword, name));
    return;
  }
  if (value.kind == AST_NON_TYPE_TEMPLATE_PARAMETER)
  {
    const AstId specifiers = FindChild(parameter, AST_DECL_SPECIFIER_SEQ);
    if (specifiers == 0)
      throw std::runtime_error("template parameter has no type");
    const TypeId base = BuildSpecifierType(specifiers, scope);
    (void)BuildDeclaratorType(FindChild(parameter, AST_DECLARATOR), base,
                              scope);
  }
}

// 7.3.1: a named namespace reopens its scope; an unnamed namespace is
// `<unnamed>` and nominated by an implicit using-directive (7.3.1.1p1).
void ScopeBuilder::BuildNamespace(AstId node, ScopeId scope)
{
  const AstNode& value = arena_.At(node);
  const bool is_inline = FindChild(node, AST_INLINE) != 0;
  ScopeId target = 0;
  if (value.first == value.last)
  {
    target = model_.CreateScope(SCOPE_NAMESPACE, "<unnamed>", scope, is_inline);
    model_.ScopeAt(target).unnamed_namespace = true;
    model_.AddUsingDirective(scope, target);
  }
  else
  {
    const string name = IdentifierName(node);
    const BindingId existing = model_.DirectBinding(scope, name,
                                                    LOOKUP_NAMESPACES);
    if (existing != 0)
    {
      target = model_.BindingAt(existing).namespace_scope;
      if (is_inline)
        model_.MarkInlineNamespace(target);
    }
    else
    {
      target = model_.CreateScope(SCOPE_NAMESPACE, name, scope, is_inline);
      model_.AddBinding(scope, name, BINDING_NAMESPACE, 0, target);
    }
  }
  SemaId previous = 0;
  if (tree_ != 0)
  {
    if (target < semantic_scopes_.size())
      previous = semantic_scopes_[target];
    BindingId namespace_binding = 0;
    if (value.first != value.last)
      namespace_binding = model_.DirectBinding(scope, IdentifierName(node),
                                               LOOKUP_NAMESPACES);
    const SemaId semantic = MakeSemantic(SEMA_NAMESPACE_DEFINITION, target,
        SemanticParent(scope), 0, namespace_binding);
    MapSemanticScope(target, semantic);
  }
  for (std::size_t i = 0; i < value.children.size(); ++i)
    if (arena_.At(value.children[i]).kind != AST_INLINE)
      BuildNode(value.children[i], target);
  // A reopened namespace prints under its own definition node; earlier
  // definitions keep theirs.
  if (tree_ != 0)
    MapSemanticScope(target, previous);
}

void ScopeBuilder::BuildNamespaceAlias(AstId node, ScopeId scope)
{
  const AstId target = FindChild(node, AST_TARGET);
  if (target == 0)
    throw std::runtime_error("namespace alias has no target");
  model_.AddBinding(scope, IdentifierName(node), BINDING_NAMESPACE, 0,
                    ResolveNamespace(scope, target));
}

void ScopeBuilder::BuildUsingDirective(AstId node, ScopeId scope)
{
  const AstId target = FindChild(node, AST_TARGET);
  if (target == 0)
    throw std::runtime_error("using-directive has no target");
  model_.AddUsingDirective(scope, ResolveNamespace(scope, target));
}

// 7.3.3: the found declaration is re-bound in the current scope with its
// kind, type and constant value.  Template-ids are rejected while the name
// is read; namespaces cannot be named (7.3.3p7).
void ScopeBuilder::BuildUsingDeclaration(AstId node, ScopeId scope)
{
  const AstId target_node = FindChild(node, AST_TARGET);
  if (target_node == 0)
    throw std::runtime_error("using-declaration has no target");
  const QualifiedName name = NodeName(target_node);
  const BindingId target = model_.Lookup(scope, name, LOOKUP_ANY);
  if (target == 0 || model_.BindingAt(target).kind == BINDING_NAMESPACE)
    throw std::runtime_error("using-declaration target not found");
  const Binding source = model_.BindingAt(target);
  Binding& imported = model_.BindingAt(model_.AddBinding(
      scope, name.Last(), source.kind, source.type));
  imported.function = source.function;
  imported.has_const_value = source.has_const_value;
  imported.const_value = source.const_value;
}

void ScopeBuilder::BuildAlias(AstId node, ScopeId scope)
{
  const AstId type_id = FindChild(node, AST_TYPE_ID);
  if (type_id == 0)
    throw std::runtime_error("alias has no type");
  const TypeId type = BuildTypeId(type_id, scope);
  const BindingId binding = model_.AddBinding(
      scope, IdentifierName(node), BINDING_TYPE_ALIAS, type);
  if (tree_ != 0)
    MakeSemantic(SEMA_TYPE_ALIAS, scope, SemanticParent(scope), type, binding);
}

void ScopeBuilder::BuildSimpleDeclaration(AstId node, ScopeId scope,
                                          SemaId semantic_parent)
{
  const AstId specifiers = FindChild(node, AST_DECL_SPECIFIER_SEQ);
  const AstId list = FindChild(node, AST_INIT_DECLARATOR_LIST);
  if (specifiers == 0)
    throw std::runtime_error("simple declaration has no specifiers");

  const bool is_typedef = SequenceHasKeyword(specifiers, KW_TYPEDEF);
  const bool is_constexpr = SequenceHasKeyword(specifiers, KW_CONSTEXPR);
  // An unnamed class or enum in the specifiers takes the first declarator's
  // name (`typedef struct { ... } S;` declares `struct S`).
  string anonymous_name;
  if (list != 0 && !arena_.At(list).children.empty())
  {
    const AstId first = arena_.At(list).children[0];
    anonymous_name = IdentifierName(FindIdentifier(
        FindChild(first, AST_DECLARATOR)));
  }
  const AstId anonymous_class = FindChild(specifiers, AST_CLASS_SPECIFIER);
  if (!is_typedef && anonymous_class != 0 &&
      model_.ScopeAt(scope).kind == SCOPE_BLOCK &&
      NodeName(anonymous_class).Empty())
  {
    std::ostringstream generated;
    generated << "__local_type" << ++unnamed_local_class_counter_;
    anonymous_name = generated.str();
  }
  const TypeId base = BuildSpecifierType(specifiers, scope, anonymous_name);
  if (list == 0)
  {
    if (EmitsSemantics() && model_.ScopeAt(scope).kind == SCOPE_BLOCK)
      MakeSemantic(SEMA_SIMPLE_DECLARATION, scope,
                   semantic_parent != 0 ? semantic_parent :
                       SemanticParent(scope));
    return;
  }

  const vector<AstId>& items = arena_.At(list).children;
  SemaId declaration_node = 0;
  if (EmitsSemantics() && model_.ScopeAt(scope).kind == SCOPE_BLOCK)
    declaration_node = MakeSemantic(SEMA_SIMPLE_DECLARATION, scope,
                                    semantic_parent != 0 ? semantic_parent :
                                        SemanticParent(scope));
  for (std::size_t i = 0; i < items.size(); ++i)
  {
    const AstId declarator = FindChild(items[i], AST_DECLARATOR);
    if (arena_.At(items[i]).kind != AST_INIT_DECLARATOR || declarator == 0)
      throw std::runtime_error("invalid init-declarator");
    // 8.3p1: a qualified declarator-id declares in the named scope and its
    // declarator is resolved there.
    string name;
    const ScopeId target_scope = ResolveDeclarationScope(
        scope, FindIdentifier(declarator), name);
    const AstId initializer = FindChild(items[i], AST_INITIALIZER);
    TypeId type = BuildDeclaratorType(
        declarator, base, target_scope, false,
        HasIncompleteArray(declarator) ? InitializerBound(initializer) : 0);
    const bool is_function = types_.Kind(type) == TYPE_FUNCTION;
    // 7.1.5p9: a constexpr object is const.
    if (is_constexpr && !is_typedef && !is_function)
      type = types_.Cv(type, true);
    const BindingKind kind = is_typedef ? BINDING_TYPE_ALIAS :
        is_function ? BINDING_FUNCTION : BINDING_VARIABLE;
    BindingId binding = 0;
    if (is_function)
    {
      vector<ParameterInfo> parameters;
      bool variadic = false;
      const AstId clause = FindChild(declarator, AST_PARAMETER_CLAUSE);
      if (clause != 0)
        BuildParameters(clause, target_scope, parameters, variadic);
      vector<AstId> default_arguments;
      for (std::size_t parameter = 0; parameter < parameters.size();
           ++parameter)
        default_arguments.push_back(parameters[parameter].default_initializer);
      const FunctionEntityId function = DeclareFunction(
          target_scope, name, type, false, binding,
          HasConstFunctionQualifier(declarator),
          SequenceHasKeyword(specifiers, KW_STATIC),
          HasNoexceptQualifier(declarator), default_arguments);
      ClassEntityId member_class = 0;
      const bool is_member = model_.ClassForScope(target_scope, member_class);
      if (EmitsSemantics() && !is_member)
        MakeSemantic(SEMA_FUNCTION_DECLARATION, target_scope,
                     declaration_node != 0 ? declaration_node :
                         SemanticParent(target_scope),
                     model_.FunctionAt(function).type, binding, function);
      continue;
    }
    binding = model_.AddBinding(target_scope, name, kind, type);
    model_.BindingAt(binding).internal_linkage =
        SequenceHasKeyword(specifiers, KW_STATIC);
    model_.BindingAt(binding).c_linkage = c_linkage_depth_ != 0;
    SemaId variable = 0;
    if (EmitsSemantics() && model_.ScopeAt(target_scope).kind != SCOPE_CLASS)
      variable = MakeSemantic(is_typedef ? SEMA_TYPE_ALIAS : SEMA_VARIABLE,
          target_scope, declaration_node != 0 ? declaration_node :
              (semantic_parent != 0 ? semantic_parent :
                  SemanticParent(target_scope)), type, binding);
    if (!is_typedef)
      BuildVariable(binding, initializer, declarator, target_scope, variable,
                    is_constexpr);
  }
}

bool ScopeBuilder::RecordsConstantValue(TypeId type) const
{
  if (types_.Kind(type) != TYPE_CV || !types_.At(type).is_const)
    return false;
  const TypeId unqualified = types_.Unqualified(type);
  return types_.Kind(unqualified) == TYPE_ENUM ||
      (types_.Kind(unqualified) == TYPE_FUNDAMENTAL &&
       FundamentalIsIntegral(types_.At(unqualified).fundamental));
}

// The initializer of a dumped variable is analyzed and appended under it.
// A const integral object's initializer is analyzed in every mode so its
// value can serve later constant expressions (5.19p2); the nodes of an
// analysis that is not dumped are released.  A class object without an
// initializer receives its default-constructor action.
void ScopeBuilder::BuildVariable(BindingId binding, AstId initializer,
                                 AstId declarator, ScopeId scope,
                                 SemaId variable, bool is_constexpr)
{
  const TypeId type = model_.BindingAt(binding).type;
  const bool records = RecordsConstantValue(type);
  if (initializer == 0)
  {
    if (is_constexpr)
      throw std::runtime_error("constexpr variable has no initializer");
    if (variable != 0 && types_.Kind(types_.Unqualified(type)) == TYPE_CLASS)
      AddConstructorAction(variable, scope, type, binding, declarator);
    return;
  }
  if (variable == 0 && !records)
    return;
  const std::size_t mark = Tree().Mark();
  const SemaId initialized = expression_.AnalyzeInitializer(initializer,
                                                            scope, type);
  if (is_constexpr)
    expression_.Initialize(initialized, type, true);
  long long value = 0;
  if (expression_.TryConstant(initialized, value))
  {
    if (records)
    {
      Binding& stored = model_.BindingAt(binding);
      stored.const_value = value;
      stored.has_const_value = true;
    }
  }
  else if (is_constexpr && records)
    throw std::runtime_error("constexpr variable is not constant");
  if (variable != 0)
    Tree().Append(variable, initialized);
  else
    Tree().Truncate(mark);
}

// The function binds in the scope its declarator-id names; parameters are
// resolved there and bound in a function scope that owns the body block.
void ScopeBuilder::BuildFunctionDefinition(AstId node, ScopeId scope)
{
  const AstId specifiers = FindChild(node, AST_DECL_SPECIFIER_SEQ);
  const AstId declarator = FindChild(node, AST_DECLARATOR);
  const AstId body = FindChild(node, AST_COMPOUND_STATEMENT);
  if (specifiers == 0 || declarator == 0 || body == 0)
    throw std::runtime_error("invalid function definition");
  string name;
  const ScopeId target_scope = ResolveDeclarationScope(
      scope, FindIdentifier(declarator), name);
  const TypeId base = BuildSpecifierType(specifiers, target_scope);
  const TypeId type = BuildDeclaratorType(declarator, base, target_scope);
  if (types_.Kind(type) != TYPE_FUNCTION)
    throw std::runtime_error("function definition is not a function");
  BindingId binding = 0;
  vector<ParameterInfo> parameters;
  bool variadic = false;
  const AstId clause = FindChild(declarator, AST_PARAMETER_CLAUSE);
  if (clause != 0)
    BuildParameters(clause, target_scope, parameters, variadic);
  vector<AstId> default_arguments;
  for (std::size_t parameter = 0; parameter < parameters.size(); ++parameter)
    default_arguments.push_back(parameters[parameter].default_initializer);
  const FunctionEntityId function = DeclareFunction(
      target_scope, name, type, true, binding,
      HasConstFunctionQualifier(declarator),
      SequenceHasKeyword(specifiers, KW_STATIC),
      HasNoexceptQualifier(declarator), default_arguments);
  ClassEntityId member_class = 0;
  const bool is_member = model_.ClassForScope(target_scope, member_class);
  SemaId function_node = 0;
  if (EmitsSemantics())
  {
    if (is_member)
    {
      function_node = MakeDetachedSemantic(
          SEMA_FUNCTION_DEFINITION, target_scope,
          model_.FunctionAt(function).type, binding, function);
      DeferSemantic(function_node);
    }
    else
      function_node = MakeSemantic(SEMA_FUNCTION_DEFINITION, target_scope,
          SemanticParent(target_scope), model_.FunctionAt(function).type,
          binding, function);
  }
  const ScopeId function_scope = model_.CreateScope(
      SCOPE_FUNCTION, name, target_scope);
  if (function_node != 0)
    MapSemanticScope(function_scope, function_node);

  if (is_member && function_node != 0)
  {
    const TypeNode& canonical = model_.Types().At(
        model_.FunctionAt(function).type);
    if (canonical.parameters.empty())
      throw std::runtime_error("member function has no this parameter");
    const BindingId this_binding = model_.AddBinding(
        function_scope, "this", BINDING_PARAMETER, canonical.parameters[0]);
    const SemaId this_parameter = tree_->Make(SEMA_PARAMETER);
    SemaNode& this_node = tree_->At(this_parameter);
    this_node.scope = function_scope;
    this_node.type = canonical.parameters[0];
    this_node.binding = this_binding;
    tree_->Append(function_node, this_parameter);
  }

  if (clause != 0)
  {
    for (std::size_t i = 0; i < parameters.size(); ++i)
    {
      const BindingId parameter = model_.AddBinding(
          function_scope, parameters[i].name, BINDING_PARAMETER,
          parameters[i].type);
      if (function_node != 0)
      {
        const TypeNode& canonical = model_.Types().At(
            model_.FunctionAt(function).type);
        const std::size_t canonical_index = is_member ? i + 1 : i;
        const TypeId parameter_type = canonical_index <
            canonical.parameters.size() ? canonical.parameters[canonical_index] :
            parameters[i].type;
        MakeSemantic(SEMA_PARAMETER, function_scope, function_node,
                     parameter_type, parameter);
      }
    }
  }
  labels_.clear();
  gotos_.clear();
  (void)BuildCompound(body, function_scope, function, 0, 0, function_node);
  for (std::size_t i = 0; i < gotos_.size(); ++i)
    if (labels_.find(gotos_[i]) == labels_.end())
      throw std::runtime_error("goto target does not name a label");
  labels_.clear();
  gotos_.clear();
}

long long ScopeBuilder::ConstantValue(AstId expression, ScopeId scope)
{
  SemaTree& tree = Tree();
  const std::size_t mark = tree.Mark();
  const long long value = expression_.Value(
      expression_.Analyze(expression, scope));
  tree.Truncate(mark);
  return value;
}

void ScopeBuilder::BuildStaticAssert(AstId node, ScopeId scope)
{
  const AstNode& value = arena_.At(node);
  if (value.children.empty())
    throw std::runtime_error("static_assert has no expression");
  if (ConstantValue(value.children[0], scope) == 0)
    throw std::runtime_error("static_assert failed");
}

void ScopeBuilder::BuildLinkage(AstId node, ScopeId scope)
{
  const bool is_c = arena_.At(node).text == "C";
  if (is_c)
    ++c_linkage_depth_;
  const vector<AstId>& children = arena_.At(node).children;
  for (std::size_t i = 0; i < children.size(); ++i)
    BuildNode(children[i], scope);
  if (is_c)
    --c_linkage_depth_;
}

// Identity for a type declared without a name or declarator: the token
// extent of its declaration, as the dump format fixes.
string ScopeBuilder::AnonymousTypeName(AstId node, const char* kind) const
{
  std::size_t first = 0;
  std::size_t last = 0;
  if (!arena_.DeclarationExtent(node, first, last))
    throw std::runtime_error("unnamed type requires a declaration name");
  std::ostringstream generated;
  generated << "__anonymous_" << kind << "_type__" << first << '_' << last;
  return generated.str();
}

// 9p1: a class-specifier completes a class entity declared earlier in the
// same scope (or the qualified scope), or introduces a new one.  A bare
// anonymous union injects its members into the enclosing scope (9.5p5).
TypeId ScopeBuilder::BuildClassDefinition(AstId node, ScopeId scope,
                                         const string& anonymous_name)
{
  const AstNode& value = arena_.At(node);
  const TypeKeyword key = ClassKey(node);
  const QualifiedName name = NodeName(node);
  string spelling = name.Empty() ? anonymous_name : name.Joined();
  const bool injected_union = name.Empty() && anonymous_name.empty() &&
      key == TK_UNION;
  if (spelling.empty())
    spelling = AnonymousTypeName(node, key == TK_UNION ? "union" :
                                 key == TK_CLASS ? "class" : "struct");

  ClassEntityId entity = 0;
  const ScopeId declaring = name.Qualified() ?
      ResolveQualifierScope(scope, name.Prefix()) : scope;
  if (!injected_union)
  {
    const BindingId existing = model_.DirectBinding(
        declaring, name.Empty() ? spelling : name.Last(), LOOKUP_TYPES);
    if (existing != 0)
    {
      const TypeNode& previous = types_.At(model_.BindingAt(existing).type);
      if (previous.kind != TYPE_CLASS)
        throw std::runtime_error("class name already denotes another type");
      if ((previous.keyword == TK_UNION) != (key == TK_UNION))
        throw std::runtime_error("class-key disagrees with prior declaration");
      entity = previous.entity;
    }
    else if (name.Qualified())
      throw std::runtime_error("qualified class definition names no class");
  }
  if (entity == 0)
    entity = model_.CreateClass(key == TK_UNION);
  if (model_.ClassAt(entity).defined)
    throw std::runtime_error("class redefinition");
  const TypeId type = types_.Class(entity, key, spelling);
  const ScopeId class_scope = model_.CreateScope(SCOPE_CLASS, spelling,
                                                 declaring);
  model_.ScopeAt(class_scope).class_entity = entity;
  // Members may declare further classes, which grows the entity table: keep
  // ids, not entity references, across the member walk.
  model_.ClassAt(entity).defined = true;
  model_.ClassAt(entity).class_scope = class_scope;
  model_.ClassAt(entity).type = type;
  if (!injected_union)
    model_.AddBinding(declaring, name.Empty() ? spelling : name.Last(),
                      BINDING_TYPE, type);
  for (std::size_t i = 0; i < value.children.size(); ++i)
  {
    const AstKind kind = arena_.At(value.children[i]).kind;
    if (kind != AST_CLASS_KEY && kind != AST_BASE_CLAUSE)
      BuildNode(value.children[i], class_scope);
  }
  if (injected_union)
  {
    const vector<BindingId> members = model_.ScopeAt(class_scope).bindings;
    for (std::size_t i = 0; i < members.size(); ++i)
    {
      const Binding member = model_.BindingAt(members[i]);
      const BindingId injected_id = model_.AddBinding(
          scope, member.name, member.kind, member.type);
      Binding& injected = model_.BindingAt(injected_id);
      injected.has_const_value = member.has_const_value;
      injected.const_value = member.const_value;
      model_.ClassAt(entity).injected_members.push_back(injected_id);
    }
  }
  return type;
}

// `struct S;` declares S in the current scope unless the scope already
// declares a class S (3.3.1, 7.1.6.3).
TypeId ScopeBuilder::BuildClassForward(AstId node, ScopeId scope)
{
  const QualifiedName name = NodeName(node);
  if (name.Empty())
    throw std::runtime_error("unnamed class forward declaration");
  if (name.Qualified())
  {
    // A qualified redeclaration must find its class and declares nothing new.
    const BindingId found = model_.LookupQualified(scope, name, LOOKUP_TYPES);
    if (found == 0 || types_.Kind(model_.BindingAt(found).type) != TYPE_CLASS)
      throw std::runtime_error("qualified class declaration names no class");
    return model_.BindingAt(found).type;
  }
  const TypeKeyword key = ClassKey(node);
  ClassEntityId entity = 0;
  const BindingId existing = model_.DirectBinding(scope, name.Last(),
                                                  LOOKUP_TYPES);
  if (existing != 0)
  {
    const TypeNode& previous = types_.At(model_.BindingAt(existing).type);
    if (previous.kind != TYPE_CLASS)
      throw std::runtime_error("class name already denotes another type");
    if ((previous.keyword == TK_UNION) != (key == TK_UNION))
      throw std::runtime_error("class-key disagrees with prior declaration");
    entity = previous.entity;
  }
  if (entity == 0)
    entity = model_.CreateClass(key == TK_UNION);
  const TypeId type = types_.Class(entity, key, name.Last());
  model_.ClassAt(entity).type = type;
  model_.AddBinding(scope, name.Last(), BINDING_TYPE, type);
  return type;
}

// 3.4.4: an elaborated-type-specifier ignores non-type names.  When nothing
// is found and the specifier is part of a declaration, it declares the class
// in the nearest enclosing namespace or block scope (7.1.6.3p2, 3.3.2p7).
TypeId ScopeBuilder::BuildElaboratedClass(AstId node, ScopeId scope,
                                          bool may_declare)
{
  const QualifiedName name = NodeName(node);
  if (name.Empty())
    throw std::runtime_error("unnamed elaborated type");
  const BindingId existing = model_.Lookup(scope, name, LOOKUP_TYPES);
  if (existing != 0)
  {
    const TypeId type = model_.BindingAt(existing).type;
    if (types_.Kind(type) != TYPE_CLASS)
      throw std::runtime_error("elaborated class specifier names a non-class");
    return type;
  }
  if (!may_declare || name.Qualified())
    throw std::runtime_error("elaborated class specifier names no class");
  ScopeId target = scope;
  while (model_.ScopeAt(target).kind != SCOPE_NAMESPACE &&
         model_.ScopeAt(target).kind != SCOPE_BLOCK)
    target = model_.ScopeAt(target).parent;
  return BuildClassForward(node, target);
}

// 7.2: enum-specifiers and opaque-enum-declarations declare or complete an
// enumeration; an elaborated `enum E` without enum-base names an existing one.
TypeId ScopeBuilder::BuildEnum(AstId node, ScopeId scope,
                               const string& anonymous_name)
{
  const AstNode& value = arena_.At(node);
  const bool definition = value.kind == AST_ENUM_SPECIFIER;
  const bool scoped = FindChild(node, AST_ENUM_KEY) != 0;
  const AstId underlying_node = FindChild(node, AST_TYPE_ID);
  vector<AstId> enumerators;
  for (std::size_t i = 0; i < value.children.size(); ++i)
    if (arena_.At(value.children[i]).kind == AST_ENUMERATOR)
      enumerators.push_back(value.children[i]);

  QualifiedName name = NodeName(node);
  if (name.Empty())
  {
    if (!definition)
      throw std::runtime_error("unnamed enum declaration");
    string generated = anonymous_name;
    if (generated.empty() && tree_ != 0 &&
        model_.ScopeAt(scope).kind == SCOPE_BLOCK)
    {
      std::ostringstream name_stream;
      name_stream << "__anonymous_enum" << ++unnamed_local_enum_counter_;
      generated = name_stream.str();
    }
    name.components.push_back(generated.empty() ?
                              AnonymousTypeName(node, "enum") : generated);
  }
  if (!definition && !scoped && underlying_node == 0)
  {
    // Elaborated enum specifier (7.1.6.3p2: the name must be found).
    const BindingId found = model_.Lookup(scope, name, LOOKUP_TYPES);
    if (found == 0 || types_.Kind(model_.BindingAt(found).type) != TYPE_ENUM)
      throw std::runtime_error("elaborated enum specifier names no enum");
    return model_.BindingAt(found).type;
  }
  if (!definition && name.Qualified())
    throw std::runtime_error("qualified opaque enum declaration");

  const ScopeId parent = name.Qualified() ?
      ResolveQualifierScope(scope, name.Prefix()) : scope;
  TypeId underlying = underlying_node == 0 ? types_.Fundamental(FT_INT) :
      BuildTypeId(underlying_node, scope);
  if (underlying_node == 0 && !scoped) {
    long long previous = -1;
    long long minimum = 0;
    long long maximum = 0;
    bool have_value = false;
    bool need_wider = false;
    bool values_known = true;
    for (std::size_t i = 0; i < enumerators.size(); ++i) {
      const AstNode& enumerator = arena_.At(enumerators[i]);
      long long value = 0;
      if (!enumerator.children.empty()) {
        try {
          value = ConstantValue(enumerator.children[0], scope);
        } catch (const std::exception&) {
          values_known = false;
          break;
        }
      } else if (previous != std::numeric_limits<long long>::max()) {
        value = previous + 1;
      } else {
        values_known = false;
        break;
      }
      if (value < std::numeric_limits<int>::min() ||
          value > std::numeric_limits<int>::max())
        need_wider = true;
      if (!have_value || value < minimum) minimum = value;
      if (!have_value || value > maximum) maximum = value;
      have_value = true;
      previous = value;
    }
    if (values_known && have_value && need_wider) {
      const long long unsigned_int_max =
          static_cast<long long>(std::numeric_limits<unsigned int>::max());
      underlying = (minimum >= 0 && maximum <= unsigned_int_max) ?
          types_.Fundamental(FT_UNSIGNED_INT) :
          types_.Fundamental(FT_LONG_INT);
    }
  }
  const TypeId underlying_kind = types_.Unqualified(underlying);
  if (types_.Kind(underlying_kind) != TYPE_FUNDAMENTAL ||
      !FundamentalIsIntegral(types_.At(underlying_kind).fundamental))
    throw std::runtime_error("enum underlying type is not integral");
  return BuildEnumDefinition(node, scope, parent, name, name.Joined(), scoped,
                             underlying, enumerators);
}

TypeId ScopeBuilder::BuildEnumDefinition(AstId node, ScopeId scope,
                                         ScopeId parent,
                                         const QualifiedName& name,
                                         const string& spelling, bool scoped,
                                         TypeId underlying,
                                         const vector<AstId>& enumerators)
{
  const bool definition = arena_.At(node).kind == AST_ENUM_SPECIFIER;
  const BindingId existing = model_.DirectBinding(parent, name.Last(),
                                                  LOOKUP_TYPES);
  EnumEntityId entity = 0;
  TypeId type = 0;
  if (existing != 0)
  {
    type = model_.BindingAt(existing).type;
    if (types_.Kind(type) != TYPE_ENUM)
      throw std::runtime_error("enum name already denotes another type");
    entity = types_.At(type).entity;
    const EnumEntity& previous = model_.EnumAt(entity);
    // 7.2p3: redeclarations agree on scoped-ness and underlying type.
    if (previous.scoped != scoped || previous.underlying != underlying)
      throw std::runtime_error("enum redeclaration disagrees with its type");
    if (definition && previous.defined)
      throw std::runtime_error("enum redefinition");
  }
  else if (name.Qualified())
    throw std::runtime_error("qualified enum definition names no enum");
  else
    entity = model_.CreateEnum(scoped, underlying);

  if (existing == 0 || name.Qualified())
  {
    // The first declaration in a scope, or the out-of-class definition,
    // prints its own type line with its own spelling; the latter also owns
    // the enumerators from here on.
    type = types_.Enum(entity, scoped, underlying, spelling);
    if (scoped)
      model_.EnumAt(entity).enum_scope =
          model_.CreateScope(SCOPE_ENUM, spelling, scope);
    model_.AddBinding(scope, spelling, BINDING_TYPE, type);
  }
  if (!definition)
    return type;
  EnumEntity& enum_entity = model_.EnumAt(entity);
  enum_entity.defined = true;
  if (!scoped)
    enum_entity.enum_scope = parent;
  const ScopeId enumerator_scope = enum_entity.enum_scope;
  BindEnumerators(enumerators, enumerator_scope, type);
  return type;
}

// 7.2p1: an omitted value is the previous value plus one, the first is zero.
void ScopeBuilder::BindEnumerators(const vector<AstId>& enumerators,
                                   ScopeId scope, TypeId type)
{
  long long previous = -1;
  for (std::size_t i = 0; i < enumerators.size(); ++i)
  {
    const AstNode& enumerator = arena_.At(enumerators[i]);
    long long value = 0;
    if (!enumerator.children.empty())
      value = ConstantValue(enumerator.children[0], scope);
    else if (previous == std::numeric_limits<long long>::max())
      throw std::runtime_error("enumerator value overflows");
    else
      value = previous + 1;
    const string name = IdentifierName(enumerators[i]);
    if (model_.DirectBinding(scope, name) != 0)
      throw std::runtime_error("enumerator redeclares a name");
    Binding& binding = model_.BindingAt(model_.AddBinding(
        scope, name, BINDING_ENUMERATOR, type));
    binding.has_const_value = true;
    binding.const_value = value;
    previous = value;
  }
}

AstId ScopeBuilder::FindChild(AstId node, AstKind kind) const
{
  if (node == 0)
    return 0;
  const vector<AstId>& children = arena_.At(node).children;
  for (std::size_t i = 0; i < children.size(); ++i)
    if (children[i] != 0 && arena_.At(children[i]).kind == kind)
      return children[i];
  return 0;
}

// The declarator-id of a (possibly nested) declarator.
AstId ScopeBuilder::FindIdentifier(AstId declarator) const
{
  if (declarator == 0)
    return 0;
  const AstNode& node = arena_.At(declarator);
  if (node.kind == AST_IDENTIFIER)
    return declarator;
  if (node.kind != AST_DECLARATOR && node.kind != AST_ABSTRACT_DECLARATOR &&
      node.kind != AST_NESTED_DECLARATOR)
    return 0;
  for (std::size_t i = 0; i < node.children.size(); ++i)
  {
    const AstId found = FindIdentifier(node.children[i]);
    if (found != 0)
      return found;
  }
  return 0;
}

QualifiedName ScopeBuilder::NodeName(AstId node) const
{
  if (node == 0)
    return QualifiedName();
  const AstNode& value = arena_.At(node);
  // Operator function identifiers are represented by a single AST span
  // beginning at `operator`, while qualified-name parsing expects ordinary
  // identifier components.  Preserve the parser's canonical spelling here
  // so declarations such as `operator delete(void*, void*)` enter the same
  // function entity path as every other unqualified declaration.
  if (value.first < value.last && value.first < tokens_.size() &&
      tokens_[value.first].IsSimple(KW_OPERATOR))
  {
    QualifiedName result;
    result.components.push_back(value.text);
    return result;
  }
  return ReadQualifiedName(tokens_, value.first, value.last);
}

string ScopeBuilder::IdentifierName(AstId identifier) const
{
  const QualifiedName name = NodeName(identifier);
  return name.Empty() ? string() : name.Last();
}

ScopeId ScopeBuilder::ResolveQualifierScope(ScopeId scope,
                                            const QualifiedName& prefix) const
{
  if (prefix.Empty())
    return prefix.global ? model_.GlobalScope() : scope;
  const BindingId binding = model_.Lookup(scope, prefix, LOOKUP_QUALIFIER);
  ScopeId target = 0;
  if (binding == 0 || !model_.NominatedScope(binding, target))
    throw std::runtime_error("qualifier does not name a scope");
  return target;
}

ScopeId ScopeBuilder::ResolveDeclarationScope(ScopeId scope, AstId identifier,
                                              string& name) const
{
  const QualifiedName components = NodeName(identifier);
  if (components.Empty())
    throw std::runtime_error("declarator has no name");
  name = components.Last();
  return components.Qualified() ?
      ResolveQualifierScope(scope, components.Prefix()) : scope;
}

ScopeId ScopeBuilder::ResolveNamespace(ScopeId scope, AstId target) const
{
  const BindingId binding = model_.Lookup(scope, NodeName(target),
                                         LOOKUP_NAMESPACES);
  if (binding == 0)
    throw std::runtime_error("namespace target not found");
  return model_.BindingAt(binding).namespace_scope;
}

TypeKeyword ScopeBuilder::ClassKey(AstId node) const
{
  const AstId key = FindChild(node, AST_CLASS_KEY);
  if (key != 0 && arena_.At(key).first < tokens_.size())
  {
    const Pa6Token& token = tokens_[arena_.At(key).first];
    if (token.IsSimple(KW_STRUCT))
      return TK_STRUCT;
    if (token.IsSimple(KW_UNION))
      return TK_UNION;
  }
  return TK_CLASS;
}

bool ScopeBuilder::SequenceHasKeyword(AstId sequence, ETokenType keyword) const
{
  const vector<AstId>& children = arena_.At(sequence).children;
  for (std::size_t i = 0; i < children.size(); ++i)
  {
    const AstNode& child = arena_.At(children[i]);
    if (child.first < tokens_.size() && child.last == child.first + 1 &&
        tokens_[child.first].IsSimple(keyword))
      return true;
  }
  return false;
}

bool ScopeBuilder::IsDeclarationKind(AstKind kind)
{
  switch (kind)
  {
  case AST_NAMESPACE_DEFINITION: case AST_NAMESPACE_ALIAS_DEFINITION:
  case AST_USING_DIRECTIVE: case AST_USING_DECLARATION:
  case AST_ALIAS_DECLARATION: case AST_SIMPLE_DECLARATION:
  case AST_FUNCTION_DEFINITION: case AST_CLASS_SPECIFIER:
  case AST_CLASS_FORWARD_DECLARATION: case AST_LINKAGE_SPECIFICATION:
  case AST_EMPTY_DECLARATION: case AST_ENUM_SPECIFIER:
  case AST_ENUM_DECLARATION: case AST_STATIC_ASSERT_DECLARATION:
  case AST_TEMPLATE_DECLARATION: case AST_EXPLICIT_INSTANTIATION_DECLARATION:
    return true;
  default:
    return false;
  }
}

SemaId ScopeBuilder::SemanticParent(ScopeId scope, SemaId fallback) const
{
  if (tree_ == 0)
    return 0;
  for (ScopeId current = scope;; current = model_.ScopeAt(current).parent)
  {
    if (current < semantic_scopes_.size() && semantic_scopes_[current] != 0)
      return semantic_scopes_[current];
    if (current == model_.GlobalScope())
      break;
  }
  return fallback != 0 ? fallback : semantic_root_;
}

SemaId ScopeBuilder::MakeSemantic(SemaKind kind, ScopeId scope,
                                  SemaId parent, TypeId type,
                                  BindingId binding,
                                  FunctionEntityId function,
                                  ValueCategory category, ETokenType op,
                                  std::size_t first, std::size_t last)
{
  if (tree_ == 0)
    return 0;
  const SemaId result = tree_->Make(kind);
  SemaNode& node = tree_->At(result);
  node.scope = scope;
  node.type = type;
  node.binding = binding;
  node.function = function;
  node.category = category;
  node.op = op;
  node.first = first;
  node.last = last;
  if (parent == 0)
    parent = SemanticParent(scope);
  if (parent != 0)
    tree_->Append(parent, result);
  return result;
}

void ScopeBuilder::MapSemanticScope(ScopeId scope, SemaId node)
{
  if (tree_ == 0)
    return;
  if (scope >= semantic_scopes_.size())
    semantic_scopes_.resize(scope + 1, 0);
  semantic_scopes_[scope] = node;
}

SemaId ScopeBuilder::MakeDetachedSemantic(SemaKind kind, ScopeId scope,
                                           TypeId type, BindingId binding,
                                           FunctionEntityId function)
{
  if (tree_ == 0)
    return 0;
  const SemaId result = tree_->Make(kind);
  SemaNode& node = tree_->At(result);
  node.scope = scope;
  node.type = type;
  node.binding = binding;
  node.function = function;
  return result;
}

void ScopeBuilder::DeferSemantic(SemaId node)
{
  if (tree_ != 0 && node != 0)
    deferred_semantics_.push_back(node);
}

TypeId ScopeBuilder::TypeForName(const QualifiedName& name, ScopeId scope) const
{
  return LookupType(scope, name);
}

bool ScopeBuilder::HasConstFunctionQualifier(AstId declarator) const
{
  if (declarator == 0)
    return false;
  const AstNode& node = arena_.At(declarator);
  bool have_parameters = false;
  for (std::size_t i = 0; i < node.children.size(); ++i)
  {
    const AstId child = node.children[i];
    const AstNode& value = arena_.At(child);
    if (value.kind == AST_PARAMETER_CLAUSE)
      have_parameters = true;
    else if (have_parameters && value.kind == AST_CV_QUALIFIER &&
             value.first < tokens_.size() &&
             tokens_[value.first].IsSimple(KW_CONST))
      return true;
  }
  return false;
}

bool ScopeBuilder::HasNoexceptQualifier(AstId declarator) const
{
  if (declarator == 0)
    return false;
  const AstNode& node = arena_.At(declarator);
  bool have_parameters = false;
  for (std::size_t i = 0; i < node.children.size(); ++i)
  {
    const AstId child = node.children[i];
    const AstNode& value = arena_.At(child);
    if (value.kind == AST_PARAMETER_CLAUSE)
      have_parameters = true;
    else if (have_parameters &&
             (value.kind == AST_NOEXCEPT_SPECIFICATION ||
              (value.kind == AST_FUNCTION_QUALIFIER &&
               value.text.find("noexcept") != string::npos)))
      return true;
  }
  return false;
}

FunctionEntityId ScopeBuilder::EnsureDefaultConstructor(TypeId type)
{
  const TypeId class_type = types_.Unqualified(type);
  if (types_.Kind(class_type) != TYPE_CLASS)
    throw std::runtime_error("default constructor requires a class type");
  const ClassEntityId class_entity = types_.At(class_type).entity;
  ClassEntity& owner = model_.ClassAt(class_entity);
  if (!owner.defined || owner.class_scope == 0)
    throw std::runtime_error("default constructor requires a defined class");
  if (owner.default_constructor != 0)
    return owner.default_constructor;

  const TypeId this_type = types_.Pointer(class_type);
  const std::vector<TypeId> parameters(1, this_type);
  const TypeId constructor_type = types_.Function(
      types_.Fundamental(FT_VOID), parameters);
  const std::string name = model_.ScopeAt(owner.class_scope).name;
  const FunctionEntityId constructor = model_.CreateFunction(
      owner.class_scope, name, constructor_type);
  FunctionEntity& function = model_.FunctionAt(constructor);
  function.is_member = true;
  function.member_class = class_entity;
  const BindingId binding = model_.AddBinding(
      owner.class_scope, name, BINDING_FUNCTION, constructor_type);
  model_.BindingAt(binding).function = constructor;
  owner.default_constructor = constructor;

  if (EmitsSemantics())
  {
    const SemaId function_node = MakeDetachedSemantic(
        SEMA_FUNCTION_DEFINITION, owner.class_scope, constructor_type,
        binding, constructor);
    DeferSemantic(function_node);
    const ScopeId function_scope = model_.CreateScope(
        SCOPE_FUNCTION, name, owner.class_scope);
    const SemaId this_parameter = tree_->Make(SEMA_PARAMETER);
    SemaNode& parameter = tree_->At(this_parameter);
    parameter.scope = function_scope;
    parameter.type = this_type;
    parameter.binding = model_.AddBinding(
        function_scope, "this", BINDING_PARAMETER, this_type);
    tree_->Append(function_node, this_parameter);
    const SemaId body = MakeSemantic(SEMA_COMPOUND_STATEMENT,
                                     function_scope, function_node);
    MapSemanticScope(function_scope, body);
  }
  return constructor;
}

// `constructor-action A::A` with the synthesized call `A::A(&object)`; the
// object is named by the declarator-id, or by its binding when synthesized.
void ScopeBuilder::AddConstructorAction(SemaId variable, ScopeId scope,
                                        TypeId type, BindingId binding,
                                        AstId declarator)
{
  if (!EmitsSemantics())
    return;
  const FunctionEntityId constructor = EnsureDefaultConstructor(type);
  const FunctionEntity& function = model_.FunctionAt(constructor);
  const SemaId action = tree_->Make(SEMA_CONSTRUCTOR_ACTION);
  SemaNode& action_node = tree_->At(action);
  action_node.scope = scope;
  action_node.function = constructor;
  tree_->Append(variable, action);

  const SemaId call = MakeSemantic(SEMA_CALL, scope, action,
      types_.Fundamental(FT_VOID), 0, constructor, VC_PRVALUE);
  const SemaId callee = tree_->Make(SEMA_CALLEE);
  SemaNode& callee_node = tree_->At(callee);
  callee_node.scope = scope;
  callee_node.type = function.type;
  callee_node.function = constructor;
  tree_->Append(call, callee);

  const TypeId class_type = types_.Unqualified(type);
  const SemaId address = MakeSemantic(SEMA_UNARY, scope, call,
      types_.Pointer(class_type), 0, 0, VC_PRVALUE, OP_AMP);
  const AstId identifier = FindIdentifier(declarator);
  MakeSemantic(SEMA_ID_EXPRESSION, scope, address, type, binding, 0,
               VC_LVALUE, KW_AUTO,
               identifier == 0 ? 0 : arena_.At(identifier).first,
               identifier == 0 ? 0 : arena_.At(identifier).last);
}

void ScopeBuilder::BuildAnonymousUnionStorage(AstId node, ScopeId scope,
                                              SemaId semantic_parent,
                                              TypeId type)
{
  std::size_t first = 0;
  std::size_t last = 0;
  if (!arena_.DeclarationExtent(node, first, last))
    throw std::runtime_error("anonymous union requires a declaration extent");
  std::ostringstream storage;
  storage << "__anonymous_union_storage__" << first << '_' << last;
  const BindingId binding = model_.AddBinding(
      scope, storage.str(), BINDING_VARIABLE, type);
  ClassEntity& owner = model_.ClassAt(types_.At(types_.Unqualified(type)).entity);
  for (std::size_t i = 0; i < owner.injected_members.size(); ++i)
    model_.BindingAt(owner.injected_members[i]).object_binding = binding;
  const SemaId declaration = MakeSemantic(SEMA_SIMPLE_DECLARATION, scope,
      semantic_parent != 0 ? semantic_parent : SemanticParent(scope));
  const SemaId variable = MakeSemantic(SEMA_VARIABLE, scope, declaration,
      type, binding);
  AddConstructorAction(variable, scope, type, binding, 0);
}

void ScopeBuilder::EmitDeferredSemantics()
{
  if (tree_ == 0)
    return;
  for (std::size_t i = 0; i < deferred_semantics_.size(); ++i)
    tree_->Append(semantic_root_, deferred_semantics_[i]);
}

namespace
{

TypeId BoundTemplateType(const std::vector<std::pair<TypeId, TypeId> >& values,
                         TypeId parameter)
{
  for (std::size_t i = 0; i < values.size(); ++i)
    if (values[i].first == parameter)
      return values[i].second;
  return 0;
}

} // namespace

TypeId ScopeBuilder::SubstituteTemplateType(TypeId type,
                                            const TemplateBindings& values)
{
  if (type == 0)
    return 0;
  const TypeNode& node = types_.At(type);
  switch (node.kind)
  {
  case TYPE_TEMPLATE_PARAM:
  {
    const TypeId bound = BoundTemplateType(values, type);
    return bound == 0 ? type : bound;
  }
  case TYPE_CV:
    return types_.Cv(SubstituteTemplateType(node.base, values),
                     node.is_const, node.is_volatile);
  case TYPE_POINTER:
    return types_.Pointer(SubstituteTemplateType(node.base, values));
  case TYPE_REFERENCE:
    return types_.Reference(SubstituteTemplateType(node.base, values),
                            node.lvalue_reference);
  case TYPE_ARRAY:
    return types_.Array(SubstituteTemplateType(node.base, values),
                        node.array_bound);
  case TYPE_FUNCTION:
  {
    std::vector<TypeId> parameters;
    parameters.reserve(node.parameters.size());
    for (std::size_t i = 0; i < node.parameters.size(); ++i)
      parameters.push_back(SubstituteTemplateType(node.parameters[i], values));
    return types_.Function(SubstituteTemplateType(node.result, values),
                           parameters, node.variadic, node.function_const);
  }
  case TYPE_MEMBER_POINTER:
    return types_.MemberPointer(
        SubstituteTemplateType(node.member_class, values),
        SubstituteTemplateType(node.base, values));
  case TYPE_FUNDAMENTAL: case TYPE_CLASS: case TYPE_ENUM:
    return type;
  case TYPE_INVALID:
    break;
  }
  return 0;
}

bool ScopeBuilder::DeduceTemplateType(TypeId pattern, TypeId argument,
                                      TemplateBindings& values) const
{
  if (pattern == 0 || argument == 0)
    return false;
  const TypeNode& expected = types_.At(pattern);
  if (expected.kind == TYPE_TEMPLATE_PARAM)
  {
    const TypeId bound = BoundTemplateType(values, pattern);
    if (bound == 0)
      values.push_back(std::make_pair(pattern, argument));
    return bound == 0 || bound == argument;
  }
  TypeId actual = argument;
  if (expected.kind == TYPE_CV)
  {
    if (types_.Kind(actual) == TYPE_CV)
      actual = types_.At(actual).base;
    return DeduceTemplateType(expected.base, actual, values);
  }
  if (expected.kind == TYPE_POINTER)
  {
    actual = types_.Unqualified(actual);
    return types_.Kind(actual) == TYPE_POINTER &&
        DeduceTemplateType(expected.base, types_.At(actual).base, values);
  }
  if (expected.kind == TYPE_REFERENCE)
  {
    if (types_.Kind(actual) == TYPE_REFERENCE)
      actual = types_.Referent(actual);
    return DeduceTemplateType(expected.base, actual, values);
  }
  if (expected.kind == TYPE_FUNCTION)
  {
    actual = types_.Unqualified(actual);
    if (types_.Kind(actual) != TYPE_FUNCTION ||
        expected.parameters.size() != types_.At(actual).parameters.size())
      return false;
    if (!DeduceTemplateType(expected.result, types_.At(actual).result, values))
      return false;
    for (std::size_t i = 0; i < expected.parameters.size(); ++i)
      if (!DeduceTemplateType(expected.parameters[i],
                              types_.At(actual).parameters[i], values))
        return false;
    return true;
  }
  if (expected.kind == TYPE_MEMBER_POINTER)
  {
    actual = types_.Unqualified(actual);
    return types_.Kind(actual) == TYPE_MEMBER_POINTER &&
        expected.member_class == types_.At(actual).member_class &&
        DeduceTemplateType(expected.base, types_.At(actual).base, values);
  }
  return types_.Unqualified(pattern) == types_.Unqualified(argument);
}

bool ScopeBuilder::BuildTemplateInstance(FunctionEntityId template_function,
                                         const TemplateBindings& values,
                                         FunctionEntityId& function,
                                         BindingId& binding)
{
  const FunctionEntity source = model_.FunctionAt(template_function);
  if (!source.is_template)
    return false;
  std::vector<TypeId> arguments;
  arguments.reserve(source.template_parameters.size());
  for (std::size_t i = 0; i < source.template_parameters.size(); ++i)
  {
    const TypeId bound = BoundTemplateType(values, source.template_parameters[i]);
    if (bound == 0)
      return false;
    arguments.push_back(bound);
  }
  const TypeId instance_type = SubstituteTemplateType(source.type, values);
  for (std::size_t i = 0; i < deferred_template_instances_.size(); ++i)
    if (deferred_template_instances_[i].template_function == template_function &&
        deferred_template_instances_[i].arguments == arguments)
    {
      function = deferred_template_instances_[i].function;
      binding = deferred_template_instances_[i].binding;
      return true;
    }

  const FunctionEntityId instance = model_.CreateFunction(
      source.scope, source.name, instance_type);
  FunctionEntity& concrete = model_.FunctionAt(instance);
  concrete.is_member = source.is_member;
  concrete.member_class = source.member_class;
  concrete.member_const = source.member_const;
  concrete.member_type = source.member_type == 0 ? 0 :
      SubstituteTemplateType(source.member_type, values);
  if (concrete.member_type != 0)
    concrete.member_pointer_type = types_.MemberPointer(
        model_.ClassAt(concrete.member_class).type, concrete.member_type);
  concrete.defined = source.defined;
  ScopeId declaration_scope = source.scope;
  while (model_.ScopeAt(declaration_scope).kind == SCOPE_TEMPLATE_PARAMETERS)
    declaration_scope = model_.ScopeAt(declaration_scope).parent;
  const BindingId instance_binding = model_.AddBinding(
      declaration_scope, source.name, BINDING_FUNCTION, instance_type);
  model_.BindingAt(instance_binding).function = instance;
  deferred_template_instances_.push_back(DeferredTemplateInstance(
      template_function, arguments, instance, instance_binding));
  function = instance;
  binding = instance_binding;
  return true;
}

bool ScopeBuilder::InstantiateFunctionTemplate(
    FunctionEntityId template_function, const std::vector<TypeId>& arguments,
    FunctionEntityId& function, BindingId& binding)
{
  const FunctionEntity& source = model_.FunctionAt(template_function);
  if (!source.is_template ||
      source.template_parameters.size() != arguments.size())
    return false;
  TemplateBindings values;
  for (std::size_t i = 0; i < arguments.size(); ++i)
    values.push_back(std::make_pair(source.template_parameters[i],
                                    arguments[i]));
  return BuildTemplateInstance(template_function, values, function, binding);
}

bool ScopeBuilder::DeduceFunctionTemplate(
    FunctionEntityId template_function, const std::vector<TypeId>& arguments,
    FunctionEntityId& function, BindingId& binding)
{
  const FunctionEntity& source = model_.FunctionAt(template_function);
  if (!source.is_template)
    return false;
  const TypeNode& type = types_.At(types_.Unqualified(source.type));
  const std::size_t offset = source.is_member ? 1 : 0;
  if (type.parameters.size() < offset ||
      (!type.variadic && type.parameters.size() - offset != arguments.size()) ||
      (type.variadic && type.parameters.size() - offset > arguments.size()))
    return false;
  TemplateBindings values;
  for (std::size_t i = 0; i < arguments.size() && i + offset <
       type.parameters.size(); ++i)
    if (!DeduceTemplateType(type.parameters[i + offset], arguments[i], values))
      return false;
  return BuildTemplateInstance(template_function, values, function, binding);
}

void ScopeBuilder::EmitTemplateInstances()
{
  if (tree_ == 0)
    return;
  for (std::size_t i = 0; i < deferred_template_instances_.size(); ++i)
  {
    const DeferredTemplateInstance& instance =
        deferred_template_instances_[i];
    if (!instance.used)
      continue;
    const FunctionEntity& function = model_.FunctionAt(instance.function);
    const SemaId declaration = MakeSemantic(
        SEMA_FUNCTION_DECLARATION, function.scope, semantic_root_,
        function.type, instance.binding, instance.function);
    const ScopeId function_scope = model_.CreateScope(
        SCOPE_FUNCTION, function.name, function.scope);
    const TypeNode& type = types_.At(types_.Unqualified(function.type));
    for (std::size_t parameter = 0; parameter < type.parameters.size();
         ++parameter)
    {
      const std::string name = function.is_member && parameter == 0 ?
          "this" : std::string();
      const BindingId binding = model_.AddBinding(
          function_scope, name, BINDING_PARAMETER, type.parameters[parameter]);
      MakeSemantic(SEMA_PARAMETER, function_scope, declaration,
                   type.parameters[parameter], binding);
    }
  }
}

void ScopeBuilder::MarkTemplateInstanceUsed(FunctionEntityId function)
{
  for (std::size_t i = 0; i < deferred_template_instances_.size(); ++i)
    if (deferred_template_instances_[i].function == function)
      deferred_template_instances_[i].used = true;
}

FunctionEntityId ScopeBuilder::DeclareFunction(ScopeId scope,
                                               const string& name,
                                               TypeId declared_type,
                                               bool definition,
                                               BindingId& binding,
                                               bool member_const,
                                               bool internal_linkage,
                                               bool noexcept_qualifier,
                                               const vector<AstId>&
                                                   default_arguments)
{
  const TypeId unqualified = types_.Unqualified(declared_type);
  if (types_.Kind(unqualified) != TYPE_FUNCTION)
    throw std::runtime_error("declaration is not a function");
  // AdjustParameter may intern additional type nodes and reallocate the
  // table, so keep the source function node by value while canonicalizing it.
  const TypeNode declared = types_.At(unqualified);
  vector<TypeId> parameters;
  parameters.reserve(declared.parameters.size());
  for (std::size_t i = 0; i < declared.parameters.size(); ++i)
    parameters.push_back(types_.AdjustParameter(declared.parameters[i]));
  ClassEntityId member_class = 0;
  const bool is_member = model_.ClassForScope(scope, member_class);
  TypeId member_type = 0;
  vector<TypeId> canonical_parameters = parameters;
  if (is_member)
  {
    const TypeId class_type = model_.ClassAt(member_class).type;
    if (class_type == 0)
      throw std::runtime_error("member function has no class type");
    member_type = types_.Function(declared.result, parameters,
                                  declared.variadic, member_const);
    const TypeId this_type = types_.Pointer(
        member_const ? types_.Cv(class_type, true) : class_type);
    canonical_parameters.insert(canonical_parameters.begin(), this_type);
  }
  const TypeId canonical = types_.Function(declared.result,
                                           canonical_parameters,
                                           declared.variadic);
  vector<AstId> canonical_defaults = default_arguments;
  if (is_member)
    canonical_defaults.insert(canonical_defaults.begin(), 0);

  // 13.1/3.3.10: a prior declaration of the name in this scope with the
  // same canonical signature declares the same function.
  vector<BindingId> priors;
  model_.DirectBindings(scope, name, LOOKUP_ANY, priors);
  for (std::size_t i = 0; i < priors.size(); ++i)
  {
    const Binding& prior = model_.BindingAt(priors[i]);
    // A type-name and a function-name occupy distinct declaration spaces.
    // Keep the type binding visible while allowing the function entity to be
    // canonicalized independently.
    if (prior.kind == BINDING_TYPE || prior.kind == BINDING_TYPE_ALIAS)
      continue;
    if (prior.kind != BINDING_FUNCTION || prior.function == 0)
      throw std::runtime_error("function conflicts with an existing name");
    const TypeNode& prior_type = types_.At(
        model_.FunctionAt(prior.function).type);
    const TypeNode& canonical_type = types_.At(canonical);
    const bool same_parameters =
        prior_type.parameters == canonical_type.parameters &&
        prior_type.variadic == canonical_type.variadic;
    if (same_parameters && model_.FunctionAt(prior.function).type != canonical)
      throw std::runtime_error("function redeclaration changes return type");
    if (model_.FunctionAt(prior.function).type == canonical)
    {
      FunctionEntity& entity = model_.FunctionAt(prior.function);
      if (definition && entity.defined)
        throw std::runtime_error("duplicate function definition");
      if (definition)
        entity.defined = true;
      if (is_member)
      {
        entity.member_type = member_type;
        entity.member_pointer_type = types_.MemberPointer(
            model_.ClassAt(member_class).type, member_type);
        entity.member_class = member_class;
        entity.is_member = true;
        entity.member_const = member_const;
      }
      entity.internal_linkage = entity.internal_linkage || internal_linkage;
      entity.c_linkage = entity.c_linkage || c_linkage_depth_ != 0;
      entity.noexcept_qualifier = entity.noexcept_qualifier ||
          noexcept_qualifier;
      if (entity.default_arguments.size() < canonical_parameters.size())
        entity.default_arguments.resize(canonical_parameters.size(), 0);
      for (std::size_t i = 0; i < canonical_defaults.size() &&
           i < entity.default_arguments.size(); ++i)
        if (canonical_defaults[i] != 0)
          entity.default_arguments[i] = canonical_defaults[i];
      binding = model_.AddBinding(scope, name, BINDING_FUNCTION,
                                  declared_type);
      model_.BindingAt(binding).function = prior.function;
      model_.BindingAt(binding).internal_linkage = entity.internal_linkage;
      model_.BindingAt(binding).c_linkage = entity.c_linkage;
      model_.BindingAt(binding).noexcept_qualifier = entity.noexcept_qualifier;
      return prior.function;
    }
  }
  const FunctionEntityId function = model_.CreateFunction(scope, name,
                                                            canonical);
  model_.FunctionAt(function).defined = definition;
  model_.FunctionAt(function).internal_linkage = internal_linkage;
  model_.FunctionAt(function).c_linkage = c_linkage_depth_ != 0;
  if (is_member)
  {
    FunctionEntity& entity = model_.FunctionAt(function);
    entity.member_type = member_type;
    entity.member_pointer_type = types_.MemberPointer(
        model_.ClassAt(member_class).type, member_type);
    entity.member_class = member_class;
    entity.is_member = true;
    entity.member_const = member_const;
  }
  model_.FunctionAt(function).noexcept_qualifier = noexcept_qualifier;
  model_.FunctionAt(function).default_arguments = canonical_defaults;
  binding = model_.AddBinding(scope, name, BINDING_FUNCTION, declared_type);
  model_.BindingAt(binding).function = function;
  model_.BindingAt(binding).internal_linkage = internal_linkage;
  model_.BindingAt(binding).c_linkage = c_linkage_depth_ != 0;
  return function;
}

bool ScopeBuilder::HasIncompleteArray(AstId declarator) const
{
  if (declarator == 0)
    return false;
  const AstNode& node = arena_.At(declarator);
  if (node.kind == AST_ARRAY_SUFFIX && node.children.empty())
    return true;
  for (std::size_t i = 0; i < node.children.size(); ++i)
    if (HasIncompleteArray(node.children[i]))
      return true;
  return false;
}

std::size_t ScopeBuilder::InitializerBound(AstId initializer) const
{
  if (initializer == 0)
    return 0;
  const AstNode& node = arena_.At(initializer);
  if (node.kind == AST_INITIALIZER || node.kind == AST_PAREN_INITIALIZER)
  {
    if (node.children.size() != 1)
      return 0;
    return InitializerBound(node.children[0]);
  }
  if (node.kind == AST_BRACED_INIT_LIST)
    return node.children.size();
  return 0;
}
