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

void Lowerer::CollectGlobalVariables(
    SemaId node, std::vector<SemaId>& result) const
{
  if (node == 0)
    return;
  const SemaNode& value = tree_.At(node);
  if (value.kind == SEMA_FUNCTION_DEFINITION ||
      value.kind == SEMA_FUNCTION_DECLARATION)
    return;
  if (value.kind == SEMA_VARIABLE && value.binding != 0 &&
      model_.ScopeAt(model_.BindingAt(value.binding).scope).kind ==
          SCOPE_NAMESPACE) {
    result.push_back(node);
    return;
  }
  for (SemaId child = value.first_child; child != 0;
       child = tree_.At(child).next_sibling)
    CollectGlobalVariables(child, result);
}

std::string Lowerer::QualifiedGlobalName(BindingId id) const
{
  const Binding& binding = model_.BindingAt(id);
  std::vector<std::string> pieces;
  ScopeId scope = binding.scope;
  while (scope != model_.GlobalScope()) {
    const Scope& value = model_.ScopeAt(scope);
    if (value.kind == SCOPE_NAMESPACE && !value.name.empty() &&
        value.name != "<unnamed>")
      pieces.push_back(value.name);
    scope = value.parent;
  }
  std::reverse(pieces.begin(), pieces.end());
  std::string result = "@";
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    if (i != 0)
      result += "__";
    result += pieces[i];
  }
  if (!pieces.empty())
    result += "__";
  result += binding.name;
  return result;
}

std::string Lowerer::GlobalObjectName(BindingId id) const
{
  const Binding& binding = model_.BindingAt(id);
  if (binding.c_linkage)
    return binding.name;
  std::vector<std::string> pieces;
  ScopeId scope = binding.scope;
  while (scope != model_.GlobalScope()) {
    const Scope& value = model_.ScopeAt(scope);
    if (value.kind == SCOPE_NAMESPACE && !value.name.empty() &&
        value.name != "<unnamed>")
      pieces.push_back(value.name);
    scope = value.parent;
  }
  std::reverse(pieces.begin(), pieces.end());
  std::ostringstream result;
  if (pieces.empty()) {
    result << "_Z" << (binding.internal_linkage ? "L" : "")
           << binding.name.size() << binding.name;
    return result.str();
  }
  result << "_ZN";
  for (std::size_t i = 0; i < pieces.size(); ++i)
    result << pieces[i].size() << pieces[i];
  if (binding.internal_linkage)
    result << "L";
  result << binding.name.size() << binding.name << "E";
  return result.str();
}

void Lowerer::BuildGlobalNames(const std::vector<SemaId>& variables)
{
  for (std::size_t i = 0; i < variables.size(); ++i) {
    const BindingId binding = tree_.At(variables[i]).binding;
    if (global_names_.find(binding) == global_names_.end())
      global_names_[binding] = QualifiedGlobalName(binding);
  }
}

bool Lowerer::GlobalAddress(SemaId node, std::string& symbol,
                            long long& addend)
{
  if (node == 0)
    return false;
  const SemaNode& value = tree_.At(node);
  if (value.kind == SEMA_ID_EXPRESSION) {
    if (value.binding != 0 &&
        model_.BindingAt(value.binding).kind == BINDING_VARIABLE) {
      const std::map<BindingId, std::string>::const_iterator found =
          global_names_.find(value.binding);
      if (found != global_names_.end()) {
        symbol = found->second;
        return true;
      }
    }
    if (value.function != 0) {
      const std::map<FunctionEntityId, std::string>::const_iterator found =
          function_names_.find(value.function);
      if (found != function_names_.end()) {
        symbol = found->second;
        return true;
      }
    }
    return false;
  }
  if (value.kind == SEMA_LITERAL && value.HasSpan() &&
      value.first < tokens_.size() &&
      tokens_[value.first].kind == PA6_LITERAL_TOKEN &&
      !tokens_[value.first].lit_scalar) {
    symbol = RegisterStringLiteral(node, value);
    return true;
  }
  if (value.kind == SEMA_UNARY && value.op == OP_AMP) {
    const std::vector<SemaId> children = Children(node);
    return children.size() == 1 &&
        GlobalAddress(children[0], symbol, addend);
  }
  if (value.kind == SEMA_CAST) {
    const std::vector<SemaId> children = Children(node);
    return children.size() == 1 &&
        GlobalAddress(children[0], symbol, addend);
  }
  if (value.kind == SEMA_BINARY &&
      (value.op == OP_PLUS || value.op == OP_MINUS)) {
    const std::vector<SemaId> children = Children(node);
    if (children.size() != 2 || !tree_.At(children[1]).has_value)
      return false;
    const TypeId left_type = types_.Unqualified(tree_.At(children[0]).type);
    const TypeId right_type = types_.Unqualified(tree_.At(children[1]).type);
    if (types_.Kind(left_type) != TYPE_ARRAY &&
        !types_.IsPointer(left_type))
      return false;
    if (!types_.IsIntegral(right_type) ||
        !GlobalAddress(children[0], symbol, addend))
      return false;
    TypeId element = types_.IsPointer(left_type) ?
        types_.At(left_type).base : types_.At(left_type).base;
    addend += (value.op == OP_PLUS ? 1 : -1) *
        tree_.At(children[1]).value * static_cast<long long>(
            types_.SizeOf(element));
    return true;
  }
  if (value.kind == SEMA_SUBSCRIPT) {
    // The LowIR static-address form names an object or function.  An
    // array-element lvalue still needs the projection instruction so that
    // its address has the same ownership and bounds semantics as a local
    // subscript; materialize it in the init function instead.
    return false;
  }
  return false;
}

lowir_model::GlobalDefinition::DataItem Lowerer::GlobalDataItem(
    SemaId node, TypeId type)
{
  lowir_model::GlobalDefinition::DataItem item;
  std::string symbol;
  long long addend = 0;
  const bool pointer_target = types_.IsPointer(type) ||
      types_.IsNullPointerType(type);
  if (pointer_target && GlobalAddress(node, symbol, addend)) {
    item.kind = lowir_model::GlobalDefinition::DataItem::ITEM_ADDR;
    item.type = LowTypeOf(type);
    item.symbol = symbol;
    item.addr_addend = addend;
    return item;
  }
  if (node != 0 && tree_.At(node).HasSpan() &&
      tree_.At(node).first < tokens_.size() &&
      tokens_[tree_.At(node).first].IsSimple(KW_NULLPTR) &&
      (types_.IsPointer(type) || types_.IsNullPointerType(type))) {
    item.kind = lowir_model::GlobalDefinition::DataItem::ITEM_ZERO;
    item.zero_bytes = types_.SizeOf(type);
    return item;
  }
  if (node != 0 && tree_.At(node).has_value) {
    item.kind = lowir_model::GlobalDefinition::DataItem::ITEM_INTEGER;
    item.type = LowTypeOf(type);
    item.literal_operand.kind = lowir_model::Operand::OP_INTEGER;
    item.literal_operand.int_value = tree_.At(node).value;
    item.literal_operand.text = std::to_string(tree_.At(node).value);
    item.literal_operand.literal_type = item.type;
    return item;
  }
  if (node != 0 && tree_.At(node).kind == SEMA_ID_EXPRESSION &&
      tree_.At(node).binding != 0 &&
      model_.BindingAt(tree_.At(node).binding).has_const_value) {
    item.kind = lowir_model::GlobalDefinition::DataItem::ITEM_INTEGER;
    item.type = LowTypeOf(type);
    item.literal_operand.kind = lowir_model::Operand::OP_INTEGER;
    item.literal_operand.int_value =
        model_.BindingAt(tree_.At(node).binding).const_value;
    item.literal_operand.text = std::to_string(item.literal_operand.int_value);
    item.literal_operand.literal_type = item.type;
    return item;
  }
  item.kind = lowir_model::GlobalDefinition::DataItem::ITEM_ZERO;
  item.zero_bytes = types_.SizeOf(type);
  return item;
}

void Lowerer::BuildGlobalDefinitions(const std::vector<SemaId>& variables)
{
  for (std::size_t i = 0; i < variables.size(); ++i) {
    const SemaNode& variable = tree_.At(variables[i]);
    const Binding& binding = model_.BindingAt(variable.binding);
    const std::string& name = global_names_[variable.binding];
    const lowir_model::LowType low_type = LowTypeOf(binding.type);
    if (binding.extern_declaration) {
      lowir_model::GlobalDeclaration declaration;
      declaration.name = name;
      const TypeId declaration_type = types_.Unqualified(binding.type);
      declaration.has_type = types_.Kind(declaration_type) != TYPE_ARRAY ||
          types_.At(declaration_type).array_bound != 0;
      declaration.type = low_type;
      declaration.metadata.binding = binding.internal_linkage ?
          lowir_model::SBM_INTERNAL : lowir_model::SBM_STRONG;
      declaration.metadata.linkage = binding.c_linkage ?
          lowir_model::LLM_C : lowir_model::LLM_DEFAULT;
      declaration.metadata.object_symbol = GlobalObjectName(variable.binding);
      program_.global_declarations.push_back(declaration);
      continue;
    }

    lowir_model::GlobalDefinition global;
    global.name = name;
    global.metadata.binding = binding.internal_linkage ?
        lowir_model::SBM_INTERNAL : lowir_model::SBM_STRONG;
    global.metadata.linkage = binding.c_linkage ?
        lowir_model::LLM_C : lowir_model::LLM_DEFAULT;
    global.metadata.object_symbol = GlobalObjectName(variable.binding);
    const TypeId object_type = types_.Unqualified(binding.type);
    const std::vector<SemaId> initializer = Children(variables[i]);
    if (types_.Kind(object_type) == TYPE_ARRAY) {
      global.structured = true;
      const TypeId element = types_.At(object_type).base;
      if (initializer.empty() ||
          tree_.At(initializer[0]).kind != SEMA_BRACED_INIT_LIST) {
        lowir_model::GlobalDefinition::DataItem zero;
        zero.kind = lowir_model::GlobalDefinition::DataItem::ITEM_ZERO;
        zero.zero_bytes = types_.SizeOf(object_type);
        global.data_items.push_back(zero);
      } else {
        const std::vector<SemaId> elements = Children(initializer[0]);
        lowir_model::GlobalDefinition::DataItem pending_zero;
        pending_zero.kind = lowir_model::GlobalDefinition::DataItem::ITEM_ZERO;
        for (std::size_t element_index = 0;
             element_index < types_.At(object_type).array_bound;
             ++element_index) {
          const bool missing = element_index >= elements.size();
          lowir_model::GlobalDefinition::DataItem item = missing ?
              pending_zero : GlobalDataItem(elements[element_index], element);
          if (item.kind == lowir_model::GlobalDefinition::DataItem::ITEM_ZERO) {
            pending_zero.zero_bytes += item.zero_bytes != 0 ? item.zero_bytes :
                types_.SizeOf(element);
            continue;
          }
          if (pending_zero.zero_bytes != 0) {
            global.data_items.push_back(pending_zero);
            pending_zero.zero_bytes = 0;
          }
          global.data_items.push_back(item);
        }
        if (pending_zero.zero_bytes != 0)
          global.data_items.push_back(pending_zero);
      }
      program_.globals.push_back(global);
      continue;
    }

    global.type = low_type;
    if (initializer.empty()) {
      global.init_kind = lowir_model::GlobalDefinition::INIT_ZERO;
    } else {
      const lowir_model::GlobalDefinition::DataItem item =
          GlobalDataItem(initializer[0], binding.type);
      if (item.kind == lowir_model::GlobalDefinition::DataItem::ITEM_ADDR) {
        global.init_kind = lowir_model::GlobalDefinition::INIT_ADDR;
        global.init_operand.kind = lowir_model::Operand::OP_GLOBAL;
        global.init_operand.text = item.symbol;
        global.addr_addend = item.addr_addend;
      } else if (item.kind ==
                 lowir_model::GlobalDefinition::DataItem::ITEM_INTEGER) {
        global.init_kind = lowir_model::GlobalDefinition::INIT_INTEGER;
        global.init_operand = item.literal_operand;
      } else {
        global.init_kind = lowir_model::GlobalDefinition::INIT_ZERO;
      }
    }
    program_.globals.push_back(global);
  }
}

void Lowerer::BuildGlobalInitializers(
    const std::vector<SemaId>& variables,
    std::vector<lowir_model::Function>& functions)
{
  std::vector<SemaId> dynamic;
  for (std::size_t i = 0; i < variables.size(); ++i) {
    const SemaNode& variable = tree_.At(variables[i]);
    const Binding& binding = model_.BindingAt(variable.binding);
    if (binding.extern_declaration)
      continue;
    const std::vector<SemaId> initializer = Children(variables[i]);
    if (initializer.empty())
      continue;
    const TypeId object_type = types_.Unqualified(binding.type);
    if (types_.Kind(object_type) == TYPE_ARRAY)
      continue;
    const lowir_model::GlobalDefinition::DataItem item =
        GlobalDataItem(initializer[0], binding.type);
    if (item.kind == lowir_model::GlobalDefinition::DataItem::ITEM_ZERO)
      dynamic.push_back(variables[i]);
  }
  if (dynamic.empty())
    return;

  function_ = lowir_model::Function();
  slots_.clear();
  controls_.clear();
  labels_.clear();
  condition_labels_.clear();
  temp_counter_ = 0;
  label_counter_ = 0;
  generated_slot_counter_ = 0;
  function_.name = "@__cppgm_init";
  function_.return_type.text = "void";
  function_.metadata.role = lowir_model::SR_INIT;
  function_.metadata.binding = lowir_model::SBM_INTERNAL;
  AddBlock("^entry");
  current_label_ = "^entry";

  for (std::size_t i = 0; i < dynamic.size(); ++i) {
    const SemaNode& variable = tree_.At(dynamic[i]);
    const Binding& binding = model_.BindingAt(variable.binding);
    const std::vector<SemaId> initializer = Children(dynamic[i]);
    Value value = LowerRValue(initializer[0], binding.type);
    lowir_model::Instruction store;
    store.kind = lowir_model::Instruction::IK_STORE;
    store.type = LowTypeOf(binding.type);
    store.first = value.operand;
    store.second.kind = lowir_model::Operand::OP_GLOBAL;
    store.second.text = global_names_[variable.binding];
    Emit(store);
  }
  EmitReturn(0);
  functions.push_back(function_);
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
    parameter.name = "%arg" + std::to_string(i);
    parameter.type = LowTypeOf(type.parameters[i]);
    if (types_.Kind(types_.Unqualified(type.parameters[i])) == TYPE_REFERENCE)
      parameter.metadata.passing = lowir_model::PPM_REFERENCE;
    if (i < parameters.size()) {
      const Binding& binding = model_.BindingAt(tree_.At(parameters[i]).binding);
      if (!binding.name.empty()) parameter.name = "%" + binding.name;
    }
    result.params.push_back(parameter);
  }
  return result;
}

}  // namespace lowir_lowering
