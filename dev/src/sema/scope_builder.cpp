// Declaration collection and scope ownership for the semantic model.
#include "sema/scope_builder.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "sema/overload.h"

using std::string;
using std::vector;

namespace
{

bool IsOverloadedOperatorName(const string& name)
{
  static const char* const suffixes[] = {
    "new", "new[]", "delete", "delete[]", "+", "-", "*", "/", "%",
    "^", "&", "|", "~", "!", "=", "<", ">", "+=", "-=", "*=",
    "/=", "%=", "^=", "&=", "|=", "<<", ">>", "<<=", ">>=", "==",
    "!=", "<=", ">=", "&&", "||", "++", "--", ",", "->*", "->", "()",
    "[]"
  };
  const string prefix = "operator";
  if (name.compare(0, prefix.size(), prefix) != 0)
    return false;
  const string suffix = name.substr(prefix.size());
  if (suffix.compare(0, 2, "\"\"") == 0)
    return true;
  for (std::size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i)
    if (suffix == suffixes[i])
      return true;
  return false;
}

}  // namespace

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
  case AST_SPECIAL_MEMBER_DECLARATION:
  case AST_SPECIAL_MEMBER_DEFINITION:
    BuildSpecialMember(node, scope); return;
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
  ClassEntityId derived = 0;
  if (model_.ClassForScope(scope, derived) && name.Qualified() &&
      name.components.size() == 2 &&
      name.components[0] == name.components[1]) {
    const ClassEntity& owner = model_.ClassAt(derived);
    for (std::size_t i = 0; i < owner.bases.size(); ++i) {
      const ClassEntity& base = model_.ClassAt(owner.bases[i].entity);
      if (model_.ScopeAt(base.class_scope).name == name.components[0]) {
        BuildInheritedConstructors(derived, owner.bases[i].entity, scope);
        return;
      }
    }
  }
  if (target == 0 || model_.BindingAt(target).kind == BINDING_NAMESPACE)
    throw std::runtime_error("using-declaration target not found");
  const Binding source = model_.BindingAt(target);
  Binding& imported = model_.BindingAt(model_.AddBinding(
      scope, name.Last(), source.kind, source.type));
  imported.function = source.function;
  imported.has_const_value = source.has_const_value;
  imported.const_value = source.const_value;
}

void ScopeBuilder::BuildInheritedConstructors(ClassEntityId derived,
                                               ClassEntityId base,
                                               ScopeId scope)
{
  ClassEntity& owner = model_.ClassAt(derived);
  const ClassEntity& base_owner = model_.ClassAt(base);
  const TypeId derived_type = owner.type;
  const std::string name = model_.ScopeAt(scope).name;
  for (std::size_t i = 0; i < base_owner.constructors.size(); ++i) {
    const FunctionEntityId base_constructor =
        base_owner.constructors[i];
    const FunctionEntity& source = model_.FunctionAt(base_constructor);
    if (source.deleted || source.member_type == 0)
      continue;
    const std::vector<AstId> source_default_arguments =
        source.default_arguments;
    const std::vector<std::size_t> source_default_semantic_arguments =
        source.default_semantic_arguments;
    const std::vector<std::string> source_parameter_names =
        source.parameter_names;
    const TypeNode& source_type = types_.At(
        types_.Unqualified(source.type));
    std::vector<TypeId> parameters;
    for (std::size_t parameter = 1;
         parameter < source_type.parameters.size(); ++parameter)
      parameters.push_back(source_type.parameters[parameter]);
    const TypeId function_type = types_.Function(
        types_.Fundamental(FT_VOID),
        [&]() {
          std::vector<TypeId> canonical;
          canonical.push_back(types_.Pointer(derived_type));
          canonical.insert(canonical.end(), parameters.begin(),
                           parameters.end());
          return canonical;
        }(), source_type.variadic);
    const FunctionEntityId inherited = model_.CreateFunction(
        scope, name, function_type);
    FunctionEntity& function = model_.FunctionAt(inherited);
    function.is_member = true;
    function.member_class = derived;
    function.member_type = types_.Function(types_.Fundamental(FT_VOID),
                                           parameters, source_type.variadic);
    function.member_pointer_type = types_.MemberPointer(
        derived_type, function.member_type);
    function.special_member = SPECIAL_MEMBER_CONSTRUCTOR;
    function.in_class_definition = true;
    function.synthesized = true;
    function.default_arguments = source_default_arguments;
    function.default_semantic_arguments = source_default_semantic_arguments;
    function.parameter_names = source_parameter_names;
    const BindingId binding = model_.AddBinding(
        scope, name, BINDING_FUNCTION, function_type);
    model_.BindingAt(binding).function = inherited;
    owner.constructor = inherited;
    owner.constructors.push_back(inherited);

    if (!EmitsSemantics())
      continue;
    const SemaId function_node = MakeDetachedSemantic(
        SEMA_FUNCTION_DEFINITION, scope, function_type, binding, inherited);
    DeferSemantic(function_node);
    const ScopeId function_scope = model_.CreateScope(
        SCOPE_FUNCTION, name, scope);
    const BindingId this_binding = model_.AddBinding(
        function_scope, "this", BINDING_PARAMETER,
        function_type == 0 ? 0 : types_.At(types_.Unqualified(
            function_type)).parameters[0]);
    MakeSemantic(SEMA_PARAMETER, function_scope, function_node,
                 types_.At(types_.Unqualified(function_type)).parameters[0],
                 this_binding);
    const TypeNode& inherited_type = types_.At(
        types_.Unqualified(function_type));
    std::vector<BindingId> parameter_bindings;
    for (std::size_t parameter = 0; parameter < parameters.size();
         ++parameter) {
      const std::string parameter_name = parameter <
          source_parameter_names.size() ? source_parameter_names[parameter] :
          std::string();
      const BindingId parameter_binding = model_.AddBinding(
          function_scope, parameter_name, BINDING_PARAMETER,
          inherited_type.parameters[parameter + 1]);
      parameter_bindings.push_back(parameter_binding);
      MakeSemantic(SEMA_PARAMETER, function_scope, function_node,
                   inherited_type.parameters[parameter + 1],
                   parameter_binding);
    }
    const SemaId member = MakeSemantic(
        SEMA_MEMBER_INITIALIZER, function_scope, function_node,
        base_owner.type, 0, base_constructor);
    for (std::size_t parameter = 0; parameter < parameter_bindings.size();
         ++parameter)
      MakeSemantic(SEMA_ID_EXPRESSION, function_scope, member,
                   inherited_type.parameters[parameter + 1],
                   parameter_bindings[parameter], 0, VC_LVALUE);
    (void)MakeSemantic(SEMA_COMPOUND_STATEMENT, function_scope,
                       function_node);
  }
  owner.inheriting_constructor_base = base;
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
    const AstId identifier = FindIdentifier(declarator);
    const QualifiedName declared_name = NodeName(identifier);
    ScopeId target_scope = ResolveDeclarationScope(scope, identifier, name);
    const bool is_friend = SequenceHasKeyword(specifiers, KW_FRIEND);
    if (is_friend && !declared_name.Qualified())
      target_scope = EnclosingNamespace(scope);
    const AstId initializer = FindChild(items[i], AST_INITIALIZER);
    const std::size_t incomplete_bound =
        HasIncompleteArray(declarator) && initializer == 0 &&
        SequenceHasKeyword(specifiers, KW_EXTERN) ?
            std::numeric_limits<std::size_t>::max() :
            (HasIncompleteArray(declarator) ? InitializerBound(initializer) : 0);
    TypeId type = BuildDeclaratorType(
        declarator, base, is_friend ? scope : target_scope, false,
        incomplete_bound);
    const bool is_function = types_.Kind(type) == TYPE_FUNCTION;
    ClassEntityId member_class = 0;
    const bool is_member = model_.ClassForScope(target_scope, member_class);
    const bool static_member = is_member &&
        SequenceHasKeyword(specifiers, KW_STATIC);
    // 7.1.5p9: a constexpr object is const.
    if (is_constexpr && !is_typedef && !is_function)
      type = types_.Cv(type, true);
    const BindingKind kind = is_typedef ? BINDING_TYPE_ALIAS :
        is_function ? BINDING_FUNCTION : BINDING_VARIABLE;
    BindingId binding = 0;
	    if (is_function && !is_typedef)
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
      const bool had_visible_declaration = [&]() {
        std::vector<BindingId> visible;
        model_.DirectBindings(target_scope, name, LOOKUP_FUNCTIONS, visible);
        return !visible.empty();
      }();
      const FunctionEntityId function = DeclareFunction(
          target_scope, name, type, false, binding,
          HasConstFunctionQualifier(declarator),
          HasVolatileFunctionQualifier(declarator),
          SequenceHasKeyword(specifiers, KW_STATIC),
          IsNoThrowDeclarator(declarator, target_scope), default_arguments);
      if (is_friend && !had_visible_declaration &&
          model_.ClassForScope(scope, member_class))
      {
        model_.BindingAt(binding).hidden_friend = true;
        model_.ClassAt(member_class).hidden_friends.push_back(binding);
      }
      if (EmitsSemantics() && !is_member)
        MakeSemantic(SEMA_FUNCTION_DECLARATION, target_scope,
                     declaration_node != 0 ? declaration_node :
                         SemanticParent(target_scope),
                     model_.FunctionAt(function).type, binding, function);
      continue;
    }
    binding = model_.AddBinding(target_scope, name, kind, type);
    model_.BindingAt(binding).access = is_member ? member_access_ :
        ACCESS_PUBLIC;
    model_.BindingAt(binding).static_member = static_member;
    TypeId linkage_type = type;
    while (linkage_type != 0 &&
           types_.Kind(types_.Unqualified(linkage_type)) == TYPE_ARRAY)
      linkage_type = types_.At(types_.Unqualified(linkage_type)).base;
    const bool namespace_const =
        model_.ScopeAt(target_scope).kind == SCOPE_NAMESPACE &&
        types_.Kind(linkage_type) == TYPE_CV &&
        types_.At(linkage_type).is_const;
    model_.BindingAt(binding).internal_linkage =
        SequenceHasKeyword(specifiers, KW_STATIC) ||
        (model_.ScopeAt(target_scope).kind == SCOPE_NAMESPACE &&
         !SequenceHasKeyword(specifiers, KW_EXTERN) &&
         namespace_const);
    model_.BindingAt(binding).c_linkage = c_linkage_depth_ != 0;
    model_.BindingAt(binding).extern_declaration =
        initializer == 0 && SequenceHasKeyword(specifiers, KW_EXTERN);
    if (kind == BINDING_VARIABLE)
    {
      if (is_member && !static_member)
      {
        ClassField field(binding, type);
        field.access = member_access_;
        field.initializer = initializer;
        model_.ClassAt(member_class).fields.push_back(field);
      }
      if (model_.ScopeAt(target_scope).kind == SCOPE_NAMESPACE)
        LinkRedeclaration(binding, target_scope, name, type);
      else if (initializer != 0 &&
               model_.ScopeAt(target_scope).kind == SCOPE_BLOCK &&
               !SequenceHasKeyword(specifiers, KW_STATIC))
        RecordInitializedLocal(target_scope);
    }
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
  const TypeId unqualified = types_.Unqualified(type);
  if (variable != 0) {
    if (types_.Kind(unqualified) == TYPE_CLASS) {
      EnsureDestructor(type);
    } else if (types_.Kind(unqualified) == TYPE_ARRAY) {
      const TypeId element = types_.Unqualified(types_.At(unqualified).base);
      if (types_.Kind(element) == TYPE_CLASS) {
        if (initializer == 0)
          EnsureDefaultConstructor(element);
        EnsureDestructor(element);
      }
    }
  }
  if (initializer == 0)
  {
    if (is_constexpr)
      throw std::runtime_error("constexpr variable has no initializer");
    if (variable != 0 && types_.Kind(types_.Unqualified(type)) == TYPE_CLASS)
      AddConstructorAction(variable, scope, type, binding, declarator);
    return;
  }
  if (variable != 0 && types_.Kind(unqualified) == TYPE_CLASS)
  {
    AstId value = initializer;
    if (arena_.At(value).kind == AST_INITIALIZER) {
      if (arena_.At(value).children.size() != 1)
        throw std::runtime_error("invalid class initializer");
      value = arena_.At(value).children[0];
    }
    const AstKind value_kind = arena_.At(value).kind;
    if ((value_kind == AST_PAREN_INITIALIZER ||
         value_kind == AST_BRACED_INIT_LIST) &&
        !model_.ClassAt(types_.At(unqualified).entity).aggregate &&
        !model_.ClassAt(types_.At(unqualified).entity).constructors.empty())
    {
      AddConstructorActionWithArguments(
          variable, scope, type, binding, arena_.At(value).children);
      return;
    }
    if (value_kind == AST_BRACED_INIT_LIST &&
        !model_.ClassAt(types_.At(unqualified).entity).aggregate)
      throw std::runtime_error("class is not an aggregate");
    if (value_kind == AST_PAREN_INITIALIZER)
      throw std::runtime_error("class has no viable constructor");
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
  const AstId identifier = FindIdentifier(declarator);
  const QualifiedName declared_name = NodeName(identifier);
  ScopeId target_scope = ResolveDeclarationScope(scope, identifier, name);
  const bool is_friend = SequenceHasKeyword(specifiers, KW_FRIEND);
  if (is_friend && !declared_name.Qualified())
    target_scope = EnclosingNamespace(scope);
  const ScopeId type_scope = is_friend ? scope : target_scope;
  const TypeId base = BuildSpecifierType(specifiers, type_scope);
  const TypeId type = BuildDeclaratorType(declarator, base, type_scope);
  if (types_.Kind(type) != TYPE_FUNCTION)
    throw std::runtime_error("function definition is not a function");
  BindingId binding = 0;
  vector<ParameterInfo> parameters;
  bool variadic = false;
  const AstId clause = FindChild(declarator, AST_PARAMETER_CLAUSE);
  if (clause != 0)
    BuildParameters(clause, type_scope, parameters, variadic);
  vector<AstId> default_arguments;
  for (std::size_t parameter = 0; parameter < parameters.size(); ++parameter)
    default_arguments.push_back(parameters[parameter].default_initializer);
  const FunctionEntityId function = DeclareFunction(
      target_scope, name, type, true, binding,
      HasConstFunctionQualifier(declarator),
      HasVolatileFunctionQualifier(declarator),
      SequenceHasKeyword(specifiers, KW_STATIC),
      IsNoThrowDeclarator(declarator, target_scope), default_arguments);
  ClassEntityId member_class = 0;
  const bool is_member = model_.ClassForScope(target_scope, member_class) &&
      model_.FunctionAt(function).is_member;
  const bool has_implicit_object = is_member &&
      !model_.FunctionAt(function).static_member;
  // In-class definitions and explicitly inline out-of-class definitions
  // have weak ODR linkage in LowIR.  The same bit also tells lowering that
  // an unused inline body may be omitted.
  model_.FunctionAt(function).in_class_definition = is_friend ||
      (is_member &&
       (scope == target_scope || SequenceHasKeyword(specifiers, KW_INLINE)));
  if (is_friend && !declared_name.Qualified() &&
      model_.ClassForScope(scope, member_class))
  {
    bool had_visible_declaration = false;
    std::vector<BindingId> visible;
    model_.DirectBindings(target_scope, name, LOOKUP_FUNCTIONS, visible);
    // The declaration just added is the only entry when this is a hidden
    // friend.  A prior namespace-scope declaration keeps the friend visible
    // through ordinary lookup.
    for (std::size_t i = 0; i < visible.size(); ++i)
      if (visible[i] != binding)
        had_visible_declaration = true;
    if (!had_visible_declaration) {
      model_.BindingAt(binding).hidden_friend = true;
      model_.ClassAt(member_class).hidden_friends.push_back(binding);
    }
  }
  SemaId function_node = 0;
  if (EmitsSemantics())
  {
    if (is_member && scope == target_scope)
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
      SCOPE_FUNCTION, name, is_friend ? scope : target_scope);
  if (function_node != 0)
    MapSemanticScope(function_scope, function_node);

  if (has_implicit_object && function_node != 0)
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
        const std::size_t canonical_index = has_implicit_object ? i + 1 : i;
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
  initialized_locals_.clear();
  jump_sequence_ = 0;
  (void)BuildCompound(body, function_scope, function, 0, 0, function_node);
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
    CheckJumpTarget(gotos_[i].sequence, tree_ != 0 && gotos_[i].node != 0 ?
                        tree_->At(gotos_[i].node).scope : function_scope,
                    label->second.sequence, label->second.scope);
  }
  labels_.clear();
  gotos_.clear();
  initialized_locals_.clear();
}

// Special members are declarations without a decl-specifier-seq in the AST.
// They still enter the same canonical FunctionEntity table as ordinary
// members; the only extra source facts are the constructor-initializer and
// the special-member kind.  Keeping those facts here lets both overload
// resolution and lowering refer to one function identity.
void ScopeBuilder::BuildSpecialMember(AstId node, ScopeId scope)
{
  ClassEntityId member_class = 0;
  if (!model_.ClassForScope(scope, member_class))
    throw std::runtime_error("special member is not declared in a class");
  const AstId declarator = FindChild(node, AST_DECLARATOR);
  if (declarator == 0)
    throw std::runtime_error("special member has no declarator");
  const AstId identifier = FindIdentifier(declarator);
  const std::string spelling = identifier == 0 ? std::string() :
      arena_.At(identifier).text;
  if (spelling.empty())
    throw std::runtime_error("special member has no name");
  const bool destructor = spelling[0] == '~';
  const std::string name = destructor ? spelling : model_.ScopeAt(scope).name;
  const AstId clause = FindChild(declarator, AST_PARAMETER_CLAUSE);
  std::vector<ParameterInfo> parameters;
  bool variadic = false;
  if (clause != 0)
    BuildParameters(clause, scope, parameters, variadic);
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
      scope, name, declared_type, definition, binding, member_const,
      member_volatile,
      false, IsNoThrowDeclarator(declarator, scope), defaults);
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
  entity.in_class_definition = true;
  entity.defaulted = defaulted;
  entity.deleted = deleted;
  if (destructor)
    model_.ClassAt(member_class).destructor = function;
  else
  {
    ClassEntity& owner = model_.ClassAt(member_class);
    owner.constructor = function;
    if (std::find(owner.constructors.begin(), owner.constructors.end(),
                  function) == owner.constructors.end())
      owner.constructors.push_back(function);
  }

  if (tree_ == 0)
    return;
  const SemaKind semantic_kind = definition ? SEMA_FUNCTION_DEFINITION :
      SEMA_FUNCTION_DECLARATION;
  const SemaId function_node = MakeDetachedSemantic(
      semantic_kind, scope, model_.FunctionAt(function).type, binding,
      function);
  DeferSemantic(function_node);
  if (!definition)
    return;

  const ScopeId function_scope = model_.CreateScope(
      SCOPE_FUNCTION, name, scope);
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
  BuildMemberInitializers(model_.FunctionAt(function).ctor_initializer,
                          function_scope,
                          function_node, function);
  labels_.clear();
  gotos_.clear();
  initialized_locals_.clear();
  jump_sequence_ = 0;
  if (model_.FunctionAt(function).body != 0)
    (void)BuildCompound(model_.FunctionAt(function).body, function_scope,
                        function, 0, 0, function_node);
  else
    (void)MakeSemantic(SEMA_COMPOUND_STATEMENT, function_scope,
                       function_node);
  labels_.clear();
  gotos_.clear();
  initialized_locals_.clear();
}

FunctionEntityId ScopeBuilder::ResolveConstructor(
    TypeId type, const std::vector<SemaId>& arguments, ScopeId scope)
{
  const TypeId class_type = types_.Unqualified(type);
  if (types_.Kind(class_type) != TYPE_CLASS)
    throw std::runtime_error("constructor target is not a class");
  const ClassEntityId class_entity = types_.At(class_type).entity;
  ClassEntity& owner = model_.ClassAt(class_entity);
  if (owner.constructors.empty())
  {
    if (arguments.empty())
      return EnsureDefaultConstructor(class_type);
    throw std::runtime_error("class has no viable constructor");
  }

  std::vector<BindingId> candidates;
  const std::vector<BindingId>& bindings =
      model_.ScopeAt(owner.class_scope).bindings;
  for (std::size_t i = 0; i < bindings.size(); ++i)
  {
    const Binding& binding = model_.BindingAt(bindings[i]);
    if (binding.kind != BINDING_FUNCTION || binding.function == 0 ||
        std::find(owner.constructors.begin(), owner.constructors.end(),
                  binding.function) == owner.constructors.end() ||
        model_.FunctionAt(binding.function).deleted)
      continue;
    candidates.push_back(bindings[i]);
  }
  if (candidates.empty())
    throw std::runtime_error("class has only deleted constructors");
  std::vector<OverloadArgument> overload_arguments;
  overload_arguments.push_back(OverloadArgument(
      types_.Pointer(class_type), VC_PRVALUE, false, false, true));
  for (std::size_t i = 0; i < arguments.size(); ++i)
  {
    const SemaNode& argument = tree_->At(arguments[i]);
    const bool null_pointer_constant =
        types_.IsNullPointerType(argument.type) ||
        (argument.has_value && argument.value == 0 &&
         types_.IsIntegral(argument.type));
    overload_arguments.push_back(OverloadArgument(
        argument.type, argument.category,
        null_pointer_constant,
        argument.kind == SEMA_ID_EXPRESSION &&
            types_.Kind(types_.Unqualified(argument.type)) == TYPE_FUNCTION));
  }
  const FunctionEntityId selected = SelectBestOverload(
      model_, types_, candidates, overload_arguments, true);
  if (selected == 0)
    throw std::runtime_error("no viable constructor");
  (void)scope;
  return selected;
}

FunctionEntityId ScopeBuilder::ResolveConstructorForExpression(
    TypeId type, const std::vector<SemaId>& arguments, ScopeId scope)
{
  return ResolveConstructor(type, arguments, scope);
}

void ScopeBuilder::BuildMemberInitializers(AstId initializer,
                                           ScopeId function_scope,
                                           SemaId function_node,
                                           FunctionEntityId owner_function)
{
  if (initializer == 0)
    return;
  const FunctionEntity& owner_function_entity =
      model_.FunctionAt(owner_function);
  const ClassEntity& owner = model_.ClassAt(owner_function_entity.member_class);
  const std::vector<AstId>& initializers = arena_.At(initializer).children;
  for (std::size_t i = 0; i < initializers.size(); ++i)
  {
    const AstId mem = initializers[i];
    const AstId id = FindChild(mem, AST_MEM_INITIALIZER_ID);
    if (id == 0)
      throw std::runtime_error("mem-initializer has no target");
    const QualifiedName name = ReadQualifiedName(
        tokens_, arena_.At(id).first, arena_.At(id).last);
    if (name.Empty())
      throw std::runtime_error("mem-initializer has an invalid target");
    const AstId argument_list = arena_.At(mem).children.size() > 1 ?
        arena_.At(mem).children[1] : 0;
    if (argument_list == 0)
      throw std::runtime_error("mem-initializer has no arguments");

    BindingId field_binding = 0;
    TypeId target_type = 0;
    FunctionEntityId target_constructor = 0;
    for (std::size_t field = 0; field < owner.fields.size(); ++field)
    {
      const Binding& binding = model_.BindingAt(owner.fields[field].binding);
      if (!owner.fields[field].static_member && binding.name == name.Last())
      {
        field_binding = owner.fields[field].binding;
        target_type = owner.fields[field].type;
        break;
      }
    }
    if (field_binding == 0)
    {
      for (std::size_t base = 0; base < owner.bases.size(); ++base)
      {
        const ClassEntity& base_entity =
            model_.ClassAt(owner.bases[base].entity);
        if (model_.ScopeAt(base_entity.class_scope).name == name.Last())
        {
          target_type = base_entity.type;
          break;
        }
      }
    }
    if (target_type == 0)
      throw std::runtime_error("mem-initializer names no direct subobject");

    const TypeId target_unqualified = types_.Unqualified(target_type);
    const bool empty_array_value_initializer =
        types_.Kind(target_unqualified) == TYPE_ARRAY &&
        (arena_.At(argument_list).kind == AST_PAREN_INITIALIZER ||
         arena_.At(argument_list).kind == AST_PAREN_ARGUMENT_LIST) &&
        arena_.At(argument_list).children.empty();
    const bool aggregate_initializer =
        types_.Kind(target_unqualified) == TYPE_CLASS &&
        model_.ClassAt(types_.At(target_unqualified).entity).aggregate &&
        arena_.At(argument_list).kind == AST_BRACED_INIT_LIST;
    std::vector<SemaId> arguments;
    if (empty_array_value_initializer) {
      const SemaId zero = tree_->Make(SEMA_BRACED_INIT_LIST);
      SemaNode& zero_node = tree_->At(zero);
      zero_node.scope = function_scope;
      zero_node.type = target_type;
      zero_node.category = VC_PRVALUE;
      arguments.push_back(zero);
    } else if (aggregate_initializer) {
      arguments.clear();
      arguments.push_back(expression_.AnalyzeInitializer(
          argument_list, function_scope, target_type));
    } else {
      const std::vector<AstId>& argument_nodes =
          arena_.At(argument_list).children;
      for (std::size_t argument = 0; argument < argument_nodes.size();
           ++argument)
        arguments.push_back(expression_.Analyze(argument_nodes[argument],
                                                function_scope));
    }
    if (!aggregate_initializer && types_.Kind(target_unqualified) == TYPE_CLASS)
    {
      target_constructor = ResolveConstructor(target_type, arguments,
                                               function_scope);
      const TypeNode& constructor_type = types_.At(types_.Unqualified(
          model_.FunctionAt(target_constructor).type));
      std::vector<SemaId> converted;
      converted.reserve(constructor_type.parameters.size() - 1);
      for (std::size_t argument = 0; argument < arguments.size(); ++argument)
        converted.push_back(expression_.Initialize(
            arguments[argument], constructor_type.parameters[argument + 1]));
      const FunctionEntity& constructor = model_.FunctionAt(target_constructor);
      for (std::size_t parameter = arguments.size() + 1;
           parameter < constructor_type.parameters.size(); ++parameter)
      {
        if (parameter >= constructor.default_arguments.size() ||
            constructor.default_arguments[parameter] == 0)
          throw std::runtime_error("missing constructor argument");
        converted.push_back(expression_.AnalyzeInitializer(
            constructor.default_arguments[parameter], constructor.scope,
            constructor_type.parameters[parameter]));
      }
      arguments.swap(converted);
    }
    else if (!aggregate_initializer && !empty_array_value_initializer)
    {
      if (arguments.size() != 1)
        throw std::runtime_error("scalar mem-initializer needs one argument");
      arguments[0] = expression_.Initialize(arguments[0], target_type);
    }

    const SemaId member = MakeSemantic(
        SEMA_MEMBER_INITIALIZER, function_scope, function_node, target_type,
        field_binding, target_constructor, VC_PRVALUE, KW_AUTO,
        arena_.At(mem).first, arena_.At(mem).last);
    for (std::size_t argument = 0; argument < arguments.size(); ++argument)
      tree_->Append(member, arguments[argument]);
  }
}

void ScopeBuilder::AddConstructorActionWithArguments(
    SemaId variable, ScopeId scope, TypeId type, BindingId binding,
    const std::vector<AstId>& argument_nodes)
{
  if (!EmitsSemantics())
    return;
  std::vector<SemaId> arguments;
  for (std::size_t i = 0; i < argument_nodes.size(); ++i)
    arguments.push_back(expression_.Analyze(argument_nodes[i], scope));
  const FunctionEntityId constructor = ResolveConstructor(type, arguments,
                                                           scope);
  const FunctionEntity& function = model_.FunctionAt(constructor);
  const TypeNode& callable = types_.At(types_.Unqualified(function.type));
  std::vector<SemaId> converted;
  converted.reserve(callable.parameters.size());
  for (std::size_t i = 0; i < arguments.size(); ++i)
    converted.push_back(expression_.Initialize(arguments[i],
                                               callable.parameters[i + 1]));
  for (std::size_t parameter = arguments.size() + 1;
       parameter < callable.parameters.size(); ++parameter)
  {
    if (parameter >= function.default_arguments.size() ||
        function.default_arguments[parameter] == 0)
      throw std::runtime_error("missing constructor argument");
    converted.push_back(expression_.AnalyzeInitializer(
        function.default_arguments[parameter], function.scope,
        callable.parameters[parameter]));
  }

  const SemaId action = tree_->Make(SEMA_CONSTRUCTOR_ACTION);
  SemaNode& action_node = tree_->At(action);
  action_node.scope = scope;
  action_node.type = type;
  action_node.binding = binding;
  action_node.function = constructor;
  tree_->Append(variable, action);
  const SemaId call = MakeSemantic(SEMA_CALL, scope, action,
                                   types_.Fundamental(FT_VOID), 0,
                                   constructor, VC_PRVALUE);
  const SemaId callee = tree_->Make(SEMA_CALLEE);
  SemaNode& callee_node = tree_->At(callee);
  callee_node.scope = scope;
  callee_node.type = function.type;
  callee_node.function = constructor;
  tree_->Append(call, callee);
  const SemaId address = MakeSemantic(SEMA_UNARY, scope, call,
      types_.Pointer(types_.Unqualified(type)), 0, 0, VC_PRVALUE, OP_AMP);
  MakeSemantic(SEMA_ID_EXPRESSION, scope, address, type, binding, 0,
               VC_LVALUE, KW_AUTO);
  for (std::size_t i = 0; i < converted.size(); ++i)
    tree_->Append(call, converted[i]);
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

  // Resolve direct bases while the completed class name is already visible
  // to its own member scope.  The entity stores base ids, not copied types,
  // so later layout and member lookup follow the same canonical hierarchy.
  const AstId base_clause = FindChild(node, AST_BASE_CLAUSE);
  if (base_clause != 0)
  {
    const std::vector<AstId>& bases = arena_.At(base_clause).children;
    for (std::size_t i = 0; i < bases.size(); ++i)
    {
      const AstId base = bases[i];
      const AstId base_name = FindChild(base, AST_BASE_NAME);
      if (base_name == 0)
        throw std::runtime_error("base specifier has no type name");
      const TypeId base_type = LookupType(class_scope, NodeName(base_name));
      const TypeId unqualified_base = types_.Unqualified(base_type);
      if (types_.Kind(unqualified_base) != TYPE_CLASS)
        throw std::runtime_error("base specifier does not name a class");
      AccessKind access = key == TK_CLASS ? ACCESS_PRIVATE : ACCESS_PUBLIC;
      const AstId access_node = FindChild(base, AST_ACCESS_SPECIFIER);
      if (access_node != 0 && arena_.At(access_node).first < tokens_.size())
      {
        switch (tokens_[arena_.At(access_node).first].simple_type)
        {
        case KW_PRIVATE: access = ACCESS_PRIVATE; break;
        case KW_PROTECTED: access = ACCESS_PROTECTED; break;
        case KW_PUBLIC: access = ACCESS_PUBLIC; break;
        default: break;
        }
      }
      model_.ClassAt(entity).bases.push_back(
          ClassBase(types_.At(unqualified_base).entity, access));
    }
  }

  const AccessKind saved_access = member_access_;
  const ClassEntityId saved_class = current_class_;
  member_access_ = key == TK_CLASS ? ACCESS_PRIVATE : ACCESS_PUBLIC;
  current_class_ = entity;
  for (std::size_t i = 0; i < value.children.size(); ++i)
  {
    const AstKind kind = arena_.At(value.children[i]).kind;
    if (kind == AST_ACCESS_SPECIFIER)
    {
      const AstNode& access = arena_.At(value.children[i]);
      if (access.first < tokens_.size())
      {
        switch (tokens_[access.first].simple_type)
        {
        case KW_PUBLIC: member_access_ = ACCESS_PUBLIC; break;
        case KW_PROTECTED: member_access_ = ACCESS_PROTECTED; break;
        case KW_PRIVATE: member_access_ = ACCESS_PRIVATE; break;
        default: break;
        }
      }
      continue;
    }
    if (kind != AST_CLASS_KEY && kind != AST_BASE_CLAUSE)
      BuildNode(value.children[i], class_scope);
  }
  member_access_ = saved_access;
  current_class_ = saved_class;
  CompleteClassLayout(entity);
  BuildDefaultMemberInitializers(entity);
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

// Default member initializers are complete-class-context expressions
// (9.2p2): a constructor declared before a later field still sees that field.
// Analyze them after the whole class body has been collected, and retain the
// resulting nodes on the constructor entity without adding synthetic nodes
// to the PA12 source-shaped semantic dump.
void ScopeBuilder::BuildDefaultMemberInitializers(ClassEntityId entity)
{
  if (!EmitsSemantics())
    return;
  const ClassEntity& owner = model_.ClassAt(entity);
  for (std::size_t constructor_index = 0;
       constructor_index < owner.constructors.size(); ++constructor_index) {
    const FunctionEntityId constructor = owner.constructors[constructor_index];
    const FunctionEntity& function = model_.FunctionAt(constructor);
    if (function.deleted)
      continue;
    SemaId function_node = 0;
    for (std::size_t i = 0; i < deferred_semantics_.size(); ++i) {
      const SemaId candidate = deferred_semantics_[i];
      if (candidate != 0 && tree_->At(candidate).function == constructor &&
          tree_->At(candidate).kind == SEMA_FUNCTION_DEFINITION) {
        function_node = candidate;
        break;
      }
    }
    if (function_node == 0)
      continue;
    ScopeId function_scope = 0;
    for (SemaId child = tree_->At(function_node).first_child; child != 0;
         child = tree_->At(child).next_sibling) {
      if (tree_->At(child).kind == SEMA_COMPOUND_STATEMENT) {
        function_scope = tree_->At(child).scope;
        break;
      }
      if (tree_->At(child).kind == SEMA_PARAMETER && function_scope == 0)
        function_scope = tree_->At(child).scope;
    }
    if (function_scope == 0)
      continue;
    std::vector<std::pair<BindingId, std::size_t> > defaults;
    for (std::size_t field_index = 0; field_index < owner.fields.size();
         ++field_index) {
      const ClassField& field = owner.fields[field_index];
      if (field.static_member || field.initializer == 0)
        continue;
      AstId source = field.initializer;
      if (arena_.At(source).kind == AST_INITIALIZER &&
          arena_.At(source).children.size() == 1)
        source = arena_.At(source).children[0];
      SemaId value = 0;
      const TypeId field_unqualified = types_.Unqualified(field.type);
      if (types_.Kind(field_unqualified) == TYPE_CLASS &&
          arena_.At(source).kind == AST_CALL_EXPRESSION &&
          !model_.ClassAt(types_.At(field_unqualified).entity).aggregate) {
        const std::vector<AstId>& call_children = arena_.At(source).children;
        if (call_children.size() != 2)
          throw std::runtime_error("invalid default member constructor");
        const std::vector<AstId>& argument_nodes =
            arena_.At(call_children[1]).children;
        std::vector<SemaId> arguments;
        for (std::size_t argument = 0; argument < argument_nodes.size();
             ++argument)
          arguments.push_back(expression_.Analyze(argument_nodes[argument],
                                                  function_scope));
        const FunctionEntityId target_constructor = ResolveConstructor(
            field.type, arguments, function_scope);
        const FunctionEntity& constructor =
            model_.FunctionAt(target_constructor);
        const TypeNode& constructor_type = types_.At(types_.Unqualified(
            constructor.type));
        std::vector<SemaId> converted;
        for (std::size_t argument = 0; argument < arguments.size();
             ++argument)
          converted.push_back(expression_.Initialize(
              arguments[argument], constructor_type.parameters[argument + 1]));
        for (std::size_t parameter = arguments.size() + 1;
             parameter < constructor_type.parameters.size(); ++parameter) {
          if (parameter >= constructor.default_arguments.size() ||
              constructor.default_arguments[parameter] == 0)
            throw std::runtime_error("missing default member constructor argument");
          converted.push_back(expression_.AnalyzeInitializer(
              constructor.default_arguments[parameter], constructor.scope,
              constructor_type.parameters[parameter]));
        }
        value = tree_->Make(SEMA_MEMBER_INITIALIZER);
        SemaNode& member = tree_->At(value);
        member.scope = function_scope;
        member.type = field.type;
        member.binding = field.binding;
        member.function = target_constructor;
        member.category = VC_PRVALUE;
        for (std::size_t argument = 0; argument < converted.size();
             ++argument)
          tree_->Append(value, converted[argument]);
      } else
        value = expression_.AnalyzeInitializer(field.initializer,
                                               function_scope, field.type);
      defaults.push_back(std::make_pair(field.binding, value));
    }
    model_.FunctionAt(constructor).default_member_initializers.swap(defaults);
  }
}

namespace
{

std::size_t AlignUp(std::size_t value, std::size_t alignment)
{
  if (alignment <= 1)
    return value;
  const std::size_t remainder = value % alignment;
  return remainder == 0 ? value : value + alignment - remainder;
}

bool LayoutKnown(const SemaModel& model, const TypeTable& types, TypeId type)
{
  const TypeNode& node = types.At(type);
  switch (node.kind)
  {
  case TYPE_CV: case TYPE_REFERENCE:
    return LayoutKnown(model, types, node.base);
  case TYPE_FUNDAMENTAL:
    return FundamentalSize(node.fundamental) != 0;
  case TYPE_POINTER: case TYPE_MEMBER_POINTER: case TYPE_ENUM:
    return true;
  case TYPE_ARRAY:
    return node.array_bound != 0 && LayoutKnown(model, types, node.base);
  case TYPE_CLASS:
    return model.ClassAt(node.entity).layout_complete;
  case TYPE_TEMPLATE_PARAM: case TYPE_FUNCTION: case TYPE_INVALID:
    return false;
  }
  return false;
}

} // namespace

void ScopeBuilder::CompleteClassLayout(ClassEntityId entity)
{
  ClassEntity& value = model_.ClassAt(entity);
  if (value.layout_complete)
    return;

  // A primary class template is a layout pattern until its template
  // parameters are substituted.  Preserve the fields for lookup, but defer
  // sizeof/alignment and offset assignment while one of them is incomplete.
  for (std::size_t i = 0; i < value.fields.size(); ++i)
    if (!LayoutKnown(model_, types_, value.fields[i].type))
      return;

  std::size_t offset = 0;
  std::size_t alignment = 1;
  std::size_t size = 0;
  for (std::size_t i = 0; i < value.bases.size(); ++i)
  {
    const ClassBase& base = value.bases[i];
    const ClassEntity& base_entity = model_.ClassAt(base.entity);
    if (!base_entity.layout_complete)
      throw std::runtime_error("base class is incomplete");
    const std::size_t base_alignment = base_entity.alignment;
    const std::size_t base_size = base_entity.size;
    alignment = std::max(alignment, base_alignment);
    if (value.is_union)
      value.bases[i].offset = 0;
    else
    {
      offset = AlignUp(offset, base_alignment);
      value.bases[i].offset = offset;
      offset += base_size;
    }
    size = std::max(size, value.is_union ? base_size : offset);
  }

  for (std::size_t i = 0; i < value.fields.size(); ++i)
  {
    ClassField& field = value.fields[i];
    if (field.static_member)
      continue;
    const std::size_t field_alignment = types_.AlignOf(field.type);
    const std::size_t field_size = types_.SizeOf(field.type);
    alignment = std::max(alignment, field_alignment);
    if (value.is_union)
      field.offset = 0;
    else
    {
      offset = AlignUp(offset, field_alignment);
      field.offset = offset;
      offset += field_size;
    }
    size = std::max(size, value.is_union ? field_size : offset);
  }

  // C++ gives every complete class object a nonzero size, and an object's
  // size is a multiple of its alignment.
  if (size == 0)
    size = 1;
  size = AlignUp(size, alignment);
  value.size = size;
  value.alignment = alignment;
  value.layout_complete = true;
  value.aggregate = value.bases.empty();
  for (std::size_t i = 0; i < value.fields.size(); ++i) {
    const ClassField& field = value.fields[i];
    if (field.static_member)
      continue;
    if (field.access != ACCESS_PUBLIC || field.initializer != 0)
      value.aggregate = false;
  }
  for (std::size_t i = 0; i < value.constructors.size(); ++i) {
    const FunctionEntity& constructor =
        model_.FunctionAt(value.constructors[i]);
    if (!constructor.synthesized && !constructor.defaulted &&
        !constructor.deleted)
      value.aggregate = false;
  }
  value.trivial_default_constructor = true;
  for (std::size_t i = 0; i < value.constructors.size(); ++i)
    if (!model_.FunctionAt(value.constructors[i]).synthesized &&
        !model_.FunctionAt(value.constructors[i]).defaulted &&
        !model_.FunctionAt(value.constructors[i]).deleted)
      value.trivial_default_constructor = false;
  for (std::size_t i = 0; i < value.fields.size(); ++i)
    if (value.fields[i].initializer != 0)
      value.trivial_default_constructor = false;
    else {
      const TypeId field_type = types_.Unqualified(value.fields[i].type);
      if (types_.Kind(field_type) == TYPE_CLASS &&
          !model_.ClassAt(types_.At(field_type).entity)
              .trivial_default_constructor)
        value.trivial_default_constructor = false;
  }
  for (std::size_t i = 0; i < value.bases.size(); ++i)
    if (!model_.ClassAt(value.bases[i].entity).trivial_default_constructor)
      value.trivial_default_constructor = false;
  types_.SetClassLayout(entity, size, alignment);
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

ScopeId ScopeBuilder::EnclosingNamespace(ScopeId scope) const
{
  ScopeId current = scope;
  while (model_.ScopeAt(current).kind != SCOPE_NAMESPACE) {
    if (current == model_.GlobalScope())
      return current;
    current = model_.ScopeAt(current).parent;
  }
  return current;
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

bool ScopeBuilder::HasVolatileFunctionQualifier(AstId declarator) const
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
             tokens_[value.first].IsSimple(KW_VOLATILE))
      return true;
  }
  return false;
}

// 15.4: `noexcept`, `noexcept(constant)` with a true constant, and the empty
// dynamic specification `throw()` promise not to throw.  The parser joins
// each specification into one function-qualifier node whose first token
// names the form; a noexcept operand is that node's only child.
bool ScopeBuilder::IsNoThrowDeclarator(AstId declarator, ScopeId scope)
{
  if (declarator == 0)
    return false;
  const AstNode& node = arena_.At(declarator);
  bool have_parameters = false;
  for (std::size_t i = 0; i < node.children.size(); ++i)
  {
    const AstNode& value = arena_.At(node.children[i]);
    if (value.kind == AST_PARAMETER_CLAUSE)
    {
      have_parameters = true;
      continue;
    }
    if (!have_parameters || value.kind != AST_FUNCTION_QUALIFIER ||
        value.first >= value.last || value.first >= tokens_.size())
      continue;
    const Pa6Token& keyword = tokens_[value.first];
    if (keyword.IsSimple(KW_NOEXCEPT))
      return value.children.empty() || value.children[0] == 0 ||
          ConstantValue(value.children[0], scope) != 0;
    if (keyword.IsSimple(KW_THROW))
      return value.last - value.first == 3; // `throw ( )`
  }
  return false;
}

// 3.3.10/3.5: a namespace-scope object declared again in the same scope is
// the same entity.  The later binding records the first one so a consumer
// that needs one symbol per object has it without repeating the lookup.
void ScopeBuilder::LinkRedeclaration(BindingId binding, ScopeId scope,
                                     const string& name, TypeId type)
{
  vector<BindingId> priors;
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
    model_.BindingAt(binding).redeclared_binding =
        prior.redeclared_binding != 0 ? prior.redeclared_binding : priors[i];
    return;
  }
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

void ScopeBuilder::RecordInitializedLocal(ScopeId scope)
{
  initialized_locals_[scope].push_back(++jump_sequence_);
}

// 6.7p3: a jump may not enter the scope of an automatic object that has an
// initializer while bypassing that initializer.  Walking outward from the
// target, every block scope that does not contain the source is entered, so
// an initialized object declared in it before the target is bypassed; once
// the walk reaches a scope that contains the source, only objects declared
// between a forward source and the target are bypassed.
void ScopeBuilder::CheckJumpTarget(unsigned source_sequence,
                                   ScopeId source_scope,
                                   unsigned target_sequence,
                                   ScopeId target_scope) const
{
  for (ScopeId scope = target_scope;
       scope != 0 && model_.ScopeAt(scope).kind == SCOPE_BLOCK;
       scope = model_.ScopeAt(scope).parent)
  {
    const std::map<ScopeId, std::vector<unsigned> >::const_iterator locals =
        initialized_locals_.find(scope);
    if (locals == initialized_locals_.end())
      continue;
    unsigned lower = 0;
    for (ScopeId probe = source_scope; probe != 0 && lower == 0;
         probe = model_.ScopeAt(probe).parent)
      if (probe == scope)
        lower = source_sequence;
    const std::vector<unsigned>& sequences = locals->second;
    const std::vector<unsigned>::const_iterator first =
        std::upper_bound(sequences.begin(), sequences.end(), lower);
    if (first != sequences.end() && *first < target_sequence)
      throw std::runtime_error("jump bypasses variable initialization");
  }
}

bool ScopeBuilder::HasNontrivialDestructor(ClassEntityId entity) const
{
  const ClassEntity& owner = model_.ClassAt(entity);
  if (owner.destructor != 0 &&
      !model_.FunctionAt(owner.destructor).synthesized)
    return true;
  for (std::size_t i = 0; i < owner.bases.size(); ++i)
    if (HasNontrivialDestructor(owner.bases[i].entity))
      return true;
  for (std::size_t i = 0; i < owner.fields.size(); ++i) {
    if (owner.fields[i].static_member)
      continue;
    TypeId field_type = types_.Unqualified(owner.fields[i].type);
    while (types_.Kind(field_type) == TYPE_ARRAY)
      field_type = types_.Unqualified(types_.At(field_type).base);
    if (types_.Kind(field_type) == TYPE_CLASS &&
        HasNontrivialDestructor(types_.At(field_type).entity))
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
  if (!owner.constructors.empty())
  {
    std::vector<SemaId> no_arguments;
    return ResolveConstructor(class_type, no_arguments, owner.class_scope);
  }

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
  function.member_type = types_.Function(types_.Fundamental(FT_VOID),
                                         std::vector<TypeId>());
  function.member_pointer_type = types_.MemberPointer(
      class_type, function.member_type);
  function.special_member = SPECIAL_MEMBER_CONSTRUCTOR;
  function.in_class_definition = true;
  function.synthesized = true;
  const BindingId binding = model_.AddBinding(
      owner.class_scope, name, BINDING_FUNCTION, constructor_type);
  model_.BindingAt(binding).function = constructor;
  owner.default_constructor = constructor;
  owner.constructor = constructor;
  owner.constructors.push_back(constructor);

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
  BuildDefaultMemberInitializers(class_entity);
  return constructor;
}

FunctionEntityId ScopeBuilder::EnsureDestructor(TypeId type)
{
  const TypeId class_type = types_.Unqualified(type);
  if (types_.Kind(class_type) != TYPE_CLASS)
    throw std::runtime_error("destructor requires a class type");
  const ClassEntityId class_entity = types_.At(class_type).entity;
  ClassEntity& owner = model_.ClassAt(class_entity);
  if (!owner.defined || owner.class_scope == 0)
    throw std::runtime_error("destructor requires a defined class");
  if (owner.destructor != 0)
    return owner.destructor;
  if (!HasNontrivialDestructor(class_entity))
    return 0;

  const TypeId this_type = types_.Pointer(class_type);
  const std::vector<TypeId> parameters(1, this_type);
  const TypeId destructor_type = types_.Function(
      types_.Fundamental(FT_VOID), parameters);
  const std::string name = "~" + model_.ScopeAt(owner.class_scope).name;
  const FunctionEntityId destructor = model_.CreateFunction(
      owner.class_scope, name, destructor_type);
  FunctionEntity& function = model_.FunctionAt(destructor);
  function.is_member = true;
  function.member_class = class_entity;
  function.member_type = types_.Function(types_.Fundamental(FT_VOID),
                                         std::vector<TypeId>());
  function.member_pointer_type = types_.MemberPointer(
      class_type, function.member_type);
  function.special_member = SPECIAL_MEMBER_DESTRUCTOR;
  function.in_class_definition = true;
  function.synthesized = true;
  const BindingId binding = model_.AddBinding(
      owner.class_scope, name, BINDING_FUNCTION, destructor_type);
  model_.BindingAt(binding).function = destructor;
  owner.destructor = destructor;

  if (EmitsSemantics())
  {
    const SemaId function_node = MakeDetachedSemantic(
        SEMA_FUNCTION_DEFINITION, owner.class_scope, destructor_type,
        binding, destructor);
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
    function.body = 0;
  }
  return destructor;
}

// `constructor-action A::A` with the synthesized call `A::A(&object)`; the
// object is named by the declarator-id, or by its binding when synthesized.
void ScopeBuilder::AddConstructorAction(SemaId variable, ScopeId scope,
                                        TypeId type, BindingId binding,
                                        AstId declarator)
{
  (void)declarator;
  AddConstructorActionWithArguments(variable, scope, type, binding,
                                    std::vector<AstId>());
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
  // Class bodies are completed before the enclosing translation unit is
  // walked.  Replaying their definitions in reverse declaration order
  // preserves the canonical LowIR order: callers and the most recently
  // declared overload are encountered first, while the source tree remains
  // the owner of every deferred node.
  for (std::size_t i = deferred_semantics_.size(); i != 0; --i)
    tree_->Append(semantic_root_, deferred_semantics_[i - 1]);
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
  concrete.member_volatile = source.member_volatile;
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
                                               bool member_volatile,
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
  // 13.5p3: an overloaded non-member operator must have at least one
  // class-or-enumeration parameter.  Allocation/deallocation functions and
  // literal operators are the language-defined exceptions; they are
  // ordinary namespace functions even though their names begin with
  // `operator`.
  const bool operator_name = IsOverloadedOperatorName(name);
  const bool literal_operator = name.compare(
      0, std::string("operator\"\"").size(), "operator\"\"") == 0;
  const bool allocation_operator = name == "operatornew" ||
      name == "operatordelete" || name == "operatornew[]" ||
      name == "operatordelete[]";
  if (operator_name && !is_member && !literal_operator &&
      !allocation_operator)
  {
    bool has_class_or_enum_parameter = false;
    for (std::size_t parameter = 0; parameter < parameters.size();
         ++parameter)
    {
      TypeId candidate = parameters[parameter];
      if (types_.Kind(candidate) == TYPE_REFERENCE)
        candidate = types_.Referent(candidate);
      candidate = types_.Unqualified(candidate);
      if (types_.Kind(candidate) == TYPE_CLASS ||
          types_.Kind(candidate) == TYPE_ENUM)
      {
        has_class_or_enum_parameter = true;
        break;
      }
    }
    if (!has_class_or_enum_parameter)
      throw std::runtime_error(
          "non-member overloaded operator needs a class or enum parameter");
  }
  // For a class member the declaration's existing `static` linkage fact is
  // also the canonical fact that no implicit object parameter is present.
  const bool static_member = is_member && internal_linkage;
  const bool effective_internal_linkage = is_member ? false : internal_linkage;
  TypeId member_type = 0;
  vector<TypeId> canonical_parameters = parameters;
  if (is_member)
  {
    const TypeId class_type = model_.ClassAt(member_class).type;
    if (class_type == 0)
      throw std::runtime_error("member function has no class type");
    member_type = types_.Function(declared.result, parameters,
                                  declared.variadic, member_const);
    if (!static_member)
    {
      const TypeId this_type = types_.Pointer(
          (member_const || member_volatile) ?
              types_.Cv(class_type, member_const, member_volatile) :
              class_type);
      canonical_parameters.insert(canonical_parameters.begin(), this_type);
    }
  }
  const TypeId canonical = types_.Function(declared.result,
                                           canonical_parameters,
                                           declared.variadic);
  vector<AstId> canonical_defaults = default_arguments;
  if (is_member && !static_member)
    canonical_defaults.insert(canonical_defaults.begin(), 0);

  // 13.1/3.3.10: a prior declaration of the name in this scope with the
  // same canonical signature declares the same function.
  vector<BindingId> priors;
  // A later friend declaration must still redeclare the canonical namespace
  // function entity even though hidden friends are excluded from ordinary
  // lookup.  Declaration matching opts into that private visibility bit.
  model_.DirectBindings(scope, name, LOOKUP_ANY | LOOKUP_HIDDEN_FRIENDS,
                        priors);
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
        entity.static_member = static_member;
        entity.member_const = member_const;
        entity.member_volatile = member_volatile;
      }
      entity.internal_linkage = entity.internal_linkage ||
          effective_internal_linkage;
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
      model_.BindingAt(binding).access = member_access_;
      model_.BindingAt(binding).static_member = static_member;
      model_.BindingAt(binding).internal_linkage = entity.internal_linkage;
      model_.BindingAt(binding).c_linkage = entity.c_linkage;
      model_.BindingAt(binding).noexcept_qualifier = entity.noexcept_qualifier;
      return prior.function;
    }
  }
  const FunctionEntityId function = model_.CreateFunction(scope, name,
                                                            canonical);
  model_.FunctionAt(function).defined = definition;
  model_.FunctionAt(function).internal_linkage = effective_internal_linkage;
  model_.FunctionAt(function).c_linkage = c_linkage_depth_ != 0;
  if (is_member)
  {
    FunctionEntity& entity = model_.FunctionAt(function);
    entity.member_type = member_type;
    entity.member_pointer_type = types_.MemberPointer(
        model_.ClassAt(member_class).type, member_type);
    entity.member_class = member_class;
    entity.is_member = true;
    entity.static_member = static_member;
    entity.member_const = member_const;
    entity.member_volatile = member_volatile;
  }
  model_.FunctionAt(function).noexcept_qualifier = noexcept_qualifier;
  model_.FunctionAt(function).default_arguments = canonical_defaults;
  binding = model_.AddBinding(scope, name, BINDING_FUNCTION, declared_type);
  model_.BindingAt(binding).function = function;
  model_.BindingAt(binding).access = member_access_;
  model_.BindingAt(binding).static_member = static_member;
  model_.BindingAt(binding).internal_linkage = effective_internal_linkage;
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
