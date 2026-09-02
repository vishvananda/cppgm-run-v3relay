#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "parser/ast_model.h"
#include "parser/recog_token.h"
#include "sema/expr_sema.h"
#include "sema/qualified_name.h"
#include "sema/sema_tree.h"
#include "sema/scope_model.h"

// Walks the PA10 AST once and populates the SemaModel: declaration
// collection and scope ownership (scope_builder.cpp), specifier- and
// declarator-derived type construction (type_builder.cpp) and statement
// scopes (stmt_builder.cpp).  Every expression - an initializer, an array
// bound, an enumerator value, a decltype or sizeof operand, a statement -
// goes through the one ExpressionAnalyzer: with a dump tree (PA12) its nodes
// form the semantic dump, otherwise they are transient facts read and
// released.  Names are read from node token spans; nothing is recovered from
// dump text.  Unsupported constructs throw, which the driver maps to
// EXIT_FAILURE.
class ScopeBuilder
{
public:
  // Scope and type model only (--emit-types).
  ScopeBuilder(const std::vector<Pa6Token>& tokens, const AstArena& arena,
               SemaModel& model);
  // Scope and type model plus the semantic dump tree (--emit-semantics).
  ScopeBuilder(const std::vector<Pa6Token>& tokens, const AstArena& arena,
               SemaModel& model, SemaTree& tree);

  void Build(AstId root);

  // Type construction and template entities for the expression analyzer.
  TypeId TypeOfTypeId(AstId type_id, ScopeId scope);
  TypeId TypeOfDecltype(AstId expression, ScopeId scope);
  TypeId TypeForName(const QualifiedName& name, ScopeId scope) const;
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
  // 13.3.1.3: the constructor that direct-initializes `type` from the
  // analyzed arguments; the synthesized default constructor when the class
  // declares none.  Expression analysis and declarations share this one
  // selection so temporaries and named objects agree.
  FunctionEntityId ResolveConstructor(TypeId type,
                                       const std::vector<SemaId>& arguments,
                                       ScopeId scope,
                                       bool copy_initialization = false);

private:
  struct ParameterInfo
  {
    std::string name;
    TypeId type;
    AstId default_initializer;
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

  // Template parameter type -> argument type, in parameter order.
  typedef std::vector<std::pair<TypeId, TypeId> > TemplateBindings;

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
  void BuildBitFieldDeclaration(AstId node, ScopeId scope);
  void BuildVariable(BindingId binding, AstId initializer, AstId declarator,
                     ScopeId scope, SemaId variable, bool is_constexpr);
  void BuildFunctionDefinition(AstId node, ScopeId scope);
  void BuildSpecialMember(AstId node, ScopeId scope);
  // 9.2p2 complete-class contexts, run once when the class body closes over
  // the function definitions deferred since `first_pending`.
  void CompleteClassMembers(ClassEntityId entity, std::size_t first_pending);
  void BuildConstructorDefaults(FunctionEntityId constructor,
                                SemaId function_node, ScopeId function_scope);
  void EnsureSubobjectConstructors(ClassEntityId entity, SemaId function_node);
  void EnsureSubobjectDestructors(ClassEntityId entity);
  void BuildInheritedConstructors(ClassEntityId derived,
                                  ClassEntityId base, ScopeId scope);
  void BuildStaticAssert(AstId node, ScopeId scope);
  void BuildLinkage(AstId node, ScopeId scope);
  ScopeId BuildCompound(AstId node, ScopeId parent,
                        FunctionEntityId function = 0,
                        unsigned loop_depth = 0,
                        unsigned switch_depth = 0,
                        SemaId semantic_parent = 0);
  void BuildStatement(AstId node, const StatementContext& context);
  void BuildDeclarationsOnly(AstId node, ScopeId scope);
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
  void CompleteClassLayout(ClassEntityId entity);
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
  bool HasVolatileFunctionQualifier(AstId declarator) const;
  bool IsNoThrowDeclarator(AstId declarator, ScopeId scope);
  FunctionEntityId EnsureDefaultConstructor(TypeId type);
  FunctionEntityId EnsureAggregateConstructor(
      TypeId type, const std::vector<SemaId>& arguments);
  FunctionEntityId EnsureDestructor(TypeId type);
  void AddConstructorAction(SemaId variable, ScopeId scope, TypeId type,
                            BindingId binding, AstId declarator);
  void AddConstructorActionWithArguments(
      SemaId variable, ScopeId scope, TypeId type, BindingId binding,
      const std::vector<AstId>& arguments,
      bool copy_initialization = false,
      bool list_initialization = false);
  void BuildMemberInitializers(AstId initializer, ScopeId function_scope,
                               SemaId function_node,
                               FunctionEntityId owner);
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
  // `deduced_bound` completes an array declarator whose bound is omitted
  // (8.5.1p4: the initializer's element count); 0 when there is none.
  TypeId BuildDeclaratorType(AstId declarator, TypeId base,
                             ScopeId lookup_scope,
                             bool parameter_context = false,
                             std::size_t deduced_bound = 0);
  TypeId ApplyPrefix(TypeId base, const std::vector<AstId>& prefix);
  TypeId ApplySuffix(TypeId base, const std::vector<AstId>& suffix,
                     ScopeId lookup_scope, bool parameter_context,
                     std::size_t deduced_bound);
  void BuildParameters(AstId clause, ScopeId lookup_scope,
                       std::vector<ParameterInfo>& parameters, bool& variadic);
  TypeId BuildDecltype(AstId expression, ScopeId lookup_scope);
  TypeId LookupType(ScopeId scope, const QualifiedName& name) const;
  // 5.19 integral constant value; the analysis nodes are released.
  long long ConstantValue(AstId expression, ScopeId scope);
  // A const integral or enumeration object records a constant initializer
  // value on its binding (5.19p2).
  bool RecordsConstantValue(TypeId type) const;

  // AST access.
  AstId FindChild(AstId node, AstKind kind) const;
  AstId FindIdentifier(AstId declarator) const;
  std::string IdentifierName(AstId identifier) const;
  QualifiedName NodeName(AstId node) const;
  ScopeId ResolveDeclarationScope(ScopeId scope, AstId identifier,
                                  std::string& name) const;
  ScopeId EnclosingNamespace(ScopeId scope) const;
  ScopeId ResolveNamespace(ScopeId scope, AstId target) const;
  ScopeId ResolveQualifierScope(ScopeId scope, const QualifiedName& prefix) const;
  TypeKeyword ClassKey(AstId node) const;
  bool SequenceHasKeyword(AstId sequence, ETokenType keyword) const;
  static bool IsDeclarationKind(AstKind kind);
  static bool IsIgnoredSpecifier(ETokenType token);

  // Semantic-tree helpers.  The scope model remains the source of truth for
  // lookup; the dense scope table only records the nearest dump container.
  SemaTree& Tree() { return tree_ != 0 ? *tree_ : scratch_tree_; }
  bool EmitsSemantics() const { return tree_ != 0 && !suppress_semantics_; }
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
                                   bool member_const = false,
                                   bool member_volatile = false,
                                   bool internal_linkage = false,
                                   bool noexcept_qualifier = false,
                                   const std::vector<AstId>& default_arguments =
                                       std::vector<AstId>(),
                                   bool explicit_constructor = false);
  SemaId MakeDetachedSemantic(SemaKind kind, ScopeId scope, TypeId type,
                              BindingId binding, FunctionEntityId function);
  TypeId SubstituteTemplateType(TypeId type, const TemplateBindings& values);
  bool DeduceTemplateType(TypeId pattern, TypeId argument,
                          TemplateBindings& values) const;
  bool BuildTemplateInstance(FunctionEntityId template_function,
                             const TemplateBindings& values,
                             FunctionEntityId& function, BindingId& binding);
  bool HasIncompleteArray(AstId declarator) const;
  std::size_t InitializerBound(AstId initializer) const;

  // Function-local jump facts (6.1, 6.7p3).  Each label receives a
  // per-function ordinal that is published on its labeled-statement node and
  // on every goto that names it, so later phases never resolve a label
  // spelling again.  Every jump destination is checked against the
  // initialized automatic objects whose scope it would enter: case labels
  // when they are built, gotos once the body is complete.  Positions are one
  // monotonic per-function sequence.
  struct LabelRecord
  {
    unsigned ordinal;
    unsigned sequence;
    ScopeId scope;
  };
  struct GotoRecord
  {
    SemaId node;
    std::string name;
    unsigned sequence;
  };
  struct SwitchEntry
  {
    unsigned sequence;
    ScopeId scope; // the scope containing the switch statement
  };
  std::map<std::string, LabelRecord> labels_;
  std::vector<GotoRecord> gotos_;
  std::map<ScopeId, std::vector<unsigned> > initialized_locals_;
  std::vector<SwitchEntry> switch_entries_;
  void RecordInitializedLocal(ScopeId scope);
  void CheckJumpTarget(unsigned source_sequence, ScopeId source_scope,
                       unsigned target_sequence, ScopeId target_scope) const;
  void LinkRedeclaration(BindingId binding, ScopeId scope,
                         const std::string& name, TypeId type);
  bool CompatibleRedeclaration(TypeId prior, TypeId current) const;

  const std::vector<Pa6Token>& tokens_;
  const AstArena& arena_;
  SemaModel& model_;
  TypeTable& types_;
  SemaTree* tree_; // the semantic dump tree; 0 when only the model is built
  SemaTree scratch_tree_; // transient analyses when there is no dump tree
  ExpressionAnalyzer expression_;
  SemaId semantic_root_;
  std::vector<SemaId> semantic_scopes_; // indexed by ScopeId
  unsigned unnamed_local_enum_counter_;
  unsigned unnamed_local_class_counter_;
  unsigned c_linkage_depth_;
  AccessKind member_access_;
  ClassEntityId current_class_;
  bool suppress_semantics_;
  unsigned jump_sequence_;
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
  // A function defined inside a class body (member, constructor, or
  // friend) whose body waits for the class to complete.  A zero body is a
  // defaulted or inherited constructor that gets an empty compound.
  struct DeferredMemberBody
  {
    AstId body;
    ScopeId scope;
    FunctionEntityId function;
    SemaId function_node;

    DeferredMemberBody(AstId body = 0, ScopeId scope = 0,
                       FunctionEntityId function = 0,
                       SemaId function_node = 0)
        : body(body), scope(scope), function(function),
          function_node(function_node) {}
  };
  std::vector<DeferredTemplateInstance> deferred_template_instances_;
  // Stack discipline: a class body pushes its definitions and pops them at
  // completion, so the vector never holds more than the open class bodies.
  std::vector<DeferredMemberBody> deferred_member_bodies_;
};
