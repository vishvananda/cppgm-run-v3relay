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

const char* BindingName(BindingKind kind)
{
  switch (kind)
  {
  case BINDING_TYPE: return "type";
  case BINDING_TYPE_ALIAS: return "type-alias";
  case BINDING_VARIABLE: return "variable";
  case BINDING_FUNCTION: return "function";
  case BINDING_ENUMERATOR: return "enumerator";
  case BINDING_PARAMETER: return "parameter";
  case BINDING_NAMESPACE: break;
  }
  return "";
}

void Indent(std::ostream& out, unsigned depth)
{
  for (unsigned i = 0; i < depth; ++i)
    out << "  ";
}

// Namespace names and aliases affect lookup only; they have no dump line.
void PrintBinding(std::ostream& out, const SemaModel& model,
                  BindingId id, unsigned depth)
{
  const Binding& binding = model.BindingAt(id);
  if (binding.kind == BINDING_NAMESPACE)
    return;
  Indent(out, depth);
  out << BindingName(binding.kind) << ' ' << binding.name << ' ';
  model.Types().Spell(out, binding.type);
  if (binding.kind == BINDING_ENUMERATOR)
    out << ' ' << binding.const_value;
  out << '\n';
}

void PrintScope(std::ostream& out, const SemaModel& model, ScopeId id,
                unsigned depth)
{
  const Scope& scope = model.ScopeAt(id);
  Indent(out, depth);
  out << "scope " << ScopeName(scope.kind);
  if (!scope.name.empty())
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
