#pragma once

#include <cstddef>
#include <vector>

#include "parser/ast_model.h"
#include "parser/recog_token.h"
#include "sema/conversions.h"
#include "sema/overload.h"
#include "sema/sema_tree.h"

class ScopeBuilder;

// Typed expression ownership: one semantic node per expression carrying its
// type, value category, resolved entity and folded integral value.  Implicit
// conversions inside an expression affect its type but add no node; the
// initialization boundary (Initialize) may retype a literal or wrap a
// converted temporary, as the dump requires.
class ExpressionAnalyzer
{
public:
  ExpressionAnalyzer(const std::vector<Pa6Token>& tokens,
                     const AstArena& arena, SemaModel& model,
                     SemaTree& tree, ScopeBuilder& builder);

  SemaId Analyze(AstId expression, ScopeId scope);
  SemaId AnalyzeInitializer(AstId initializer, ScopeId scope, TypeId target);
  // Copy-initialization of `expression` to `target` (8.5p14).  The literal
  // initializer of a constexpr object takes the object's cv-qualified type.
  SemaId Initialize(SemaId expression, TypeId target,
                    bool constexpr_object = false,
                    bool list_initialization = false);
  bool TryConstant(SemaId expression, long long& value) const;
  long long Value(SemaId expression) const;
  const SemaNode& Node(SemaId expression) const;
  // 7.1.6.2p4: the declared type of the entity an unparenthesized
  // id-expression names; the expression's type for anything else.
  TypeId DeclaredType(SemaId expression) const;

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

  struct CallResolution
  {
    QualifiedName name;
    std::vector<BindingId> bindings;
    SemaId implicit_object;
    SemaId pseudo_expression;
    bool member_callee;
    bool named_callee;
    bool has_implicit_object;
    bool pseudo_destructor;

    CallResolution()
        : implicit_object(0), pseudo_expression(0), member_callee(false),
          named_callee(false), has_implicit_object(false),
          pseudo_destructor(false) {}
  };

  SemaId AnalyzeNode(AstId expression, ScopeId scope);
  SemaId AnalyzeAmbiguousParameter(AstId parameter, ScopeId scope,
                                   bool unevaluated = false);
  SemaId AnalyzeAmbiguousTypeId(AstId type_id, ScopeId scope);
  SemaId AnalyzeNamedCall(AstId source, const QualifiedName& name,
                          ScopeId scope,
                          const std::vector<SemaId>& arguments);
  SemaId AnalyzeLiteral(AstId expression, ScopeId scope);
  SemaId AnalyzeName(AstId expression, ScopeId scope);
  SemaId AnalyzeUnary(AstId expression, ScopeId scope);
  SemaId AnalyzePostfix(AstId expression, ScopeId scope);
  SemaId AnalyzeMember(AstId expression, ScopeId scope);
  SemaId AnalyzeBinary(AstId expression, ScopeId scope);
  SemaId AnalyzeAssignment(AstId expression, ScopeId scope);
  SemaId AnalyzeConditional(AstId expression, ScopeId scope);
  SemaId AnalyzeSubscript(AstId expression, ScopeId scope);
  SemaId AnalyzeCast(AstId expression, ScopeId scope);
  SemaId AnalyzeSizeof(AstId expression, ScopeId scope);
  SemaId AnalyzeCall(AstId expression, ScopeId scope);
  SemaId AnalyzeNew(AstId expression, ScopeId scope);
  void ResolveCallCallee(AstId callee, ScopeId scope, CallResolution& result);
  void ResolveNamedCallee(const QualifiedName& name, ScopeId scope,
                          CallResolution& result);
  SemaId FinishCall(AstId expression, AstId callee, ScopeId scope,
                    CallResolution& resolution,
                    const std::vector<SemaId>& analyzed_arguments);
  SemaId TryFunctionalCast(AstId expression, AstId callee, AstId arguments,
                           ScopeId scope);
  SemaId TryCallableObjectExpression(AstId expression, AstId callee,
                                     AstId arguments, ScopeId scope);
  SemaId AnalyzeFunctionalCast(AstId expression, TypeId target,
                               ScopeId scope,
                               const std::vector<AstId>& arguments);
  SemaId AnalyzeBraced(AstId expression, ScopeId scope, TypeId target);
  bool IsAggregateType(TypeId type) const;
  bool IsStringLiteralArrayClause(AstId clause, TypeId target) const;
  SemaId AnalyzeAggregateClause(const std::vector<AstId>& clauses,
                                AstId clause, ScopeId scope, TypeId target,
                                std::size_t& index);
  void AnalyzeAggregateElements(const std::vector<AstId>& clauses,
                                std::size_t& index, ScopeId scope,
                                TypeId target, SemaId result);
  SemaId AnalyzeElidedAggregate(const std::vector<AstId>& clauses,
                                std::size_t& index, ScopeId scope,
                                TypeId target);

  Info NodeInfo(SemaId node) const;
  SemaId MakeExpression(SemaKind kind, AstId source, TypeId type,
                        ValueCategory category, ScopeId scope,
                        ETokenType op = KW_AUTO);
  void Append(SemaId parent, SemaId child);
  AstId Child(AstId node, std::size_t index) const;
  AstId FindChild(AstId node, AstKind kind) const;
  ETokenType Operator(AstId node) const;
  QualifiedName ExpressionName(AstId node) const;
  bool IsZeroLiteral(SemaId semantic) const;
  bool IsNullPointerConstant(SemaId semantic) const;
  bool IsTypeName(AstId expression, ScopeId scope, TypeId& type) const;
  bool IsFundamentalCastCallee(AstId callee) const;
  bool IsModifiableLvalue(SemaId node) const;
  void AppendDefaultArguments(FunctionEntityId function, TypeId function_type,
                              std::size_t supplied,
                              std::vector<SemaId>& arguments);
  bool RetargetFunctionName(SemaId expression, TypeId target);
  bool RetargetFunctionAddress(SemaId expression, TypeId target);
  bool TemplateArgumentTypes(const QualifiedName& name, ScopeId scope,
                             std::vector<TypeId>& arguments) const;
  void LookupNameBindings(const QualifiedName& name, ScopeId scope,
                          std::vector<BindingId>& bindings);
  void FilterAccessibleBindings(ScopeId scope,
                                std::vector<BindingId>& bindings) const;
  void FunctionCandidates(const QualifiedName& name, ScopeId scope,
                          std::vector<FunctionEntityId>& candidates);
  bool IsOperatorFunction(ETokenType op) const;
  std::string OperatorFunctionName(ETokenType op) const;
  SemaId MakeImplicitObject(SemaId object, ScopeId scope);
  OverloadArgument MakeOperatorArgument(SemaId expression, TypeId target,
                                        ScopeId scope);
  SemaId BuildConstructorTemporary(
      AstId source, TypeId target, ScopeId scope,
      const std::vector<SemaId>& arguments,
      bool list_initialization = false,
      bool copy_initialization = false);
  SemaId BuildResolvedCall(AstId source, ScopeId scope,
                           FunctionEntityId function,
                           SemaId implicit_object,
                           const std::vector<SemaId>& arguments,
                           bool bypass_implicit_object = false);
  SemaId TryOperatorCall(AstId source, ScopeId scope, ETokenType op,
                         const std::vector<SemaId>& operands,
                         bool allow_member = true,
                         bool allow_nonmember = true,
                         bool postfix = false);
  SemaId TryCallableObjectCall(AstId source, ScopeId scope, SemaId object,
                               const std::vector<SemaId>& arguments);
  TypeId CommonConditionalType(SemaId left, SemaId right) const;
  bool CanConvert(SemaId expression, TypeId target) const;
  SemaId BindImplicitObject(SemaId object, TypeId parameter);
  bool IsNarrowingListInitialization(SemaId expression,
                                     TypeId target) const;
  void CheckBaseConversionAccess(TypeId source, TypeId target,
                                 ScopeId scope) const;
  void FoldLiteral(SemaId node, AstId expression);
  void FoldUnary(SemaId node, ETokenType op, SemaId operand);
  void FoldBinary(SemaId node, ETokenType op, SemaId left, SemaId right);
  void FoldConditional(SemaId node, SemaId condition, SemaId left,
                       SemaId right);
  void FoldConversion(SemaId node, SemaId operand, TypeId target);
  static long long Checked(__int128 value);

  const std::vector<Pa6Token>& tokens_;
  const AstArena& arena_;
  SemaModel& model_;
  TypeTable& types_;
  SemaTree& tree_;
  ScopeBuilder& builder_;
};
