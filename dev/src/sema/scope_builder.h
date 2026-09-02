#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "parser/ast_model.h"
#include "parser/recog_token.h"
#include "sema/const_eval.h"
#include "sema/qualified_name.h"
#include "sema/sema_tree.h"
#include "sema/scope_model.h"

class ExpressionAnalyzer;

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
  ScopeBuilder(const std::vector<Pa6Token>& tokens, const AstArena& arena,
               SemaModel& model, SemaTree& tree);
  ~ScopeBuilder();

  void Build(AstId root);

  // ConstantOperandTypes
  TypeId TypeOfTypeId(AstId type_id, ScopeId scope);
  TypeId TypeOfExpression(AstId expression, ScopeId scope);
  TypeId TypeIdForSemantics(AstId type_id, ScopeId scope);
  TypeId DecltypeForSemantics(AstId expression, ScopeId scope);
  SemaId AnalyzeExpression(AstId expression, ScopeId scope);
  SemaId AnalyzeInitializer(AstId initializer, ScopeId scope, TypeId target);
  SemaId InitializeExpression(SemaId expression, TypeId target,
                              bool variable = false, bool constexpr_value = false,
                              bool condition = false, bool return_value = false,
                              bool argument = false);
  bool TryConstant(SemaId expression, long long& value) const;
  bool IsSemantic() const { return tree_ != 0; }
  SemaTree* SemanticTree() const { return tree_; }

  // Template entities are owned by the scope builder until a use supplies
  // concrete arguments.  Instances are interned here so overload resolution
  // and the deferred semantic dump observe the same function entity.
  bool InstantiateFunctionTemplate(FunctionEntityId template_function,
                                   const std::vector<TypeId>& arguments,
                                   FunctionEntityId& function,
                                   BindingId& binding);
  void MarkTemplateInstanceUsed(FunctionEntityId function);
  bool DeduceFunctionTemplate(FunctionEntityId template_function,
                              const std::vector<TypeId>& arguments,
                              FunctionEntityId& function,
                              BindingId& binding);
  TypeId TypeForName(const QualifiedName& name, ScopeId scope) const;

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

  struct StatementContext
  {
    ScopeId scope;
    FunctionEntityId function;
    unsigned loop_depth;
    unsigned switch_depth;
    SemaId semantic_parent;

    StatementContext(ScopeId scope = 0, FunctionEntityId function = 0,
                     unsigned loop_depth = 0, unsigned switch_depth = 0,
                     SemaId semantic_parent = 0)
        : scope(scope), function(function), loop_depth(loop_depth),
          switch_depth(switch_depth), semantic_parent(semantic_parent) {}
  };

  // Declarations and scopes.
  void BuildNode(AstId node, ScopeId scope, SemaId semantic_parent = 0);
  void BuildTemplate(AstId node, ScopeId scope);
  void BuildTemplateParameter(AstId parameter, ScopeId scope);
  void BuildNamespace(AstId node, ScopeId scope);
  void BuildNamespaceAlias(AstId node, ScopeId scope);
  void BuildUsingDirective(AstId node, ScopeId scope);
  void BuildUsingDeclaration(AstId node, ScopeId scope);
  void BuildAlias(AstId node, ScopeId scope);
  void BuildSimpleDeclaration(AstId node, ScopeId scope,
                              SemaId semantic_parent = 0);
  void RecordConstantValue(BindingId binding, AstId init_declarator,
                           bool is_constexpr, ScopeId scope);
  void BuildFunctionDefinition(AstId node, ScopeId scope);
  void BuildStaticAssert(AstId node, ScopeId scope);
  void BuildLinkage(AstId node, ScopeId scope);
  ScopeId BuildCompound(AstId node, ScopeId parent,
                        FunctionEntityId function = 0,
                        unsigned loop_depth = 0,
                        unsigned switch_depth = 0,
                        SemaId semantic_parent = 0);
  void BuildStatement(AstId node, const StatementContext& context);
  void BuildBranch(AstId node, const StatementContext& context,
                   SemaId semantic_parent);
  void BuildCondition(AstId node, const StatementContext& context,
                      SemaId semantic_parent, bool switch_condition = false);
  void BuildConditionDeclaration(AstId node, const StatementContext& context,
                                 SemaId semantic_parent);
  void BuildIfStatement(AstId node, const StatementContext& context);
  void BuildLoopStatement(AstId node, const StatementContext& context);
  void BuildForStatement(AstId node, const StatementContext& context);
  void BuildSwitchStatement(AstId node, const StatementContext& context);
  void BuildCaseStatement(AstId node, const StatementContext& context);
  void BuildDefaultStatement(AstId node, const StatementContext& context);

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
  bool HasConstFunctionQualifier(AstId declarator) const;
  FunctionEntityId EnsureDefaultConstructor(TypeId type);
  void AddConstructorAction(SemaId variable, ScopeId scope, TypeId type,
                            BindingId binding, AstId declarator,
                            const std::string& object_name = std::string());
  void BuildAnonymousUnionStorage(AstId node, ScopeId scope,
                                  SemaId semantic_parent, TypeId type);
  void EmitDeferredSemantics();
  void EmitTemplateInstances();
  void DeferSemantic(SemaId node);

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
                             ScopeId lookup_scope,
                             bool parameter_context = false);
  TypeId ApplyPrefix(TypeId base, const std::vector<AstId>& prefix);
  TypeId ApplySuffix(TypeId base, const std::vector<AstId>& suffix,
                     ScopeId lookup_scope, bool parameter_context);
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

  // Semantic-tree helpers.  The scope model remains the source of truth for
  // lookup; this map only records the nearest dump container for a scope.
  SemaId SemanticParent(ScopeId scope, SemaId fallback = 0) const;
  SemaId MakeSemantic(SemaKind kind, ScopeId scope, SemaId parent,
                      TypeId type = 0, BindingId binding = 0,
                      FunctionEntityId function = 0,
                      ValueCategory category = VC_PRVALUE,
                      ETokenType op = KW_AUTO,
                      std::size_t first = 0, std::size_t last = 0);
  void MapSemanticScope(ScopeId scope, SemaId node);
  FunctionEntityId DeclareFunction(ScopeId scope, const std::string& name,
                                   TypeId declared_type, bool definition,
                                   BindingId& binding,
                                   bool member_const = false);
  SemaId MakeDetachedSemantic(SemaKind kind, ScopeId scope, TypeId type,
                              BindingId binding, FunctionEntityId function);
  TypeId SubstituteTemplateType(TypeId type,
                                const std::map<std::string, TypeId>& values);
  bool DeduceTemplateType(TypeId pattern, TypeId argument,
                          std::map<std::string, TypeId>& values) const;
  bool BuildTemplateInstance(FunctionEntityId template_function,
                             const std::map<std::string, TypeId>& values,
                             FunctionEntityId& function, BindingId& binding);
  bool HasIncompleteArray(AstId declarator) const;
  std::size_t InitializerBound(AstId initializer) const;

  const std::vector<Pa6Token>& tokens_;
  const AstArena& arena_;
  SemaModel& model_;
  TypeTable& types_;
  ConstEvaluator const_eval_;
  SemaTree* tree_;
  ExpressionAnalyzer* expression_;
  SemaId semantic_root_;
  std::map<ScopeId, SemaId> semantic_scopes_;
  std::size_t pending_array_bound_;
  unsigned unnamed_local_enum_counter_;
  unsigned unnamed_local_class_counter_;
  bool suppress_semantics_;
  std::vector<SemaId> deferred_semantics_;
  struct DeferredTemplateInstance
  {
    FunctionEntityId template_function;
    std::vector<TypeId> arguments;
    FunctionEntityId function;
    BindingId binding;
    bool used;

    DeferredTemplateInstance(FunctionEntityId template_function = 0,
                             const std::vector<TypeId>& arguments =
                                 std::vector<TypeId>(),
                             FunctionEntityId function = 0,
                             BindingId binding = 0,
                             bool used = false)
        : template_function(template_function), arguments(arguments),
          function(function), binding(binding), used(used) {}
  };
  std::vector<DeferredTemplateInstance> deferred_template_instances_;
};
