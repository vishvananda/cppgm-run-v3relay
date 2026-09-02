#include "sema/semantics_dump.h"

#include <map>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

using std::size_t;
using std::string;
using std::vector;

namespace
{

void Indent(std::ostream& out, unsigned depth)
{
  for (unsigned i = 0; i < depth; ++i)
    out << "  ";
}

const char* Category(ValueCategory category)
{
  switch (category)
  {
  case VC_LVALUE: return "lvalue";
  case VC_PRVALUE: return "prvalue";
  case VC_XVALUE: return "xvalue";
  }
  return "prvalue";
}

const char* KindName(SemaKind kind)
{
  switch (kind)
  {
  case SEMA_TRANSLATION_UNIT: return "translation-unit";
  case SEMA_NAMESPACE_DEFINITION: return "namespace-definition";
  case SEMA_TYPE_ALIAS: return "type-alias";
  case SEMA_VARIABLE: return "variable";
  case SEMA_FUNCTION_DECLARATION: return "function-declaration";
  case SEMA_FUNCTION_DEFINITION: return "function-definition";
  case SEMA_PARAMETER: return "parameter";
  case SEMA_SIMPLE_DECLARATION: return "simple-declaration";
  case SEMA_COMPOUND_STATEMENT: return "compound-statement";
  case SEMA_EXPRESSION_STATEMENT: return "expression-statement";
  case SEMA_RETURN_STATEMENT: return "return-statement";
  case SEMA_IF_STATEMENT: return "if-statement";
  case SEMA_THEN: return "then";
  case SEMA_ELSE: return "else";
  case SEMA_WHILE_STATEMENT: return "while-statement";
  case SEMA_DO_STATEMENT: return "do-statement";
  case SEMA_FOR_STATEMENT: return "for-statement";
  case SEMA_FOR_INIT_STATEMENT: return "for-init-statement";
  case SEMA_ITERATION: return "iteration";
  case SEMA_SWITCH_STATEMENT: return "switch-statement";
  case SEMA_CASE_STATEMENT: return "case-statement";
  case SEMA_DEFAULT_STATEMENT: return "default-statement";
  case SEMA_BREAK_STATEMENT: return "break-statement";
  case SEMA_CONTINUE_STATEMENT: return "continue-statement";
  case SEMA_CONDITION: return "condition";
  case SEMA_CONDITION_DECLARATION: return "condition-declaration";
  case SEMA_CALL: return "call-expression";
  case SEMA_CALLEE: return "callee";
  case SEMA_ID_EXPRESSION: return "id-expression";
  case SEMA_LITERAL: return "literal";
  case SEMA_UNARY: return "unary-expression";
  case SEMA_POSTFIX: return "postfix-expression";
  case SEMA_BINARY: return "binary-expression";
  case SEMA_ASSIGNMENT: return "assignment-expression";
  case SEMA_CONDITIONAL: return "conditional-expression";
  case SEMA_SUBSCRIPT: return "subscript-expression";
  case SEMA_MEMBER: return "member-expression";
  case SEMA_CAST: return "cast-expression";
  case SEMA_SIZEOF: return "sizeof-expression";
  case SEMA_BRACED_INIT_LIST: return "braced-init-list";
  case SEMA_CONSTRUCTOR_ACTION: return "constructor-action";
  case SEMA_MEMBER_INITIALIZER: return "member-initializer";
  case SEMA_LABELED_STATEMENT: return "labeled-statement";
  case SEMA_GOTO_STATEMENT: return "goto-statement";
  }
  return "unknown";
}

// Source spelling of a node's token span.
void PrintSpan(std::ostream& out, const SemaNode& node,
               const vector<Pa6Token>& tokens)
{
  for (size_t i = node.first; i < node.last && i < tokens.size(); ++i)
  {
    const Pa6Token& token = tokens[i];
    if (token.kind == PA6_EOF_TOKEN)
      break;
    if (token.kind == PA6_RSHIFT_1_TOKEN ||
        token.kind == PA6_RSHIFT_2_TOKEN)
      out << '>';
    else
      out << token.spelling;
  }
}

// `A::B::f`: the named namespace and class scopes enclosing the entity.
void PrintScopePrefix(std::ostream& out, const SemaModel& model,
                      ScopeId scope)
{
  if (scope == model.GlobalScope())
    return;
  const Scope& owner = model.ScopeAt(scope);
  PrintScopePrefix(out, model, owner.parent);
  if ((owner.kind == SCOPE_NAMESPACE || owner.kind == SCOPE_CLASS) &&
      !owner.unnamed_namespace && !owner.name.empty())
    out << owner.name << "::";
}

void PrintFunctionName(std::ostream& out, const SemaModel& model,
                       FunctionEntityId function)
{
  if (function == 0)
    return;
  const FunctionEntity& entity = model.FunctionAt(function);
  PrintScopePrefix(out, model, entity.scope);
  out << entity.name;
}

// Spelling of an operator a synthesized node carries without a source token.
const char* SynthesizedSpelling(ETokenType op)
{
  switch (op)
  {
  case OP_AMP: return "&";
  default: break;
  }
  throw std::runtime_error("synthesized operator has no spelling");
}

// `OP_PLUS:+`: the token kind and the source spelling of the operator.
void PrintOperator(std::ostream& out, const SemaNode& node,
                   const vector<Pa6Token>& tokens)
{
  const std::map<ETokenType, string>::const_iterator found =
      TokenTypeToStringMap.find(node.op);
  out << (found == TokenTypeToStringMap.end() ? string() : found->second)
      << ':';
  if (node.op == OP_LPAREN)
    return;
  if (node.op == OP_RSHIFT)
    out << ">>";
  else if (node.HasSpan())
    out << tokens[node.first].spelling;
  else
    out << SynthesizedSpelling(node.op);
}

void PrintNode(std::ostream& out, const SemaTree& tree, const SemaModel& model,
               const vector<Pa6Token>& tokens, SemaId id, unsigned depth)
{
  const SemaNode& node = tree.At(id);
  Indent(out, depth);
  out << KindName(node.kind);
  switch (node.kind)
  {
  case SEMA_TRANSLATION_UNIT:
    break;
  case SEMA_NAMESPACE_DEFINITION:
  {
    const Scope& scope = model.ScopeAt(node.scope);
    out << ' ' << (scope.unnamed_namespace ? "<unnamed>" : scope.name);
    break;
  }
  case SEMA_TYPE_ALIAS: case SEMA_VARIABLE: case SEMA_PARAMETER:
    out << ' ' << model.BindingAt(node.binding).name << ' ';
    model.Types().Spell(out, node.type);
    break;
  case SEMA_FUNCTION_DECLARATION: case SEMA_FUNCTION_DEFINITION:
  case SEMA_CALLEE:
    out << ' ';
    PrintFunctionName(out, model, node.function);
    out << ' ';
    model.Types().Spell(out, node.type);
    break;
  case SEMA_ID_EXPRESSION:
    // A synthesized name (the implicit anonymous-union object) has no span
    // and prints the spelling of the binding it denotes.
    out << ' ' << Category(node.category) << ' ';
    model.Types().Spell(out, node.type);
    out << ' ';
    if (node.HasSpan())
      PrintSpan(out, node, tokens);
    else
      out << model.BindingAt(node.binding).name;
    break;
  case SEMA_LITERAL:
    out << ' ' << Category(node.category) << ' ';
    model.Types().Spell(out, node.type);
    out << ' ';
    if (node.binding != 0 &&
        model.BindingAt(node.binding).kind == BINDING_ENUMERATOR)
      out << node.value;
    else if (node.op != KW_AUTO)
      PrintOperator(out, node, tokens);
    else if (node.HasSpan())
      out << tokens[node.first].spelling;
    else if (node.has_value)
      out << node.value;
    break;
  case SEMA_CALL: case SEMA_CONDITIONAL: case SEMA_SUBSCRIPT:
  case SEMA_SIZEOF: case SEMA_BRACED_INIT_LIST:
    out << ' ' << Category(node.category) << ' ';
    model.Types().Spell(out, node.type);
    break;
  case SEMA_UNARY: case SEMA_POSTFIX: case SEMA_BINARY: case SEMA_ASSIGNMENT:
  case SEMA_CAST:
    out << ' ' << Category(node.category) << ' ';
    model.Types().Spell(out, node.type);
    if (node.op != KW_AUTO)
    {
      out << ' ';
      PrintOperator(out, node, tokens);
    }
    break;
  case SEMA_MEMBER:
    // `OP_DOT:x` for an access expression; an injected anonymous-union
    // member is named through its binding.
    out << ' ' << Category(node.category) << ' ';
    model.Types().Spell(out, node.type);
    out << ' ';
    if (node.op != KW_AUTO)
      PrintOperator(out, node, tokens);
    else
      out << model.BindingAt(node.binding).name;
    break;
  case SEMA_CONSTRUCTOR_ACTION:
    if (node.function != 0)
    {
      out << ' ';
      PrintFunctionName(out, model, node.function);
    }
    break;
  case SEMA_MEMBER_INITIALIZER:
    if (node.binding != 0)
      out << ' ' << model.BindingAt(node.binding).name;
    else if (node.function != 0)
    {
      out << ' ';
      PrintFunctionName(out, model, node.function);
    }
    break;
  case SEMA_LABELED_STATEMENT:
  case SEMA_GOTO_STATEMENT:
    if (node.HasSpan())
    {
      out << ' ';
      PrintSpan(out, node, tokens);
    }
    break;
  default:
    break;
  }
  out << '\n';
  for (SemaId child = node.first_child; child != 0;
       child = tree.At(child).next_sibling)
    PrintNode(out, tree, model, tokens, child, depth + 1);
}

} // namespace

void PrintSemanticsUnit(std::ostream& out, size_t unit, const SemaTree& tree,
                        const SemaModel& model,
                        const vector<Pa6Token>& tokens)
{
  out << "start translation unit " << unit << '\n';
  PrintNode(out, tree, model, tokens, tree.Root(), 0);
  out << "end translation unit\n";
}
