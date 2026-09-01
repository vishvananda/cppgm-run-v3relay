#pragma once

#include <cstdint>

#include "parser/ast_model.h"
#include "parser/recog_token.h"

class ConstEvaluator
{
public:
  ConstEvaluator(const std::vector<Pa6Token>& tokens, const AstArena& arena);

  long long Evaluate(AstId expression) const;

private:
  const std::vector<Pa6Token>& tokens_;
  const AstArena& arena_;
};
