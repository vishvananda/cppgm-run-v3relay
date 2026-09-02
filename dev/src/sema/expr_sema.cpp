#include "sema/expr_sema.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "sema/scope_builder.h"

using std::size_t;
using std::string;
using std::vector;

namespace
{

bool IsConstObject(const TypeTable& types, TypeId type)
{
  return type != 0 && types.Kind(type) == TYPE_CV &&
      types.At(type).is_const;
}

bool IsScopedEnum(const TypeTable& types, TypeId type)
{
  type = types.Unqualified(type);
  return types.Kind(type) == TYPE_ENUM && types.At(type).scoped;
}

bool IsVoid(const TypeTable& types, TypeId type)
{
  type = types.Unqualified(type);
  return types.Kind(type) == TYPE_FUNDAMENTAL &&
      types.At(type).fundamental == FT_VOID;
}

// 4.7 integral conversion of a folded value to a fundamental integral type
// or to the underlying type of an enumeration; false for other targets.
bool ConvertIntegral(const TypeTable& types, long long value, TypeId type,
                     long long& converted)
{
  TypeId target = types.Unqualified(type);
  if (types.Kind(target) == TYPE_ENUM)
    target = types.Unqualified(types.At(target).base);
  if (types.Kind(target) != TYPE_FUNDAMENTAL ||
      !FundamentalIsIntegral(types.At(target).fundamental))
    return false;
  const EFundamentalType fundamental = types.At(target).fundamental;
  if (fundamental == FT_BOOL)
  {
    converted = value == 0 ? 0 : 1;
    return true;
  }
  const unsigned bits = 8 * FundamentalSize(fundamental);
  if (bits >= 64)
  {
    converted = value;
    return true;
  }
  const unsigned long long modulus = 1ULL << bits;
  const unsigned long long raw =
      static_cast<unsigned long long>(value) & (modulus - 1);
  if (FundamentalIsUnsigned(fundamental))
    converted = static_cast<long long>(raw);
  else
  {
    const unsigned long long sign = 1ULL << (bits - 1);
    converted = raw >= sign ? static_cast<long long>(raw - modulus) :
        static_cast<long long>(raw);
  }
  return true;
}

} // namespace

ExpressionAnalyzer::ExpressionAnalyzer(const vector<Pa6Token>& tokens,
                                       const AstArena& arena,
                                       SemaModel& model, SemaTree& tree,
                                       ScopeBuilder& builder)
    : tokens_(tokens), arena_(arena), model_(model), types_(model.Types()),
      tree_(tree), builder_(builder)
{
}

SemaId ExpressionAnalyzer::MakeExpression(SemaKind kind, AstId source,
                                           TypeId type,
                                           ValueCategory category,
                                           ScopeId scope, ETokenType op)
{
  const SemaId result = tree_.Make(kind);
  SemaNode& node = tree_.At(result);
  node.type = type;
  node.category = category;
  node.op = op;
  node.scope = scope;
  if (source != 0)
  {
    node.first = arena_.At(source).first;
    node.last = arena_.At(source).last;
  }
  return result;
}

void ExpressionAnalyzer::Append(SemaId parent, SemaId child)
{
  if (child != 0)
    tree_.Append(parent, child);
}

AstId ExpressionAnalyzer::Child(AstId node, size_t index) const
{
  const vector<AstId>& children = arena_.At(node).children;
  return index < children.size() ? children[index] : 0;
}

AstId ExpressionAnalyzer::FindChild(AstId node, AstKind kind) const
{
  if (node == 0)
    return 0;
  const vector<AstId>& children = arena_.At(node).children;
  for (size_t i = 0; i < children.size(); ++i)
    if (children[i] != 0 && arena_.At(children[i]).kind == kind)
      return children[i];
  return 0;
}

ETokenType ExpressionAnalyzer::Operator(AstId node) const
{
  const AstNode& value = arena_.At(node);
  if (value.first >= tokens_.size())
    return KW_AUTO;
  const Pa6Token& token = tokens_[value.first];
  if (token.IsRshiftPart())
    return OP_RSHIFT;
  return token.simple_type;
}

QualifiedName ExpressionAnalyzer::ExpressionName(AstId node) const
{
  const AstNode& value = arena_.At(node);
  return ReadQualifiedName(tokens_, value.first, value.last, true);
}

ExpressionAnalyzer::Info ExpressionAnalyzer::NodeInfo(SemaId node) const
{
  if (node == 0)
    return Info();
  const SemaNode& value = tree_.At(node);
  return Info(value.type, value.category,
              value.kind == SEMA_LITERAL && value.type != 0 &&
                  types_.IsNullPointerType(value.type),
              value.kind == SEMA_ID_EXPRESSION &&
                  types_.Kind(types_.Unqualified(value.type)) == TYPE_FUNCTION);
}

const SemaNode& ExpressionAnalyzer::Node(SemaId expression) const
{
  return tree_.At(expression);
}

TypeId ExpressionAnalyzer::DeclaredType(SemaId expression) const
{
  const SemaNode& node = tree_.At(expression);
  if (node.binding == 0 ||
      (node.kind != SEMA_ID_EXPRESSION && node.kind != SEMA_MEMBER))
    return node.type;
  const Binding& binding = model_.BindingAt(node.binding);
  if (binding.kind == BINDING_FUNCTION)
    return node.type;
  if (binding.kind == BINDING_PARAMETER &&
      (types_.Kind(binding.type) == TYPE_ARRAY ||
       types_.Kind(binding.type) == TYPE_FUNCTION))
    return types_.Decay(binding.type);
  return binding.type;
}

SemaId ExpressionAnalyzer::Analyze(AstId expression, ScopeId scope)
{
  if (expression == 0)
    throw std::runtime_error("missing expression");
  return AnalyzeNode(expression, scope);
}

SemaId ExpressionAnalyzer::AnalyzeInitializer(AstId initializer,
                                               ScopeId scope, TypeId target)
{
  if (initializer == 0)
    throw std::runtime_error("missing initializer");
  const AstNode& node = arena_.At(initializer);
  if (node.kind == AST_INITIALIZER)
  {
    if (node.children.size() != 1)
      throw std::runtime_error("invalid initializer");
    return AnalyzeInitializer(node.children[0], scope, target);
  }
  if (node.kind == AST_PAREN_INITIALIZER)
  {
    if (node.children.size() != 1)
      throw std::runtime_error("invalid parenthesized initializer");
    return Initialize(Analyze(node.children[0], scope), target);
  }
  if (node.kind == AST_BRACED_INIT_LIST)
    return AnalyzeBraced(initializer, scope, target);
  return Initialize(Analyze(initializer, scope), target);
}

SemaId ExpressionAnalyzer::AnalyzeNode(AstId expression, ScopeId scope)
{
  const AstKind kind = arena_.At(expression).kind;
  switch (kind)
  {
  case AST_PARENTHESIZED_EXPRESSION:
    if (arena_.At(expression).children.size() != 1)
      throw std::runtime_error("invalid parenthesized expression");
    return AnalyzeNode(Child(expression, 0), scope);
  case AST_LITERAL: return AnalyzeLiteral(expression, scope);
  case AST_KEYWORD_LITERAL: return AnalyzeLiteral(expression, scope);
  case AST_ID_EXPRESSION: case AST_IDENTIFIER:
    return AnalyzeName(expression, scope);
  case AST_PARAMETER_DECLARATION:
    return AnalyzeAmbiguousParameter(expression, scope);
  case AST_UNARY_EXPRESSION: return AnalyzeUnary(expression, scope);
  case AST_POSTFIX_EXPRESSION: return AnalyzePostfix(expression, scope);
  case AST_MEMBER_EXPRESSION: return AnalyzeMember(expression, scope);
  case AST_BINARY_EXPRESSION: return AnalyzeBinary(expression, scope);
  case AST_ASSIGNMENT_EXPRESSION: return AnalyzeAssignment(expression, scope);
  case AST_CONDITIONAL_EXPRESSION: return AnalyzeConditional(expression, scope);
  case AST_SUBSCRIPT_EXPRESSION: return AnalyzeSubscript(expression, scope);
  case AST_CAST_EXPRESSION: return AnalyzeCast(expression, scope);
  case AST_SIZEOF_EXPRESSION: case AST_TYPE_TRAIT_EXPRESSION:
    return AnalyzeSizeof(expression, scope);
  case AST_CALL_EXPRESSION: return AnalyzeCall(expression, scope);
  case AST_NEW_EXPRESSION: return AnalyzeNew(expression, scope);
  case AST_BRACED_INIT_LIST: return AnalyzeBraced(expression, scope, 0);
  default:
    throw std::runtime_error("unsupported expression in semantic analysis");
  }
}

// Declaration parsing has priority over expression parsing at several
// grammar boundaries.  Once semantic lookup has established that a
// parameter-shaped node is actually a zero-argument expression, analyze it
// through the same constructor or overload path as an ordinary call.
SemaId ExpressionAnalyzer::AnalyzeAmbiguousParameter(AstId parameter,
                                                     ScopeId scope,
                                                     bool unevaluated)
{
  const AstId specifiers = FindChild(parameter, AST_DECL_SPECIFIER_SEQ);
  const AstId declarator = FindChild(parameter, AST_DECLARATOR);
  if (specifiers == 0 || declarator == 0 ||
      arena_.At(declarator).children.size() != 1)
    throw std::runtime_error("invalid ambiguous expression parameter");
  const AstId clause = FindChild(declarator, AST_PARAMETER_CLAUSE);
  if (clause == 0 || !arena_.At(clause).children.empty())
    throw std::runtime_error("ambiguous expression parameter is not a call");

  TypeId type = 0;
  try
  {
    type = builder_.TypeOfSpecifierSequence(specifiers, scope);
  }
  catch (const std::runtime_error&)
  {
    type = 0;
  }
  if (type != 0)
  {
    if (unevaluated &&
        types_.Kind(types_.Unqualified(type)) == TYPE_CLASS)
      return MakeExpression(SEMA_ID_EXPRESSION, parameter, type,
                            VC_PRVALUE, scope);
    return AnalyzeFunctionalCast(parameter, type, scope, vector<AstId>());
  }

  const vector<AstId>& specifier_nodes = arena_.At(specifiers).children;
  if (specifier_nodes.size() != 1)
    throw std::runtime_error("ambiguous expression has no callable name");
  const QualifiedName name = ReadQualifiedName(
      tokens_, arena_.At(specifier_nodes[0]).first,
      arena_.At(specifier_nodes[0]).last, true);
  return AnalyzeNamedCall(parameter, name, scope, vector<SemaId>());
}

SemaId ExpressionAnalyzer::AnalyzeAmbiguousTypeId(AstId type_id,
                                                  ScopeId scope)
{
  const vector<AstId>& children = arena_.At(type_id).children;
  if (children.size() != 2 || children[0] == 0 || children[1] == 0)
    throw std::runtime_error("invalid ambiguous type-id expression");
  const vector<AstId>& specifier_nodes =
      arena_.At(children[0]).children;
  if (specifier_nodes.size() != 1 ||
      arena_.At(specifier_nodes[0]).kind != AST_TYPE_NAME)
    throw std::runtime_error("ambiguous type-id has no callable name");
  const QualifiedName name = ReadQualifiedName(
      tokens_, arena_.At(specifier_nodes[0]).first,
      arena_.At(specifier_nodes[0]).last, true);
  const AstId clause = FindChild(children[1], AST_PARAMETER_CLAUSE);
  if (clause == 0)
    throw std::runtime_error("ambiguous type-id has no argument list");
  vector<SemaId> arguments;
  const vector<AstId>& argument_nodes = arena_.At(clause).children;
  for (size_t i = 0; i < argument_nodes.size(); ++i)
    arguments.push_back(AnalyzeAmbiguousParameter(argument_nodes[i], scope,
                                                  true));
  return AnalyzeNamedCall(type_id, name, scope, arguments);
}

SemaId ExpressionAnalyzer::AnalyzeNamedCall(
    AstId source, const QualifiedName& name, ScopeId scope,
    const vector<SemaId>& arguments)
{
  vector<BindingId> bindings;
  LookupNameBindings(name, scope, bindings);
  if (!name.Qualified())
  {
    vector<TypeId> argument_types;
    for (size_t i = 0; i < arguments.size(); ++i)
      argument_types.push_back(NodeInfo(arguments[i]).type);
    vector<BindingId> call_bindings;
    model_.LookupCallSet(scope, name.Last(), argument_types, call_bindings);
    FilterAccessibleBindings(scope, call_bindings);
    bindings.swap(call_bindings);
  }
  if (bindings.empty())
    throw std::runtime_error("unknown callable name in expression");

  SemaId implicit_object = 0;
  bool has_implicit_object = false;
  for (size_t i = 0; i < bindings.size(); ++i)
  {
    const Binding& binding = model_.BindingAt(bindings[i]);
    if (binding.kind != BINDING_FUNCTION || binding.function == 0)
      continue;
    const FunctionEntity& function = model_.FunctionAt(binding.function);
    if (!function.is_member || function.static_member)
      continue;
    const BindingId this_binding = model_.LookupUnqualified(
        scope, "this", LOOKUP_VALUES);
    if (this_binding == 0)
      continue;
    implicit_object = MakeExpression(
        SEMA_ID_EXPRESSION, 0,
        model_.BindingAt(this_binding).type, VC_PRVALUE, scope, KW_THIS);
    tree_.At(implicit_object).binding = this_binding;
    has_implicit_object = true;
    break;
  }

  vector<OverloadArgument> overload_arguments;
  if (has_implicit_object)
  {
    const Info object = NodeInfo(implicit_object);
    overload_arguments.push_back(OverloadArgument(
        object.type, object.category, IsNullPointerConstant(implicit_object),
        object.is_function_lvalue, true));
  }
  for (size_t i = 0; i < arguments.size(); ++i)
  {
    const Info info = NodeInfo(arguments[i]);
    OverloadArgument argument(info.type, info.category,
                              IsNullPointerConstant(arguments[i]),
                              info.is_function_lvalue);
    const SemaNode& semantic = tree_.At(arguments[i]);
    if (semantic.kind == SEMA_ID_EXPRESSION && semantic.function != 0)
      FunctionCandidates(ReadQualifiedName(tokens_, semantic.first,
                                           semantic.last, true),
                         semantic.scope, argument.function_candidates);
    overload_arguments.push_back(argument);
  }
  const FunctionEntityId function = SelectBestOverload(
      model_, types_, bindings, overload_arguments, has_implicit_object);
  if (function == 0)
    throw std::runtime_error("no unique viable callable overload");
  return BuildResolvedCall(source, scope, function, implicit_object,
                           arguments);
}

void ExpressionAnalyzer::FoldLiteral(SemaId node, AstId expression)
{
  if (node == 0 || expression == 0)
    return;
  const AstNode& ast = arena_.At(expression);
  if (ast.first >= tokens_.size())
    return;
  const Pa6Token& token = tokens_[ast.first];
  if (ast.kind == AST_KEYWORD_LITERAL)
  {
    if (token.IsSimple(KW_TRUE))
    {
      tree_.At(node).has_value = true;
      tree_.At(node).value = 1;
    }
    else if (token.IsSimple(KW_FALSE))
    {
      tree_.At(node).has_value = true;
      tree_.At(node).value = 0;
    }
    return;
  }
  if (!token.lit_scalar || !types_.IsIntegral(tree_.At(node).type))
    return;
  if (!FundamentalIsIntegral(token.lit_type))
    return;
  if (token.lit_value > static_cast<unsigned long long>(
          std::numeric_limits<long long>::max()))
    return;
  tree_.At(node).has_value = true;
  tree_.At(node).value = static_cast<long long>(token.lit_value);
}

SemaId ExpressionAnalyzer::AnalyzeLiteral(AstId expression, ScopeId scope)
{
  const AstNode& ast = arena_.At(expression);
  if (ast.first >= tokens_.size())
    throw std::runtime_error("invalid literal");
  const Pa6Token& token = tokens_[ast.first];
  TypeId type = 0;
  ValueCategory category = VC_PRVALUE;
  ETokenType op = KW_AUTO;
  if (ast.kind == AST_KEYWORD_LITERAL)
  {
    if (token.IsSimple(KW_THIS))
    {
      const BindingId this_binding = model_.LookupUnqualified(
          scope, "this", LOOKUP_VALUES);
      if (this_binding == 0)
        throw std::runtime_error("this used outside a member function");
      const Binding& binding = model_.BindingAt(this_binding);
      const SemaId result = MakeExpression(SEMA_ID_EXPRESSION, expression,
                                           binding.type, VC_PRVALUE, scope,
                                           KW_THIS);
      tree_.At(result).binding = this_binding;
      return result;
    }
    if (token.IsSimple(KW_NULLPTR))
      type = types_.Fundamental(FT_NULLPTR_T);
    else if (token.IsSimple(KW_TRUE) || token.IsSimple(KW_FALSE))
      type = types_.Fundamental(FT_BOOL);
    else
      throw std::runtime_error("unsupported keyword literal");
    op = token.simple_type;
  }
  else if (token.lit_scalar)
    type = types_.Fundamental(token.lit_type);
  else if (token.lit_count != 0)
  {
    type = types_.Array(types_.Cv(types_.Fundamental(token.lit_type), true),
                        token.lit_count);
    category = VC_LVALUE;
  }
  else
    throw std::runtime_error("unsupported literal");

  const SemaId result = MakeExpression(SEMA_LITERAL, expression, type,
                                       category, scope, op);
  FoldLiteral(result, expression);
  if (ast.kind == AST_LITERAL && !token.lit_scalar &&
      token.lit_count != 0)
  {
    const size_t closing_quote = token.spelling.rfind('"');
    if (closing_quote != string::npos && closing_quote + 1 <
        token.spelling.size() && token.spelling[closing_quote + 1] == '_')
    {
      const string operator_name = "operator\"\"" +
          token.spelling.substr(closing_quote + 1);
      QualifiedName name;
      name.components.push_back(operator_name);
      vector<BindingId> bindings;
      LookupNameBindings(name, scope, bindings);
      const SemaId length = MakeExpression(
          SEMA_LITERAL, 0, types_.Fundamental(FT_INT),
          VC_PRVALUE, scope);
      tree_.At(length).has_value = true;
      tree_.At(length).value = static_cast<long long>(token.lit_count - 1);
      vector<SemaId> arguments;
      arguments.push_back(result);
      arguments.push_back(length);
      vector<OverloadCandidate> candidates;
      for (size_t i = 0; i < bindings.size(); ++i)
      {
        const Binding& binding = model_.BindingAt(bindings[i]);
        if (binding.kind != BINDING_FUNCTION || binding.function == 0 ||
            model_.FunctionAt(binding.function).is_member)
          continue;
        const TypeNode& callable = types_.At(types_.Unqualified(
            model_.FunctionAt(binding.function).type));
        OverloadCandidate candidate(binding.function);
        for (size_t argument = 0; argument < arguments.size(); ++argument)
        {
          if (argument < callable.parameters.size())
            candidate.arguments.push_back(MakeOperatorArgument(
                arguments[argument], callable.parameters[argument], scope));
          else
            candidate.arguments.push_back(OverloadArgument(
                NodeInfo(arguments[argument]).type,
                NodeInfo(arguments[argument]).category,
                IsNullPointerConstant(arguments[argument]),
                NodeInfo(arguments[argument]).is_function_lvalue));
        }
        candidates.push_back(candidate);
      }
      const FunctionEntityId function = SelectBestOverloadCandidates(
          model_, types_, candidates);
      if (function == 0)
        throw std::runtime_error("user-defined string literal is not viable");
      return BuildResolvedCall(expression, scope, function, 0, arguments);
    }
  }
  return result;
}

SemaId ExpressionAnalyzer::AnalyzeName(AstId expression, ScopeId scope)
{
  const QualifiedName name = ExpressionName(expression);
  vector<BindingId> candidates;
  LookupNameBindings(name, scope, candidates);
  if (candidates.empty())
    throw std::runtime_error("unknown name in expression");

  // 7.3.4p6: the level names one entity or an overload set; non-function
  // declarations from two scopes make the name ambiguous.
  BindingId binding = 0;
  for (size_t i = 0; i < candidates.size(); ++i)
  {
    const Binding& candidate = model_.BindingAt(candidates[i]);
    if (candidate.kind == BINDING_FUNCTION)
      continue;
    if (binding != 0 && model_.BindingAt(binding).scope != candidate.scope)
      throw std::runtime_error("ambiguous name in expression");
    binding = candidates[i];
  }
  if (binding == 0)
    binding = candidates.back();
  const Binding& value = model_.BindingAt(binding);
  if (value.kind == BINDING_TYPE || value.kind == BINDING_TYPE_ALIAS ||
      value.kind == BINDING_NAMESPACE)
    throw std::runtime_error("type or namespace used as expression");

  if (value.kind == BINDING_ENUMERATOR)
  {
    const SemaId result = MakeExpression(SEMA_LITERAL, expression, value.type,
                                         VC_PRVALUE, scope);
    SemaNode& node = tree_.At(result);
    node.binding = binding;
    node.has_value = value.has_const_value;
    node.value = value.const_value;
    return result;
  }
  if (value.kind == BINDING_FUNCTION)
  {
    if (value.function == 0)
      throw std::runtime_error("function has no semantic entity");
    const TypeId type = model_.FunctionAt(value.function).type;
    const SemaId result = MakeExpression(SEMA_ID_EXPRESSION, expression,
                                         type, VC_LVALUE, scope);
    SemaNode& node = tree_.At(result);
    node.binding = binding;
    node.function = value.function;
    return result;
  }

  // An unqualified name in a member function can denote a member declared in
  // the enclosing class.  Preserve that fact as the same member projection
  // used by explicit `object.member` syntax, with the canonical `this`
  // parameter as its object for non-static members.
  if ((value.kind == BINDING_VARIABLE || value.kind == BINDING_PARAMETER) &&
      model_.ScopeAt(value.scope).kind == SCOPE_CLASS)
  {
    const SemaId result = MakeExpression(SEMA_MEMBER, expression, value.type,
                                         VC_LVALUE, scope);
    SemaNode& member = tree_.At(result);
    member.binding = binding;
    member.has_value = value.has_const_value;
    member.value = value.const_value;
    if (!value.static_member)
    {
      const BindingId this_binding = model_.LookupUnqualified(
          scope, "this", LOOKUP_VALUES);
      if (this_binding == 0)
        throw std::runtime_error("non-static member used outside a member function");
      const SemaId object = MakeExpression(SEMA_ID_EXPRESSION, 0,
                                           model_.BindingAt(this_binding).type,
                                           VC_PRVALUE, scope, KW_THIS);
      tree_.At(object).binding = this_binding;
      Append(result, object);
    }
    return result;
  }

  // 8.3.5p5: a parameter declared with array or function type has the
  // adjusted pointer type; a reference names its referent (5p5).
  TypeId type = value.type;
  if (value.kind == BINDING_PARAMETER &&
      (types_.Kind(type) == TYPE_ARRAY || types_.Kind(type) == TYPE_FUNCTION))
    type = types_.Decay(type);
  if (types_.Kind(type) == TYPE_REFERENCE)
    type = types_.Referent(type);
  const SemaId result = MakeExpression(SEMA_ID_EXPRESSION, expression, type,
                                       VC_LVALUE, scope);
  tree_.At(result).binding = binding;
  tree_.At(result).has_value = value.has_const_value;
  tree_.At(result).value = value.const_value;
  if (value.object_binding != 0)
  {
    // 9.5p5: an injected anonymous-union member is accessed through the
    // implicit object; both nodes print through their bindings.
    tree_.At(result).kind = SEMA_MEMBER;
    const SemaId object = tree_.Make(SEMA_ID_EXPRESSION);
    SemaNode& object_node = tree_.At(object);
    object_node.category = VC_LVALUE;
    object_node.type = model_.BindingAt(value.object_binding).type;
    object_node.binding = value.object_binding;
    object_node.scope = scope;
    tree_.Append(result, object);
  }
  return result;
}

void ExpressionAnalyzer::FilterAccessibleBindings(
    ScopeId scope, vector<BindingId>& bindings) const
{
  vector<BindingId> accessible;
  accessible.reserve(bindings.size());
  for (size_t i = 0; i < bindings.size(); ++i)
    if (model_.IsAccessible(bindings[i], scope))
      accessible.push_back(bindings[i]);
  bindings.swap(accessible);
}

void ExpressionAnalyzer::LookupNameBindings(const QualifiedName& name,
                                            ScopeId scope,
                                            vector<BindingId>& bindings)
{
  if (name.Qualified())
    model_.LookupQualifiedSet(scope, name, LOOKUP_ANY, bindings);
  else
    model_.LookupSet(scope, name.Last(), LOOKUP_ANY, bindings);
  if (!name.template_id)
  {
    FilterAccessibleBindings(scope, bindings);
    return;
  }

  // A template-id names the instances of the function templates in the set.
  vector<TypeId> arguments;
  if (!TemplateArgumentTypes(name, scope, arguments))
  {
    bindings.clear();
    return;
  }
  const vector<BindingId> templates = bindings;
  bindings.clear();
  for (size_t i = 0; i < templates.size(); ++i)
  {
    const Binding& candidate = model_.BindingAt(templates[i]);
    if (candidate.kind != BINDING_FUNCTION || candidate.function == 0 ||
        !model_.FunctionAt(candidate.function).is_template)
      continue;
    FunctionEntityId function = 0;
    BindingId binding = 0;
    if (builder_.InstantiateFunctionTemplate(candidate.function, arguments,
                                             function, binding))
      bindings.push_back(binding);
  }
  FilterAccessibleBindings(scope, bindings);
}

// Types of a template-id's arguments: each depth-0 comma-separated span is a
// fundamental type-specifier run or a (qualified) type name.
bool ExpressionAnalyzer::TemplateArgumentTypes(
    const QualifiedName& name, ScopeId scope, vector<TypeId>& arguments) const
{
  arguments.clear();
  if (!name.template_id)
    return false;
  size_t start = name.template_first;
  size_t depth = 0;
  for (size_t i = name.template_first; i <= name.template_last; ++i)
  {
    const bool end = i == name.template_last;
    if (!end && tokens_[i].IsSimple(OP_LT))
      ++depth;
    else if (!end && (tokens_[i].IsSimple(OP_GT) || tokens_[i].IsRshiftPart()))
      --depth;
    if (!end && !(tokens_[i].IsSimple(OP_COMMA) && depth == 0))
      continue;
    if (i == start)
      return false;
    vector<ETokenType> fundamental;
    for (size_t token = start; token < i; ++token)
    {
      if (tokens_[token].kind != PA6_SIMPLE_TOKEN ||
          !IsFundamentalTypeKeyword(tokens_[token].simple_type))
      {
        fundamental.clear();
        break;
      }
      fundamental.push_back(tokens_[token].simple_type);
    }
    if (!fundamental.empty())
      arguments.push_back(types_.FundamentalFromKeywords(fundamental));
    else
      arguments.push_back(builder_.TypeForName(
          ReadQualifiedName(tokens_, start, i), scope));
    start = i + 1;
  }
  return true;
}

void ExpressionAnalyzer::FunctionCandidates(
    const QualifiedName& name, ScopeId scope,
    vector<FunctionEntityId>& candidates)
{
  vector<BindingId> bindings;
  LookupNameBindings(name, scope, bindings);
  for (size_t i = 0; i < bindings.size(); ++i)
  {
    const Binding& binding = model_.BindingAt(bindings[i]);
    if (binding.kind != BINDING_FUNCTION || binding.function == 0)
      continue;
    if (std::find(candidates.begin(), candidates.end(), binding.function) ==
        candidates.end())
      candidates.push_back(binding.function);
  }
}

bool ExpressionAnalyzer::IsOperatorFunction(ETokenType op) const
{
  switch (op)
  {
  case OP_BOR: case OP_XOR: case OP_COMPL: case OP_AMP: case OP_LNOT:
  case OP_PLUS: case OP_MINUS: case OP_STAR: case OP_DIV: case OP_MOD:
  case OP_ASS: case OP_PLUSASS: case OP_MINUSASS: case OP_STARASS:
  case OP_DIVASS: case OP_MODASS: case OP_XORASS: case OP_BANDASS:
  case OP_BORASS: case OP_LSHIFT: case OP_RSHIFT: case OP_RSHIFTASS:
  case OP_LSHIFTASS: case OP_EQ: case OP_NE: case OP_LE: case OP_GE:
  case OP_LT: case OP_GT: case OP_LAND: case OP_LOR: case OP_INC:
  case OP_DEC: case OP_COMMA: case OP_ARROWSTAR: case OP_LSQUARE:
    return true;
  default:
    return false;
  }
}

string ExpressionAnalyzer::OperatorFunctionName(ETokenType op) const
{
  switch (op)
  {
  case OP_BOR: return "operator|";
  case OP_XOR: return "operator^";
  case OP_COMPL: return "operator~";
  case OP_AMP: return "operator&";
  case OP_LNOT: return "operator!";
  case OP_PLUS: return "operator+";
  case OP_MINUS: return "operator-";
  case OP_STAR: return "operator*";
  case OP_DIV: return "operator/";
  case OP_MOD: return "operator%";
  case OP_ASS: return "operator=";
  case OP_PLUSASS: return "operator+=";
  case OP_MINUSASS: return "operator-=";
  case OP_STARASS: return "operator*=";
  case OP_DIVASS: return "operator/=";
  case OP_MODASS: return "operator%=";
  case OP_XORASS: return "operator^=";
  case OP_BANDASS: return "operator&=";
  case OP_BORASS: return "operator|=";
  case OP_LSHIFT: return "operator<<";
  case OP_RSHIFT: return "operator>>";
  case OP_RSHIFTASS: return "operator>>=";
  case OP_LSHIFTASS: return "operator<<=";
  case OP_EQ: return "operator==";
  case OP_NE: return "operator!=";
  case OP_LE: return "operator<=";
  case OP_GE: return "operator>=";
  case OP_LT: return "operator<";
  case OP_GT: return "operator>";
  case OP_LAND: return "operator&&";
  case OP_LOR: return "operator||";
  case OP_INC: return "operator++";
  case OP_DEC: return "operator--";
  case OP_COMMA: return "operator,";
  case OP_ARROWSTAR: return "operator->*";
  case OP_LSQUARE: return "operator[]";
  default: break;
  }
  return string();
}

SemaId ExpressionAnalyzer::MakeImplicitObject(SemaId object, ScopeId scope)
{
  if (object == 0)
    return 0;
  TypeId object_type = NodeInfo(object).type;
  if (types_.Kind(object_type) == TYPE_REFERENCE)
    object_type = types_.Referent(object_type);
  const SemaId address = MakeExpression(
      SEMA_UNARY, 0, types_.Pointer(object_type), VC_PRVALUE, scope, OP_AMP);
  Append(address, object);
  return address;
}

OverloadArgument ExpressionAnalyzer::MakeOperatorArgument(
    SemaId expression, TypeId target, ScopeId scope)
{
  const Info info = NodeInfo(expression);
  OverloadArgument result(
      info.type, info.category, IsNullPointerConstant(expression),
      info.is_function_lvalue);
  const ImplicitConversion standard = Classify(
      model_, types_, result.type, result.category, result.is_null_literal,
      result.is_function_lvalue, target);
  if (standard.Viable())
    return result;

  TypeId class_type = target;
  if (types_.Kind(class_type) == TYPE_REFERENCE)
    class_type = types_.Referent(class_type);
  class_type = types_.Unqualified(class_type);
  if (types_.Kind(class_type) != TYPE_CLASS)
    return result;
  vector<SemaId> constructor_arguments(1, expression);
  try
  {
    (void)builder_.ResolveConstructor(
        class_type, constructor_arguments, scope, true);
  }
  catch (const std::runtime_error&)
  {
    return result;
  }
  result.type = class_type;
  result.category = VC_XVALUE;
  result.is_null_literal = false;
  result.is_function_lvalue = false;
  result.user_defined_conversion = true;
  return result;
}

SemaId ExpressionAnalyzer::BuildConstructorTemporary(
    AstId source, TypeId target, ScopeId scope,
    const vector<SemaId>& arguments, bool list_initialization,
    bool copy_initialization)
{
  const TypeId class_type = types_.Unqualified(target);
  if (types_.Kind(class_type) != TYPE_CLASS)
    throw std::runtime_error("constructor temporary target is not a class");
  const FunctionEntityId constructor =
      builder_.ResolveConstructor(
          class_type, arguments, scope, copy_initialization);
  const FunctionEntity& entity = model_.FunctionAt(constructor);
  const TypeNode& callable = types_.At(types_.Unqualified(entity.type));
  if (arguments.size() + 1 > callable.parameters.size())
    throw std::runtime_error("constructor temporary has too many arguments");

  vector<SemaId> converted;
  converted.reserve(callable.parameters.size() - 1);
  for (size_t i = 0; i < arguments.size(); ++i)
    converted.push_back(Initialize(arguments[i], callable.parameters[i + 1],
                                   false, list_initialization));
  for (size_t parameter = arguments.size() + 1;
       parameter < callable.parameters.size(); ++parameter)
  {
    if (parameter >= entity.default_arguments.size() ||
        entity.default_arguments[parameter] == 0)
      throw std::runtime_error("missing constructor argument");
    converted.push_back(AnalyzeInitializer(
        entity.default_arguments[parameter], entity.scope,
        callable.parameters[parameter]));
  }

  const SemaId action = MakeExpression(SEMA_CONSTRUCTOR_ACTION, source,
                                       class_type, VC_XVALUE, scope);
  tree_.At(action).function = constructor;
  const SemaId call = MakeExpression(
      SEMA_CALL, 0, types_.Fundamental(FT_VOID), VC_PRVALUE, scope);
  tree_.At(call).function = constructor;
  const SemaId callee = tree_.Make(SEMA_CALLEE);
  SemaNode& callee_node = tree_.At(callee);
  callee_node.scope = scope;
  callee_node.type = entity.type;
  callee_node.function = constructor;
  Append(action, call);
  Append(call, callee);
  for (size_t i = 0; i < converted.size(); ++i)
    Append(call, converted[i]);
  return action;
}

SemaId ExpressionAnalyzer::BuildResolvedCall(
    AstId source, ScopeId scope, FunctionEntityId function,
    SemaId implicit_object, const vector<SemaId>& arguments)
{
  if (function == 0)
    throw std::runtime_error("operator call has no function");
  const FunctionEntity& entity = model_.FunctionAt(function);
  const TypeId function_type = entity.type;
  const TypeNode& callable = types_.At(types_.Unqualified(function_type));
  const bool member_object = entity.is_member && !entity.static_member;
  if (member_object && implicit_object == 0)
    throw std::runtime_error("member operator has no implicit object");
  const std::size_t required_start = member_object ? 1 : 0;
  const std::size_t supplied = arguments.size() +
      (member_object ? 1 : 0);
  std::size_t required = callable.parameters.size();
  while (required > required_start &&
         required <= entity.default_arguments.size() &&
         entity.default_arguments[required - 1] != 0)
    --required;
  if (supplied < required ||
      (!callable.variadic && supplied > callable.parameters.size()))
    throw std::runtime_error("wrong number of operator arguments");

  vector<SemaId> converted;
  converted.reserve(callable.parameters.size());
  if (member_object)
    converted.push_back(BindImplicitObject(implicit_object,
                                           callable.parameters[0]));
  for (size_t i = 0; i < arguments.size(); ++i)
  {
    const size_t parameter = i + required_start;
    if (parameter < callable.parameters.size()) {
      try
      {
        converted.push_back(Initialize(arguments[i],
                                       callable.parameters[parameter]));
      }
      catch (const std::runtime_error&) {
        TypeId class_type = callable.parameters[parameter];
        if (types_.Kind(class_type) == TYPE_REFERENCE)
          class_type = types_.Referent(class_type);
        class_type = types_.Unqualified(class_type);
        if (types_.Kind(class_type) != TYPE_CLASS)
          throw;
        const vector<SemaId> constructor_arguments(1, arguments[i]);
        const SemaId temporary = BuildConstructorTemporary(
            0, class_type, scope, constructor_arguments, false, true);
        converted.push_back(Initialize(
            temporary, callable.parameters[parameter]));
      }
    } else
      converted.push_back(arguments[i]);
  }
  if (function != 0)
    AppendDefaultArguments(function, function_type, converted.size(),
                            converted);

  TypeId result_type = callable.result;
  ValueCategory result_category = VC_PRVALUE;
  if (types_.Kind(result_type) == TYPE_REFERENCE)
    result_category = types_.At(result_type).lvalue_reference ? VC_LVALUE :
        VC_XVALUE;
  const SemaId result = MakeExpression(SEMA_CALL, source, result_type,
                                       result_category, scope);
  const SemaId callee = tree_.Make(SEMA_CALLEE);
  SemaNode& callee_node = tree_.At(callee);
  callee_node.type = function_type;
  callee_node.function = function;
  callee_node.scope = scope;
  Append(result, callee);
  for (size_t i = 0; i < converted.size(); ++i)
    Append(result, converted[i]);
  builder_.MarkTemplateInstanceUsed(function);
  return result;
}

SemaId ExpressionAnalyzer::TryOperatorCall(
    AstId source, ScopeId scope, ETokenType op,
    const vector<SemaId>& operands, bool allow_member,
    bool allow_nonmember, bool postfix)
{
  if (!IsOperatorFunction(op) || operands.empty())
    return 0;
  const string name = OperatorFunctionName(op);
  if (name.empty())
    return 0;

  vector<TypeId> operand_types;
  operand_types.reserve(operands.size());
  for (size_t i = 0; i < operands.size(); ++i)
    operand_types.push_back(NodeInfo(operands[i]).type);

  vector<SemaId> explicit_operands = operands;
  SemaId postfix_argument = 0;
  if (postfix)
  {
    postfix_argument = MakeExpression(
        SEMA_LITERAL, 0, types_.Fundamental(FT_INT), VC_PRVALUE, scope);
    tree_.At(postfix_argument).has_value = true;
    tree_.At(postfix_argument).value = 0;
    explicit_operands.push_back(postfix_argument);
  }

  vector<OverloadCandidate> candidates;
  if (allow_nonmember)
  {
    vector<BindingId> bindings;
    model_.LookupOperatorSet(scope, name, operand_types, bindings);
    for (size_t i = 0; i < bindings.size(); ++i)
    {
      const Binding& binding = model_.BindingAt(bindings[i]);
      if (binding.kind != BINDING_FUNCTION || binding.function == 0 ||
          model_.FunctionAt(binding.function).is_member)
        continue;
      OverloadCandidate candidate(binding.function);
      const TypeNode& callable = types_.At(types_.Unqualified(
          model_.FunctionAt(binding.function).type));
      for (size_t j = 0; j < explicit_operands.size(); ++j)
      {
        if (j < callable.parameters.size())
          candidate.arguments.push_back(MakeOperatorArgument(
              explicit_operands[j], callable.parameters[j], scope));
        else
          candidate.arguments.push_back(OverloadArgument(
              NodeInfo(explicit_operands[j]).type,
              NodeInfo(explicit_operands[j]).category,
              IsNullPointerConstant(explicit_operands[j]),
              NodeInfo(explicit_operands[j]).is_function_lvalue));
      }
      candidates.push_back(candidate);
    }
  }

  SemaId implicit_object = 0;
  bool have_member_candidate = false;
  if (allow_member)
  {
    TypeId object_type = NodeInfo(operands[0]).type;
    if (types_.Kind(object_type) == TYPE_REFERENCE)
      object_type = types_.Referent(object_type);
    const TypeId object_unqualified = types_.Unqualified(object_type);
    if (types_.Kind(object_unqualified) == TYPE_CLASS)
    {
      vector<BindingId> member_bindings;
      model_.LookupMember(types_.At(object_unqualified).entity, name,
                          LOOKUP_FUNCTIONS, member_bindings);
      FilterAccessibleBindings(scope, member_bindings);
      for (size_t i = 0; i < member_bindings.size(); ++i)
      {
        const Binding& binding = model_.BindingAt(member_bindings[i]);
        if (binding.kind != BINDING_FUNCTION || binding.function == 0 ||
            !model_.FunctionAt(binding.function).is_member ||
            model_.FunctionAt(binding.function).static_member)
          continue;
        if (!have_member_candidate)
          implicit_object = MakeImplicitObject(operands[0], scope);
        have_member_candidate = true;
        OverloadCandidate candidate(binding.function);
        const Info object_info = NodeInfo(implicit_object);
        candidate.arguments.push_back(OverloadArgument(
            object_info.type, object_info.category,
            IsNullPointerConstant(implicit_object),
            object_info.is_function_lvalue, true));
        for (size_t j = 1; j < explicit_operands.size(); ++j)
        {
          const TypeNode& callable = types_.At(types_.Unqualified(
              model_.FunctionAt(binding.function).type));
          if (j < callable.parameters.size())
            candidate.arguments.push_back(MakeOperatorArgument(
                explicit_operands[j], callable.parameters[j], scope));
          else
            candidate.arguments.push_back(OverloadArgument(
                NodeInfo(explicit_operands[j]).type,
                NodeInfo(explicit_operands[j]).category,
                IsNullPointerConstant(explicit_operands[j]),
                NodeInfo(explicit_operands[j]).is_function_lvalue));
        }
        candidates.push_back(candidate);
      }
    }
  }
  if (candidates.empty())
    return 0;
  const FunctionEntityId selected = SelectBestOverloadCandidates(
      model_, types_, candidates);
  if (selected == 0)
    return 0;
  const FunctionEntity& selected_entity = model_.FunctionAt(selected);
  if (selected_entity.is_member && !selected_entity.static_member)
  {
    vector<SemaId> member_arguments;
    for (size_t i = 1; i < explicit_operands.size(); ++i)
      member_arguments.push_back(explicit_operands[i]);
    return BuildResolvedCall(source, scope, selected, implicit_object,
                             member_arguments);
  }
  return BuildResolvedCall(source, scope, selected, 0, explicit_operands);
}

SemaId ExpressionAnalyzer::TryCallableObjectCall(
    AstId source, ScopeId scope, SemaId object,
    const vector<SemaId>& arguments)
{
  TypeId object_type = NodeInfo(object).type;
  if (types_.Kind(object_type) == TYPE_REFERENCE)
    object_type = types_.Referent(object_type);
  object_type = types_.Unqualified(object_type);
  if (types_.Kind(object_type) != TYPE_CLASS)
    return 0;
  const SemaId implicit_object = MakeImplicitObject(object, scope);
  vector<BindingId> bindings;
  model_.LookupMember(types_.At(object_type).entity, "operator()",
                      LOOKUP_FUNCTIONS, bindings);
  FilterAccessibleBindings(scope, bindings);
  vector<OverloadCandidate> candidates;
  for (size_t i = 0; i < bindings.size(); ++i)
  {
    const Binding& binding = model_.BindingAt(bindings[i]);
    if (binding.kind != BINDING_FUNCTION || binding.function == 0 ||
        !model_.FunctionAt(binding.function).is_member ||
        model_.FunctionAt(binding.function).static_member)
      continue;
    OverloadCandidate candidate(binding.function);
    const Info object_info = NodeInfo(implicit_object);
    candidate.arguments.push_back(OverloadArgument(
        object_info.type, object_info.category,
        IsNullPointerConstant(implicit_object),
        object_info.is_function_lvalue, true));
    for (size_t j = 0; j < arguments.size(); ++j)
    {
      const TypeNode& callable = types_.At(types_.Unqualified(
          model_.FunctionAt(binding.function).type));
      candidate.arguments.push_back(MakeOperatorArgument(
          arguments[j], callable.parameters[j], scope));
    }
    candidates.push_back(candidate);
  }
  if (candidates.empty())
    return 0;
  const FunctionEntityId selected = SelectBestOverloadCandidates(
      model_, types_, candidates);
  if (selected == 0)
    return 0;
  return BuildResolvedCall(source, scope, selected, implicit_object, arguments);
}

SemaId ExpressionAnalyzer::AnalyzeMember(AstId expression, ScopeId scope)
{
  if (arena_.At(expression).children.size() != 2)
    throw std::runtime_error("invalid member expression");
  const SemaId object = Analyze(Child(expression, 0), scope);
  TypeId object_type = NodeInfo(object).type;
  if (types_.Kind(object_type) == TYPE_REFERENCE)
    object_type = types_.Referent(object_type);
  bool object_const = false;
  bool object_volatile = false;
  if (types_.Kind(object_type) == TYPE_CV)
  {
    object_const = types_.At(object_type).is_const;
    object_volatile = types_.At(object_type).is_volatile;
  }

  const ETokenType op = Operator(expression);
  TypeId class_type = types_.Unqualified(object_type);
  if (op == OP_ARROW)
  {
    if (!types_.IsPointer(object_type))
      throw std::runtime_error("arrow requires a pointer to class");
    const TypeId pointee = types_.At(types_.Unqualified(object_type)).base;
    object_const = object_const ||
        (types_.Kind(pointee) == TYPE_CV && types_.At(pointee).is_const);
    object_volatile = object_volatile ||
        (types_.Kind(pointee) == TYPE_CV && types_.At(pointee).is_volatile);
    class_type = types_.Unqualified(pointee);
  }
  if (types_.Kind(class_type) != TYPE_CLASS)
    throw std::runtime_error("member access requires a class object");
  ScopeId class_scope = 0;
  if (!model_.ScopeOfType(class_type, class_scope))
    throw std::runtime_error("member access names an incomplete class");
  const AstId member_name = Child(expression, 1);
  const QualifiedName name = ReadQualifiedName(
      tokens_, arena_.At(member_name).first, arena_.At(member_name).last);
  if (name.Qualified())
    throw std::runtime_error("member access has a qualified member name");
  std::vector<BindingId> candidates;
  model_.LookupMember(types_.At(class_type).entity, name.Last(), LOOKUP_ANY,
                      candidates);
  FilterAccessibleBindings(scope, candidates);
  if (candidates.empty())
    throw std::runtime_error("unknown class member");
  BindingId binding = candidates.back();
  for (std::size_t i = 0; i < candidates.size(); ++i)
    if (model_.BindingAt(candidates[i]).kind != BINDING_FUNCTION)
    {
      if (binding != candidates[i] &&
          model_.BindingAt(binding).kind != BINDING_FUNCTION)
        throw std::runtime_error("ambiguous class member");
      binding = candidates[i];
      break;
    }
  const Binding& member = model_.BindingAt(binding);
  TypeId type = member.type;
  if (!member.static_member && (object_const || object_volatile))
    type = types_.Cv(type, object_const, object_volatile);
  const SemaId result = MakeExpression(SEMA_MEMBER, expression, type,
                                       VC_LVALUE, scope, op);
  SemaNode& semantic = tree_.At(result);
  semantic.binding = binding;
  semantic.has_value = member.has_const_value;
  semantic.value = member.const_value;
  semantic.first = arena_.At(member_name).first;
  semantic.last = arena_.At(member_name).last;
  Append(result, object);
  return result;
}

bool ExpressionAnalyzer::IsModifiableLvalue(SemaId node) const
{
  if (node == 0 || tree_.At(node).category != VC_LVALUE)
    return false;
  TypeId type = tree_.At(node).type;
  if (types_.Kind(type) == TYPE_REFERENCE)
    type = types_.Referent(type);
  if (types_.Kind(type) == TYPE_ARRAY || types_.Kind(type) == TYPE_FUNCTION)
    return false;
  return !IsConstObject(types_, type);
}

SemaId ExpressionAnalyzer::AnalyzeUnary(AstId expression, ScopeId scope)
{
  if (arena_.At(expression).children.size() != 1)
    throw std::runtime_error("invalid unary expression");
  const ETokenType op = Operator(expression);
  const SemaId operand = Analyze(Child(expression, 0), scope);
  const Info info = NodeInfo(operand);
  if (IsOperatorFunction(op))
  {
    const vector<SemaId> operands(1, operand);
    const SemaId overloaded = TryOperatorCall(
        expression, scope, op, operands, true, true, false);
    if (overloaded != 0)
      return overloaded;
  }
  const TypeId value_type = types_.Kind(info.type) == TYPE_REFERENCE ?
      types_.Referent(info.type) : info.type;
  TypeId result_type = 0;
  ValueCategory category = VC_PRVALUE;
  switch (op)
  {
  case OP_AMP:
    if (info.category != VC_LVALUE && !info.is_function_lvalue)
      throw std::runtime_error("address-of requires an lvalue");
    {
      const ClassField* field = tree_.At(operand).kind == SEMA_MEMBER ?
          model_.FieldFor(tree_.At(operand).binding) : 0;
      if (field != 0 && field->bit_width != 0)
        throw std::runtime_error("address-of a bit-field is invalid");
    }
    if (info.is_function_lvalue && tree_.At(operand).function != 0 &&
        model_.FunctionAt(tree_.At(operand).function).is_member)
      result_type = model_.FunctionAt(tree_.At(operand).function).
          member_pointer_type;
    else
      result_type = types_.Pointer(value_type);
    break;
  case OP_STAR:
  {
    TypeId pointer = types_.Decay(info.type);
    if (types_.Kind(pointer) != TYPE_POINTER)
      throw std::runtime_error("dereference requires a pointer");
    result_type = types_.At(pointer).base;
    category = VC_LVALUE;
    break;
  }
  case OP_INC: case OP_DEC:
    if (!IsModifiableLvalue(operand) || !types_.IsScalar(value_type))
      throw std::runtime_error("increment requires a modifiable scalar lvalue");
    result_type = value_type;
    category = VC_LVALUE;
    break;
  case OP_LNOT:
    if (!types_.IsScalar(value_type) || IsScopedEnum(types_, value_type))
      throw std::runtime_error("logical not requires a scalar operand");
    result_type = types_.Fundamental(FT_BOOL);
    break;
  case OP_PLUS: case OP_MINUS: case OP_COMPL:
    if (op == OP_PLUS &&
        types_.Kind(types_.Unqualified(value_type)) == TYPE_ARRAY)
    {
      // Unary plus is an lvalue-to-rvalue context.  Arrays first undergo the
      // standard array-to-pointer conversion, preserving the element cv.
      result_type = types_.Decay(value_type);
      break;
    }
    if (!types_.IsArithmetic(value_type) ||
        (op == OP_COMPL && !types_.IsIntegral(value_type)))
      throw std::runtime_error("unary arithmetic operator has invalid operand");
    result_type = types_.IsIntegral(value_type) ? types_.Promote(value_type) :
        types_.Unqualified(value_type);
    break;
  default:
    throw std::runtime_error("unsupported unary operator");
  }
  const SemaId result = MakeExpression(SEMA_UNARY, expression, result_type,
                                       category, scope, op);
  Append(result, operand);
  if (op != OP_INC && op != OP_DEC)
    FoldUnary(result, op, operand);
  return result;
}

SemaId ExpressionAnalyzer::AnalyzePostfix(AstId expression, ScopeId scope)
{
  if (arena_.At(expression).children.size() != 1)
    throw std::runtime_error("invalid postfix expression");
  const ETokenType op = Operator(expression);
  const SemaId operand = Analyze(Child(expression, 0), scope);
  const Info info = NodeInfo(operand);
  if (op == OP_INC || op == OP_DEC)
  {
    const vector<SemaId> operands(1, operand);
    const SemaId overloaded = TryOperatorCall(
        expression, scope, op, operands, true, true, true);
    if (overloaded != 0)
      return overloaded;
  }
  if ((op != OP_INC && op != OP_DEC) || !IsModifiableLvalue(operand) ||
      !types_.IsScalar(types_.Kind(info.type) == TYPE_REFERENCE ?
          types_.Referent(info.type) : info.type))
    throw std::runtime_error("postfix operator has invalid operand");
  const SemaId result = MakeExpression(SEMA_POSTFIX, expression,
                                       types_.Decay(info.type), VC_PRVALUE,
                                       scope, op);
  Append(result, operand);
  return result;
}

bool ExpressionAnalyzer::CanConvert(SemaId expression, TypeId target) const
{
  const Info source = NodeInfo(expression);
  return Classify(model_, types_, source.type, source.category,
                  IsNullPointerConstant(expression),
                  source.is_function_lvalue, target).Viable();
}

void ExpressionAnalyzer::CheckBaseConversionAccess(TypeId source,
                                                    TypeId target,
                                                    ScopeId scope) const
{
  TypeId source_type = source;
  TypeId target_type = target;
  if (types_.Kind(source_type) == TYPE_REFERENCE)
    source_type = types_.Referent(source_type);
  if (types_.Kind(target_type) == TYPE_REFERENCE)
    target_type = types_.Referent(target_type);
  source_type = types_.Unqualified(source_type);
  target_type = types_.Unqualified(target_type);

  if (types_.Kind(source_type) == TYPE_POINTER &&
      types_.Kind(target_type) == TYPE_POINTER)
  {
    source_type = types_.Unqualified(types_.At(source_type).base);
    target_type = types_.Unqualified(types_.At(target_type).base);
  }
  if (types_.Kind(source_type) != TYPE_CLASS ||
      types_.Kind(target_type) != TYPE_CLASS)
    return;
  const ClassEntityId source_class = static_cast<ClassEntityId>(
      types_.At(source_type).entity);
  const ClassEntityId target_class = static_cast<ClassEntityId>(
      types_.At(target_type).entity);
  if (model_.IsDerivedFrom(source_class, target_class) &&
      !model_.IsBaseAccessible(source_class, target_class, scope))
    throw std::runtime_error("inaccessible base-class conversion");
}

bool ExpressionAnalyzer::IsNarrowingListInitialization(
    SemaId expression, TypeId target) const
{
  TypeId source_type = types_.Unqualified(NodeInfo(expression).type);
  TypeId target_type = target;
  if (types_.Kind(target_type) == TYPE_REFERENCE)
    target_type = types_.Referent(target_type);
  target_type = types_.Unqualified(target_type);
  if (types_.Kind(source_type) != TYPE_FUNDAMENTAL ||
      types_.Kind(target_type) != TYPE_FUNDAMENTAL)
    return false;

  const EFundamentalType source_fundamental =
      types_.At(source_type).fundamental;
  const EFundamentalType target_fundamental =
      types_.At(target_type).fundamental;
  const bool source_floating = source_fundamental == FT_FLOAT ||
      source_fundamental == FT_DOUBLE ||
      source_fundamental == FT_LONG_DOUBLE;
  const bool target_floating = target_fundamental == FT_FLOAT ||
      target_fundamental == FT_DOUBLE ||
      target_fundamental == FT_LONG_DOUBLE;
  const bool target_integral = FundamentalIsIntegral(target_fundamental);
  if (source_floating && target_integral)
    return true;
  if (source_floating && target_floating &&
      FundamentalSize(source_fundamental) > FundamentalSize(target_fundamental))
    return true;
  if (FundamentalIsIntegral(source_fundamental) && target_floating)
  {
    // A constant integral expression is permitted when its value can be
    // represented exactly by the destination floating type.  This bounded
    // test covers the target's binary precision without evaluating a float.
    const SemaNode& node = tree_.At(expression);
    if (!node.has_value)
      return true;
    const long long value = node.value;
    const long long limit = target_fundamental == FT_FLOAT ?
        (1LL << 24) : target_fundamental == FT_DOUBLE ?
        (1LL << 53) : std::numeric_limits<long long>::max();
    return value > limit || value < -limit;
  }
  return false;
}

bool ExpressionAnalyzer::RetargetFunctionName(SemaId expression,
                                               TypeId target)
{
  if (expression == 0 || tree_.At(expression).kind != SEMA_ID_EXPRESSION ||
      tree_.At(expression).function == 0)
    return false;
  const SemaNode& source = tree_.At(expression);
  const QualifiedName name = ReadQualifiedName(tokens_, source.first,
                                               source.last, true);
  vector<BindingId> bindings;
  LookupNameBindings(name, source.scope, bindings);
  BindingId binding = 0;
  FunctionEntityId function = 0;
  if (!SelectTargetFunction(model_, types_, bindings, target, binding,
                            function))
    return false;
  SemaNode& retargeted = tree_.At(expression);
  retargeted.binding = binding;
  retargeted.function = function;
  retargeted.type = model_.FunctionAt(function).type;
  builder_.MarkTemplateInstanceUsed(function);
  return true;
}

bool ExpressionAnalyzer::RetargetFunctionAddress(SemaId expression,
                                                  TypeId target)
{
  if (expression == 0 || tree_.At(expression).kind != SEMA_UNARY ||
      tree_.At(expression).op != OP_AMP ||
      tree_.At(expression).first_child == 0)
    return false;
  const SemaId name_node = tree_.At(expression).first_child;
  if (tree_.At(name_node).kind != SEMA_ID_EXPRESSION)
    return false;
  if (!RetargetFunctionName(name_node, target))
    return false;
  const FunctionEntityId function = tree_.At(name_node).function;
  const FunctionEntity& entity = model_.FunctionAt(function);
  tree_.At(expression).type = entity.is_member ? entity.member_pointer_type :
      types_.Pointer(entity.type);
  return true;
}

SemaId ExpressionAnalyzer::AnalyzeBinary(AstId expression, ScopeId scope)
{
  if (arena_.At(expression).children.size() != 2)
    throw std::runtime_error("invalid binary expression");
  const ETokenType op = Operator(expression);
  const SemaId left = Analyze(Child(expression, 0), scope);
  const SemaId right = Analyze(Child(expression, 1), scope);
  if (IsOperatorFunction(op))
  {
    vector<SemaId> operands;
    operands.push_back(left);
    operands.push_back(right);
    const SemaId overloaded = TryOperatorCall(
        expression, scope, op, operands, true, true, false);
    if (overloaded != 0)
      return overloaded;
  }
  const Info lhs = NodeInfo(left);
  const Info rhs = NodeInfo(right);
  // Arithmetic, comparison, logical and pointer contexts apply the array and
  // function lvalue-to-rvalue conversions to their operands.  Keep the
  // original node types for the dump (notably `a` in `a + 2` remains an
  // array lvalue), but reason with their contextual types here.
  const TypeId left_type = types_.Decay(lhs.type);
  const TypeId right_type = types_.Decay(rhs.type);
  TypeId result_type = 0;
  switch (op)
  {
  case OP_COMMA:
    result_type = rhs.type;
    break;
  case OP_LAND: case OP_LOR:
    if (!types_.IsScalar(left_type) || !types_.IsScalar(right_type) ||
        IsScopedEnum(types_, left_type) || IsScopedEnum(types_, right_type))
      throw std::runtime_error("logical operator requires scalar operands");
    result_type = types_.Fundamental(FT_BOOL);
    break;
  case OP_EQ: case OP_NE: case OP_LT: case OP_GT: case OP_LE: case OP_GE:
    if (types_.IsArithmetic(left_type) && types_.IsArithmetic(right_type))
      result_type = types_.Fundamental(FT_BOOL);
    else if (types_.IsNullPointerType(left_type) &&
             types_.IsNullPointerType(right_type))
      result_type = types_.Fundamental(FT_BOOL);
    else if (types_.IsPointer(left_type) && types_.IsPointer(right_type))
    {
      bool ok = false;
      (void)types_.CompositePointer(left_type, right_type, ok);
      if (!ok)
        throw std::runtime_error("incompatible pointer comparison");
      result_type = types_.Fundamental(FT_BOOL);
    }
    else if ((types_.IsPointer(left_type) && IsNullPointerConstant(right)) ||
             (types_.IsPointer(right_type) && IsNullPointerConstant(left)))
      result_type = types_.Fundamental(FT_BOOL);
    else
      throw std::runtime_error("invalid comparison operands");
    break;
  case OP_LSHIFT: case OP_RSHIFT:
    if (!types_.IsIntegral(left_type) || !types_.IsIntegral(right_type))
      throw std::runtime_error("shift requires integral operands");
    result_type = types_.Promote(left_type);
    break;
  case OP_AMP: case OP_BOR: case OP_XOR:
    if (!types_.IsIntegral(left_type) || !types_.IsIntegral(right_type))
      throw std::runtime_error("bitwise operator requires integral operands");
    result_type = types_.UsualArithmetic(left_type, right_type);
    break;
  case OP_PLUS: case OP_MINUS:
    if (types_.IsPointer(left_type) && types_.IsIntegral(right_type))
      result_type = left_type;
    else if (op == OP_PLUS && types_.IsIntegral(left_type) &&
             types_.IsPointer(right_type))
      result_type = right_type;
    else if (op == OP_MINUS && types_.IsPointer(left_type) &&
             types_.IsPointer(right_type))
    {
      bool ok = false;
      (void)types_.CompositePointer(left_type, right_type, ok);
      if (!ok)
        throw std::runtime_error("incompatible pointer subtraction");
      result_type = types_.Fundamental(FT_LONG_INT);
    }
    else if (types_.IsArithmetic(left_type) && types_.IsArithmetic(right_type))
      result_type = types_.UsualArithmetic(left_type, right_type);
    else
      throw std::runtime_error("invalid additive operands");
    break;
  case OP_STAR: case OP_DIV: case OP_MOD:
    if (!types_.IsArithmetic(left_type) || !types_.IsArithmetic(right_type) ||
        (op == OP_MOD && (!types_.IsIntegral(left_type) ||
                          !types_.IsIntegral(right_type))))
      throw std::runtime_error("invalid multiplicative operands");
    result_type = types_.UsualArithmetic(left_type, right_type);
    break;
  default:
    throw std::runtime_error("unsupported binary operator");
  }
  const ValueCategory result_category = op == OP_COMMA ? rhs.category :
      VC_PRVALUE;
  const SemaId result = MakeExpression(SEMA_BINARY, expression, result_type,
                                       result_category, scope, op);
  Append(result, left);
  Append(result, right);
  FoldBinary(result, op, left, right);
  return result;
}

SemaId ExpressionAnalyzer::AnalyzeAssignment(AstId expression, ScopeId scope)
{
  if (arena_.At(expression).children.size() != 2)
    throw std::runtime_error("invalid assignment expression");
  const ETokenType op = Operator(expression);
  const SemaId left = Analyze(Child(expression, 0), scope);
  const Info lhs = NodeInfo(left);
  const TypeId lhs_value = types_.Kind(lhs.type) == TYPE_REFERENCE ?
      types_.Referent(lhs.type) : lhs.type;
  const AstId right_ast = Child(expression, 1);
  const SemaId right = arena_.At(right_ast).kind == AST_BRACED_INIT_LIST ?
      AnalyzeBraced(right_ast, scope, lhs_value) :
      Analyze(right_ast, scope);
  if (IsOperatorFunction(op))
  {
    vector<SemaId> operands;
    operands.push_back(left);
    operands.push_back(right);
    const SemaId overloaded = TryOperatorCall(
        expression, scope, op, operands, true, true, false);
    if (overloaded != 0)
      return overloaded;
  }
  const Info rhs = NodeInfo(right);
  if (!IsModifiableLvalue(left))
    throw std::runtime_error("assignment requires a modifiable lvalue");
  if (op == OP_ASS)
  {
    if (!CanConvert(right, lhs_value))
      throw std::runtime_error("assignment conversion is not viable");
  }
  else
  {
    const bool pointer_add = (op == OP_PLUSASS || op == OP_MINUSASS) &&
        types_.IsPointer(lhs_value) && types_.IsIntegral(rhs.type);
    const bool arithmetic = types_.IsArithmetic(lhs_value) &&
        types_.IsArithmetic(rhs.type);
    if (!pointer_add && !arithmetic)
      throw std::runtime_error("compound assignment operands are invalid");
    if (op == OP_STARASS || op == OP_DIVASS || op == OP_MODASS ||
        op == OP_LSHIFTASS || op == OP_RSHIFTASS || op == OP_BANDASS ||
        op == OP_XORASS || op == OP_BORASS)
    {
      if (!arithmetic || (op == OP_MODASS &&
                          (!types_.IsIntegral(lhs_value) ||
                           !types_.IsIntegral(rhs.type))))
        throw std::runtime_error("compound assignment operands are invalid");
    }
  }
  const SemaId result = MakeExpression(SEMA_ASSIGNMENT, expression, lhs_value,
                                       VC_LVALUE, scope, op);
  Append(result, left);
  Append(result, right);
  return result;
}

TypeId ExpressionAnalyzer::CommonConditionalType(SemaId left,
                                                 SemaId right) const
{
  const Info lhs = NodeInfo(left);
  const Info rhs = NodeInfo(right);
  TypeTable& mutable_types = const_cast<TypeTable&>(types_);
  const TypeId left_type = mutable_types.Decay(lhs.type);
  const TypeId right_type = mutable_types.Decay(rhs.type);
  if (left_type == right_type)
    return left_type;
  if (types_.IsArithmetic(left_type) && types_.IsArithmetic(right_type))
    return mutable_types.UsualArithmetic(left_type, right_type);
  if (types_.IsPointer(left_type) && types_.IsPointer(right_type))
  {
    bool ok = false;
    const TypeId composite = mutable_types.CompositePointer(
        left_type, right_type, ok);
    if (ok)
      return composite;
  }
  if (types_.IsPointer(left_type) && IsNullPointerConstant(right))
    return left_type;
  if (types_.IsPointer(right_type) && IsNullPointerConstant(left))
    return right_type;
  if (CanConvert(left, right_type))
    return right_type;
  if (CanConvert(right, left_type))
    return left_type;
  return 0;
}

SemaId ExpressionAnalyzer::AnalyzeConditional(AstId expression, ScopeId scope)
{
  if (arena_.At(expression).children.size() != 3)
    throw std::runtime_error("invalid conditional expression");
  const SemaId condition = Analyze(Child(expression, 0), scope);
  const Info condition_info = NodeInfo(condition);
  if (!types_.IsScalar(condition_info.type) ||
      IsScopedEnum(types_, condition_info.type))
    throw std::runtime_error("conditional condition is not contextual bool");
  const SemaId left = Analyze(Child(expression, 1), scope);
  const SemaId right = Analyze(Child(expression, 2), scope);
  const TypeId result_type = CommonConditionalType(left, right);
  if (result_type == 0)
    throw std::runtime_error("conditional operands have no common type");
  const Info lhs = NodeInfo(left);
  const Info rhs = NodeInfo(right);
  const ValueCategory category = lhs.category == VC_LVALUE &&
      rhs.category == VC_LVALUE && lhs.type == rhs.type ? VC_LVALUE :
      VC_PRVALUE;
  const SemaId result = MakeExpression(SEMA_CONDITIONAL, expression,
                                       result_type, category, scope);
  Append(result, condition);
  Append(result, left);
  Append(result, right);
  FoldConditional(result, condition, left, right);
  return result;
}

SemaId ExpressionAnalyzer::AnalyzeSubscript(AstId expression, ScopeId scope)
{
  if (arena_.At(expression).children.size() != 2)
    throw std::runtime_error("invalid subscript expression");
  SemaId first = Analyze(Child(expression, 0), scope);
  SemaId second = Analyze(Child(expression, 1), scope);
  {
    vector<SemaId> operands;
    operands.push_back(first);
    operands.push_back(second);
    const SemaId overloaded = TryOperatorCall(
        expression, scope, OP_LSQUARE, operands, true, false, false);
    if (overloaded != 0)
      return overloaded;
  }
  TypeId first_type = NodeInfo(first).type;
  TypeId second_type = NodeInfo(second).type;
  if (types_.Kind(first_type) == TYPE_REFERENCE)
    first_type = types_.Referent(first_type);
  if (types_.Kind(second_type) == TYPE_REFERENCE)
    second_type = types_.Referent(second_type);
  const bool first_pointer = types_.IsPointer(first_type) ||
      types_.Kind(first_type) == TYPE_ARRAY;
  const bool second_pointer = types_.IsPointer(second_type) ||
      types_.Kind(second_type) == TYPE_ARRAY;
  if (!first_pointer && second_pointer)
  {
    std::swap(first, second);
    std::swap(first_type, second_type);
  }
  if (types_.Kind(first_type) != TYPE_ARRAY && !types_.IsPointer(first_type))
    throw std::runtime_error("subscript base is not an array or pointer");
  if (!types_.IsIntegral(second_type))
    throw std::runtime_error("subscript index is not integral");
  TypeId element = types_.Kind(first_type) == TYPE_ARRAY ?
      types_.At(first_type).base :
      types_.At(types_.Unqualified(first_type)).base;
  const SemaId result = MakeExpression(SEMA_SUBSCRIPT, expression, element,
                                       VC_LVALUE, scope);
  Append(result, first);
  Append(result, second);
  return result;
}

bool ExpressionAnalyzer::IsTypeName(AstId expression, ScopeId scope,
                                    TypeId& type) const
{
  if (expression == 0)
    return false;
  const AstNode& node = arena_.At(expression);
  if (node.kind != AST_ID_EXPRESSION && node.kind != AST_IDENTIFIER)
    return false;
  const QualifiedName name = ReadQualifiedName(tokens_, node.first,
                                               node.last, true);
  if (name.template_id)
    return false;
  if (!name.Qualified() && name.Last() == "nullptr_t")
  {
    type = types_.Fundamental(FT_NULLPTR_T);
    return true;
  }
  const BindingId binding = name.Qualified() ?
      model_.LookupQualified(scope, name, LOOKUP_TYPES) :
      model_.LookupTypeName(scope, name.Last());
  if (binding == 0)
    return false;
  const Binding& value = model_.BindingAt(binding);
  if (value.kind != BINDING_TYPE && value.kind != BINDING_TYPE_ALIAS)
    return false;
  type = value.type;
  return true;
}

bool ExpressionAnalyzer::IsFundamentalCastCallee(AstId callee) const
{
  if (callee == 0)
    return false;
  const AstNode& node = arena_.At(callee);
  if (node.kind != AST_ID_EXPRESSION && node.kind != AST_IDENTIFIER)
    return false;
  if (node.first >= tokens_.size())
    return false;
  const Pa6Token& token = tokens_[node.first];
  return token.kind == PA6_SIMPLE_TOKEN &&
      (IsFundamentalTypeKeyword(token.simple_type) ||
       token.IsSimple(KW_DECLTYPE));
}

void ExpressionAnalyzer::AppendDefaultArguments(
    FunctionEntityId function, TypeId function_type, std::size_t supplied,
    vector<SemaId>& arguments)
{
  const FunctionEntity& entity = model_.FunctionAt(function);
  const TypeNode& callable = types_.At(types_.Unqualified(function_type));
  for (size_t i = supplied; i < callable.parameters.size(); ++i)
  {
    if (i >= entity.default_arguments.size() ||
        entity.default_arguments[i] == 0)
      throw std::runtime_error("missing default argument");
    arguments.push_back(AnalyzeInitializer(
        entity.default_arguments[i], entity.scope, callable.parameters[i]));
  }
}

SemaId ExpressionAnalyzer::AnalyzeCast(AstId expression, ScopeId scope)
{
  if (arena_.At(expression).children.size() != 2)
    throw std::runtime_error("invalid cast expression");
  const TypeId target = builder_.TypeOfTypeId(Child(expression, 0), scope);
  const SemaId operand = Analyze(Child(expression, 1), scope);
  CheckBaseConversionAccess(NodeInfo(operand).type, target, scope);
  const TypeId target_kind = types_.Unqualified(target);
  if (types_.Kind(target_kind) == TYPE_POINTER ||
      types_.Kind(target_kind) == TYPE_MEMBER_POINTER)
    (void)RetargetFunctionAddress(operand, target);
  if (types_.Kind(target) == TYPE_REFERENCE)
  {
    const Info source = NodeInfo(operand);
    if (types_.At(target).lvalue_reference && source.category != VC_LVALUE)
      throw std::runtime_error("lvalue reference cast requires an lvalue");
    // The cast expression retains its reference type in the semantic tree;
    // its value category is the category of the referred-to expression.
    // Keeping the reference wrapper is observable for decltype and for an
    // rvalue-reference-to-array used as a subscript base.
    tree_.At(operand).type = target;
    tree_.At(operand).category = types_.At(target).lvalue_reference ?
        VC_LVALUE : VC_XVALUE;
    return operand;
  }
  if (!types_.IsScalar(target) && !IsVoid(types_, target))
    throw std::runtime_error("unsupported cast target");
  if (types_.Kind(target_kind) == TYPE_MEMBER_POINTER &&
      types_.Unqualified(tree_.At(operand).type) == target_kind)
    return operand;
  const SemaId result = MakeExpression(SEMA_CAST, expression, target,
                                       VC_PRVALUE, scope, Operator(expression));
  Append(result, operand);
  FoldConversion(result, operand, target);
  return result;
}

SemaId ExpressionAnalyzer::AnalyzeSizeof(AstId expression, ScopeId scope)
{
  if (arena_.At(expression).children.size() != 1)
    throw std::runtime_error("invalid sizeof expression");
  const ETokenType op = Operator(expression);
  const bool alignment = op == KW_ALIGNOF;
  if (!alignment && op != KW_SIZEOF &&
      arena_.At(expression).kind != AST_SIZEOF_EXPRESSION)
    throw std::runtime_error("unsupported type trait");
  const AstId operand = Child(expression, 0);
  TypeId type = 0;
  if (arena_.At(operand).kind == AST_TYPE_ID)
  {
    try
    {
      type = builder_.TypeOfTypeId(operand, scope);
    }
    catch (const std::runtime_error&)
    {
      // `sizeof(derived::select(index()))` is parsed as a type-id because
      // the parser cannot use semantic overload lookup at that point.  The
      // type-name is a callable member set, so resolve the expression-shaped
      // form and retain only its result type in this unevaluated context.
      const std::size_t mark = tree_.Mark();
      const SemaId analyzed = AnalyzeAmbiguousTypeId(operand, scope);
      type = tree_.At(analyzed).type;
      tree_.Truncate(mark);
    }
  }
  else if (!IsTypeName(operand, scope, type))
    type = tree_.At(Analyze(operand, scope)).type;
  // 5.3.3p6, 5.3.6p3: std::size_t, which is unsigned long on this target.
  const size_t size = alignment ? types_.AlignOf(type) : types_.SizeOf(type);
  const SemaId result = MakeExpression(SEMA_SIZEOF, expression,
      types_.Fundamental(FT_UNSIGNED_LONG_INT), VC_PRVALUE, scope);
  tree_.At(result).has_value = true;
  tree_.At(result).value = static_cast<long long>(size);
  return result;
}

void ExpressionAnalyzer::ResolveCallCallee(AstId callee, ScopeId scope,
                                            CallResolution& result)
{
  const AstNode& callee_node = arena_.At(callee);
  result.member_callee = callee_node.kind == AST_MEMBER_EXPRESSION;
  result.named_callee = result.member_callee ||
      callee_node.kind == AST_ID_EXPRESSION ||
      callee_node.kind == AST_IDENTIFIER;
  if (result.member_callee) {
    const SemaId member = Analyze(callee, scope);
    std::vector<SemaId> member_children;
    for (SemaId child = tree_.At(member).first_child; child != 0;
         child = tree_.At(child).next_sibling)
      member_children.push_back(child);
    if (member_children.empty())
      throw std::runtime_error("member call has no object");
    const SemaId object = member_children[0];
    TypeId object_type = NodeInfo(object).type;
    if (types_.Kind(object_type) == TYPE_REFERENCE)
      object_type = types_.Referent(object_type);
    if (types_.IsPointer(object_type))
      object_type = types_.At(types_.Unqualified(object_type)).base;
    object_type = types_.Unqualified(object_type);
    if (types_.Kind(object_type) != TYPE_CLASS)
      throw std::runtime_error("member call object is not a class");
    const AstId member_name = Child(callee, 1);
    model_.LookupMember(types_.At(object_type).entity,
                        arena_.At(member_name).text, LOOKUP_FUNCTIONS,
                        result.bindings);
    FilterAccessibleBindings(scope, result.bindings);
    if (result.bindings.empty())
      throw std::runtime_error("member call names no function");
    for (std::size_t i = 0; i < result.bindings.size(); ++i) {
      const FunctionEntity& function = model_.FunctionAt(
          model_.BindingAt(result.bindings[i]).function);
      if (!function.is_member || function.static_member)
        continue;
      result.implicit_object = object;
      if (tokens_[arena_.At(callee).first].IsSimple(OP_DOT)) {
        TypeId address_type = NodeInfo(object).type;
        if (types_.Kind(address_type) == TYPE_REFERENCE)
          address_type = types_.Referent(address_type);
        const SemaId address = MakeExpression(
            SEMA_UNARY, 0, types_.Pointer(address_type), VC_PRVALUE, scope,
            OP_AMP);
        Append(address, object);
        result.implicit_object = address;
      }
      result.has_implicit_object = true;
      break;
    }
  } else if (result.named_callee) {
    result.name = ExpressionName(callee);
    LookupNameBindings(result.name, scope, result.bindings);
    for (std::size_t i = 0; i < result.bindings.size(); ++i) {
      const Binding& binding = model_.BindingAt(result.bindings[i]);
      if (binding.kind != BINDING_FUNCTION || binding.function == 0)
        continue;
      const FunctionEntity& function = model_.FunctionAt(binding.function);
      if (!function.is_member || function.static_member)
        continue;
      const BindingId this_binding = model_.LookupUnqualified(
          scope, "this", LOOKUP_VALUES);
      if (this_binding != 0) {
        result.implicit_object = MakeExpression(
            SEMA_ID_EXPRESSION, 0,
            model_.BindingAt(this_binding).type, VC_PRVALUE, scope, KW_THIS);
        tree_.At(result.implicit_object).binding = this_binding;
        result.has_implicit_object = true;
      }
      break;
    }
  }
}

SemaId ExpressionAnalyzer::TryFunctionalCast(AstId expression, AstId callee,
                                              AstId arguments, ScopeId scope)
{
  TypeId target = 0;
  if (IsFundamentalCastCallee(callee))
  {
    const AstNode& node = arena_.At(callee);
    vector<ETokenType> keywords;
    for (size_t i = node.first; i < node.last; ++i)
      if (tokens_[i].kind == PA6_SIMPLE_TOKEN &&
          IsFundamentalTypeKeyword(tokens_[i].simple_type))
        keywords.push_back(tokens_[i].simple_type);
    if (keywords.empty())
    {
      AstId decltype_node = FindChild(callee, AST_DECL_SPECIFIER);
      if (decltype_node == 0)
        decltype_node = FindChild(callee, AST_DECLTYPE_SPECIFIER);
      if (decltype_node == 0 || arena_.At(decltype_node).children.empty())
        throw std::runtime_error("invalid decltype functional cast");
      target = builder_.TypeOfDecltype(
          arena_.At(decltype_node).children[0], scope);
    }
    else
      target = types_.FundamentalFromKeywords(keywords);
  }
  else if (!IsTypeName(callee, scope, target))
    return 0;
  return AnalyzeFunctionalCast(expression, target, scope,
                               arena_.At(arguments).children);
}

SemaId ExpressionAnalyzer::TryCallableObjectExpression(
    AstId expression, AstId callee, AstId arguments, ScopeId scope)
{
  bool may_be_callable_object = callee != 0 &&
      arena_.At(callee).kind != AST_ID_EXPRESSION &&
      arena_.At(callee).kind != AST_IDENTIFIER;
  if (!may_be_callable_object && callee != 0)
  {
    const QualifiedName callee_name = ExpressionName(callee);
    vector<BindingId> direct_bindings;
    LookupNameBindings(callee_name, scope, direct_bindings);
    for (size_t i = 0; i < direct_bindings.size(); ++i)
      if (model_.BindingAt(direct_bindings[i]).kind != BINDING_FUNCTION)
      {
        may_be_callable_object = true;
        break;
      }
  }
  if (!may_be_callable_object)
    return 0;

  const SemaId object = Analyze(callee, scope);
  TypeId object_type = NodeInfo(object).type;
  if (types_.Kind(object_type) == TYPE_REFERENCE)
    object_type = types_.Referent(object_type);
  object_type = types_.Unqualified(object_type);
  if (types_.Kind(object_type) != TYPE_CLASS)
    return 0;

  const vector<AstId>& args = arena_.At(arguments).children;
  vector<SemaId> analyzed_arguments;
  analyzed_arguments.reserve(args.size());
  for (size_t i = 0; i < args.size(); ++i)
    analyzed_arguments.push_back(Analyze(args[i], scope));
  return TryCallableObjectCall(expression, scope, object,
                               analyzed_arguments);
}

SemaId ExpressionAnalyzer::AnalyzeCall(AstId expression, ScopeId scope)
{
  if (arena_.At(expression).children.size() != 2)
    throw std::runtime_error("invalid call expression");
  const AstId callee = Child(expression, 0);
  const AstId arguments = Child(expression, 1);
  const vector<AstId>& args = arena_.At(arguments).children;
  const SemaId functional_cast = TryFunctionalCast(
      expression, callee, arguments, scope);
  if (functional_cast != 0)
    return functional_cast;
  const SemaId callable_object = TryCallableObjectExpression(
      expression, callee, arguments, scope);
  if (callable_object != 0)
    return callable_object;

  CallResolution resolution;
  ResolveCallCallee(callee, scope, resolution);
  const bool member_callee = resolution.member_callee;
  const bool named_callee = resolution.named_callee;
  QualifiedName& name = resolution.name;
  vector<BindingId>& bindings = resolution.bindings;
  const SemaId implicit_object = resolution.implicit_object;
  const bool has_implicit_object = resolution.has_implicit_object;

  // Analyze arguments before selecting a candidate so every source
  // expression is owned by the semantic tree in source order.  Conversion
  // nodes are added only after the selected parameter list is known.
  vector<SemaId> analyzed_arguments;
  vector<OverloadArgument> overload_arguments;
  analyzed_arguments.reserve(args.size());
  overload_arguments.reserve(args.size() + (has_implicit_object ? 1 : 0));
  for (size_t i = 0; i < args.size(); ++i)
  {
    const SemaId argument = Analyze(args[i], scope);
    const Info info = NodeInfo(argument);
    analyzed_arguments.push_back(argument);
    overload_arguments.push_back(OverloadArgument(
        info.type, info.category, IsNullPointerConstant(argument),
        info.is_function_lvalue));
    const SemaNode& semantic = tree_.At(argument);
    if (semantic.kind == SEMA_ID_EXPRESSION && semantic.function != 0)
      FunctionCandidates(
          ReadQualifiedName(tokens_, semantic.first, semantic.last, true),
          semantic.scope, overload_arguments.back().function_candidates);
  }
  if (has_implicit_object)
  {
    const Info object_info = NodeInfo(implicit_object);
    overload_arguments.insert(overload_arguments.begin(), OverloadArgument(
        object_info.type, member_callee ? VC_PRVALUE : object_info.category,
        IsNullPointerConstant(implicit_object), object_info.is_function_lvalue,
        true));
  }

  // An unqualified call combines ordinary lookup with ADL at the call
  // site.  AnalyzeName intentionally does not do this, since a bare value
  // expression must not acquire function candidates merely from its type.
  if (named_callee && !member_callee && !name.Qualified())
  {
    vector<TypeId> argument_types;
    argument_types.reserve(overload_arguments.size());
    for (size_t i = 0; i < overload_arguments.size(); ++i)
      argument_types.push_back(overload_arguments[i].type);
    vector<BindingId> call_bindings;
    model_.LookupCallSet(scope, name.Last(), argument_types, call_bindings);
    FilterAccessibleBindings(scope, call_bindings);
    bindings.swap(call_bindings);
  }

  // Template candidates are instantiated from the already-typed arguments
  // before ordinary overload ranking.  This keeps deduction in the semantic
  // owner of template entities and gives SelectBestOverload only canonical
  // concrete function types to compare.
  if (named_callee)
  {
    vector<BindingId> resolved;
    for (size_t i = 0; i < bindings.size(); ++i)
    {
      const Binding& candidate = model_.BindingAt(bindings[i]);
      if (candidate.kind != BINDING_FUNCTION || candidate.function == 0 ||
          !model_.FunctionAt(candidate.function).is_template)
      {
        resolved.push_back(bindings[i]);
        continue;
      }
      vector<TypeId> argument_types;
      argument_types.reserve(overload_arguments.size());
      for (size_t argument = 0; argument < overload_arguments.size();
           ++argument)
        argument_types.push_back(overload_arguments[argument].type);
      FunctionEntityId function = 0;
      BindingId binding = 0;
      if (builder_.DeduceFunctionTemplate(candidate.function, argument_types,
                                          function, binding))
        resolved.push_back(binding);
    }
    bindings.swap(resolved);
  }

  FunctionEntityId function = 0;
  TypeId function_type = 0;
  SemaId indirect_callee = 0;
  bool has_function_binding = false;
  for (size_t i = 0; i < bindings.size(); ++i)
    if (model_.BindingAt(bindings[i]).kind == BINDING_FUNCTION)
    {
      has_function_binding = true;
      break;
    }
  const bool builtin_name = named_callee && !member_callee && bindings.empty() &&
      name.components.size() == 1 && !name.global && !name.template_id;
  if (named_callee && has_function_binding)
  {
    function = SelectBestOverload(model_, types_, bindings,
                                  overload_arguments, has_implicit_object);
    if (function == 0)
      throw std::runtime_error("no unique viable function overload");
    function_type = model_.FunctionAt(function).type;
    builder_.MarkTemplateInstanceUsed(function);
  }
  else if (builtin_name && name.Last() == "__builtin_abort")
  {
    if (!args.empty())
      throw std::runtime_error("__builtin_abort takes no arguments");
    function_type = types_.Function(types_.Fundamental(FT_VOID),
                                    vector<TypeId>());
    function = model_.CreateFunction(model_.GlobalScope(), name.Last(),
                                     function_type);
  }
  else if (builtin_name && name.Last() == "__builtin_constant_p")
  {
    if (args.size() != 1)
      throw std::runtime_error("__builtin_constant_p takes one argument");
    const SemaId result = MakeExpression(SEMA_LITERAL, 0,
        types_.Fundamental(FT_INT), VC_PRVALUE, scope);
    long long value = 0;
    tree_.At(result).has_value = true;
    tree_.At(result).value = TryConstant(analyzed_arguments[0], value) ? 1 : 0;
    return result;
  }
  else
  {
    indirect_callee = Analyze(callee, scope);
    if (!CallableFunctionType(types_, NodeInfo(indirect_callee).type,
                              function_type))
      throw std::runtime_error("called expression is not a function");
  }

  const TypeNode& callable = types_.At(types_.Unqualified(function_type));
  if (function != 0)
  {
    const FunctionEntity& entity = model_.FunctionAt(function);
    std::size_t required = callable.parameters.size();
    while (required > 0 && required <= entity.default_arguments.size() &&
           entity.default_arguments[required - 1] != 0)
      --required;
    const std::size_t supplied = args.size() +
        (entity.is_member && !entity.static_member ? 1 : 0);
    if (supplied < required ||
        (!callable.variadic && supplied > callable.parameters.size()))
      throw std::runtime_error("wrong number of call arguments");
  }
  else if ((!callable.variadic && callable.parameters.size() != args.size()) ||
           (callable.variadic && args.size() < callable.parameters.size()))
    throw std::runtime_error("wrong number of call arguments");

  vector<SemaId> converted_arguments;
  converted_arguments.reserve(callable.parameters.size());
  const bool selected_implicit_object = function != 0 &&
      model_.FunctionAt(function).is_member &&
      !model_.FunctionAt(function).static_member;
  if (selected_implicit_object)
    converted_arguments.push_back(BindImplicitObject(
        implicit_object, callable.parameters[0]));
  for (size_t i = 0; i < analyzed_arguments.size(); ++i)
  {
    SemaId argument = analyzed_arguments[i];
    const size_t parameter = i + (selected_implicit_object ? 1 : 0);
    if (parameter < callable.parameters.size())
      argument = Initialize(argument, callable.parameters[parameter]);
    converted_arguments.push_back(argument);
  }
  if (function != 0)
    AppendDefaultArguments(function, function_type, converted_arguments.size(),
                            converted_arguments);

  const TypeId result_type = callable.result;
  ValueCategory result_category = VC_PRVALUE;
  if (types_.Kind(result_type) == TYPE_REFERENCE)
    result_category = types_.At(result_type).lvalue_reference ? VC_LVALUE :
        VC_XVALUE;
  const SemaId result = MakeExpression(SEMA_CALL, expression, result_type,
                                       result_category, scope);
  if (function != 0)
  {
    const SemaId callee_semantic = tree_.Make(SEMA_CALLEE);
    SemaNode& callee_value = tree_.At(callee_semantic);
    callee_value.type = function_type;
    callee_value.function = function;
    callee_value.scope = scope;
    Append(result, callee_semantic);
  }
  else
    Append(result, indirect_callee);
  for (size_t i = 0; i < converted_arguments.size(); ++i)
    Append(result, converted_arguments[i]);
  return result;
}

SemaId ExpressionAnalyzer::AnalyzeNew(AstId expression, ScopeId scope)
{
  const vector<AstId>& children = arena_.At(expression).children;
  size_t index = 0;
  bool global_lookup = false;
  if (index < children.size() &&
      arena_.At(children[index]).kind == AST_GLOBAL_SCOPE) {
    global_lookup = true;
    ++index;
  }

  AstId placement = 0;
  if (index < children.size() &&
      arena_.At(children[index]).kind == AST_PLACEMENT) {
    placement = children[index++];
  }
  if (index >= children.size() ||
      arena_.At(children[index]).kind != AST_TYPE_ID)
    throw std::runtime_error("new-expression has no type-id");
  const AstId type_node = children[index++];
  const TypeId allocated_type = builder_.TypeOfTypeId(type_node, scope);
  if (allocated_type == 0 || types_.Kind(types_.Unqualified(allocated_type)) ==
      TYPE_FUNCTION)
    throw std::runtime_error("new-expression has an invalid allocation type");

  AstId initializer = 0;
  if (index < children.size()) {
    initializer = children[index++];
    if (arena_.At(initializer).kind != AST_INITIALIZER)
      throw std::runtime_error("new-expression has an invalid initializer");
    if (arena_.At(initializer).children.size() != 1)
      throw std::runtime_error("new-expression has an invalid initializer");
    initializer = arena_.At(initializer).children[0];
  }
  if (index != children.size())
    throw std::runtime_error("new-expression has extra syntax");

  // The allocation-size operand is an int-valued synthesized constant.  The
  // selected operator new parameter performs the ordinary integral
  // conversion, preserving the LowIR widening operation required by the
  // target ABI.
  const SemaId size = MakeExpression(
      SEMA_LITERAL, 0, types_.Fundamental(FT_INT), VC_PRVALUE, scope);
  tree_.At(size).has_value = true;
  tree_.At(size).value = static_cast<long long>(types_.SizeOf(allocated_type));

  vector<SemaId> allocation_arguments;
  allocation_arguments.push_back(size);
  if (placement != 0) {
    const vector<AstId>& placement_children =
        arena_.At(placement).children;
    if (placement_children.size() != 1 ||
        arena_.At(placement_children[0]).kind != AST_PAREN_ARGUMENT_LIST)
      throw std::runtime_error("new-expression has invalid placement");
    const vector<AstId>& arguments = arena_.At(placement_children[0]).children;
    for (size_t i = 0; i < arguments.size(); ++i)
      allocation_arguments.push_back(Analyze(arguments[i], scope));
  }

  vector<TypeId> allocation_types;
  vector<OverloadArgument> overload_arguments;
  allocation_types.reserve(allocation_arguments.size());
  overload_arguments.reserve(allocation_arguments.size());
  for (size_t i = 0; i < allocation_arguments.size(); ++i) {
    const Info info = NodeInfo(allocation_arguments[i]);
    allocation_types.push_back(info.type);
    overload_arguments.push_back(OverloadArgument(
        info.type, info.category, IsNullPointerConstant(allocation_arguments[i]),
        info.is_function_lvalue));
  }

  const ScopeId lookup_scope = global_lookup ? model_.GlobalScope() : scope;
  vector<BindingId> allocation_bindings;
  model_.LookupCallSet(lookup_scope, "operatornew", allocation_types,
                       allocation_bindings);
  if (allocation_bindings.empty())
    throw std::runtime_error("new-expression has no allocation function");
  const FunctionEntityId allocation_function = SelectBestOverload(
      model_, types_, allocation_bindings, overload_arguments, false);
  if (allocation_function == 0)
    throw std::runtime_error("new-expression has no viable allocation function");
  const SemaId allocation = BuildResolvedCall(
      0, scope, allocation_function, 0, allocation_arguments);

  SemaId initialization = 0;
  const TypeId class_type = types_.Unqualified(allocated_type);
  if (types_.Kind(class_type) == TYPE_CLASS) {
    vector<SemaId> constructor_arguments;
    if (initializer != 0) {
      const AstKind initializer_kind = arena_.At(initializer).kind;
      if (initializer_kind != AST_PAREN_INITIALIZER &&
          initializer_kind != AST_BRACED_INIT_LIST)
        throw std::runtime_error("new-expression has an invalid initializer");
      const vector<AstId>& arguments = arena_.At(initializer).children;
      if (initializer_kind == AST_BRACED_INIT_LIST &&
          model_.ClassAt(types_.At(class_type).entity).aggregate)
      {
        const SemaId aggregate =
            AnalyzeBraced(initializer, scope, allocated_type);
        constructor_arguments.reserve(tree_.At(aggregate).last_child == 0 ?
                                      0 : arguments.size());
        for (SemaId child = tree_.At(aggregate).first_child; child != 0;
             child = tree_.At(child).next_sibling)
          constructor_arguments.push_back(child);
        initialization = BuildConstructorTemporary(
            initializer, allocated_type, scope, constructor_arguments);
      }
      else {
        constructor_arguments.reserve(arguments.size());
        for (size_t i = 0; i < arguments.size(); ++i)
          constructor_arguments.push_back(Analyze(arguments[i], scope));
        initialization = BuildConstructorTemporary(
            initializer, allocated_type, scope, constructor_arguments,
            initializer_kind == AST_BRACED_INIT_LIST);
      }
    } else {
      initialization = BuildConstructorTemporary(
          0, allocated_type, scope, constructor_arguments);
    }
  } else if (initializer != 0) {
    const vector<AstId>& arguments = arena_.At(initializer).children;
    if (arguments.size() != 1)
      throw std::runtime_error("scalar new-expression needs one initializer");
    const SemaId value = Analyze(arguments[0], scope);
    initialization = Initialize(value, allocated_type);
  }

  const SemaId result = MakeExpression(
      SEMA_NEW_EXPRESSION, expression, types_.Pointer(allocated_type),
      VC_PRVALUE, scope);
  Append(result, allocation);
  Append(result, initialization);
  return result;
}

SemaId ExpressionAnalyzer::AnalyzeFunctionalCast(
    AstId expression, TypeId target, ScopeId scope,
    const vector<AstId>& args)
{
  const TypeId target_unqualified = types_.Unqualified(target);
  if (types_.Kind(target_unqualified) == TYPE_CLASS)
  {
    vector<SemaId> analyzed_arguments;
    analyzed_arguments.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i)
      analyzed_arguments.push_back(Analyze(args[i], scope));
    const FunctionEntityId constructor =
        builder_.ResolveConstructor(
            target, analyzed_arguments, scope);
    const FunctionEntity& entity = model_.FunctionAt(constructor);
    const TypeNode& callable = types_.At(types_.Unqualified(entity.type));
    if (args.size() + 1 > callable.parameters.size())
      throw std::runtime_error("functional cast has too many arguments");
    vector<SemaId> converted;
    converted.reserve(callable.parameters.size() - 1);
    for (size_t i = 0; i < analyzed_arguments.size(); ++i)
      converted.push_back(Initialize(analyzed_arguments[i],
                                     callable.parameters[i + 1], false, true));
    for (size_t parameter = analyzed_arguments.size() + 1;
         parameter < callable.parameters.size(); ++parameter)
    {
      if (parameter >= entity.default_arguments.size() ||
          entity.default_arguments[parameter] == 0)
        throw std::runtime_error("missing constructor argument");
      converted.push_back(AnalyzeInitializer(
          entity.default_arguments[parameter], entity.scope,
          callable.parameters[parameter]));
    }

    const SemaId action = MakeExpression(SEMA_CONSTRUCTOR_ACTION, expression,
                                         target, VC_XVALUE, scope);
    tree_.At(action).function = constructor;
    const SemaId call = MakeExpression(
        SEMA_CALL, 0, types_.Fundamental(FT_VOID), VC_PRVALUE, scope);
    tree_.At(call).function = constructor;
    const SemaId callee = tree_.Make(SEMA_CALLEE);
    SemaNode& callee_node = tree_.At(callee);
    callee_node.scope = scope;
    callee_node.type = entity.type;
    callee_node.function = constructor;
    Append(action, call);
    Append(call, callee);
    for (size_t i = 0; i < converted.size(); ++i)
      Append(call, converted[i]);
    return action;
  }

  if (args.size() > 1)
    throw std::runtime_error("functional cast has too many arguments");
  if (args.empty())
  {
    // 5.2.3p2: value-initialization of a scalar is a zero of that type.
    const SemaId result = MakeExpression(SEMA_LITERAL, 0, target,
                                         VC_PRVALUE, scope);
    tree_.At(result).has_value = types_.IsIntegral(target) ||
        types_.IsNullPointerType(target);
    tree_.At(result).value = 0;
    return result;
  }
  const SemaId operand = Analyze(args[0], scope);
  const Info source = NodeInfo(operand);
  // A typedef naming a reference is still an explicit reference cast when
  // written in functional notation (`R(x)`).  Keep the operand as the
  // canonical storage expression, including an intentional qualification
  // adjustment, just as AnalyzeCast does for `static_cast<T&>(x)`.
  if (types_.Kind(target) == TYPE_REFERENCE &&
      source.category == VC_LVALUE)
  {
    tree_.At(operand).type = target;
    tree_.At(operand).category = types_.At(target).lvalue_reference ?
        VC_LVALUE : VC_XVALUE;
    return operand;
  }
  const bool implicit_viable = Classify(
      model_, types_, source.type, source.category,
      IsNullPointerConstant(operand), source.is_function_lvalue,
      target).Viable();
  // Functional casts are explicit conversions.  In particular, a scoped
  // enumeration may be converted to its arithmetic destination here even
  // though the same source-to-int path is not an implicit conversion.
  const bool explicit_enum = IsScopedEnum(types_, source.type) &&
      types_.IsArithmetic(target);
  if (!implicit_viable && !explicit_enum)
    throw std::runtime_error("functional cast is not viable");
  const SemaId result = MakeExpression(SEMA_CAST, expression, target,
                                       VC_PRVALUE, scope);
  Append(result, operand);
  FoldConversion(result, operand, target);
  return result;
}

SemaId ExpressionAnalyzer::AnalyzeBraced(AstId expression, ScopeId scope,
                                         TypeId target)
{
  const vector<AstId>& children = arena_.At(expression).children;
  if (target == 0)
    throw std::runtime_error("braced initializer requires an array target");
  const TypeId target_unqualified = types_.Unqualified(target);
  if (types_.Kind(target_unqualified) == TYPE_CLASS) {
    const ClassEntity& class_entity = model_.ClassAt(
        types_.At(target_unqualified).entity);
    if (!class_entity.aggregate)
      throw std::runtime_error("braced initializer requires an aggregate");
    const SemaId result = MakeExpression(SEMA_BRACED_INIT_LIST, expression,
                                         target, VC_PRVALUE, scope);
    size_t index = 0;
    AnalyzeAggregateElements(children, index, scope, target, result);
    if (index != children.size())
      throw std::runtime_error("initializer list has the wrong bound");
    return result;
  }
  if (types_.Kind(target) != TYPE_ARRAY) {
    if (children.empty() || children.size() == 1)
    {
      const SemaId result = MakeExpression(SEMA_BRACED_INIT_LIST, expression,
                                           target, VC_PRVALUE, scope);
      if (children.size() == 1)
      {
        const SemaId child = Analyze(children[0], scope);
        Initialize(child, target, false, true);
        Append(result, child);
      }
      return result;
    }
    throw std::runtime_error("braced initializer target is not an array");
  }
  const TypeId element = types_.At(target).base;
  if (children.size() > types_.At(target).array_bound)
    throw std::runtime_error("initializer list has the wrong bound");
  const SemaId result = MakeExpression(SEMA_BRACED_INIT_LIST, expression,
                                       target, VC_LVALUE, scope);
  if (children.size() == 1 &&
      IsStringLiteralArrayClause(children[0], target)) {
    Append(result, Analyze(children[0], scope));
    return result;
  }
  size_t index = 0;
  while (index < children.size())
    Append(result, AnalyzeAggregateClause(children, children[index], scope,
                                          element, index));
  return result;
}

bool ExpressionAnalyzer::IsAggregateType(TypeId type) const
{
  const TypeId unqualified = types_.Unqualified(type);
  if (types_.Kind(unqualified) == TYPE_ARRAY)
    return true;
  return types_.Kind(unqualified) == TYPE_CLASS &&
      model_.ClassAt(types_.At(unqualified).entity).aggregate;
}

bool ExpressionAnalyzer::IsStringLiteralArrayClause(AstId clause,
                                                     TypeId target) const
{
  if (clause == 0 || arena_.At(clause).kind != AST_LITERAL ||
      arena_.At(clause).first >= tokens_.size())
    return false;
  const TypeId unqualified = types_.Unqualified(target);
  if (types_.Kind(unqualified) != TYPE_ARRAY)
    return false;
  const TypeId element = types_.Unqualified(types_.At(unqualified).base);
  if (types_.Kind(element) != TYPE_FUNDAMENTAL)
    return false;
  switch (types_.At(element).fundamental)
  {
  case FT_CHAR: case FT_SIGNED_CHAR: case FT_UNSIGNED_CHAR:
  case FT_WCHAR_T: case FT_CHAR16_T: case FT_CHAR32_T:
    break;
  default:
    return false;
  }
  return tokens_[arena_.At(clause).first].lit_count != 0;
}

SemaId ExpressionAnalyzer::AnalyzeAggregateClause(
    const vector<AstId>& clauses, AstId clause, ScopeId scope, TypeId target,
    std::size_t& index)
{
  if (IsStringLiteralArrayClause(clause, target)) {
    const Pa6Token& token = tokens_[arena_.At(clause).first];
    const TypeId array = types_.Unqualified(target);
    if (token.lit_count > types_.At(array).array_bound)
      throw std::runtime_error("string initializer exceeds array bound");
    const SemaId result = MakeExpression(SEMA_BRACED_INIT_LIST, 0, target,
                                         VC_PRVALUE, scope);
    Append(result, Analyze(clause, scope));
    ++index;
    return result;
  }
  if (arena_.At(clause).kind == AST_BRACED_INIT_LIST) {
    if (IsAggregateType(target)) {
      ++index;
      return AnalyzeBraced(clause, scope, target);
    }
    const TypeId unqualified = types_.Unqualified(target);
    if (types_.Kind(unqualified) != TYPE_CLASS) {
      ++index;
      return AnalyzeBraced(clause, scope, target);
    }
    const vector<AstId>& arguments = arena_.At(clause).children;
    vector<SemaId> analyzed;
    analyzed.reserve(arguments.size());
    for (size_t i = 0; i < arguments.size(); ++i)
      analyzed.push_back(Analyze(arguments[i], scope));
    ++index;
    return BuildConstructorTemporary(clause, target, scope, analyzed, true);
  }
  if (IsAggregateType(target))
    return AnalyzeElidedAggregate(clauses, index, scope, target);

  const SemaId expression = Analyze(clause, scope);
  ++index;
  const TypeId unqualified = types_.Unqualified(target);
  if (types_.Kind(unqualified) == TYPE_CLASS)
    if (tree_.At(expression).kind == SEMA_CONSTRUCTOR_ACTION)
      return expression;
  if (types_.Kind(unqualified) == TYPE_CLASS)
    return BuildConstructorTemporary(clause, target, scope,
                                     std::vector<SemaId>(1, expression));
  Initialize(expression, target, false, true);
  return expression;
}

void ExpressionAnalyzer::AnalyzeAggregateElements(
    const vector<AstId>& clauses, size_t& index, ScopeId scope,
    TypeId target, SemaId result)
{
  const TypeId unqualified = types_.Unqualified(target);
  if (types_.Kind(unqualified) == TYPE_CLASS) {
    const ClassEntity& owner = model_.ClassAt(types_.At(unqualified).entity);
    for (size_t field_index = 0; field_index < owner.fields.size();
         ++field_index) {
      const ClassField& field = owner.fields[field_index];
      if (field.static_member || field.binding == 0)
        continue;
      if (index >= clauses.size())
        return;
      const AstId clause = clauses[index];
      const SemaId child = AnalyzeAggregateClause(
          clauses, clause, scope, field.type, index);
      if (child == 0)
        throw std::runtime_error("invalid aggregate initializer element");
      Append(result, child);
    }
    return;
  }
  if (types_.Kind(unqualified) == TYPE_ARRAY) {
    const TypeId element = types_.At(unqualified).base;
    const size_t bound = types_.At(unqualified).array_bound;
    for (size_t element_index = 0; element_index < bound; ++element_index) {
      if (index >= clauses.size())
        return;
      const AstId clause = clauses[index];
      const SemaId child = AnalyzeAggregateClause(
          clauses, clause, scope, element, index);
      Append(result, child);
    }
  }
}

SemaId ExpressionAnalyzer::AnalyzeElidedAggregate(
    const vector<AstId>& clauses, size_t& index, ScopeId scope,
    TypeId target)
{
  const SemaId result = MakeExpression(SEMA_BRACED_INIT_LIST, 0, target,
                                       VC_PRVALUE, scope);
  AnalyzeAggregateElements(clauses, index, scope, target, result);
  return result;
}

// 4.10: an integer literal with value zero (the token's zero flag), never
// an enumerator or a folded expression.
bool ExpressionAnalyzer::IsZeroLiteral(SemaId semantic) const
{
  if (semantic == 0 || tree_.At(semantic).kind != SEMA_LITERAL ||
      tree_.At(semantic).binding != 0 || !tree_.At(semantic).HasSpan())
    return false;
  const Pa6Token& token = tokens_[tree_.At(semantic).first];
  return token.IsLiteral() && (token.flags & PA6_ZERO_FLAG) != 0;
}

bool ExpressionAnalyzer::IsNullPointerConstant(SemaId semantic) const
{
  return NodeInfo(semantic).is_null_literal || IsZeroLiteral(semantic);
}

// The implicit object argument reaches the member's class as part of the
// member access itself: 11.2p5 checks the member in the naming class, so a
// member re-exposed by a using-declaration is callable through a private
// base.  Only the conversion's viability is checked here.
SemaId ExpressionAnalyzer::BindImplicitObject(SemaId object, TypeId parameter)
{
  const Info source = NodeInfo(object);
  if (!Classify(model_, types_, source.type, source.category, false,
                source.is_function_lvalue, parameter).Viable())
    throw std::runtime_error("implicit object argument does not convert");
  return object;
}

SemaId ExpressionAnalyzer::Initialize(SemaId expression, TypeId target,
                                       bool constexpr_object,
                                       bool list_initialization)
{
  if (expression == 0 || target == 0)
    throw std::runtime_error("invalid initializer");

  // A function name is initially analyzed against the ordinary lookup set,
  // which is enough to give it a semantic node but not enough to choose an
  // overload.  Re-resolve it here once the destination function pointer or
  // reference type is known (13.4).
  if (tree_.At(expression).kind == SEMA_ID_EXPRESSION &&
      tree_.At(expression).function != 0)
    (void)RetargetFunctionName(expression, target);

  const Info source = NodeInfo(expression);
  const ImplicitConversion conversion = Classify(
      model_, types_, source.type, source.category,
      IsNullPointerConstant(expression), source.is_function_lvalue, target);
  if (!conversion.Viable())
    throw std::runtime_error("initializer conversion is not viable");
  CheckBaseConversionAccess(source.type, target, tree_.At(expression).scope);
  if (list_initialization &&
      IsNarrowingListInitialization(expression, target))
    throw std::runtime_error("narrowing conversion in list-initialization");
  if (types_.Kind(target) == TYPE_REFERENCE)
  {
    // 8.5.3p5: a temporary created through a promotion or conversion is
    // materialized as a cast to the referent type; identity and
    // qualification bindings add nothing.
    if (conversion.reference == REFERENCE_TEMPORARY &&
        conversion.rank != RANK_EXACT &&
        conversion.kind != CONV_DERIVED_TO_BASE)
    {
      const SemaId converted = tree_.Make(SEMA_CAST);
      const SemaNode& original = tree_.At(expression);
      SemaNode& wrapper = tree_.At(converted);
      wrapper.type = types_.Referent(target);
      wrapper.category = VC_PRVALUE;
      wrapper.scope = original.scope;
      wrapper.first = original.first;
      wrapper.last = original.last;
      tree_.Append(converted, expression);
      return converted;
    }
    return expression;
  }
  SemaNode& source_node = tree_.At(expression);
  if (IsZeroLiteral(expression))
  {
    if (types_.IsPointer(target))
      source_node.type = types_.Unqualified(target);
    else if (types_.IsNullPointerType(target))
      source_node.type = target;
  }
  else if (constexpr_object && source_node.kind == SEMA_LITERAL &&
           source_node.HasSpan() && tokens_[source_node.first].lit_scalar &&
           types_.IsIntegral(target))
    source_node.type = target;
  return expression;
}

bool ExpressionAnalyzer::TryConstant(SemaId expression, long long& value) const
{
  if (expression == 0 || !tree_.At(expression).has_value)
    return false;
  value = tree_.At(expression).value;
  return true;
}

long long ExpressionAnalyzer::Value(SemaId expression) const
{
  long long value = 0;
  if (!TryConstant(expression, value))
    throw std::runtime_error("constant expression required");
  return value;
}

void ExpressionAnalyzer::FoldUnary(SemaId node, ETokenType op, SemaId operand)
{
  if (operand == 0 || !tree_.At(operand).has_value)
    return;
  const long long value = tree_.At(operand).value;
  try
  {
    long long result = value;
    switch (op)
    {
    case OP_PLUS: break;
    case OP_MINUS: result = Checked(-static_cast<__int128>(value)); break;
    case OP_LNOT: result = value == 0 ? 1 : 0; break;
    case OP_COMPL: result = ~value; break;
    default: return;
    }
    tree_.At(node).has_value = true;
    tree_.At(node).value = result;
  }
  catch (const std::runtime_error&)
  {
    }
  }
// 5.19p2: an operation with undefined behaviour (overflow, division by
// zero, a shift out of range or of a negative value) is not a constant
// expression; the node simply has no value.
void ExpressionAnalyzer::FoldBinary(SemaId node, ETokenType op, SemaId left,
                                    SemaId right)
{
  const bool left_value = tree_.At(left).has_value;
  if ((op == OP_LAND && left_value && tree_.At(left).value == 0) ||
      (op == OP_LOR && left_value && tree_.At(left).value != 0))
  {
    tree_.At(node).has_value = true;
    tree_.At(node).value = op == OP_LAND ? 0 : 1;
    return;
  }
  if (!left_value || !tree_.At(right).has_value)
    return;
  const long long lhs = tree_.At(left).value;
  const long long rhs = tree_.At(right).value;
  try
  {
    long long result = 0;
    switch (op)
    {
    case OP_PLUS: result = Checked(static_cast<__int128>(lhs) + rhs); break;
    case OP_MINUS: result = Checked(static_cast<__int128>(lhs) - rhs); break;
    case OP_STAR: result = Checked(static_cast<__int128>(lhs) * rhs); break;
    case OP_DIV: if (rhs == 0) return;
      result = Checked(static_cast<__int128>(lhs) / rhs); break;
    case OP_MOD: if (rhs == 0) return;
      result = Checked(static_cast<__int128>(lhs) % rhs); break;
    case OP_LSHIFT: if (rhs < 0 || rhs >= 64 || lhs < 0) return;
      result = Checked(static_cast<__int128>(lhs) << rhs); break;
    case OP_RSHIFT: if (rhs < 0 || rhs >= 64) return;
      result = static_cast<long long>(static_cast<__int128>(lhs) >> rhs); break;
    case OP_AMP: result = lhs & rhs; break;
    case OP_BOR: result = lhs | rhs; break;
    case OP_XOR: result = lhs ^ rhs; break;
    case OP_EQ: result = lhs == rhs; break;
    case OP_NE: result = lhs != rhs; break;
    case OP_LT: result = lhs < rhs; break;
    case OP_GT: result = lhs > rhs; break;
    case OP_LE: result = lhs <= rhs; break;
    case OP_GE: result = lhs >= rhs; break;
    case OP_LAND: result = rhs != 0; break;
    case OP_LOR: result = rhs != 0; break;
    case OP_COMMA: result = rhs; break;
    default: return;
    }
    tree_.At(node).has_value = true;
    tree_.At(node).value = result;
  }
  catch (const std::runtime_error&)
  {
  }
}

void ExpressionAnalyzer::FoldConditional(SemaId node, SemaId condition,
                                          SemaId left, SemaId right)
{
  if (!tree_.At(condition).has_value)
    return;
  const SemaId selected = tree_.At(condition).value != 0 ? left : right;
  if (tree_.At(selected).has_value)
  {
    tree_.At(node).has_value = true;
    tree_.At(node).value = tree_.At(selected).value;
  }
}

// A cast to an integral or enumeration type keeps a folded operand constant
// (5.19p2); casts to other types have no value.
void ExpressionAnalyzer::FoldConversion(SemaId node, SemaId operand,
                                        TypeId target)
{
  long long converted = 0;
  if (operand == 0 || !tree_.At(operand).has_value ||
      !ConvertIntegral(types_, tree_.At(operand).value, target, converted))
    return;
  tree_.At(node).has_value = true;
  tree_.At(node).value = converted;
}

long long ExpressionAnalyzer::Checked(__int128 value)
{
  const __int128 minimum = static_cast<__int128>(
      std::numeric_limits<long long>::min());
  const __int128 maximum = static_cast<__int128>(
      std::numeric_limits<long long>::max());
  if (value < minimum || value > maximum)
    throw std::runtime_error("constant expression overflows");
  return static_cast<long long>(value);
}
