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
      (types.At(type).is_const || types.At(type).is_volatile);
}

bool IsScopedEnum(const TypeTable& types, TypeId type)
{
  type = types.Unqualified(type);
  return types.Kind(type) == TYPE_ENUM && types.At(type).scoped;
}

bool IsNullPointerConstantType(const TypeTable& types, TypeId type)
{
  return types.IsNullPointerType(type);
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

ExpressionAnalyzer::Info ExpressionAnalyzer::NodeInfo(SemaId node) const
{
  if (node == 0)
    return Info();
  const SemaNode& value = tree_.At(node);
  return Info(value.type, value.category,
              value.kind == SEMA_LITERAL && value.type != 0 &&
                  IsNullPointerConstantType(types_, value.type),
              value.kind == SEMA_ID_EXPRESSION &&
                  types_.Kind(types_.Unqualified(value.type)) == TYPE_FUNCTION);
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
    const SemaId expression = Analyze(node.children[0], scope);
    return Initialize(expression, target, InitContext(true));
  }
  if (node.kind == AST_BRACED_INIT_LIST)
    return AnalyzeBraced(initializer, scope, target);
  const SemaId expression = Analyze(initializer, scope);
  return Initialize(expression, target, InitContext(true));
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
  case AST_BRACED_INIT_LIST: return AnalyzeBraced(expression, scope, 0);
  default:
    throw std::runtime_error("unsupported expression in semantic analysis");
  }
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
  return result;
}

SemaId ExpressionAnalyzer::AnalyzeName(AstId expression, ScopeId scope)
{
  const AstNode& ast = arena_.At(expression);
  const QualifiedName name = ReadQualifiedName(tokens_, ast.first, ast.last);
  vector<BindingId> candidates;
  LookupNameBindings(name, ast.first, ast.last, scope, candidates);
  if (candidates.empty())
    throw std::runtime_error("unknown name in expression");

  BindingId binding = candidates.back();
  for (size_t i = candidates.size(); i != 0; --i)
    if (model_.BindingAt(candidates[i - 1]).kind != BINDING_FUNCTION)
    {
      binding = candidates[i - 1];
      break;
    }
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

  TypeId type = value.type;
  if (types_.Kind(type) == TYPE_REFERENCE)
    type = types_.Referent(type);
  const SemaId result = MakeExpression(SEMA_ID_EXPRESSION, expression, type,
                                       VC_LVALUE, scope);
  SemaNode& node = tree_.At(result);
  node.binding = binding;
  node.has_value = value.has_const_value;
  node.value = value.const_value;
  if (value.object_binding != 0)
  {
    node.kind = SEMA_MEMBER;
    node.expression_name = name.Last();
    const Binding& object = model_.BindingAt(value.object_binding);
    const SemaId object_node = tree_.Make(SEMA_ID_EXPRESSION);
    SemaNode& object_semantic = tree_.At(object_node);
    object_semantic.category = VC_LVALUE;
    object_semantic.type = object.type;
    object_semantic.binding = value.object_binding;
    object_semantic.scope = scope;
    object_semantic.expression_name = object.name;
    tree_.Append(result, object_node);
  }
  return result;
}

void ExpressionAnalyzer::LookupNameBindings(const QualifiedName& name,
                                            size_t first, size_t last,
                                            ScopeId scope,
                                            vector<BindingId>& bindings)
{
  QualifiedName lookup = name;
  const std::string spelling = name.Last();
  const std::string::size_type template_start = spelling.find('<');
  const bool template_id = template_start != std::string::npos;
  if (template_id)
    lookup.components.back() = spelling.substr(0, template_start);

  if (lookup.Qualified())
    model_.LookupQualifiedSet(scope, lookup, LOOKUP_ANY, bindings);
  else
    model_.LookupSet(scope, lookup.Last(), LOOKUP_ANY, bindings);
  if (!template_id)
    return;

  vector<TypeId> arguments;
  if (!TemplateArgumentTypes(first, last, scope, arguments))
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
}

bool ExpressionAnalyzer::TemplateArgumentTypes(
    size_t first, size_t last, ScopeId scope, vector<TypeId>& arguments) const
{
  arguments.clear();
  if (first >= last)
    return false;
  size_t open = last;
  for (size_t i = first; i < last && i < tokens_.size(); ++i)
    if (tokens_[i].IsSimple(OP_LT))
    {
      open = i;
      break;
    }
  if (open == last)
    return false;

  vector<std::pair<size_t, size_t> > spans;
  size_t start = open + 1;
  size_t depth = 0;
  size_t close = last;
  for (size_t i = start; i < last && i < tokens_.size(); ++i)
  {
    const Pa6Token& token = tokens_[i];
    if (token.IsSimple(OP_LT))
      ++depth;
    else if (token.IsSimple(OP_GT) || token.IsRshiftPart())
    {
      if (depth == 0)
      {
        close = i;
        if (i > start)
          spans.push_back(std::make_pair(start, i));
        break;
      }
      --depth;
    }
    else if (token.IsSimple(OP_COMMA) && depth == 0)
    {
      if (i == start)
        return false;
      spans.push_back(std::make_pair(start, i));
      start = i + 1;
    }
  }
  if (close == last || depth != 0)
    return false;

  for (size_t i = 0; i < spans.size(); ++i)
  {
    const size_t first = spans[i].first;
    const size_t last = spans[i].second;
    if (first >= last)
      return false;
    vector<ETokenType> fundamental;
    bool all_fundamental = true;
    for (size_t token = first; token < last; ++token)
    {
      if (tokens_[token].kind != PA6_SIMPLE_TOKEN ||
          !IsFundamentalTypeKeyword(tokens_[token].simple_type))
      {
        all_fundamental = false;
        break;
      }
      fundamental.push_back(tokens_[token].simple_type);
    }
    if (all_fundamental && !fundamental.empty())
      arguments.push_back(types_.FundamentalFromKeywords(fundamental));
    else
    {
      const QualifiedName type_name = ReadQualifiedName(tokens_, first, last);
      arguments.push_back(builder_.TypeForName(type_name, scope));
    }
  }
  return true;
}

SemaId ExpressionAnalyzer::AnalyzeMember(AstId expression, ScopeId scope)
{
  if (arena_.At(expression).children.size() != 2)
    throw std::runtime_error("invalid member expression");
  const SemaId object = Analyze(Child(expression, 0), scope);
  const TypeId raw_object_type = NodeInfo(object).type;
  TypeId object_type = raw_object_type;
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
  const BindingId binding = model_.DirectBinding(
      class_scope, name.Last(), LOOKUP_ANY);
  if (binding == 0)
    throw std::runtime_error("unknown class member");
  const Binding& member = model_.BindingAt(binding);
  TypeId type = member.type;
  if (object_const || object_volatile)
    type = types_.Cv(type, object_const, object_volatile);
  const SemaId result = MakeExpression(SEMA_MEMBER, expression, type,
                                       VC_LVALUE, scope, op);
  SemaNode& semantic = tree_.At(result);
  semantic.binding = binding;
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
  const TypeId value_type = types_.Kind(info.type) == TYPE_REFERENCE ?
      types_.Referent(info.type) : info.type;
  TypeId result_type = 0;
  ValueCategory category = VC_PRVALUE;
  switch (op)
  {
  case OP_AMP:
    if (info.category != VC_LVALUE && !info.is_function_lvalue)
      throw std::runtime_error("address-of requires an lvalue");
    if (info.is_function_lvalue && tree_.At(operand).function != 0 &&
        model_.FunctionAt(tree_.At(operand).function).is_member)
      result_type = model_.FunctionAt(tree_.At(operand).function).
          member_pointer_type;
    else
      result_type = types_.Pointer(types_.Kind(info.type) == TYPE_REFERENCE ?
          types_.Referent(info.type) : info.type);
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
    result_type = types_.Kind(info.type) == TYPE_REFERENCE ?
        types_.Referent(info.type) : info.type;
    category = VC_LVALUE;
    break;
  case OP_LNOT:
    if (!types_.IsScalar(value_type) || IsScopedEnum(types_, value_type))
      throw std::runtime_error("logical not requires a scalar operand");
    result_type = types_.Fundamental(FT_BOOL);
    break;
  case OP_PLUS: case OP_MINUS: case OP_COMPL:
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
  const SemaId operand = Analyze(Child(expression, 0), scope);
  const Info info = NodeInfo(operand);
  if ((Operator(expression) != OP_INC && Operator(expression) != OP_DEC) ||
      !IsModifiableLvalue(operand) ||
      !types_.IsScalar(types_.Kind(info.type) == TYPE_REFERENCE ?
          types_.Referent(info.type) : info.type))
    throw std::runtime_error("postfix operator has invalid operand");
  const SemaId result = MakeExpression(SEMA_POSTFIX, expression,
                                       types_.Decay(info.type), VC_PRVALUE,
                                       scope, Operator(expression));
  Append(result, operand);
  return result;
}

bool ExpressionAnalyzer::CanConvert(SemaId expression, TypeId target) const
{
  const Info source = NodeInfo(expression);
  const ImplicitConversion conversion = Classify(
      const_cast<TypeTable&>(types_), source.type, source.category,
      source.is_null_literal || IsZeroLiteral(0, expression),
      source.is_function_lvalue, target);
  return conversion.Viable();
}

bool ExpressionAnalyzer::RetargetFunctionName(SemaId expression,
                                               TypeId target)
{
  if (expression == 0 || tree_.At(expression).kind != SEMA_ID_EXPRESSION ||
      tree_.At(expression).function == 0)
    return false;
  const SemaNode& source = tree_.At(expression);
  const QualifiedName name = ReadQualifiedName(tokens_, source.first,
                                               source.last);
  vector<BindingId> bindings;
  LookupNameBindings(name, source.first, source.last, source.scope, bindings);
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
    else if ((types_.IsPointer(left_type) &&
              (rhs.is_null_literal || IsZeroLiteral(0, right))) ||
             (types_.IsPointer(right_type) &&
              (lhs.is_null_literal || IsZeroLiteral(0, left))))
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
  const SemaId result = MakeExpression(SEMA_BINARY, expression, result_type,
                                       VC_PRVALUE, scope, op);
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
  const SemaId right = Analyze(Child(expression, 1), scope);
  const Info lhs = NodeInfo(left);
  const Info rhs = NodeInfo(right);
  const TypeId lhs_value = types_.Kind(lhs.type) == TYPE_REFERENCE ?
      types_.Referent(lhs.type) : lhs.type;
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
  if (lhs.type == rhs.type)
    return lhs.type;
  if (types_.IsArithmetic(lhs.type) && types_.IsArithmetic(rhs.type))
    return const_cast<TypeTable&>(types_).UsualArithmetic(lhs.type, rhs.type);
  if (types_.IsPointer(lhs.type) && types_.IsPointer(rhs.type))
  {
    bool ok = false;
    const TypeId composite = const_cast<TypeTable&>(types_).CompositePointer(
        lhs.type, rhs.type, ok);
    if (ok)
      return composite;
  }
  if (types_.IsPointer(lhs.type) &&
      (rhs.is_null_literal || IsZeroLiteral(0, right)))
    return lhs.type;
  if (types_.IsPointer(rhs.type) &&
      (lhs.is_null_literal || IsZeroLiteral(0, left)))
    return rhs.type;
  if (CanConvert(left, rhs.type))
    return rhs.type;
  if (CanConvert(right, lhs.type))
    return lhs.type;
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
  const QualifiedName name = ReadQualifiedName(tokens_, node.first, node.last);
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

bool ExpressionAnalyzer::IsFunctionType(TypeId type) const
{
  return type != 0 && types_.Kind(types_.Unqualified(type)) == TYPE_FUNCTION;
}

SemaId ExpressionAnalyzer::AnalyzeCast(AstId expression, ScopeId scope)
{
  if (arena_.At(expression).children.size() != 2)
    throw std::runtime_error("invalid cast expression");
  const TypeId target = builder_.TypeIdForSemantics(Child(expression, 0), scope);
  const SemaId operand = Analyze(Child(expression, 1), scope);
  if (types_.Kind(types_.Unqualified(target)) == TYPE_POINTER ||
      types_.Kind(types_.Unqualified(target)) == TYPE_MEMBER_POINTER)
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
  if (types_.Kind(types_.Unqualified(target)) != TYPE_FUNDAMENTAL &&
      types_.Kind(types_.Unqualified(target)) != TYPE_ENUM &&
      !types_.IsPointer(target) &&
      types_.Kind(types_.Unqualified(target)) != TYPE_MEMBER_POINTER)
    throw std::runtime_error("unsupported cast target");
  if (!types_.IsScalar(target) &&
      !(types_.Kind(types_.Unqualified(target)) == TYPE_FUNDAMENTAL &&
        types_.At(types_.Unqualified(target)).fundamental == FT_VOID))
    throw std::runtime_error("unsupported cast target");
  if (types_.Kind(types_.Unqualified(target)) == TYPE_MEMBER_POINTER &&
      types_.Unqualified(tree_.At(operand).type) == types_.Unqualified(target))
    return operand;
  const SemaId result = MakeExpression(SEMA_CAST, expression, target,
                                       VC_PRVALUE, scope, Operator(expression));
  Append(result, operand);
  return result;
}

SemaId ExpressionAnalyzer::AnalyzeSizeof(AstId expression, ScopeId scope)
{
  if (arena_.At(expression).children.size() != 1)
    throw std::runtime_error("invalid sizeof expression");
  const AstId operand = Child(expression, 0);
  TypeId type = 0;
  if (arena_.At(operand).kind == AST_TYPE_ID)
    type = builder_.TypeIdForSemantics(operand, scope);
  else
  {
    TypeId type_name = 0;
    if (IsTypeName(operand, scope, type_name))
      type = type_name;
    else
    {
      const SemaId analyzed = Analyze(operand, scope);
      type = tree_.At(analyzed).type;
    }
  }
  if (Operator(expression) == KW_SIZEOF ||
      arena_.At(expression).kind == AST_SIZEOF_EXPRESSION)
    type = type;
  else if (Operator(expression) == KW_ALIGNOF)
    type = type;
  else
    throw std::runtime_error("unsupported type trait");
  const std::size_t size = Operator(expression) == KW_ALIGNOF ?
      types_.AlignOf(type) : types_.SizeOf(type);
  const SemaId result = MakeExpression(SEMA_SIZEOF, expression,
      types_.Fundamental(FT_UNSIGNED_LONG_INT), VC_PRVALUE, scope);
  tree_.At(result).has_value = true;
  tree_.At(result).value = static_cast<long long>(size);
  return result;
}

SemaId ExpressionAnalyzer::AnalyzeCall(AstId expression, ScopeId scope)
{
  if (arena_.At(expression).children.size() != 2)
    throw std::runtime_error("invalid call expression");
  const AstId callee = Child(expression, 0);
  const AstId arguments = Child(expression, 1);

  // A functional cast is represented by the same postfix-call AST shape as a
  // function call.  Resolve its type before ordinary name lookup, so a type
  // alias cannot be mistaken for an overload set.
  TypeId target = 0;
  bool functional_cast = false;
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
      // `decltype(e)(v)` is parsed as a type-specifier-shaped callee.  The
      // parser keeps the parsed operand as its only child when available.
      AstId decltype_node = FindChild(callee, AST_DECL_SPECIFIER);
      if (decltype_node == 0)
        decltype_node = FindChild(callee, AST_DECLTYPE_SPECIFIER);
      if (decltype_node != 0 && !arena_.At(decltype_node).children.empty())
        target = builder_.DecltypeForSemantics(
            arena_.At(decltype_node).children[0], scope);
      else
        throw std::runtime_error("invalid decltype functional cast");
    }
    else
      target = types_.FundamentalFromKeywords(keywords);
    functional_cast = true;
  }
  else if (IsTypeName(callee, scope, target))
    functional_cast = true;

  const vector<AstId>& args = arena_.At(arguments).children;
  if (functional_cast)
  {
    if (args.size() > 1)
      throw std::runtime_error("functional cast has too many arguments");
    if (args.empty())
    {
      const SemaId result = MakeExpression(SEMA_LITERAL, 0, target,
                                           VC_PRVALUE, scope);
      tree_.At(result).has_value = types_.IsIntegral(target) ||
          types_.IsNullPointerType(target);
      tree_.At(result).value = 0;
      return result;
    }
    const SemaId operand = Analyze(args[0], scope);
    const Info source = NodeInfo(operand);
    if (!Classify(types_, source.type, source.category,
                  source.is_null_literal || IsZeroLiteral(0, operand),
                  source.is_function_lvalue, target).Viable())
      throw std::runtime_error("functional cast is not viable");
    const SemaId result = MakeExpression(SEMA_CAST, expression, target,
                                         VC_PRVALUE, scope);
    Append(result, operand);
    return result;
  }

  const AstNode& callee_node = arena_.At(callee);
  const bool named_callee = callee_node.kind == AST_ID_EXPRESSION ||
      callee_node.kind == AST_IDENTIFIER;
  QualifiedName name;
  vector<BindingId> bindings;
  if (named_callee)
  {
    name = ReadQualifiedName(tokens_, callee_node.first, callee_node.last);
    LookupNameBindings(name, callee_node.first, callee_node.last, scope,
                       bindings);
  }

  // Analyze arguments before selecting a candidate so every source
  // expression is owned by the semantic tree in source order.  Conversion
  // nodes are added only after the selected parameter list is known.
  vector<SemaId> analyzed_arguments;
  vector<OverloadArgument> overload_arguments;
  analyzed_arguments.reserve(args.size());
  overload_arguments.reserve(args.size());
  for (size_t i = 0; i < args.size(); ++i)
  {
    const SemaId argument = Analyze(args[i], scope);
    const Info info = NodeInfo(argument);
    analyzed_arguments.push_back(argument);
    OverloadArgument overload_argument(
        info.type, info.category,
        info.is_null_literal || IsZeroLiteral(0, argument),
        info.is_function_lvalue);
    if (tree_.At(argument).kind == SEMA_ID_EXPRESSION &&
        tree_.At(argument).function != 0)
    {
      const SemaNode& semantic = tree_.At(argument);
      const QualifiedName argument_name = ReadQualifiedName(
          tokens_, semantic.first, semantic.last);
      vector<BindingId> argument_bindings;
      if (argument_name.Qualified())
        model_.LookupQualifiedSet(semantic.scope, argument_name,
                                  LOOKUP_ANY, argument_bindings);
      else
        model_.LookupSet(semantic.scope, argument_name.Last(), LOOKUP_ANY,
                         argument_bindings);
      for (size_t candidate = 0; candidate < argument_bindings.size();
           ++candidate)
      {
        const Binding& binding = model_.BindingAt(argument_bindings[candidate]);
        if (binding.kind != BINDING_FUNCTION || binding.function == 0)
          continue;
        if (std::find(overload_argument.function_candidates.begin(),
                      overload_argument.function_candidates.end(),
                      binding.function) ==
            overload_argument.function_candidates.end())
          overload_argument.function_candidates.push_back(binding.function);
      }
    }
    overload_arguments.push_back(overload_argument);
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
  if (named_callee && has_function_binding)
  {
    OverloadSelection selection;
    if (!SelectBestOverload(model_, types_, bindings, overload_arguments,
                            selection))
      throw std::runtime_error("no unique viable function overload");
    function = selection.function;
    function_type = model_.FunctionAt(function).type;
    builder_.MarkTemplateInstanceUsed(function);
  }
  else if (named_callee && bindings.empty() && name.components.size() == 1 &&
           !name.global && name.Last() == "__builtin_abort")
  {
    if (!args.empty())
      throw std::runtime_error("__builtin_abort takes no arguments");
    vector<TypeId> parameters;
    function_type = types_.Function(types_.Fundamental(FT_VOID), parameters);
    function = model_.CreateFunction(model_.GlobalScope(), name.Last(),
                                     function_type);
  }
  else if (named_callee && bindings.empty() && name.components.size() == 1 &&
           !name.global && name.Last() == "__builtin_constant_p")
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
  if ((!callable.variadic && callable.parameters.size() != args.size()) ||
      (callable.variadic && args.size() < callable.parameters.size()))
    throw std::runtime_error("wrong number of call arguments");

  vector<SemaId> converted_arguments;
  converted_arguments.reserve(analyzed_arguments.size());
  for (size_t i = 0; i < analyzed_arguments.size(); ++i)
  {
    SemaId argument = analyzed_arguments[i];
    if (i < callable.parameters.size())
      argument = Initialize(argument, callable.parameters[i],
                             InitContext(false, false, false, false, true));
    converted_arguments.push_back(argument);
  }

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

SemaId ExpressionAnalyzer::AnalyzeBraced(AstId expression, ScopeId scope,
                                         TypeId target)
{
  TypeId array_type = target;
  if (array_type == 0)
    throw std::runtime_error("braced initializer requires an array target");
  if (types_.Kind(array_type) != TYPE_ARRAY)
    throw std::runtime_error("braced initializer target is not an array");
  const TypeId element = types_.At(array_type).base;
  const vector<AstId>& children = arena_.At(expression).children;
  if (children.size() != types_.At(array_type).array_bound)
    throw std::runtime_error("initializer list has the wrong bound");
  const SemaId result = MakeExpression(SEMA_BRACED_INIT_LIST, expression,
                                       array_type, VC_LVALUE, scope);
  for (size_t i = 0; i < children.size(); ++i)
  {
    const SemaId child = Analyze(children[i], scope);
    Initialize(child, element, InitContext(true));
    Append(result, child);
  }
  return result;
}

bool ExpressionAnalyzer::IsZeroLiteral(AstId expression,
                                       SemaId semantic) const
{
  (void)expression;
  if (semantic == 0 || tree_.At(semantic).kind != SEMA_LITERAL ||
      tree_.At(semantic).first >= tokens_.size())
    return false;
  const Pa6Token& token = tokens_[tree_.At(semantic).first];
  return token.IsLiteral() && (token.flags & PA6_ZERO_FLAG) != 0 &&
      tree_.At(semantic).binding == 0;
}

bool ExpressionAnalyzer::IsNullptrLiteral(AstId expression,
                                          SemaId semantic) const
{
  (void)expression;
  return semantic != 0 && types_.IsNullPointerType(tree_.At(semantic).type);
}

SemaId ExpressionAnalyzer::Initialize(SemaId expression, TypeId target,
                                       const InitContext& context)
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

  SemaNode& source_node = tree_.At(expression);
  const Info source = NodeInfo(expression);
  const ImplicitConversion conversion = Classify(
      types_, source.type, source.category,
      source.is_null_literal || IsZeroLiteral(0, expression),
      source.is_function_lvalue, target);
  if (!conversion.Viable())
    throw std::runtime_error("initializer conversion is not viable");
  if (types_.Kind(target) == TYPE_REFERENCE)
  {
    if (conversion.reference == REFERENCE_TEMPORARY &&
        conversion.rank != RANK_EXACT)
    {
      const SemaNode original = source_node;
      const SemaId converted = tree_.Make(SEMA_CAST);
      SemaNode& wrapper = tree_.At(converted);
      wrapper.type = types_.Referent(target);
      wrapper.category = VC_PRVALUE;
      wrapper.op = KW_AUTO;
      wrapper.scope = original.scope;
      wrapper.first = original.first;
      wrapper.last = original.last;
      tree_.Append(converted, expression);
      return converted;
    }
    return expression;
  }
  if (IsZeroLiteral(0, expression))
  {
    if (types_.IsPointer(target))
      source_node.type = types_.Unqualified(target);
    else if (types_.IsNullPointerType(target))
      source_node.type = target;
  }
  else if (context.constexpr_value && source_node.kind == SEMA_LITERAL &&
           source_node.first < tokens_.size() &&
           tokens_[source_node.first].lit_scalar &&
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
    case OP_LSHIFT: if (rhs < 0 || rhs >= 64) return;
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
