#pragma once

#include <vector>

#include "parser/ast_model.h"
#include "parser/recog_token.h"
#include "sema/scope_model.h"

// Types of constant-expression operands are owned by the type builder; the
// evaluator asks for them through this interface so sizeof, alignof and cast
// operands use the same type construction as declarations.
class ConstantOperandTypes
{
public:
  virtual ~ConstantOperandTypes() {}
  // Type denoted by a type-id node.
  virtual TypeId TypeOfTypeId(AstId type_id, ScopeId scope) = 0;
  // Static type of a supported expression operand, including an
  // id-expression that lookup resolves to a type name (sizeof(T)).
  virtual TypeId TypeOfExpression(AstId expression, ScopeId scope) = 0;
};

// Integral constant evaluation over AST expression nodes: literals, names
// with recorded constant values, the arithmetic, shift, relational, bitwise
// and short-circuit logical operators, conditionals, integral casts, sizeof
// and alignof.  Values are signed 64-bit with checked overflow.
class ConstEvaluator
{
public:
  ConstEvaluator(const std::vector<Pa6Token>& tokens, const AstArena& arena,
                 const SemaModel& model, ConstantOperandTypes& operand_types);

  long long Evaluate(AstId expression, ScopeId scope) const;

private:
  long long EvaluateNode(AstId expression, ScopeId scope) const;
  long long EvaluateName(const AstNode& node, ScopeId scope) const;
  long long EvaluateBinary(const AstNode& node, ScopeId scope) const;
  long long EvaluateUnary(const AstNode& node, ScopeId scope) const;
  long long EvaluateConditional(const AstNode& node, ScopeId scope) const;
  long long EvaluateCast(const AstNode& node, ScopeId scope) const;
  long long EvaluateSizeOf(const AstNode& node, ScopeId scope) const;
  long long Convert(long long value, TypeId type) const;
  static long long Checked(__int128 value);

  const std::vector<Pa6Token>& tokens_;
  const AstArena& arena_;
  const SemaModel& model_;
  ConstantOperandTypes& operand_types_;
};
