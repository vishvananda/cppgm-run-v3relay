#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "parser/ast_model.h"
#include "parser/recog_token.h"
#include "sema/conversions.h"
#include "sema/sema_tree.h"

class ScopeBuilder;

struct InitContext
{
  bool variable;
  bool constexpr_value;
  bool condition;
  bool return_value;
  bool argument;

  InitContext(bool variable = false, bool constexpr_value = false,
              bool condition = false, bool return_value = false,
              bool argument = false)
      : variable(variable), constexpr_value(constexpr_value),
        condition(condition), return_value(return_value),
        argument(argument) {}
};

// Typed expression ownership for PA12.  It appends one semantic node per
// expression and performs conversions only at the initialization boundary;
// implicit conversions inside an expression affect its type but never add a
// dump node.
class ExpressionAnalyzer
{
public:
  ExpressionAnalyzer(const std::vector<Pa6Token>& tokens,
                     const AstArena& arena, SemaModel& model,
                     SemaTree& tree, ScopeBuilder& builder);

  SemaId Analyze(AstId expression, ScopeId scope);
  SemaId AnalyzeInitializer(AstId initializer, ScopeId scope, TypeId target);
  SemaId Initialize(SemaId expression, TypeId target,
                    const InitContext& context = InitContext());
  bool TryConstant(SemaId expression, long long& value) const;
  long long Value(SemaId expression) const;

private:
  struct Info
  {
    TypeId type;
    ValueCategory category;
    bool is_null_literal;
    bool is_function_lvalue;

    Info(TypeId type = 0, ValueCategory category = VC_PRVALUE,
         bool is_null_literal = false, bool is_function_lvalue = false)
        : type(type), category(category), is_null_literal(is_null_literal),
          is_function_lvalue(is_function_lvalue) {}
  };

  SemaId AnalyzeNode(AstId expression, ScopeId scope);
  SemaId AnalyzeLiteral(AstId expression, ScopeId scope);
  SemaId AnalyzeName(AstId expression, ScopeId scope);
  SemaId AnalyzeUnary(AstId expression, ScopeId scope);
  SemaId AnalyzePostfix(AstId expression, ScopeId scope);
  SemaId AnalyzeBinary(AstId expression, ScopeId scope);
  SemaId AnalyzeAssignment(AstId expression, ScopeId scope);
  SemaId AnalyzeConditional(AstId expression, ScopeId scope);
  SemaId AnalyzeSubscript(AstId expression, ScopeId scope);
  SemaId AnalyzeCast(AstId expression, ScopeId scope);
  SemaId AnalyzeSizeof(AstId expression, ScopeId scope);
  SemaId AnalyzeCall(AstId expression, ScopeId scope);
  SemaId AnalyzeBraced(AstId expression, ScopeId scope, TypeId target);

  Info NodeInfo(SemaId node) const;
  SemaId MakeExpression(SemaKind kind, AstId source, TypeId type,
                        ValueCategory category, ScopeId scope,
                        ETokenType op = KW_AUTO);
  void Append(SemaId parent, SemaId child);
  AstId Child(AstId node, std::size_t index) const;
  AstId FindChild(AstId node, AstKind kind) const;
  ETokenType Operator(AstId node) const;
  bool IsZeroLiteral(AstId expression, SemaId semantic) const;
  bool IsNullptrLiteral(AstId expression, SemaId semantic) const;
  bool IsTypeName(AstId expression, ScopeId scope, TypeId& type) const;
  bool IsFundamentalCastCallee(AstId callee) const;
  bool IsFunctionType(TypeId type) const;
  bool IsModifiableLvalue(SemaId node) const;
  TypeId ExpressionType(SemaId node) const;
  TypeId CommonConditionalType(SemaId left, SemaId right) const;
  bool CanConvert(SemaId expression, TypeId target) const;
  void FoldLiteral(SemaId node, AstId expression);
  void FoldUnary(SemaId node, ETokenType op, SemaId operand);
  void FoldBinary(SemaId node, ETokenType op, SemaId left, SemaId right);
  void FoldConditional(SemaId node, SemaId condition, SemaId left,
                       SemaId right);
  static long long Checked(__int128 value);

  const std::vector<Pa6Token>& tokens_;
  const AstArena& arena_;
  SemaModel& model_;
  TypeTable& types_;
  SemaTree& tree_;
  ScopeBuilder& builder_;
};
