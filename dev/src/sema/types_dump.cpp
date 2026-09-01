#include "sema/types_dump.h"

#include <ostream>

namespace
{

const char* ScopeName(ScopeKind kind)
{
  switch (kind)
  {
  case SCOPE_NAMESPACE: return "namespace";
  case SCOPE_CLASS: return "class";
  case SCOPE_FUNCTION: return "function";
  case SCOPE_BLOCK: return "block";
  case SCOPE_ENUM: return "enum";
  case SCOPE_TEMPLATE_PARAMETERS: return "template-parameters";
  }
  return "unknown";
}

void Indent(std::ostream& out, unsigned depth)
{
  for (unsigned i = 0; i < depth; ++i)
    out << "  ";
}

void PrintBinding(std::ostream& out, const SemaModel& model,
                  BindingId id, unsigned depth)
{
  const Binding& binding = model.BindingAt(id);
  if (!binding.print)
    return;
  Indent(out, depth);
  switch (binding.kind)
  {
  case BINDING_TYPE: out << "type "; break;
  case BINDING_TYPE_ALIAS: out << "type-alias "; break;
  case BINDING_VARIABLE: out << "variable "; break;
  case BINDING_FUNCTION: out << "function "; break;
  case BINDING_ENUMERATOR: out << "enumerator "; break;
  case BINDING_PARAMETER: out << "parameter "; break;
  case BINDING_NAMESPACE: return;
  }
  out << binding.name << ' ' << model.Types().Spell(binding.type);
  if (binding.kind == BINDING_ENUMERATOR && binding.has_const_value)
    out << ' ' << binding.const_value;
  out << '\n';
}

void PrintScope(std::ostream& out, const SemaModel& model, ScopeId id,
                unsigned depth)
{
  const Scope& scope = model.ScopeAt(id);
  Indent(out, depth);
  out << "scope " << ScopeName(scope.kind);
  if (scope.kind != SCOPE_BLOCK)
    out << ' ' << scope.name;
  out << '\n';
  for (std::size_t i = 0; i < scope.bindings.size(); ++i)
    PrintBinding(out, model, scope.bindings[i], depth + 1);
  for (std::size_t i = 0; i < scope.children.size(); ++i)
    PrintScope(out, model, scope.children[i], depth + 1);
}

} // namespace

void PrintTypesUnit(std::ostream& out, std::size_t unit,
                    const SemaModel& model)
{
  out << "start translation unit " << unit << '\n';
  out << "translation-unit\n";
  PrintScope(out, model, model.GlobalScope(), 1);
  out << "end translation unit\n";
}
