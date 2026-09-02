#include "sema/const_eval.h"

#include <limits>
#include <stdexcept>

#include "sema/qualified_name.h"

ConstEvaluator::ConstEvaluator(const std::vector<Pa6Token>& tokens,
                               const AstArena& arena, const SemaModel& model,
                               ConstantOperandTypes& operand_types)
    : tokens_(tokens), arena_(arena), model_(model),
      operand_types_(operand_types)
{
}

long long ConstEvaluator::Evaluate(AstId expression, ScopeId scope) const
{
  if (expression == 0)
    throw std::runtime_error("missing constant expression");
  return EvaluateNode(expression, scope);
}

long long ConstEvaluator::Checked(__int128 value)
{
  const __int128 minimum =
      static_cast<__int128>(std::numeric_limits<long long>::min());
  const __int128 maximum =
      static_cast<__int128>(std::numeric_limits<long long>::max());
  if (value < minimum || value > maximum)
    throw std::runtime_error("constant expression overflows signed integer");
  return static_cast<long long>(value);
}

long long ConstEvaluator::EvaluateNode(AstId expression, ScopeId scope) const
{
  const AstNode& node = arena_.At(expression);
  switch (node.kind)
  {
  case AST_PARENTHESIZED_EXPRESSION:
    if (node.children.size() != 1)
      throw std::runtime_error("invalid parenthesized constant expression");
    return EvaluateNode(node.children[0], scope);

  case AST_LITERAL:
  {
    if (node.first >= tokens_.size())
      throw std::runtime_error("invalid literal");
    const Pa6Token& token = tokens_[node.first];
    if (!token.lit_scalar || !FundamentalIsIntegral(token.lit_type) ||
        token.lit_value > static_cast<unsigned long long>(
            std::numeric_limits<long long>::max()))
      throw std::runtime_error("constant expression is not a signed integer");
    return static_cast<long long>(token.lit_value);
  }

  case AST_KEYWORD_LITERAL:
    if (node.first < tokens_.size() && tokens_[node.first].IsSimple(KW_TRUE))
      return 1;
    if (node.first < tokens_.size() && tokens_[node.first].IsSimple(KW_FALSE))
      return 0;
    throw std::runtime_error("keyword is not an integer constant");

  case AST_ID_EXPRESSION:
  case AST_IDENTIFIER:
    return EvaluateName(node, scope);
  case AST_UNARY_EXPRESSION:
    return EvaluateUnary(node, scope);
  case AST_BINARY_EXPRESSION:
    return EvaluateBinary(node, scope);
  case AST_CONDITIONAL_EXPRESSION:
    return EvaluateConditional(node, scope);
  case AST_CAST_EXPRESSION:
    return EvaluateCast(node, scope);
  case AST_SIZEOF_EXPRESSION:
  case AST_TYPE_TRAIT_EXPRESSION:
    return EvaluateSizeOf(node, scope);
  default:
    throw std::runtime_error("unsupported constant expression");
  }
}

// Enumerators and const integral objects carry their value on the binding.
long long ConstEvaluator::EvaluateName(const AstNode& node, ScopeId scope) const
{
  const QualifiedName name = ReadQualifiedName(tokens_, node.first, node.last);
  const BindingId binding = model_.Lookup(scope, name, LOOKUP_VALUES);
  if (binding == 0 || !model_.BindingAt(binding).has_const_value)
    throw std::runtime_error("name is not a constant expression");
  return model_.BindingAt(binding).const_value;
}

long long ConstEvaluator::EvaluateUnary(const AstNode& node, ScopeId scope) const
{
  if (node.children.size() != 1 || node.first >= tokens_.size())
    throw std::runtime_error("invalid unary constant expression");
  const long long value = EvaluateNode(node.children[0], scope);
  switch (tokens_[node.first].simple_type)
  {
  case OP_PLUS: return value;
  case OP_MINUS: return Checked(-static_cast<__int128>(value));
  case OP_LNOT: return value == 0 ? 1 : 0;
  case OP_COMPL: return ~value;
  default: throw std::runtime_error("unsupported unary constant expression");
  }
}

long long ConstEvaluator::EvaluateBinary(const AstNode& node, ScopeId scope) const
{
  if (node.children.size() != 2 || node.first >= tokens_.size())
    throw std::runtime_error("invalid binary constant expression");
  const ETokenType op = tokens_[node.first].IsRshiftPart() ? OP_RSHIFT :
      tokens_[node.first].simple_type;
  const long long left = EvaluateNode(node.children[0], scope);
  // 5.14/5.15: the unselected operand is not evaluated.
  if (op == OP_LAND && left == 0)
    return 0;
  if (op == OP_LOR && left != 0)
    return 1;
  const long long right = EvaluateNode(node.children[1], scope);
  switch (op)
  {
  case OP_PLUS: return Checked(static_cast<__int128>(left) + right);
  case OP_MINUS: return Checked(static_cast<__int128>(left) - right);
  case OP_STAR: return Checked(static_cast<__int128>(left) * right);
  case OP_DIV:
    if (right == 0)
      throw std::runtime_error("constant division by zero");
    return Checked(static_cast<__int128>(left) / right);
  case OP_MOD:
    if (right == 0)
      throw std::runtime_error("constant remainder by zero");
    return Checked(static_cast<__int128>(left) % right);
  case OP_LSHIFT:
    if (right < 0 || right >= 64 || left < 0)
      throw std::runtime_error("invalid constant shift");
    return Checked(static_cast<__int128>(left) << right);
  case OP_RSHIFT:
    if (right < 0 || right >= 64)
      throw std::runtime_error("invalid constant shift");
    return static_cast<long long>(static_cast<__int128>(left) >> right);
  case OP_AMP: return left & right;
  case OP_BOR: return left | right;
  case OP_XOR: return left ^ right;
  case OP_EQ: return left == right ? 1 : 0;
  case OP_NE: return left != right ? 1 : 0;
  case OP_LT: return left < right ? 1 : 0;
  case OP_GT: return left > right ? 1 : 0;
  case OP_LE: return left <= right ? 1 : 0;
  case OP_GE: return left >= right ? 1 : 0;
  case OP_LAND: return right != 0 ? 1 : 0;
  case OP_LOR: return right != 0 ? 1 : 0;
  case OP_COMMA: return right;
  default: throw std::runtime_error("unsupported binary constant expression");
  }
}

long long ConstEvaluator::EvaluateConditional(const AstNode& node,
                                              ScopeId scope) const
{
  if (node.children.size() != 3)
    throw std::runtime_error("invalid conditional constant expression");
  return EvaluateNode(node.children[0], scope) != 0 ?
      EvaluateNode(node.children[1], scope) :
      EvaluateNode(node.children[2], scope);
}

// Integral conversion (4.7) to a fundamental integral type or to the
// underlying type of an enumeration.
long long ConstEvaluator::Convert(long long value, TypeId type) const
{
  const TypeTable& types = model_.Types();
  TypeId target = types.Unqualified(type);
  if (types.Kind(target) == TYPE_ENUM)
    target = types.Unqualified(types.At(target).base);
  if (types.Kind(target) != TYPE_FUNDAMENTAL ||
      !FundamentalIsIntegral(types.At(target).fundamental))
    throw std::runtime_error("cast target is not an integer type");
  const EFundamentalType fundamental = types.At(target).fundamental;
  if (fundamental == FT_BOOL)
    return value == 0 ? 0 : 1;
  const unsigned bits = 8 * FundamentalSize(fundamental);
  if (bits >= 64)
    return value;
  const unsigned long long modulus = 1ULL << bits;
  const unsigned long long raw =
      static_cast<unsigned long long>(value) & (modulus - 1);
  if (FundamentalIsUnsigned(fundamental))
    return static_cast<long long>(raw);
  const unsigned long long sign = 1ULL << (bits - 1);
  return raw >= sign ? static_cast<long long>(raw - modulus) :
      static_cast<long long>(raw);
}

long long ConstEvaluator::EvaluateCast(const AstNode& node, ScopeId scope) const
{
  if (node.children.size() != 2)
    throw std::runtime_error("invalid cast constant expression");
  return Convert(EvaluateNode(node.children[1], scope),
                 operand_types_.TypeOfTypeId(node.children[0], scope));
}

long long ConstEvaluator::EvaluateSizeOf(const AstNode& node,
                                         ScopeId scope) const
{
  if (node.children.size() != 1 || node.first >= tokens_.size())
    throw std::runtime_error("invalid sizeof expression");
  const AstId operand = node.children[0];
  const TypeId type = arena_.At(operand).kind == AST_TYPE_ID ?
      operand_types_.TypeOfTypeId(operand, scope) :
      operand_types_.TypeOfExpression(operand, scope);
  const Pa6Token& keyword = tokens_[node.first];
  if (node.kind == AST_SIZEOF_EXPRESSION || keyword.IsSimple(KW_SIZEOF))
    return Checked(static_cast<__int128>(model_.Types().SizeOf(type)));
  if (keyword.IsSimple(KW_ALIGNOF))
    return Checked(static_cast<__int128>(model_.Types().AlignOf(type)));
  throw std::runtime_error("unsupported type trait constant expression");
}
