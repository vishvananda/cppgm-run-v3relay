#include "sema/const_eval.h"

#include <limits>
#include <stdexcept>

ConstEvaluator::ConstEvaluator(const std::vector<Pa6Token>& tokens,
                               const AstArena& arena)
    : tokens_(tokens), arena_(arena)
{
}
long long ConstEvaluator::Evaluate(AstId expression) const
{
  if (expression == 0)
    throw std::runtime_error("missing constant expression");
  const AstNode& node = arena_.At(expression);
  if (node.kind == AST_PARENTHESIZED_EXPRESSION)
  {
    if (node.children.size() != 1)
      throw std::runtime_error("unsupported constant expression");
    return Evaluate(node.children[0]);
  }
  if (node.kind != AST_LITERAL || node.first >= tokens_.size())
    throw std::runtime_error("unsupported constant expression");
  const Pa6Token& token = tokens_[node.first];
  if (!token.lit_scalar || token.lit_value >
      static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
    throw std::runtime_error("constant expression is not an integer literal");
  return static_cast<long long>(token.lit_value);
}
