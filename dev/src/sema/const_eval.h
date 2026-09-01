#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "parser/ast_model.h"
#include "parser/recog_token.h"
#include "sema/scope_model.h"

class ConstEvaluator
{
public:
  ConstEvaluator(const std::vector<Pa6Token>& tokens, const AstArena& arena);
  ConstEvaluator(const std::vector<Pa6Token>& tokens, const AstArena& arena,
                const SemaModel& model, TypeTable& types);

  long long Evaluate(AstId expression, ScopeId scope = 0) const;

private:
  long long EvaluateNode(AstId expression, ScopeId scope) const;
  long long EvaluateBinary(const AstNode& node, ScopeId scope) const;
  long long EvaluateUnary(const AstNode& node, ScopeId scope) const;
  long long EvaluateConditional(const AstNode& node, ScopeId scope) const;
  long long EvaluateCast(const AstNode& node, ScopeId scope) const;
  long long EvaluateSizeOf(const AstNode& node, ScopeId scope) const;
  TypeId ResolveType(AstId node, ScopeId scope) const;
  TypeId ResolveTypeSequence(AstId node, ScopeId scope) const;
  TypeId ResolveTypeSpecifier(AstId node, ScopeId scope) const;
  TypeId ResolveAbstractDeclarator(AstId node, TypeId base,
                                   ScopeId scope) const;
  TypeId ResolveExpressionType(AstId node, ScopeId scope) const;
  TypeId BuildFundamental(const std::vector<ETokenType>& tokens) const;
  std::vector<std::string> NameComponents(AstId node) const;
  std::vector<std::string> NameComponents(std::size_t first,
                                          std::size_t last) const;
  long long Convert(long long value, TypeId type) const;
  static long long Checked(__int128 value);

  const std::vector<Pa6Token>& tokens_;
  const AstArena& arena_;
  const SemaModel* model_;
  TypeTable* types_;
};
