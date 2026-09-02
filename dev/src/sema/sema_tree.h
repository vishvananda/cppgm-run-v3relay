#pragma once

#include <cstddef>
#include <vector>

#include "posttoken_types.h"
#include "sema/scope_model.h"

// The semantic tree is an append-only, intrusive tree.  Spans and bindings
// are kept as ids so the dump can recover source spellings and canonical
// entities without duplicating strings on every expression node.
typedef std::size_t SemaId;

enum ValueCategory
{
  VC_LVALUE,
  VC_PRVALUE,
  VC_XVALUE
};

enum SemaKind
{
  SEMA_TRANSLATION_UNIT,
  SEMA_NAMESPACE_DEFINITION,
  SEMA_TYPE_ALIAS,
  SEMA_VARIABLE,
  SEMA_FUNCTION_DECLARATION,
  SEMA_FUNCTION_DEFINITION,
  SEMA_PARAMETER,
  SEMA_SIMPLE_DECLARATION,
  SEMA_COMPOUND_STATEMENT,
  SEMA_EXPRESSION_STATEMENT,
  SEMA_RETURN_STATEMENT,
  SEMA_IF_STATEMENT,
  SEMA_THEN,
  SEMA_ELSE,
  SEMA_WHILE_STATEMENT,
  SEMA_DO_STATEMENT,
  SEMA_FOR_STATEMENT,
  SEMA_FOR_INIT_STATEMENT,
  SEMA_ITERATION,
  SEMA_SWITCH_STATEMENT,
  SEMA_CASE_STATEMENT,
  SEMA_DEFAULT_STATEMENT,
  SEMA_BREAK_STATEMENT,
  SEMA_CONTINUE_STATEMENT,
  SEMA_CONDITION,
  SEMA_CONDITION_DECLARATION,
  SEMA_CALL,
  SEMA_CALLEE,
  SEMA_ID_EXPRESSION,
  SEMA_LITERAL,
  SEMA_UNARY,
  SEMA_POSTFIX,
  SEMA_BINARY,
  SEMA_ASSIGNMENT,
  SEMA_CONDITIONAL,
  SEMA_SUBSCRIPT,
  SEMA_MEMBER,
  SEMA_CAST,
  SEMA_SIZEOF,
  SEMA_BRACED_INIT_LIST,
  SEMA_CONSTRUCTOR_ACTION
};

struct SemaNode
{
  SemaKind kind;
  ValueCategory category;
  TypeId type;
  ETokenType op;
  BindingId binding;
  FunctionEntityId function;
  ScopeId scope;
  std::size_t first;
  std::size_t last;
  bool has_value;
  long long value;
  SemaId first_child;
  SemaId last_child;
  SemaId next_sibling;

  SemaNode();
};

class SemaTree
{
public:
  SemaTree();

  SemaId Make(SemaKind kind);
  void Append(SemaId parent, SemaId child);
  SemaNode& At(SemaId id);
  const SemaNode& At(SemaId id) const;

  SemaId Root() const;
  void SetRoot(SemaId root);

private:
  std::vector<SemaNode> nodes_;
  SemaId root_;
};
