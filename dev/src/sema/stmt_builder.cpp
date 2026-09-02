#include "sema/scope_builder.h"

#include <stdexcept>

using std::size_t;
using std::vector;

namespace
{

bool IsVoidType(const TypeTable& types, TypeId type)
{
  type = types.Unqualified(type);
  return types.Kind(type) == TYPE_FUNDAMENTAL &&
      types.At(type).fundamental == FT_VOID;
}

} // namespace

ScopeId ScopeBuilder::BuildCompound(AstId node, ScopeId parent,
                                    FunctionEntityId function,
                                    unsigned loop_depth,
                                    unsigned switch_depth,
                                    SemaId semantic_parent)
{
  const ScopeId block = model_.CreateScope(SCOPE_BLOCK, std::string(), parent);
  SemaId compound = semantic_parent;
  if (EmitsSemantics())
  {
    compound = MakeSemantic(SEMA_COMPOUND_STATEMENT, block,
                            semantic_parent != 0 ? semantic_parent :
                                SemanticParent(parent));
    MapSemanticScope(block, compound);
  }
  const vector<AstId>& children = arena_.At(node).children;
  const StatementContext context(block, function, loop_depth, switch_depth,
                                 compound);
  for (size_t i = 0; i < children.size(); ++i)
    BuildStatement(children[i], context);
  return block;
}

void ScopeBuilder::BuildBranch(AstId node, const StatementContext& context,
                               SemaId semantic_parent)
{
  if (node == 0)
    return;
  AstId statement = node;
  if (arena_.At(statement).kind == AST_THEN ||
      arena_.At(statement).kind == AST_ELSE)
  {
    if (arena_.At(statement).children.size() != 1)
      throw std::runtime_error("invalid selection branch");
    statement = arena_.At(statement).children[0];
  }
  if (statement == 0)
    return;
  const AstKind kind = arena_.At(statement).kind;
  if (IsDeclarationKind(kind))
  {
    // 6.4p1: a declaration used as an unbraced substatement owns an
    // implicit block scope, so sibling branches cannot redeclare one name.
    const ScopeId implicit = model_.CreateScope(SCOPE_BLOCK, std::string(),
                                                context.scope);
    if (tree_ != 0)
      MapSemanticScope(implicit, semantic_parent);
    BuildNode(statement, implicit);
    return;
  }
  BuildStatement(statement, StatementContext(
      context.scope, context.function, context.loop_depth,
      context.switch_depth, semantic_parent));
}

void ScopeBuilder::BuildConditionDeclaration(AstId node,
                                             const StatementContext& context,
                                             SemaId semantic_parent)
{
  const AstId specifiers = FindChild(node, AST_DECL_SPECIFIER_SEQ);
  const AstId declarator = FindChild(node, AST_DECLARATOR);
  const AstId initializer = FindChild(node, AST_INITIALIZER);
  if (specifiers == 0 || declarator == 0 || initializer == 0)
    throw std::runtime_error("invalid condition declaration");
  const TypeId base = BuildSpecifierType(specifiers, context.scope);
  const TypeId type = BuildDeclaratorType(declarator, base, context.scope);
  if (types_.Kind(type) == TYPE_FUNCTION)
    throw std::runtime_error("condition declares a function");
  std::string name;
  ResolveDeclarationScope(context.scope, FindIdentifier(declarator), name);
  const BindingId binding = model_.AddBinding(context.scope, name,
                                              BINDING_VARIABLE, type);
  RecordInitializedLocal(context.scope);
  SemaId condition = 0;
  if (tree_ != 0)
    condition = MakeSemantic(SEMA_CONDITION_DECLARATION, context.scope,
                             semantic_parent);
  if (tree_ != 0)
  {
    const SemaId variable = MakeSemantic(SEMA_VARIABLE, context.scope,
                                         condition, type, binding);
    const SemaId initialized = expression_.AnalyzeInitializer(
        initializer, context.scope, type);
    tree_->Append(variable, initialized);
  }
}

void ScopeBuilder::BuildCondition(AstId node, const StatementContext& context,
                                  SemaId semantic_parent, bool switch_condition)
{
  if (node == 0)
    throw std::runtime_error("missing condition");
  AstId condition = node;
  if (arena_.At(condition).kind == AST_CONDITION)
  {
    if (arena_.At(condition).children.size() != 1)
      throw std::runtime_error("invalid condition");
    condition = arena_.At(condition).children[0];
  }
  if (condition != 0 && arena_.At(condition).kind == AST_CONDITION_DECLARATION)
  {
    BuildConditionDeclaration(condition, context, semantic_parent);
    return;
  }
  if (tree_ == 0)
    return;
  const SemaId expression = expression_.Analyze(condition, context.scope);
  const SemaNode& value = tree_->At(expression);
  if (!switch_condition &&
      (!types_.IsScalar(value.type) ||
       (types_.Kind(types_.Unqualified(value.type)) == TYPE_ENUM &&
        types_.At(types_.Unqualified(value.type)).scoped)))
    throw std::runtime_error("condition is not contextual bool");
  if (tree_ != 0)
    tree_->Append(semantic_parent, expression);
}

void ScopeBuilder::BuildIfStatement(AstId node,
                                    const StatementContext& context)
{
  SemaId selection = 0;
  if (tree_ != 0)
    selection = MakeSemantic(SEMA_IF_STATEMENT, context.scope,
                             context.semantic_parent);
  const AstId condition = FindChild(node, AST_CONDITION);
  const AstId condition_declaration = FindChild(
      condition, AST_CONDITION_DECLARATION);
  ScopeId branch_scope = context.scope;
  if (condition_declaration != 0)
    branch_scope = model_.CreateScope(SCOPE_BLOCK, std::string(),
                                      context.scope);
  SemaId condition_node = 0;
  if (tree_ != 0)
    condition_node = MakeSemantic(SEMA_CONDITION, branch_scope, selection);
  BuildCondition(condition, StatementContext(
      branch_scope, context.function, context.loop_depth,
      context.switch_depth, condition_node), condition_node);
  const AstId then_node = FindChild(node, AST_THEN);
  if (tree_ != 0)
  {
    const SemaId then_semantic = MakeSemantic(SEMA_THEN, branch_scope,
                                              selection);
    BuildBranch(then_node, StatementContext(
        branch_scope, context.function, context.loop_depth,
        context.switch_depth, then_semantic), then_semantic);
  }
  else
    BuildBranch(then_node, StatementContext(
        branch_scope, context.function, context.loop_depth,
        context.switch_depth), 0);
  const AstId else_node = FindChild(node, AST_ELSE);
  if (else_node == 0)
    return;
  if (tree_ != 0)
  {
    const SemaId else_semantic = MakeSemantic(SEMA_ELSE, branch_scope,
                                              selection);
    BuildBranch(else_node, StatementContext(
        branch_scope, context.function, context.loop_depth,
        context.switch_depth, else_semantic), else_semantic);
  }
  else
    BuildBranch(else_node, StatementContext(
        branch_scope, context.function, context.loop_depth,
        context.switch_depth), 0);
}

void ScopeBuilder::BuildLoopStatement(AstId node,
                                      const StatementContext& context)
{
  const AstKind kind = arena_.At(node).kind;
  const SemaKind loop_kind = kind == AST_WHILE_STATEMENT ?
      SEMA_WHILE_STATEMENT : SEMA_DO_STATEMENT;
  SemaId loop = 0;
  if (tree_ != 0)
    loop = MakeSemantic(loop_kind, context.scope, context.semantic_parent);
  const AstId condition = FindChild(node, AST_CONDITION);
  const AstId body = FindChild(node, AST_COMPOUND_STATEMENT);
  if (kind == AST_DO_STATEMENT)
  {
    if (body != 0)
      BuildCompound(body, context.scope, context.function,
                    context.loop_depth + 1, context.switch_depth, loop);
    else
    {
      const vector<AstId>& children = arena_.At(node).children;
      for (size_t i = 0; i < children.size(); ++i)
        if (children[i] != condition)
          BuildBranch(children[i], StatementContext(
              context.scope, context.function, context.loop_depth + 1,
              context.switch_depth, loop), loop);
    }
    SemaId condition_node = 0;
    if (tree_ != 0)
      condition_node = MakeSemantic(SEMA_CONDITION, context.scope, loop);
    BuildCondition(condition, StatementContext(
        context.scope, context.function, context.loop_depth + 1,
        context.switch_depth, condition_node), condition_node);
    return;
  }
  SemaId condition_node = 0;
  if (tree_ != 0)
    condition_node = MakeSemantic(SEMA_CONDITION, context.scope, loop);
  BuildCondition(condition, context, condition_node);
  if (body != 0)
    BuildCompound(body, context.scope, context.function,
                  context.loop_depth + 1, context.switch_depth, loop);
  else
  {
    const vector<AstId>& children = arena_.At(node).children;
    for (size_t i = 0; i < children.size(); ++i)
      if (children[i] != condition)
        BuildBranch(children[i], StatementContext(
            context.scope, context.function, context.loop_depth + 1,
            context.switch_depth, loop), loop);
  }
}

void ScopeBuilder::BuildForStatement(AstId node,
                                     const StatementContext& context)
{
  SemaId loop = 0;
  if (tree_ != 0)
    loop = MakeSemantic(SEMA_FOR_STATEMENT, context.scope,
                        context.semantic_parent);
  const ScopeId for_scope = model_.CreateScope(SCOPE_BLOCK, std::string(),
                                               context.scope);
  const AstId init = FindChild(node, AST_FOR_INIT_STATEMENT);
  SemaId init_semantic = 0;
  if (tree_ != 0)
    init_semantic = MakeSemantic(SEMA_FOR_INIT_STATEMENT, for_scope, loop);
  if (init != 0)
  {
    MapSemanticScope(for_scope, init_semantic);
    const vector<AstId>& children = arena_.At(init).children;
    if (!children.empty())
    {
      const AstId initializer = children[0];
      if (IsDeclarationKind(arena_.At(initializer).kind))
        BuildStatement(initializer, StatementContext(
            for_scope, context.function, context.loop_depth,
            context.switch_depth, init_semantic));
      else
      {
        const SemaId expression = expression_.Analyze(initializer, for_scope);
        tree_->Append(init_semantic, expression);
      }
    }
  }
  const AstId condition = FindChild(node, AST_CONDITION);
  if (condition != 0)
  {
    SemaId condition_node = 0;
    if (tree_ != 0)
      condition_node = MakeSemantic(SEMA_CONDITION, for_scope, loop);
    BuildCondition(condition, StatementContext(
        for_scope, context.function, context.loop_depth,
        context.switch_depth, condition_node), condition_node);
  }
  const AstId iteration = FindChild(node, AST_ITERATION);
  if (iteration != 0)
  {
    SemaId iteration_node = 0;
    if (tree_ != 0)
      iteration_node = MakeSemantic(SEMA_ITERATION, for_scope, loop);
    const vector<AstId>& children = arena_.At(iteration).children;
    if (!children.empty())
    {
      const SemaId expression = expression_.Analyze(children[0], for_scope);
      if (tree_ != 0)
        tree_->Append(iteration_node, expression);
    }
  }
  const AstId body = FindChild(node, AST_COMPOUND_STATEMENT);
  if (body != 0)
    BuildCompound(body, for_scope, context.function,
                  context.loop_depth + 1, context.switch_depth, loop);
  else
  {
    const vector<AstId>& children = arena_.At(node).children;
    for (size_t i = 0; i < children.size(); ++i)
      if (children[i] != init && children[i] != condition &&
          children[i] != iteration)
        BuildBranch(children[i], StatementContext(
            for_scope, context.function, context.loop_depth + 1,
            context.switch_depth, loop), loop);
  }
}

void ScopeBuilder::BuildSwitchStatement(AstId node,
                                        const StatementContext& context)
{
  SemaId statement = 0;
  if (tree_ != 0)
    statement = MakeSemantic(SEMA_SWITCH_STATEMENT, context.scope,
                             context.semantic_parent);
  const AstId condition = FindChild(node, AST_CONDITION);
  SemaId condition_node = 0;
  if (tree_ != 0)
    condition_node = MakeSemantic(SEMA_CONDITION, context.scope, statement);
  BuildCondition(condition, context, condition_node, true);
  const AstId body = FindChild(node, AST_COMPOUND_STATEMENT);
  // The dispatch jumps from this point into the body, so every case label is
  // a jump target measured from here (6.7p3).
  SwitchEntry entry;
  entry.sequence = ++jump_sequence_;
  entry.scope = context.scope;
  switch_entries_.push_back(entry);
  if (body != 0)
    BuildCompound(body, context.scope, context.function, context.loop_depth,
                  context.switch_depth + 1, statement);
  switch_entries_.pop_back();
}

void ScopeBuilder::BuildCaseStatement(AstId node,
                                      const StatementContext& context)
{
  if (context.switch_depth == 0 || switch_entries_.empty())
    throw std::runtime_error("case outside switch");
  const vector<AstId>& children = arena_.At(node).children;
  if (children.size() < 2)
    throw std::runtime_error("invalid case statement");
  CheckJumpTarget(switch_entries_.back().sequence,
                  switch_entries_.back().scope, ++jump_sequence_,
                  context.scope);
  SemaId statement = 0;
  if (tree_ != 0)
    statement = MakeSemantic(SEMA_CASE_STATEMENT, context.scope,
                             context.semantic_parent);
  const SemaId label = expression_.Analyze(children[0], context.scope);
  long long value = 0;
  if (!expression_.TryConstant(label, value))
    throw std::runtime_error("case label is not constant");
  if (tree_ != 0)
    tree_->Append(statement, label);
  BuildStatement(children[1], StatementContext(
      context.scope, context.function, context.loop_depth,
      context.switch_depth, statement));
}

void ScopeBuilder::BuildDefaultStatement(AstId node,
                                         const StatementContext& context)
{
  if (context.switch_depth == 0 || switch_entries_.empty())
    throw std::runtime_error("default outside switch");
  CheckJumpTarget(switch_entries_.back().sequence,
                  switch_entries_.back().scope, ++jump_sequence_,
                  context.scope);
  if (tree_ != 0)
  {
    const SemaId statement = MakeSemantic(SEMA_DEFAULT_STATEMENT,
        context.scope, context.semantic_parent);
    if (!arena_.At(node).children.empty())
      BuildStatement(arena_.At(node).children[0], StatementContext(
          context.scope, context.function, context.loop_depth,
          context.switch_depth, statement));
  }
  else if (!arena_.At(node).children.empty())
    BuildStatement(arena_.At(node).children[0], context);
}

// Without a semantic dump only the declarations inside statements matter:
// block scopes, local types and constants.  Expressions are not analyzed.
void ScopeBuilder::BuildDeclarationsOnly(AstId node, ScopeId scope)
{
  if (node == 0)
    return;
  const AstKind kind = arena_.At(node).kind;
  if (kind == AST_COMPOUND_STATEMENT)
    (void)BuildCompound(node, scope);
  else if (IsDeclarationKind(kind))
    BuildNode(node, scope);
  else
  {
    const vector<AstId>& children = arena_.At(node).children;
    for (size_t i = 0; i < children.size(); ++i)
      BuildDeclarationsOnly(children[i], scope);
  }
}

// A function body is parsed before semantic inheritance is known.  If a
// nested type is not visible to the parser's declaration disambiguator,
// `Result & resolved = resolve(value)` arrives as an assignment expression
// instead of an AST_SIMPLE_DECLARATION.  Reclassify the unambiguous
// type-and-reference shape in the completed member context, then feed its
// initializer through the ordinary declaration/lifetime path.
bool ScopeBuilder::TryBuildAmbiguousReferenceDeclaration(
    AstId node, const StatementContext& context)
{
  if (node != 0 && arena_.At(node).kind == AST_EXPRESSION_STATEMENT)
  {
    if (arena_.At(node).children.size() != 1)
      return false;
    node = arena_.At(node).children[0];
  }
  if (!EmitsSemantics() || node == 0 ||
      arena_.At(node).kind != AST_ASSIGNMENT_EXPRESSION ||
      arena_.At(node).children.size() != 2)
    return false;
  const AstId left = arena_.At(node).children[0];
  const AstId initializer = arena_.At(node).children[1];
  if (arena_.At(left).kind != AST_BINARY_EXPRESSION ||
      arena_.At(left).children.size() != 2)
    return false;
  const AstId type_expression = arena_.At(left).children[0];
  const AstId identifier = arena_.At(left).children[1];
  if ((arena_.At(type_expression).kind != AST_ID_EXPRESSION &&
       arena_.At(type_expression).kind != AST_IDENTIFIER) ||
      (arena_.At(identifier).kind != AST_ID_EXPRESSION &&
       arena_.At(identifier).kind != AST_IDENTIFIER) ||
      arena_.At(type_expression).last >= tokens_.size() ||
      !tokens_[arena_.At(type_expression).last].IsSimple(OP_AMP))
    return false;

  TypeId referent = 0;
  try
  {
    referent = LookupType(context.scope, ReadQualifiedName(
        tokens_, arena_.At(type_expression).first,
        arena_.At(type_expression).last));
  }
  catch (const std::runtime_error&)
  {
    return false;
  }
  const std::string name = IdentifierName(identifier);
  if (name.empty())
    return false;
  const TypeId type = types_.Reference(referent, true);
  const BindingId binding = model_.AddBinding(
      context.scope, name, BINDING_VARIABLE, type);
  const SemaId declaration = MakeSemantic(
      SEMA_SIMPLE_DECLARATION, context.scope, context.semantic_parent);
  const SemaId variable = MakeSemantic(
      SEMA_VARIABLE, context.scope, declaration, type, binding);
  RecordInitializedLocal(context.scope);
  BuildVariable(binding, initializer, 0, context.scope, variable, false);
  return true;
}

void ScopeBuilder::BuildStatement(AstId node, const StatementContext& context)
{
  if (node == 0)
    return;
  if (!EmitsSemantics())
  {
    BuildDeclarationsOnly(node, context.scope);
    return;
  }
  const AstKind kind = arena_.At(node).kind;
  if (IsDeclarationKind(kind))
  {
    // The statement context owns the semantic placement, while the scope
    // passed to declaration analysis remains the lexical lookup scope.
    BuildNode(node, context.scope, context.semantic_parent);
    return;
  }
  if (TryBuildAmbiguousReferenceDeclaration(node, context))
    return;
  switch (kind)
  {
  case AST_COMPOUND_STATEMENT:
    BuildCompound(node, context.scope, context.function, context.loop_depth,
                  context.switch_depth, context.semantic_parent);
    return;
  case AST_EXPRESSION_STATEMENT:
  {
    SemaId statement = 0;
    if (tree_ != 0)
      statement = MakeSemantic(SEMA_EXPRESSION_STATEMENT, context.scope,
                               context.semantic_parent);
    if (!arena_.At(node).children.empty())
    {
      const SemaId expression = expression_.Analyze(
          arena_.At(node).children[0], context.scope);
      if (tree_ != 0)
        tree_->Append(statement, expression);
    }
    return;
  }
  case AST_RETURN_STATEMENT:
  {
    SemaId statement = 0;
    if (tree_ != 0)
      statement = MakeSemantic(SEMA_RETURN_STATEMENT, context.scope,
                               context.semantic_parent);
    TypeId return_type = 0;
    if (context.function != 0)
    {
      const TypeId function_type = model_.FunctionAt(context.function).type;
      return_type = types_.At(types_.Unqualified(function_type)).result;
    }
    const vector<AstId>& children = arena_.At(node).children;
    if (children.empty())
    {
      if (return_type == 0 || !IsVoidType(types_, return_type))
        throw std::runtime_error("non-void function requires a return value");
    }
    else
    {
      if (return_type == 0)
        throw std::runtime_error("return statement has no function context");
      const bool braced = arena_.At(children[0]).kind == AST_BRACED_INIT_LIST;
      const SemaId expression = braced ?
          expression_.AnalyzeInitializer(children[0], context.scope,
                                          return_type) :
          expression_.Analyze(children[0], context.scope);
      if (IsVoidType(types_, return_type)) {
        if (!IsVoidType(types_, expression_.Node(expression).type))
          throw std::runtime_error("void function cannot return a value");
      } else if (!braced) {
        expression_.Initialize(expression, return_type);
      }
      if (tree_ != 0)
        tree_->Append(statement, expression);
    }
    return;
  }
  case AST_IF_STATEMENT:
    BuildIfStatement(node, context);
    return;
  case AST_WHILE_STATEMENT:
  case AST_DO_STATEMENT:
    BuildLoopStatement(node, context);
    return;
  case AST_FOR_STATEMENT:
    BuildForStatement(node, context);
    return;
  case AST_SWITCH_STATEMENT:
    BuildSwitchStatement(node, context);
    return;
  case AST_CASE_STATEMENT:
    BuildCaseStatement(node, context);
    return;
  case AST_DEFAULT_STATEMENT:
    BuildDefaultStatement(node, context);
    return;
  case AST_BREAK_STATEMENT:
    if (context.loop_depth == 0 && context.switch_depth == 0)
      throw std::runtime_error("break outside loop or switch");
    if (tree_ != 0)
      MakeSemantic(SEMA_BREAK_STATEMENT, context.scope, context.semantic_parent);
    return;
  case AST_CONTINUE_STATEMENT:
    if (context.loop_depth == 0)
      throw std::runtime_error("continue outside loop");
    if (tree_ != 0)
      MakeSemantic(SEMA_CONTINUE_STATEMENT, context.scope,
                   context.semantic_parent);
    return;
  case AST_LABELED_STATEMENT:
  {
    const AstNode& label = arena_.At(node);
    const std::string name = label.text;
    if (name.empty() || label.children.size() != 1)
      throw std::runtime_error("invalid labeled statement");
    if (labels_.find(name) != labels_.end())
      throw std::runtime_error("duplicate label");
    LabelRecord& record = labels_[name];
    record.ordinal = static_cast<unsigned>(labels_.size());
    record.sequence = ++jump_sequence_;
    record.scope = context.scope;
    const SemaId statement = MakeSemantic(
        SEMA_LABELED_STATEMENT, context.scope, context.semantic_parent,
        0, 0, 0, VC_PRVALUE, KW_AUTO, label.first, label.last);
    if (statement != 0)
    {
      tree_->At(statement).has_value = true;
      tree_->At(statement).value = record.ordinal;
    }
    BuildStatement(label.children[0], StatementContext(
        context.scope, context.function, context.loop_depth,
        context.switch_depth, statement));
    return;
  }
  case AST_GOTO_STATEMENT:
  {
    const AstNode& jump = arena_.At(node);
    const std::string name = jump.text;
    if (name.empty() || !jump.children.empty())
      throw std::runtime_error("invalid goto statement");
    GotoRecord record;
    record.node = MakeSemantic(SEMA_GOTO_STATEMENT, context.scope,
                               context.semantic_parent, 0, 0, 0, VC_PRVALUE,
                               KW_AUTO, jump.first, jump.last);
    record.name = name;
    record.sequence = ++jump_sequence_;
    gotos_.push_back(record);
    return;
  }
  default:
    throw std::runtime_error("unsupported statement in semantic analysis");
  }
}
