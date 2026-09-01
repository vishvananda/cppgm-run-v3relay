#include "sema/scope_builder.h"

#include <stdexcept>

using std::string;
using std::vector;

void ScopeBuilder::Build(AstId root)
{
  if (root == 0)
    throw std::runtime_error("empty AST");
  if (arena_.At(root).kind == AST_TRANSLATION_UNIT)
  {
    const vector<AstId>& children = arena_.At(root).children;
    for (std::size_t i = 0; i < children.size(); ++i)
      BuildNode(children[i], model_.GlobalScope());
    return;
  }
  BuildNode(root, model_.GlobalScope());
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
  case AST_CLASS_SPECIFIER:
    (void)BuildClassSpecifier(node, scope);
    return;
  case AST_CLASS_FORWARD_DECLARATION: BuildClassForward(node, scope); return;
  case AST_LINKAGE_SPECIFICATION: BuildLinkage(node, scope); return;
  case AST_COMPOUND_STATEMENT:
    (void)BuildCompound(node, scope);
    return;
  case AST_EMPTY_DECLARATION:
    return;
  case AST_ENUM_SPECIFIER:
  case AST_STATIC_ASSERT_DECLARATION:
  case AST_TEMPLATE_DECLARATION:
  case AST_EXPLICIT_INSTANTIATION_DECLARATION:
    throw std::runtime_error("unsupported pa11 declaration");
  default:
    return;
  }
}

void ScopeBuilder::BuildNamespace(AstId node, ScopeId scope)
{
  const AstNode& value = arena_.At(node);
  const string name = value.text.empty() ? "<unnamed>" : value.text;
  bool is_inline = false;
  for (std::size_t i = 0; i < value.children.size(); ++i)
    if (arena_.At(value.children[i]).kind == AST_INLINE)
      is_inline = true;

  ScopeId target = 0;
  if (name == "<unnamed>")
  {
    target = model_.CreateScope(SCOPE_NAMESPACE, name, scope, is_inline);
    model_.AddUsingDirective(scope, target);
  }
  else
  {
    const BindingId existing = model_.DirectBinding(
        scope, name, LOOKUP_NAMESPACES);
    if (existing != 0)
    {
      target = model_.BindingAt(existing).target_scope;
      if (target == 0)
        throw std::runtime_error("invalid namespace binding");
      model_.ScopeAt(target).inline_namespace =
          model_.ScopeAt(target).inline_namespace || is_inline;
    }
    else
    {
      if (model_.DirectBinding(scope, name, LOOKUP_ANY) != 0)
        throw std::runtime_error("namespace and ordinary name conflict");
      target = model_.CreateScope(SCOPE_NAMESPACE, name, scope, is_inline);
      model_.AddBinding(scope, name, BINDING_NAMESPACE, 0, target, false);
    }
  }

  for (std::size_t i = 0; i < value.children.size(); ++i)
  {
    const AstKind kind = arena_.At(value.children[i]).kind;
    if (kind != AST_INLINE)
      BuildNode(value.children[i], target);
  }
}

void ScopeBuilder::BuildNamespaceAlias(AstId node, ScopeId scope)
{
  const AstNode& value = arena_.At(node);
  if (value.children.empty())
    throw std::runtime_error("namespace alias has no target");
  const ScopeId target = ResolveNamespace(scope, value.children[0]);
  const string name = value.text;
  if (name.empty())
    throw std::runtime_error("namespace alias has no name");
  model_.AddBinding(scope, name, BINDING_NAMESPACE, 0, target, false);
}

void ScopeBuilder::BuildUsingDirective(AstId node, ScopeId scope)
{
  const AstNode& value = arena_.At(node);
  if (value.children.empty())
    throw std::runtime_error("using-directive has no target");
  model_.AddUsingDirective(scope, ResolveNamespace(scope, value.children[0]));
}

void ScopeBuilder::BuildUsingDeclaration(AstId node, ScopeId scope)
{
  const AstNode& value = arena_.At(node);
  if (value.children.empty())
    throw std::runtime_error("using-declaration has no target");
  const AstId target_node = value.children[0];
  const vector<string> name = NameComponents(target_node);
  const BindingId target = ResolveName(scope, name, LOOKUP_ANY);
  if (target == 0 || model_.BindingAt(target).kind == BINDING_NAMESPACE)
    throw std::runtime_error("using-declaration target not found");
  const Binding& binding = model_.BindingAt(target);
  model_.AddBinding(scope, name.back(), binding.kind, binding.type,
                    binding.target_scope, true, binding.class_entity);
}

void ScopeBuilder::BuildAlias(AstId node, ScopeId scope)
{
  const AstNode& value = arena_.At(node);
  if (value.children.empty())
    throw std::runtime_error("alias has no type");
  const TypeId type = BuildTypeId(value.children[0], scope);
  model_.AddBinding(scope, value.text, BINDING_TYPE_ALIAS, type,
                    model_.TargetScopeForType(type));
}

void ScopeBuilder::BuildSimpleDeclaration(AstId node, ScopeId scope)
{
  const AstNode& value = arena_.At(node);
  AstId specifiers = 0;
  AstId list = 0;
  for (std::size_t i = 0; i < value.children.size(); ++i)
  {
    const AstKind kind = arena_.At(value.children[i]).kind;
    if (kind == AST_DECL_SPECIFIER_SEQ)
      specifiers = value.children[i];
    else if (kind == AST_INIT_DECLARATOR_LIST)
      list = value.children[i];
  }
  if (specifiers == 0)
    throw std::runtime_error("simple declaration has no specifiers");

  string anonymous_name;
  const AstNode& declarations = list == 0 ? arena_.At(node) : arena_.At(list);
  for (std::size_t i = 0; i < declarations.children.size() &&
      anonymous_name.empty(); ++i)
  {
    const AstNode& item = arena_.At(declarations.children[i]);
    if (item.kind == AST_INIT_DECLARATOR && !item.children.empty())
      anonymous_name = IdentifierName(FindIdentifier(item.children[0]));
  }
  const string saved_anonymous = active_anonymous_name_;
  active_anonymous_name_ = anonymous_name;
  const TypeId base = BuildSpecifierType(specifiers, scope);
  active_anonymous_name_ = saved_anonymous;
  if (list == 0)
    return;

  const bool is_typedef = SequenceHasKeyword(specifiers, KW_TYPEDEF);
  const bool is_constexpr = SequenceHasKeyword(specifiers, KW_CONSTEXPR);
  const vector<AstId>& items = arena_.At(list).children;
  for (std::size_t i = 0; i < items.size(); ++i)
  {
    const AstNode& item = arena_.At(items[i]);
    if (item.kind != AST_INIT_DECLARATOR || item.children.empty())
      throw std::runtime_error("invalid init-declarator");
    const AstId declarator = item.children[0];
    string name;
    const ScopeId target_scope = ResolveDeclarationScope(
        scope, FindIdentifier(declarator), name);
    TypeId type = BuildDeclaratorType(declarator, base, target_scope);
    const TypeKind kind = types_.Kind(type);
    if (is_constexpr && !is_typedef && kind != TYPE_FUNCTION)
      type = AddCv(type, true, false);
    const BindingKind binding_kind = is_typedef ? BINDING_TYPE_ALIAS :
        kind == TYPE_FUNCTION ? BINDING_FUNCTION : BINDING_VARIABLE;
    model_.AddBinding(target_scope, name, binding_kind, type);
  }
}

void ScopeBuilder::BuildFunctionDefinition(AstId node, ScopeId scope)
{
  const AstNode& value = arena_.At(node);
  AstId specifiers = 0;
  AstId declarator = 0;
  AstId body = 0;
  for (std::size_t i = 0; i < value.children.size(); ++i)
  {
    const AstKind kind = arena_.At(value.children[i]).kind;
    if (kind == AST_DECL_SPECIFIER_SEQ) specifiers = value.children[i];
    else if (kind == AST_DECLARATOR) declarator = value.children[i];
    else if (kind == AST_COMPOUND_STATEMENT) body = value.children[i];
  }
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
  AddFunctionParameters(function_scope, declarator, target_scope);
  (void)BuildCompound(body, function_scope);
}

void ScopeBuilder::BuildClassForward(AstId node, ScopeId scope)
{
  const AstNode& value = arena_.At(node);
  const string name = TypeName(node);
  if (name.empty())
    throw std::runtime_error("unnamed class forward declaration");
  const ClassEntityId entity = model_.GetOrCreateClass(scope, name);
  const TypeId type = types_.Class(name, ClassKey(node));
  ClassEntity& class_entity = model_.ClassAt(entity);
  class_entity.current_type = type;
  class_entity.class_key = ClassKey(node);
  model_.AddBinding(scope, name, BINDING_TYPE, type, 0, true, entity);
  (void)value;
}

TypeId ScopeBuilder::BuildClassSpecifier(AstId node, ScopeId scope,
                                         const string& anonymous_name)
{
  return BuildClassType(node, scope,
                        anonymous_name.empty() ? active_anonymous_name_ :
                        anonymous_name);
}

TypeId ScopeBuilder::BuildClassType(AstId node, ScopeId scope,
                                    const string& anonymous_name)
{
  const AstNode& value = arena_.At(node);
  const string name = value.text.empty() ? anonymous_name : value.text;
  if (name.empty())
    throw std::runtime_error("unnamed class requires a declaration name");
  const string key = ClassKey(node);
  const ClassEntityId entity = model_.GetOrCreateClass(scope, name);
  ClassEntity& class_entity = model_.ClassAt(entity);
  if (class_entity.defined)
    throw std::runtime_error("class redefinition");
  const TypeId type = types_.Class(name, key);
  class_entity.current_type = type;
  class_entity.class_key = key;
  class_entity.defined = true;
  const BindingId binding = model_.AddBinding(
      scope, name, BINDING_TYPE, type, 0, true, entity);
  class_entity.class_scope = model_.CreateScope(SCOPE_CLASS, name, scope);
  model_.BindingAt(binding).target_scope = class_entity.class_scope;
  for (std::size_t i = 0; i < value.children.size(); ++i)
  {
    const AstKind kind = arena_.At(value.children[i]).kind;
    if (kind == AST_CLASS_KEY || kind == AST_BASE_CLAUSE ||
        kind == AST_ACCESS_SPECIFIER)
      continue;
    BuildNode(value.children[i], class_entity.class_scope);
  }
  return type;
}

TypeId ScopeBuilder::BuildForwardType(AstId node, ScopeId scope,
                                      bool declaration)
{
  const string name = TypeName(node);
  if (name.empty())
    throw std::runtime_error("unnamed elaborated type");
  const BindingId existing = model_.LookupUnqualified(scope, name,
                                                      LOOKUP_TYPES);
  if (existing != 0 && model_.BindingAt(existing).class_entity != 0)
    return model_.BindingAt(existing).type;
  if (existing != 0 && !declaration)
    return model_.BindingAt(existing).type;
  const ClassEntityId entity = model_.GetOrCreateClass(scope, name);
  ClassEntity& class_entity = model_.ClassAt(entity);
  if (class_entity.current_type != 0)
    return class_entity.current_type;
  const TypeId type = types_.Class(name, ClassKey(node));
  class_entity.current_type = type;
  class_entity.class_key = ClassKey(node);
  model_.AddBinding(scope, name, BINDING_TYPE, type, 0, true, entity);
  return type;
}

void ScopeBuilder::BuildLinkage(AstId node, ScopeId scope)
{
  const vector<AstId>& children = arena_.At(node).children;
  for (std::size_t i = 0; i < children.size(); ++i)
    BuildNode(children[i], scope);
}

ScopeId ScopeBuilder::BuildCompound(AstId node, ScopeId parent)
{
  const ScopeId block = model_.CreateScope(SCOPE_BLOCK, "block", parent);
  const vector<AstId>& children = arena_.At(node).children;
  for (std::size_t i = 0; i < children.size(); ++i)
    BuildStatement(children[i], block);
  return block;
}

void ScopeBuilder::BuildStatement(AstId node, ScopeId scope)
{
  if (node == 0)
    return;
  const AstKind kind = arena_.At(node).kind;
  if (IsDeclarationKind(kind))
  {
    BuildNode(node, scope);
    return;
  }
  if (kind == AST_COMPOUND_STATEMENT)
  {
    (void)BuildCompound(node, scope);
    return;
  }
  const vector<AstId>& children = arena_.At(node).children;
  for (std::size_t i = 0; i < children.size(); ++i)
    BuildStatement(children[i], scope);
}

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
    const AstKind kind = arena_.At(node.children[i]).kind;
    if (kind == AST_IDENTIFIER)
      return node.children[i];
    if (kind == AST_DECLARATOR || kind == AST_ABSTRACT_DECLARATOR ||
        kind == AST_NESTED_DECLARATOR)
    {
      const AstId found = FindIdentifier(node.children[i]);
      if (found != 0)
        return found;
    }
  }
  return 0;
}

AstId ScopeBuilder::FindParameterClause(AstId declarator) const
{
  if (declarator == 0)
    return 0;
  const AstNode& node = arena_.At(declarator);
  for (std::size_t i = 0; i < node.children.size(); ++i)
    if (arena_.At(node.children[i]).kind == AST_PARAMETER_CLAUSE)
      return node.children[i];
  return 0;
}

string ScopeBuilder::IdentifierName(AstId identifier) const
{
  const vector<string> components = NameComponents(identifier);
  return components.empty() ? string() : components.back();
}

vector<string> ScopeBuilder::NameComponents(std::size_t first,
                                            std::size_t last) const
{
  vector<string> result;
  string component;
  for (std::size_t i = first; i < last && i < tokens_.size(); ++i)
  {
    const Pa6Token& token = tokens_[i];
    if (token.kind == PA6_IDENTIFIER_TOKEN)
    {
      component += token.spelling;
      continue;
    }
    if (token.IsSimple(OP_COLON2))
    {
      if (!component.empty())
      {
        result.push_back(component);
        component.clear();
      }
      continue;
    }
    if (token.IsRshiftPart())
      component += ">";
    else if (token.kind == PA6_SIMPLE_TOKEN && token.simple_type == OP_LT)
      component += "<";
    else if (token.kind == PA6_SIMPLE_TOKEN && token.simple_type == OP_GT)
      component += ">";
  }
  if (!component.empty())
    result.push_back(component);
  return result;
}

vector<string> ScopeBuilder::NameComponents(AstId node) const
{
  if (node == 0)
    return vector<string>();
  const AstNode& value = arena_.At(node);
  if (value.first < value.last)
    return NameComponents(value.first, value.last);
  if (!value.text.empty())
    return vector<string>(1, value.text);
  return vector<string>();
}

BindingId ScopeBuilder::ResolveName(ScopeId scope,
                                   const vector<string>& name,
                                   unsigned filter) const
{
  if (name.empty())
    return 0;
  if (name.size() == 1)
    return model_.LookupUnqualified(scope, name[0], filter);
  return model_.LookupQualified(scope, name, filter);
}

ScopeId ScopeBuilder::ResolveDeclarationScope(ScopeId scope, AstId identifier,
                                              string& name) const
{
  const vector<string> components = NameComponents(identifier);
  if (components.empty())
    throw std::runtime_error("declarator has no name");
  name = components.back();
  if (components.size() == 1)
    return scope;
  vector<string> prefix(components.begin(), components.end() - 1);
  const BindingId binding = ResolveName(scope, prefix, LOOKUP_QUALIFIER);
  if (binding == 0 || model_.BindingAt(binding).target_scope == 0)
    throw std::runtime_error("qualified declarator scope not found");
  return model_.BindingAt(binding).target_scope;
}

ScopeId ScopeBuilder::ResolveNamespace(ScopeId scope, AstId target) const
{
  const vector<string> name = NameComponents(target);
  const BindingId binding = ResolveName(scope, name, LOOKUP_NAMESPACES);
  if (binding == 0 || model_.BindingAt(binding).target_scope == 0)
    throw std::runtime_error("namespace target not found");
  return model_.BindingAt(binding).target_scope;
}

string ScopeBuilder::ClassKey(AstId node) const
{
  const AstNode& value = arena_.At(node);
  for (std::size_t i = 0; i < value.children.size(); ++i)
  {
    const AstNode& child = arena_.At(value.children[i]);
    if (child.kind != AST_CLASS_KEY || child.first >= tokens_.size())
      continue;
    switch (tokens_[child.first].simple_type)
    {
    case KW_CLASS: return "class";
    case KW_UNION: return "union";
    case KW_STRUCT: return "struct";
    default: break;
    }
  }
  return "class";
}

string ScopeBuilder::TypeName(AstId node) const
{
  const vector<string> name = NameComponents(node);
  if (name.empty())
    return string();
  string result = name[0];
  for (std::size_t i = 1; i < name.size(); ++i)
    result += "::" + name[i];
  return result;
}

bool ScopeBuilder::SequenceHasKeyword(AstId sequence, ETokenType keyword) const
{
  if (sequence == 0)
    return false;
  const AstNode& value = arena_.At(sequence);
  for (std::size_t i = 0; i < value.children.size(); ++i)
  {
    const AstNode& child = arena_.At(value.children[i]);
    if (child.first < tokens_.size() && child.last == child.first + 1 &&
        tokens_[child.first].IsSimple(keyword))
      return true;
  }
  return false;
}

bool ScopeBuilder::IsDeclarationKind(AstKind kind) const
{
  switch (kind)
  {
  case AST_NAMESPACE_DEFINITION: case AST_NAMESPACE_ALIAS_DEFINITION:
  case AST_USING_DIRECTIVE: case AST_USING_DECLARATION:
  case AST_ALIAS_DECLARATION: case AST_SIMPLE_DECLARATION:
  case AST_FUNCTION_DEFINITION: case AST_CLASS_SPECIFIER:
  case AST_CLASS_FORWARD_DECLARATION: case AST_LINKAGE_SPECIFICATION:
  case AST_EMPTY_DECLARATION: case AST_ENUM_SPECIFIER:
  case AST_STATIC_ASSERT_DECLARATION: case AST_TEMPLATE_DECLARATION:
  case AST_EXPLICIT_INSTANTIATION_DECLARATION:
    return true;
  default:
    return false;
  }
}

void ScopeBuilder::AddFunctionParameters(ScopeId function_scope,
                                         AstId declarator,
                                         ScopeId lookup_scope)
{
  const AstId clause = FindParameterClause(declarator);
  if (clause == 0)
    return;
  vector<ParameterInfo> parameters;
  bool variadic = false;
  BuildParameters(clause, lookup_scope, parameters, variadic);
  (void)variadic;
  for (std::size_t i = 0; i < parameters.size(); ++i)
    model_.AddBinding(function_scope, parameters[i].name,
                      BINDING_PARAMETER, parameters[i].type);
}
