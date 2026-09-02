#pragma once

#include <cstddef>
#include <vector>

#include "posttoken_types.h"
#include "sema/scope_model.h"

// The semantic tree is an append-only, intrusive tree of typed facts.  A node
// records the type, value category, operator token, resolved binding or
// function entity, and the token span it was analyzed from; every spelling
// the dump prints is rendered from those identities at print time.
typedef std::size_t SemaId; // 0 is the null node

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
  SEMA_CONSTRUCTOR_ACTION,
  SEMA_LABELED_STATEMENT,
  SEMA_GOTO_STATEMENT
};

struct SemaNode
{
  SemaKind kind;
  ValueCategory category;
  ETokenType op; // operator or cast keyword; KW_AUTO when the node has none
  bool has_value; // integral constant value folded bottom-up
  TypeId type;
  BindingId binding;
  FunctionEntityId function;
  ScopeId scope;
  std::size_t first; // token span; empty for synthesized nodes
  std::size_t last;
  long long value;
  SemaId first_child;
  SemaId last_child;
  SemaId next_sibling;

  SemaNode();
  bool HasSpan() const { return first < last; }
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

  // Nodes made for a transient analysis (an array bound, an enumerator
  // value, a decltype operand) are released once their fact has been read:
  // the caller takes a mark before the analysis and truncates back to it.
  std::size_t Mark() const;
  void Truncate(std::size_t mark);

private:
  std::vector<SemaNode> nodes_;
  SemaId root_;
};
