#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "parser/ast_model.h"
#include "parser/recog_token.h"
#include "sema/const_eval.h"
#include "sema/scope_model.h"

class ScopeBuilder
{
public:
  ScopeBuilder(const std::vector<Pa6Token>& tokens, AstArena& arena,
               TypeTable& types, SemaModel& model);

  void Build(AstId root);

  TypeId BuildSpecifierType(AstId specifier_sequence, ScopeId lookup_scope);
  TypeId BuildDeclaratorType(AstId declarator, TypeId base,
                             ScopeId lookup_scope);

private:
  struct ParameterInfo
  {
    std::string name;
    TypeId type;
  };

  void BuildNode(AstId node, ScopeId scope);
  void BuildNamespace(AstId node, ScopeId scope);
  void BuildNamespaceAlias(AstId node, ScopeId scope);
  void BuildUsingDirective(AstId node, ScopeId scope);
  void BuildUsingDeclaration(AstId node, ScopeId scope);
  void BuildAlias(AstId node, ScopeId scope);
  void BuildSimpleDeclaration(AstId node, ScopeId scope);
  void BuildFunctionDefinition(AstId node, ScopeId scope);
  TypeId BuildEnumType(AstId node, ScopeId scope,
                       const std::string& anonymous_name = std::string());
  void BuildStaticAssert(AstId node, ScopeId scope);
  TypeId BuildClassSpecifier(AstId node, ScopeId scope,
                             const std::string& anonymous_name = std::string());
  void BuildClassForward(AstId node, ScopeId scope);
  void BuildLinkage(AstId node, ScopeId scope);
  ScopeId BuildCompound(AstId node, ScopeId parent);
  void BuildStatement(AstId node, ScopeId scope);

  TypeId BuildTypeSequence(AstId sequence, ScopeId lookup_scope,
                           bool allow_elaborated_declaration);
  TypeId BuildTypeNode(AstId node, ScopeId lookup_scope,
                       bool allow_elaborated_declaration);
  TypeId BuildTypeId(AstId node, ScopeId lookup_scope);
  TypeId BuildDecltype(AstId expression, ScopeId lookup_scope);
  TypeId BuildExpressionType(AstId expression, ScopeId lookup_scope,
                             bool& lvalue);
  TypeId ApplyPrefix(TypeId base, const std::vector<AstId>& prefix);
  TypeId ApplySuffix(TypeId base, const std::vector<AstId>& suffix,
                     ScopeId lookup_scope);
  TypeId AddCv(TypeId base, bool is_const, bool is_volatile);
  bool IsBuiltinToken(ETokenType token) const;
  bool IsIgnoredSpecifier(ETokenType token) const;
  TypeId BuildFundamental(const std::vector<ETokenType>& tokens) const;
  void BuildParameters(AstId clause, ScopeId lookup_scope,
                       std::vector<ParameterInfo>& parameters, bool& variadic);
  AstId FindIdentifier(AstId declarator) const;
  AstId FindInitializer(AstId init_declarator) const;
  AstId FindParameterClause(AstId declarator) const;
  std::string IdentifierName(AstId identifier) const;
  std::vector<std::string> NameComponents(std::size_t first,
                                          std::size_t last) const;
  std::vector<std::string> NameComponents(AstId node) const;
  BindingId ResolveName(ScopeId scope, const std::vector<std::string>& name,
                        unsigned filter) const;
  ScopeId ResolveDeclarationScope(ScopeId scope, AstId identifier,
                                  std::string& name) const;
  ScopeId ResolveNamespace(ScopeId scope, AstId target) const;
  TypeId BuildClassType(AstId node, ScopeId scope,
                        const std::string& anonymous_name);
  TypeId BuildForwardType(AstId node, ScopeId scope, bool declaration);
  std::string ClassKey(AstId node) const;
  std::string TypeName(AstId node) const;
  bool SequenceHasKeyword(AstId sequence, ETokenType keyword) const;
  bool IsDeclarationKind(AstKind kind) const;
  void AddFunctionParameters(ScopeId function_scope, AstId declarator,
                             ScopeId lookup_scope);

  const std::vector<Pa6Token>& tokens_;
  AstArena& arena_;
  TypeTable& types_;
  SemaModel& model_;
  ConstEvaluator const_eval_;
  std::string active_anonymous_name_;
};
