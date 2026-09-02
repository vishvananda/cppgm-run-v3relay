#include "sema/expr_sema.h"

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
  model_.LookupSet(scope, name.Joined(), LOOKUP_ANY, candidates);
  if (name.Qualified())
    model_.LookupQualifiedSet(scope, name, LOOKUP_ANY, candidates);
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
  return result;
}

bool ExpressionAnalyzer::IsModifiableLvalue(SemaId node) const
{
  if (node == 0 || tree_.At(node).category != VC_LVALUE)
    return false;
  const TypeId type = tree_.At(node).type;
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
  TypeId result_type = 0;
  ValueCategory category = VC_PRVALUE;
  switch (op)
  {
  case OP_AMP:
    if (info.category != VC_LVALUE && !info.is_function_lvalue)
      throw std::runtime_error("address-of requires an lvalue");
    result_type = types_.Pointer(info.type);
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
    if (!IsModifiableLvalue(operand) || !types_.IsScalar(info.type))
      throw std::runtime_error("increment requires a modifiable scalar lvalue");
    result_type = info.type;
    category = VC_LVALUE;
    break;
  case OP_LNOT:
    if (!types_.IsScalar(info.type) || IsScopedEnum(types_, info.type))
      throw std::runtime_error("logical not requires a scalar operand");
    result_type = types_.Fundamental(FT_BOOL);
    break;
  case OP_PLUS: case OP_MINUS: case OP_COMPL:
    if (!types_.IsArithmetic(info.type) ||
        (op == OP_COMPL && !types_.IsIntegral(info.type)))
      throw std::runtime_error("unary arithmetic operator has invalid operand");
    result_type = types_.IsIntegral(info.type) ? types_.Promote(info.type) :
        types_.Unqualified(info.type);
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
      !IsModifiableLvalue(operand) || !types_.IsScalar(info.type))
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
  if (!IsModifiableLvalue(left))
    throw std::runtime_error("assignment requires a modifiable lvalue");
  if (op == OP_ASS)
  {
    if (!CanConvert(right, lhs.type))
      throw std::runtime_error("assignment conversion is not viable");
  }
  else
  {
    const bool pointer_add = (op == OP_PLUSASS || op == OP_MINUSASS) &&
        types_.IsPointer(lhs.type) && types_.IsIntegral(rhs.type);
    const bool arithmetic = types_.IsArithmetic(lhs.type) &&
        types_.IsArithmetic(rhs.type);
    if (!pointer_add && !arithmetic)
      throw std::runtime_error("compound assignment operands are invalid");
    if (op == OP_STARASS || op == OP_DIVASS || op == OP_MODASS ||
        op == OP_LSHIFTASS || op == OP_RSHIFTASS || op == OP_BANDASS ||
        op == OP_XORASS || op == OP_BORASS)
    {
      if (!arithmetic || (op == OP_MODASS &&
                          (!types_.IsIntegral(lhs.type) ||
                           !types_.IsIntegral(rhs.type))))
        throw std::runtime_error("compound assignment operands are invalid");
    }
  }
  const SemaId result = MakeExpression(SEMA_ASSIGNMENT, expression, lhs.type,
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
  const bool first_pointer = types_.IsPointer(NodeInfo(first).type) ||
      types_.Kind(NodeInfo(first).type) == TYPE_ARRAY;
  const bool second_pointer = types_.IsPointer(NodeInfo(second).type) ||
      types_.Kind(NodeInfo(second).type) == TYPE_ARRAY;
  if (!first_pointer && second_pointer)
    std::swap(first, second);
  const Info base = NodeInfo(first);
  if (types_.Kind(base.type) != TYPE_ARRAY && !types_.IsPointer(base.type))
    throw std::runtime_error("subscript base is not an array or pointer");
  if (!types_.IsIntegral(NodeInfo(second).type))
    throw std::runtime_error("subscript index is not integral");
  TypeId element = types_.Kind(base.type) == TYPE_ARRAY ?
      types_.At(base.type).base : types_.At(types_.Unqualified(base.type)).base;
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
  if (types_.Kind(target) == TYPE_REFERENCE)
  {
    const TypeId referent = types_.Referent(target);
    const Info source = NodeInfo(operand);
    if (types_.At(target).lvalue_reference && source.category != VC_LVALUE)
      throw std::runtime_error("lvalue reference cast requires an lvalue");
    tree_.At(operand).type = referent;
    tree_.At(operand).category = types_.At(target).lvalue_reference ?
        VC_LVALUE : VC_XVALUE;
    return operand;
  }
  if (types_.Kind(types_.Unqualified(target)) != TYPE_FUNDAMENTAL &&
      types_.Kind(types_.Unqualified(target)) != TYPE_ENUM &&
      !types_.IsPointer(target))
    throw std::runtime_error("unsupported cast target");
  if (!types_.IsScalar(target) &&
      !(types_.Kind(types_.Unqualified(target)) == TYPE_FUNDAMENTAL &&
        types_.At(types_.Unqualified(target)).fundamental == FT_VOID))
    throw std::runtime_error("unsupported cast target");
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
      throw std::runtime_error("invalid functional cast type");
    target = types_.FundamentalFromKeywords(keywords);
  }
  else if (!IsTypeName(callee, scope, target))
    throw std::runtime_error("call expressions: CP2");

  const vector<AstId>& args = arena_.At(arguments).children;
  if (args.size() > 1)
    throw std::runtime_error("functional cast has too many arguments");
  if (args.empty())
  {
    const SemaId result = MakeExpression(SEMA_LITERAL, expression, target,
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
    const TypeId referent = types_.Referent(target);
    if (types_.At(target).lvalue_reference &&
        source.category != VC_LVALUE &&
        !(types_.Kind(referent) == TYPE_CV && types_.At(referent).is_const))
      throw std::runtime_error("non-const lvalue reference binds a temporary");
    if (!types_.At(target).lvalue_reference && source.category == VC_LVALUE &&
        !source.is_function_lvalue)
      throw std::runtime_error("rvalue reference binds an lvalue");
    source_node.type = referent;
    source_node.category = types_.At(target).lvalue_reference ? VC_LVALUE :
        VC_XVALUE;
    return expression;
  }
  if (IsZeroLiteral(0, expression))
    source_node.type = types_.IsPointer(target) ? types_.Unqualified(target) :
        target;
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
