#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "parser/ast_model.h"
#include "parser/recog_token.h"
#include "sema/const_eval.h"
#include "sema/qualified_name.h"
#include "sema/scope_model.h"

// Walks the PA10 AST once and populates the SemaModel: declaration
// collection and scope ownership (scope_builder.cpp) and specifier- and
// declarator-derived type construction (type_builder.cpp).  Names are read
// from node token spans; nothing is recovered from dump text.  Unsupported
// constructs throw, which the driver maps to EXIT_FAILURE.
class ScopeBuilder : public ConstantOperandTypes
{
public:
  ScopeBuilder(const std::vector<Pa6Token>& tokens, const AstArena& arena,
               SemaModel& model);

  void Build(AstId root);

  // ConstantOperandTypes
  TypeId TypeOfTypeId(AstId type_id, ScopeId scope);
  TypeId TypeOfExpression(AstId expression, ScopeId scope);

private:
  struct ParameterInfo
  {
    std::string name;
    TypeId type;
  };
  // Static type of a decltype/sizeof operand and whether it is an lvalue.
  struct ExpressionType
  {
    TypeId type;
    bool lvalue;
    bool names_type; // the operand is a type name, not an expression
  };

  // Declarations and scopes.
  void BuildNode(AstId node, ScopeId scope);
  void BuildTemplate(AstId node, ScopeId scope);
  void BuildTemplateParameter(AstId parameter, ScopeId scope);
  void BuildNamespace(AstId node, ScopeId scope);
  void BuildNamespaceAlias(AstId node, ScopeId scope);
  void BuildUsingDirective(AstId node, ScopeId scope);
  void BuildUsingDeclaration(AstId node, ScopeId scope);
  void BuildAlias(AstId node, ScopeId scope);
  void BuildSimpleDeclaration(AstId node, ScopeId scope);
  void RecordConstantValue(BindingId binding, AstId init_declarator,
                           bool is_constexpr, ScopeId scope);
  void BuildFunctionDefinition(AstId node, ScopeId scope);
  void BuildStaticAssert(AstId node, ScopeId scope);
  void BuildLinkage(AstId node, ScopeId scope);
  ScopeId BuildCompound(AstId node, ScopeId parent);
  void BuildStatement(AstId node, ScopeId scope);

  // Class and enum declarations; each returns the type the specifier denotes.
  TypeId BuildClassDefinition(AstId node, ScopeId scope,
                              const std::string& anonymous_name);
  TypeId BuildClassForward(AstId node, ScopeId scope);
  TypeId BuildElaboratedClass(AstId node, ScopeId scope,
                              bool may_declare);
  TypeId BuildEnum(AstId node, ScopeId scope,
                   const std::string& anonymous_name);
  TypeId BuildEnumDefinition(AstId node, ScopeId scope, ScopeId parent,
                             const QualifiedName& name,
                             const std::string& spelling, bool scoped,
                             TypeId underlying,
                             const std::vector<AstId>& enumerators);
  void BindEnumerators(const std::vector<AstId>& enumerators, ScopeId scope,
                       TypeId type);
  std::string AnonymousTypeName(AstId node, const char* kind) const;

  // Types (type_builder.cpp).
  TypeId BuildSpecifierType(AstId specifier_sequence, ScopeId lookup_scope,
                            const std::string& anonymous_name = std::string());
  TypeId BuildTypeSequence(AstId sequence, ScopeId lookup_scope,
                           bool in_declaration,
                           const std::string& anonymous_name);
  TypeId BuildTypeNode(AstId node, ScopeId lookup_scope, bool in_declaration,
                       const std::string& anonymous_name);
  TypeId BuildTypeId(AstId node, ScopeId lookup_scope);
  TypeId BuildDeclaratorType(AstId declarator, TypeId base,
                             ScopeId lookup_scope);
  TypeId ApplyPrefix(TypeId base, const std::vector<AstId>& prefix);
  TypeId ApplySuffix(TypeId base, const std::vector<AstId>& suffix,
                     ScopeId lookup_scope);
  void BuildParameters(AstId clause, ScopeId lookup_scope,
                       std::vector<ParameterInfo>& parameters, bool& variadic);
  TypeId BuildDecltype(AstId expression, ScopeId lookup_scope);
  ExpressionType BuildExpressionType(AstId expression, ScopeId lookup_scope);
  TypeId LookupType(ScopeId scope, const QualifiedName& name) const;

  // AST access.
  AstId FindChild(AstId node, AstKind kind) const;
  AstId FindIdentifier(AstId declarator) const;
  std::string IdentifierName(AstId identifier) const;
  QualifiedName NodeName(AstId node) const;
  ScopeId ResolveDeclarationScope(ScopeId scope, AstId identifier,
                                  std::string& name) const;
  ScopeId ResolveNamespace(ScopeId scope, AstId target) const;
  ScopeId ResolveQualifierScope(ScopeId scope, const QualifiedName& prefix) const;
  TypeKeyword ClassKey(AstId node) const;
  bool SequenceHasKeyword(AstId sequence, ETokenType keyword) const;
  static bool IsDeclarationKind(AstKind kind);
  static bool IsIgnoredSpecifier(ETokenType token);

  const std::vector<Pa6Token>& tokens_;
  const AstArena& arena_;
  SemaModel& model_;
  TypeTable& types_;
  ConstEvaluator const_eval_;
};
