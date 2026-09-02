// Declaration collection and scope ownership for the PA11 semantic model.
#include "sema/scope_builder.h"

#include <limits>
#include <sstream>
#include <stdexcept>

using std::string;
using std::vector;

void ScopeBuilder::Build(AstId root)
{
  if (root == 0)
    throw std::runtime_error("empty AST");
  if (arena_.At(root).kind != AST_TRANSLATION_UNIT)
    throw std::runtime_error("AST root is not a translation unit");
  const vector<AstId>& children = arena_.At(root).children;
  for (std::size_t i = 0; i < children.size(); ++i)
    BuildNode(children[i], model_.GlobalScope());
}

void ScopeBuilder::BuildNode(AstId node, ScopeId scope)
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
  case AST_SIMPLE_DECLARATION: BuildSimpleDeclaration(node, scope); return;
  case AST_FUNCTION_DEFINITION: BuildFunctionDefinition(node, scope); return;
  case AST_ENUM_SPECIFIER: case AST_ENUM_DECLARATION:
    (void)BuildEnum(node, scope, string());
    return;
  case AST_CLASS_SPECIFIER:
    (void)BuildClassDefinition(node, scope, string());
    return;
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
  BuildNode(declaration, template_scope);
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
        model_.ScopeAt(target).inline_namespace = true;
    }
    else
    {
      target = model_.CreateScope(SCOPE_NAMESPACE, name, scope, is_inline);
      model_.AddBinding(scope, name, BINDING_NAMESPACE, 0, target);
    }
  }
  for (std::size_t i = 0; i < value.children.size(); ++i)
    if (arena_.At(value.children[i]).kind != AST_INLINE)
      BuildNode(value.children[i], target);
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
  imported.has_const_value = source.has_const_value;
  imported.const_value = source.const_value;
}

void ScopeBuilder::BuildAlias(AstId node, ScopeId scope)
{
  const AstId type_id = FindChild(node, AST_TYPE_ID);
  if (type_id == 0)
    throw std::runtime_error("alias has no type");
  model_.AddBinding(scope, IdentifierName(node), BINDING_TYPE_ALIAS,
                    BuildTypeId(type_id, scope));
}

void ScopeBuilder::BuildSimpleDeclaration(AstId node, ScopeId scope)
{
  const AstId specifiers = FindChild(node, AST_DECL_SPECIFIER_SEQ);
  const AstId list = FindChild(node, AST_INIT_DECLARATOR_LIST);
  if (specifiers == 0)
    throw std::runtime_error("simple declaration has no specifiers");

  // An unnamed class or enum in the specifiers takes the first declarator's
  // name (`typedef struct { ... } S;` declares `struct S`).
  string anonymous_name;
  if (list != 0 && !arena_.At(list).children.empty())
  {
    const AstId first = arena_.At(list).children[0];
    anonymous_name = IdentifierName(FindIdentifier(
        FindChild(first, AST_DECLARATOR)));
  }
  const TypeId base = BuildSpecifierType(specifiers, scope, anonymous_name);
  if (list == 0)
    return;

  const bool is_typedef = SequenceHasKeyword(specifiers, KW_TYPEDEF);
  const bool is_constexpr = SequenceHasKeyword(specifiers, KW_CONSTEXPR);
  const vector<AstId>& items = arena_.At(list).children;
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
    TypeId type = BuildDeclaratorType(declarator, base, target_scope);
    const bool is_function = types_.Kind(type) == TYPE_FUNCTION;
    // 7.1.5p9: a constexpr object is const.
    if (is_constexpr && !is_typedef && !is_function)
      type = types_.Cv(type, true);
    const BindingKind kind = is_typedef ? BINDING_TYPE_ALIAS :
        is_function ? BINDING_FUNCTION : BINDING_VARIABLE;
    const BindingId binding = model_.AddBinding(target_scope, name, kind, type);
    if (kind == BINDING_VARIABLE)
      RecordConstantValue(binding, items[i], is_constexpr, target_scope);
  }
}

// A const integral object with a constant initializer is usable in later
// constant expressions (5.19p2); its value is recorded once here.
void ScopeBuilder::RecordConstantValue(BindingId binding, AstId init_declarator,
                                       bool is_constexpr, ScopeId scope)
{
  const TypeId type = model_.BindingAt(binding).type;
  const TypeId unqualified = types_.Unqualified(type);
  const bool is_const = types_.Kind(type) == TYPE_CV && types_.At(type).is_const;
  const TypeKind kind = types_.Kind(unqualified);
  if (!is_const || (kind != TYPE_FUNDAMENTAL && kind != TYPE_ENUM))
    return;
  const AstId initializer = FindChild(init_declarator, AST_INITIALIZER);
  if (initializer == 0)
  {
    if (is_constexpr)
      throw std::runtime_error("constexpr variable has no initializer");
    return;
  }
  if (arena_.At(initializer).children.size() != 1)
    throw std::runtime_error("invalid constant initializer");
  const long long value = const_eval_.Evaluate(
      arena_.At(initializer).children[0], scope);
  Binding& stored = model_.BindingAt(binding);
  stored.const_value = value;
  stored.has_const_value = true;
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
  model_.AddBinding(target_scope, name, BINDING_FUNCTION, type);
  const ScopeId function_scope = model_.CreateScope(
      SCOPE_FUNCTION, name, target_scope);

  const AstId clause = FindChild(declarator, AST_PARAMETER_CLAUSE);
  if (clause != 0)
  {
    vector<ParameterInfo> parameters;
    bool variadic = false;
    BuildParameters(clause, target_scope, parameters, variadic);
    for (std::size_t i = 0; i < parameters.size(); ++i)
      model_.AddBinding(function_scope, parameters[i].name, BINDING_PARAMETER,
                        parameters[i].type);
  }
  (void)BuildCompound(body, function_scope);
}

void ScopeBuilder::BuildStaticAssert(AstId node, ScopeId scope)
{
  const AstNode& value = arena_.At(node);
  if (value.children.empty())
    throw std::runtime_error("static_assert has no expression");
  if (const_eval_.Evaluate(value.children[0], scope) == 0)
    throw std::runtime_error("static_assert failed");
}

void ScopeBuilder::BuildLinkage(AstId node, ScopeId scope)
{
  const vector<AstId>& children = arena_.At(node).children;
  for (std::size_t i = 0; i < children.size(); ++i)
    BuildNode(children[i], scope);
}

ScopeId ScopeBuilder::BuildCompound(AstId node, ScopeId parent)
{
  const ScopeId block = model_.CreateScope(SCOPE_BLOCK, string(), parent);
  const vector<AstId>& children = arena_.At(node).children;
  for (std::size_t i = 0; i < children.size(); ++i)
    BuildStatement(children[i], block);
  return block;
}

// Statements contribute declarations and nested blocks only.
void ScopeBuilder::BuildStatement(AstId node, ScopeId scope)
{
  if (node == 0)
    return;
  const AstKind kind = arena_.At(node).kind;
  if (IsDeclarationKind(kind))
    BuildNode(node, scope);
  else if (kind == AST_COMPOUND_STATEMENT)
    (void)BuildCompound(node, scope);
  else
  {
    const vector<AstId>& children = arena_.At(node).children;
    for (std::size_t i = 0; i < children.size(); ++i)
      BuildStatement(children[i], scope);
  }
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
  if (!injected_union)
  {
    const ScopeId declaring = name.Qualified() ?
        ResolveQualifierScope(scope, name.Prefix()) : scope;
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
  const ScopeId class_scope = model_.CreateScope(SCOPE_CLASS, spelling, scope);
  // Members may declare further classes, which grows the entity table: keep
  // ids, not entity references, across the member walk.
  model_.ClassAt(entity).defined = true;
  model_.ClassAt(entity).class_scope = class_scope;
  if (!injected_union)
    model_.AddBinding(scope, spelling, BINDING_TYPE, type);
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
      Binding& injected = model_.BindingAt(model_.AddBinding(
          scope, member.name, member.kind, member.type));
      injected.has_const_value = member.has_const_value;
      injected.const_value = member.const_value;
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
    name.components.push_back(anonymous_name.empty() ?
                              AnonymousTypeName(node, "enum") : anonymous_name);
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
      value = const_eval_.Evaluate(enumerator.children[0], scope);
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
