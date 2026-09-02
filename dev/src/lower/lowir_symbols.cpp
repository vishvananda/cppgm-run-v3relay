#include "lower/lowir_lowering.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace lowir_lowering {

namespace {

std::string Join(const std::vector<std::string>& pieces,
                 const std::string& separator)
{
  std::string result;
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    if (i != 0) result += separator;
    result += pieces[i];
  }
  return result;
}

lowir_model::SymbolBindingMode BindingFor(const FunctionEntity& entity,
                                          bool definition)
{
  if (entity.internal_linkage)
    return lowir_model::SBM_INTERNAL;
  return definition ? lowir_model::SBM_STRONG : lowir_model::SBM_STRONG;
}

}  // namespace

std::string Lowerer::QualifiedFunctionName(FunctionEntityId id) const
{
  const FunctionEntity& entity = model_.FunctionAt(id);
  std::vector<std::string> pieces;
  ScopeId scope = entity.scope;
  while (scope != model_.GlobalScope()) {
    const Scope& value = model_.ScopeAt(scope);
    if (!value.name.empty() && value.name != "<unnamed>" &&
        value.kind == SCOPE_NAMESPACE)
      pieces.push_back(value.name);
    scope = value.parent;
  }
  std::reverse(pieces.begin(), pieces.end());
  pieces.push_back(entity.name);
  return Join(pieces, "::");
}

std::string Lowerer::FunctionBaseName(FunctionEntityId id) const
{
  const std::string qualified = QualifiedFunctionName(id);
  std::string result;
  std::size_t start = 0;
  while (start < qualified.size()) {
    const std::size_t separator = qualified.find("::", start);
    const std::size_t end = separator == std::string::npos ? qualified.size() : separator;
    if (!result.empty()) result += "__";
    result += qualified.substr(start, end - start);
    if (separator == std::string::npos) break;
    start = separator + 2;
  }
  return result;
}

std::string Lowerer::OperatorName(const std::string& name) const
{
  const std::string prefix = "operator";
  if (name.compare(0, prefix.size(), prefix) != 0)
    return std::string();
  const std::string spelling = name.substr(prefix.size());
  if (spelling == "new") return "new";
  if (spelling == "newarray") return "new-array";
  if (spelling == "delete") return "delete";
  if (spelling == "deletearray") return "delete-array";
  if (spelling == "plus") return "plus";
  if (spelling == "minus") return "minus";
  if (spelling == "star") return "multiply";
  if (spelling == "slash") return "divide";
  if (spelling == "percent") return "remainder";
  if (spelling == "amp") return "bit-and";
  if (spelling == "pipe") return "bit-or";
  if (spelling == "caret") return "bit-xor";
  if (spelling == "equal") return "equal";
  if (spelling == "notequal") return "not-equal";
  if (spelling == "lessthan") return "less";
  if (spelling == "greaterthan") return "greater";
  if (spelling == "lessequal") return "less-equal";
  if (spelling == "greaterequal") return "greater-equal";
  if (spelling == "call") return "call";
  if (spelling == "subscript") return "index";
  return std::string();
}

std::string Lowerer::MangleFunction(FunctionEntityId id) const
{
  const FunctionEntity& entity = model_.FunctionAt(id);
  abi_mangle::AbiTargetRecord target;
  target.kind = abi_mangle::ABI_TARGET_FACT_FUNCTION;
  target.c_linkage = entity.c_linkage;
  target.function.qualified_name = QualifiedFunctionName(id);
  target.function.kind = abi_mangle::ABI_FUNCTION_TARGET_PATH;

  const std::string operator_name = OperatorName(entity.name);
  std::vector<abi_mangle::AbiFunctionRecord> records;
  if (!operator_name.empty()) {
    target.function.kind = abi_mangle::ABI_FUNCTION_TARGET_ENCODING;
    abi_mangle::AbiFunctionRecord source;
    source.kind = abi_mangle::ABI_FUNCTION_RECORD_NAME_SOURCE;
    source.source_name = "operator";
    records.push_back(source);
    abi_mangle::AbiFunctionRecord terminal;
    terminal.kind = abi_mangle::ABI_FUNCTION_RECORD_TERMINAL;
    terminal.terminal.kind = abi_mangle::ABI_TERMINAL_OPERATOR;
    if (!abi_mangle::lookup_operator(operator_name,
                                     &terminal.terminal.operator_kind))
      throw std::logic_error("unsupported in CP1: operator ABI spelling");
    records.push_back(terminal);
  }

  const TypeNode& type = types_.At(types_.Unqualified(entity.type));
  for (std::size_t i = 0; i < type.parameters.size(); ++i) {
    if (target.function.kind == abi_mangle::ABI_FUNCTION_TARGET_ENCODING) {
      abi_mangle::AbiFunctionRecord parameter;
      parameter.kind = abi_mangle::ABI_FUNCTION_RECORD_PARAMETER;
      parameter.type = AbiTypeOf(type.parameters[i]);
      records.push_back(parameter);
    } else {
      abi_mangle::AbiFunctionPathOperand parameter;
      parameter.kind = abi_mangle::ABI_FUNCTION_PATH_TYPE;
      parameter.type = AbiTypeOf(type.parameters[i]);
      target.function.path_operands.push_back(parameter);
    }
  }
  if (type.variadic) {
    if (target.function.kind == abi_mangle::ABI_FUNCTION_TARGET_ENCODING) {
      abi_mangle::AbiFunctionRecord variadic;
      variadic.kind = abi_mangle::ABI_FUNCTION_RECORD_VARIADIC;
      records.push_back(variadic);
    } else {
      abi_mangle::AbiFunctionPathOperand variadic;
      variadic.kind = abi_mangle::ABI_FUNCTION_PATH_VARIADIC;
      target.function.path_operands.push_back(variadic);
    }
  }
  abi_mangle::AbiDefinitionTable definitions;
  return abi_mangle::mangle_target(target, records, definitions);
}

std::string Lowerer::FunctionObjectName(FunctionEntityId id) const
{
  const FunctionEntity& entity = model_.FunctionAt(id);
  if (entity.c_linkage)
    return entity.name;
  return MangleFunction(id);
}

void Lowerer::CollectFunctions(SemaId node, std::vector<FunctionUse>& result,
                               std::set<FunctionEntityId>& seen) const
{
  if (node == 0)
    return;
  const SemaNode& value = tree_.At(node);
  if ((value.kind == SEMA_FUNCTION_DEFINITION ||
       value.kind == SEMA_FUNCTION_DECLARATION) && value.function != 0) {
    const bool definition = value.kind == SEMA_FUNCTION_DEFINITION;
    result.push_back(FunctionUse(value.function, node, definition));
    seen.insert(value.function);
  }
  for (SemaId child = value.first_child; child != 0;
       child = tree_.At(child).next_sibling)
    CollectFunctions(child, result, seen);
}

void Lowerer::BuildFunctionNames(const std::vector<FunctionUse>& uses)
{
  std::map<std::string, unsigned> counts;
  std::set<FunctionEntityId> named;
  for (std::size_t i = 0; i < uses.size(); ++i) {
    if (!named.insert(uses[i].id).second)
      continue;
    const std::string base = FunctionBaseName(uses[i].id);
    const unsigned ordinal = ++counts[base];
    function_names_[uses[i].id] = "@" + base +
        (ordinal == 1 ? std::string() : "__ov" + std::to_string(ordinal));
    object_names_[uses[i].id] = FunctionObjectName(uses[i].id);
  }
  for (std::size_t i = 0; i < uses.size(); ++i) {
    if (uses[i].definition)
      definitions_.insert(uses[i].id);
    else
      declarations_.insert(uses[i].id);
  }
}

void Lowerer::BuildDeclarations(const std::vector<FunctionUse>& uses)
{
  std::set<FunctionEntityId> emitted;
  for (std::size_t i = 0; i < uses.size(); ++i) {
    const FunctionEntityId id = uses[i].id;
    if (definitions_.count(id) != 0 || emitted_function_uses_.count(id) == 0 ||
        !emitted.insert(id).second)
      continue;
    program_.function_declarations.push_back(
        BuildFunctionDeclaration(id, uses[i].node));
  }
}

lowir_model::FunctionDeclaration Lowerer::BuildFunctionDeclaration(
    FunctionEntityId id, SemaId node)
{
  lowir_model::FunctionDeclaration result;
  result.name = function_names_[id];
  const FunctionEntity& entity = model_.FunctionAt(id);
  const TypeNode& type = types_.At(types_.Unqualified(entity.type));
  result.return_type = LowTypeOf(type.result);
  result.boundary.arity = type.variadic ? lowir_model::CAM_VARIADIC :
      lowir_model::CAM_FIXED;
  result.metadata.binding = BindingFor(entity, false);
  result.metadata.linkage = entity.c_linkage ? lowir_model::LLM_C :
      lowir_model::LLM_DEFAULT;
  if (entity.internal_linkage)
    result.metadata.binding = lowir_model::SBM_INTERNAL;
  if (!entity.c_linkage)
    result.metadata.object_symbol = object_names_[id];
  std::vector<SemaId> parameters;
  CollectParameters(node, parameters);
  for (std::size_t i = 0; i < type.parameters.size(); ++i) {
    lowir_model::Parameter parameter;
    parameter.name = "%__param" + std::to_string(i);
    parameter.type = LowTypeOf(type.parameters[i]);
    if (i < parameters.size()) {
      const Binding& binding = model_.BindingAt(tree_.At(parameters[i]).binding);
      if (!binding.name.empty()) parameter.name = "%" + binding.name;
      if (types_.Kind(types_.Unqualified(type.parameters[i])) == TYPE_REFERENCE)
        parameter.metadata.passing = lowir_model::PPM_REFERENCE;
    }
    result.params.push_back(parameter);
  }
  return result;
}

}  // namespace lowir_lowering
