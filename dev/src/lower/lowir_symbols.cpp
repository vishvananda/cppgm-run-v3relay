// Symbol collection and naming, ABI object names, global data, dynamic
// initialization, and on-demand declarations.
#include "lower/lowir_lowering.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <stdexcept>

namespace lowir_lowering {

namespace {

std::string Join(const std::vector<std::string>& pieces,
                 const std::string& separator, const std::string& last)
{
  std::string result;
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    result += pieces[i];
    result += separator;
  }
  return result + last;
}

std::string LowirNamePiece(const std::string& source)
{
  std::string result;
  result.reserve(source.size());
  for (std::size_t i = 0; i < source.size(); ++i) {
    const unsigned char character =
        static_cast<unsigned char>(source[i]);
    result += (std::isalnum(character) || source[i] == '_') ?
        static_cast<char>(character) : '_';
  }
  return result;
}

// The semantic model spells an operator function `operator<token>`; the
// PA14 encoder classifies operators by their README word.  This is the only
// place that bridges the two fixed vocabularies.
const char* OperatorWord(const std::string& spelling)
{
  static const struct { const char* token; const char* word; } table[] = {
    { "new", "new" }, { "new[]", "new-array" }, { "delete", "delete" },
    { "delete[]", "delete-array" }, { "+", "plus" },
    { "-", "minus" }, { "*", "multiply" }, { "/", "divide" },
    { "%", "remainder" }, { "&", "bit-and" }, { "|", "bit-or" },
    { "^", "bit-xor" }, { "=", "assign" }, { "+=", "plus-assign" },
    { "-=", "minus-assign" }, { "*=", "multiply-assign" },
    { "/=", "divide-assign" }, { "%=", "remainder-assign" },
    { "&=", "bit-and-assign" }, { "|=", "bit-or-assign" },
    { "^=", "bit-xor-assign" }, { "<<", "shift-left" },
    { ">>", "shift-right" }, { "<<=", "shift-left-assign" },
    { ">>=", "shift-right-assign" }, { "==", "equal" },
    { "!=", "not-equal" }, { "<", "less" }, { ">", "greater" },
    { "<=", "less-equal" }, { ">=", "greater-equal" },
    { "&&", "logical-and" }, { "||", "logical-or" },
    { "!", "logical-not" }, { "~", "complement" },
    { "++", "increment" }, { "--", "decrement" }, { ",", "comma" },
    { "->*", "member-pointer" }, { "->", "arrow" },
    { "()", "call" }, { "[]", "index" }
  };
  for (std::size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i)
    if (spelling == table[i].token)
      return table[i].word;
  return 0;
}

}  // namespace

// One walk over the unit in source order records every function entity the
// unit declares and every namespace-scope object.  Repeated declarations of
// one function share its entity; repeated declarations of one object share
// the first binding (Binding::redeclared_binding), so each symbol is created
// once and its definition, if any, is remembered.
void Lowerer::CollectTemporaryConstructorUses(SemaId node)
{
  if (node == 0)
    return;
  const SemaNode& value = tree_.At(node);
  if (value.kind == SEMA_CONSTRUCTOR_ACTION && value.binding == 0 &&
      value.function != 0)
    temporary_constructors_.insert(value.function);
  for (SemaId child = value.first_child; child != 0;
       child = tree_.At(child).next_sibling)
    CollectTemporaryConstructorUses(child);
}

void Lowerer::CollectSymbols(SemaId node)
{
  if (node == 0)
    return;
  const SemaNode& value = tree_.At(node);
  if (value.kind == SEMA_FUNCTION_DEFINITION ||
      value.kind == SEMA_FUNCTION_DECLARATION) {
    if (value.function != 0) {
      const FunctionEntity& entity = model_.FunctionAt(value.function);
      if (entity.special_member == SPECIAL_MEMBER_CONSTRUCTOR &&
          entity.member_class != 0 &&
          model_.ClassAt(entity.member_class).trivial_default_constructor &&
          temporary_constructors_.count(value.function) == 0)
        return;
      FunctionSymbol& symbol = functions_[value.function];
      if (symbol.declaration == 0) {
        symbol.declaration = node;
        function_order_.push_back(value.function);
      }
      if (value.kind == SEMA_FUNCTION_DEFINITION && symbol.definition == 0)
        symbol.definition = node;
    }
  } else if (value.kind == SEMA_VARIABLE && value.binding != 0) {
    const Binding& binding = model_.BindingAt(value.binding);
    const Scope& declaration_scope = model_.ScopeAt(binding.scope);
    if (declaration_scope.kind == SCOPE_NAMESPACE ||
        (declaration_scope.kind == SCOPE_CLASS && binding.static_member)) {
      const BindingId canonical = CanonicalBinding(value.binding);
      GlobalSymbol& symbol = globals_[canonical];
      if (symbol.binding == 0) {
        symbol.binding = canonical;
        global_order_.push_back(canonical);
      }
      symbol.internal_linkage = symbol.internal_linkage ||
          binding.internal_linkage;
      symbol.c_linkage = symbol.c_linkage || binding.c_linkage;
      symbol.thread_local_storage = symbol.thread_local_storage ||
          binding.thread_local_storage;
      if (!binding.extern_declaration && symbol.definition == 0)
        symbol.definition = node;
    }
    return;
  }
  for (SemaId child = value.first_child; child != 0;
       child = tree_.At(child).next_sibling)
    CollectSymbols(child);
}

void Lowerer::CollectReferencedFunctions(
    SemaId node, std::set<FunctionEntityId>& result) const
{
  if (node == 0)
    return;
  const SemaNode& value = tree_.At(node);
  if (value.kind == SEMA_FUNCTION_DEFINITION ||
      value.kind == SEMA_FUNCTION_DECLARATION) {
    for (SemaId child = value.first_child; child != 0;
         child = tree_.At(child).next_sibling)
      CollectReferencedFunctions(child, result);
    return;
  }
  if ((value.kind == SEMA_CALLEE || value.kind == SEMA_ID_EXPRESSION) &&
      value.function != 0)
    result.insert(value.function);
  for (SemaId child = value.first_child; child != 0;
       child = tree_.At(child).next_sibling)
    CollectReferencedFunctions(child, result);
}

void Lowerer::ComputeReferencedFunctions()
{
  referenced_functions_.clear();
  std::vector<FunctionEntityId> pending;
  for (std::size_t i = 0; i < function_order_.size(); ++i) {
    const FunctionEntity& entity = model_.FunctionAt(function_order_[i]);
    const FunctionSymbol& symbol = functions_[function_order_[i]];
    // Non-inline definitions are emitted independently of use.  Their
    // callees, and only their transitive callees, keep declarations alive.
    if (symbol.definition != 0 && !entity.in_class_definition)
      pending.push_back(function_order_[i]);
  }

  // Global initializers are roots too.  Do not descend into function
  // definitions here: an unreferenced inline body must not retain an
  // external declaration used only by that body.
  std::function<void(SemaId)> collect_globals =
      [&](SemaId node) {
        if (node == 0)
          return;
        const SemaNode& value = tree_.At(node);
        if (value.kind == SEMA_FUNCTION_DEFINITION ||
            value.kind == SEMA_FUNCTION_DECLARATION)
          return;
        if ((value.kind == SEMA_CALLEE ||
             value.kind == SEMA_ID_EXPRESSION) && value.function != 0)
          referenced_functions_.insert(value.function);
        for (SemaId child = value.first_child; child != 0;
             child = tree_.At(child).next_sibling)
          collect_globals(child);
      };
  collect_globals(tree_.Root());

  std::set<FunctionEntityId> visited;
  while (!pending.empty()) {
    const FunctionEntityId function = pending.back();
    pending.pop_back();
    if (!visited.insert(function).second)
      continue;
    const std::map<FunctionEntityId, FunctionSymbol>::const_iterator found =
        functions_.find(function);
    if (found == functions_.end() || found->second.definition == 0)
      continue;
    std::set<FunctionEntityId> references;
    CollectReferencedFunctions(found->second.definition, references);
    for (std::set<FunctionEntityId>::const_iterator reference =
             references.begin(); reference != references.end(); ++reference) {
      referenced_functions_.insert(*reference);
      const std::map<FunctionEntityId, FunctionSymbol>::const_iterator
          callee = functions_.find(*reference);
      if (callee != functions_.end() && callee->second.definition != 0 &&
          model_.FunctionAt(*reference).in_class_definition)
        pending.push_back(*reference);
    }
  }
}

BindingId Lowerer::CanonicalBinding(BindingId id) const
{
  const BindingId first = model_.BindingAt(id).redeclared_binding;
  return first != 0 ? first : id;
}

// Every use of an object symbol passes through here, so a declaration-only
// constant learns whether the unit ever needs its storage.
const Lowerer::GlobalSymbol* Lowerer::GlobalFor(BindingId id)
{
  const std::map<BindingId, GlobalSymbol>::iterator found =
      globals_.find(CanonicalBinding(id));
  if (found == globals_.end())
    return 0;
  found->second.referenced = true;
  return &found->second;
}

// LowIR names are assigned in first-declaration order: functions, then
// objects.  The LowIR spelling joins the namespace components with `__`;
// the object name comes from the PA14 encoder, which takes the components
// joined with `::` at its API boundary.
void Lowerer::NameSymbols()
{
  // Overload suffixes are declaration-order facts and entity ids are
  // assigned in that order; deferred class members reach the tree after
  // the unit's other functions, so name from a sorted copy and emit in the
  // semantic order retained by function_order_.
  std::vector<FunctionEntityId> naming_order = function_order_;
  std::sort(naming_order.begin(), naming_order.end());
  for (std::size_t i = 0; i < naming_order.size(); ++i) {
    FunctionSymbol& symbol = functions_[naming_order[i]];
    const FunctionEntity& entity = model_.FunctionAt(naming_order[i]);
    symbol.object = FunctionObjectName(naming_order[i], false);
    const bool destructor = model_.FunctionAt(naming_order[i]).special_member ==
        SPECIAL_MEMBER_DESTRUCTOR;
    const std::string destructor_name = destructor ?
        model_.ScopeAt(entity.scope).name : entity.name;
    const std::string member_name = destructor ?
        Join(NamespacePieces(entity.scope), "__", "_" + destructor_name) :
        Join(NamespacePieces(entity.scope), "__",
             LowirNamePiece(entity.name));
    symbol.name = TopLevelName(
        "@" + member_name,
        entity.internal_linkage ? std::string() : symbol.object);
    if (entity.special_member != SPECIAL_MEMBER_NONE) {
      symbol.base_object = FunctionObjectName(naming_order[i], true);
      symbol.base_name = TopLevelName(
          "@" + member_name + "__base_entry",
          entity.internal_linkage ? std::string() : symbol.base_object);
    }
  }
  for (std::size_t i = 0; i < global_order_.size(); ++i) {
    GlobalSymbol& symbol = globals_[global_order_[i]];
    const Binding& binding = model_.BindingAt(global_order_[i]);
    symbol.object = GlobalObjectName(symbol);
    symbol.name = TopLevelName(
        "@" + Join(NamespacePieces(binding.scope), "__", binding.name),
        symbol.internal_linkage ? std::string() : symbol.object);
  }
}

// An external symbol keeps one LowIR name across every unit that declares
// it, keyed by its object name.  Every other top-level name is unique in the
// program: the second entity rendering to one base name (an overload, two
// scopes whose `__` joins coincide, or a unit-local symbol of another unit)
// gets an `__ovN` suffix.
std::string Lowerer::TopLevelName(const std::string& base,
                                  const std::string& external_object)
{
  if (!external_object.empty()) {
    const std::map<std::string, std::string>::const_iterator known =
        shared_.external_names_.find(external_object);
    if (known != shared_.external_names_.end())
      return known->second;
  }
  unsigned& count = shared_.top_level_names_[base];
  std::string name = base;
  if (++count != 1) {
    while (true) {
      name = base + "__ov" + std::to_string(count);
      if (shared_.top_level_names_.insert(std::make_pair(name, 1u)).second)
        break;
      ++count;
    }
  }
  if (!external_object.empty())
    shared_.external_names_[external_object] = name;
  return name;
}

std::vector<std::string> Lowerer::NamespacePieces(ScopeId scope) const
{
  std::vector<std::string> pieces;
  while (scope != model_.GlobalScope()) {
    const Scope& value = model_.ScopeAt(scope);
    if ((value.kind == SCOPE_NAMESPACE || value.kind == SCOPE_CLASS) &&
        !value.name.empty() && value.name != "<unnamed>")
      pieces.push_back(value.name);
    scope = value.parent;
  }
  std::reverse(pieces.begin(), pieces.end());
  return pieces;
}

namespace {

bool OperatorTerminal(const FunctionEntity& entity, bool unary,
                      abi_mangle::AbiOperatorKind& kind)
{
  const std::string& name = entity.name;
  const std::string prefix = "operator";
  if (name.compare(0, prefix.size(), prefix) != 0)
    return false;
  std::string spelling = name.substr(prefix.size());
  const char* word = OperatorWord(spelling);
  if (spelling == "*")
    word = unary ? "deref" : "multiply";
  else if (spelling == "&")
    word = unary ? "address-of" : "bit-and";
  if (word == 0 || !abi_mangle::lookup_operator(word, &kind))
    throw std::logic_error("LowIR lowering does not support the ABI spelling "
                           "of " + name);
  return true;
}

}  // namespace

std::string Lowerer::MangleFunction(FunctionEntityId id) const
{
  return MangleFunction(id, false);
}

std::string Lowerer::MangleFunction(FunctionEntityId id,
                                    bool base_variant) const
{
  const FunctionEntity& entity = model_.FunctionAt(id);
  abi_mangle::AbiTargetRecord target;
  target.kind = abi_mangle::ABI_TARGET_FACT_FUNCTION;
  target.c_linkage = entity.c_linkage;
  const bool special = entity.special_member != SPECIAL_MEMBER_NONE;
  const bool literal_operator = entity.name.compare(
      0, std::string("operator\"\"_").size(), "operator\"\"_") == 0;
  target.function.qualified_name = special ?
      Join(NamespacePieces(entity.scope), "::", "operator") :
      Join(NamespacePieces(entity.scope), "::",
           literal_operator ? "operator" : entity.name);
  target.function.kind = abi_mangle::ABI_FUNCTION_TARGET_PATH;

  std::vector<abi_mangle::AbiFunctionRecord> records;
  abi_mangle::AbiOperatorKind operator_kind;
  const TypeId signature = entity.is_member && entity.member_type != 0 ?
      entity.member_type : entity.type;
  const TypeNode& signature_type = types_.At(types_.Unqualified(signature));
  const std::size_t operand_count = signature_type.parameters.size() +
      (entity.is_member && !entity.static_member ? 1 : 0);
  if (literal_operator) {
    abi_mangle::AbiFunctionRecord terminal;
    terminal.kind = abi_mangle::ABI_FUNCTION_RECORD_TERMINAL;
    terminal.terminal.kind = abi_mangle::ABI_TERMINAL_LITERAL_OPERATOR;
    terminal.terminal.name = entity.name.substr(
        std::string("operator\"\"").size());
    records.push_back(terminal);
  } else if (OperatorTerminal(entity, operand_count == 1, operator_kind)) {
    abi_mangle::AbiFunctionRecord terminal;
    terminal.kind = abi_mangle::ABI_FUNCTION_RECORD_TERMINAL;
    terminal.terminal.kind = abi_mangle::ABI_TERMINAL_OPERATOR;
    terminal.terminal.operator_kind = operator_kind;
    records.push_back(terminal);
  }
  const bool encoding =
      target.function.kind == abi_mangle::ABI_FUNCTION_TARGET_ENCODING;
  if (special) {
    abi_mangle::AbiFunctionRecord terminal;
    terminal.kind = abi_mangle::ABI_FUNCTION_RECORD_TERMINAL;
    terminal.terminal.kind = abi_mangle::ABI_TERMINAL_SPECIAL;
    if (entity.special_member == SPECIAL_MEMBER_CONSTRUCTOR)
      terminal.terminal.special_function = base_variant ?
          abi_mangle::ABI_SPECIAL_CONSTRUCTOR_BASE :
          abi_mangle::ABI_SPECIAL_CONSTRUCTOR_COMPLETE;
    else
      terminal.terminal.special_function = base_variant ?
          abi_mangle::ABI_SPECIAL_DESTRUCTOR_BASE :
          abi_mangle::ABI_SPECIAL_DESTRUCTOR_COMPLETE;
    records.push_back(terminal);
  }
  const TypeNode& type = types_.At(types_.Unqualified(signature));
  if (entity.is_member && entity.member_const)
  {
    abi_mangle::AbiFunctionRecord qualifier;
    qualifier.kind = abi_mangle::ABI_FUNCTION_RECORD_QUALIFIER;
    qualifier.qualifiers.push_back(abi_mangle::ABI_FUNCTION_QUALIFIER_CONST);
    records.push_back(qualifier);
  }
  if (entity.is_member && entity.member_volatile)
  {
    abi_mangle::AbiFunctionRecord qualifier;
    qualifier.kind = abi_mangle::ABI_FUNCTION_RECORD_QUALIFIER;
    qualifier.qualifiers.push_back(abi_mangle::ABI_FUNCTION_QUALIFIER_VOLATILE);
    records.push_back(qualifier);
  }
  for (std::size_t i = 0; i < type.parameters.size(); ++i) {
    if (encoding) {
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
    if (encoding) {
      abi_mangle::AbiFunctionRecord variadic;
      variadic.kind = abi_mangle::ABI_FUNCTION_RECORD_VARIADIC;
      records.push_back(variadic);
    } else {
      abi_mangle::AbiFunctionPathOperand variadic;
      variadic.kind = abi_mangle::ABI_FUNCTION_PATH_VARIADIC;
      target.function.path_operands.push_back(variadic);
    }
  }
  const abi_mangle::AbiDefinitionTable definitions;
  return abi_mangle::mangle_target(target, records, definitions);
}

std::string Lowerer::FunctionObjectName(FunctionEntityId id) const
{
  return FunctionObjectName(id, false);
}

std::string Lowerer::FunctionObjectName(FunctionEntityId id,
                                        bool base_variant) const
{
  const FunctionEntity& entity = model_.FunctionAt(id);
  if (entity.c_linkage)
    return entity.name;
  return MangleFunction(id, base_variant);
}

// Object names come from the same PA14 encoder as function names; the
// variable target carries the internal-linkage fact (`_ZL…`, `_ZN…L…E`).
std::string Lowerer::GlobalObjectName(const GlobalSymbol& symbol) const
{
  const Binding& binding = model_.BindingAt(symbol.binding);
  if (symbol.c_linkage)
    return binding.name;
  abi_mangle::AbiTargetRecord target;
  target.kind = abi_mangle::ABI_TARGET_FACT_VARIABLE;
  target.internal_linkage = symbol.internal_linkage;
  target.qualified_name =
      Join(NamespacePieces(binding.scope), "::", binding.name);
  const std::vector<abi_mangle::AbiFunctionRecord> records;
  const abi_mangle::AbiDefinitionTable definitions;
  return abi_mangle::mangle_target(target, records, definitions);
}

std::string Lowerer::ThreadLocalWrapperObjectName(
    const GlobalSymbol& symbol) const
{
  const Binding& binding = model_.BindingAt(symbol.binding);
  if (symbol.internal_linkage || binding.c_linkage)
    return std::string();
  abi_mangle::AbiTargetRecord target;
  target.kind = abi_mangle::ABI_TARGET_FACT_THREAD_LOCAL_WRAPPER;
  target.qualified_name =
      Join(NamespacePieces(binding.scope), "::", binding.name);
  const std::vector<abi_mangle::AbiFunctionRecord> records;
  const abi_mangle::AbiDefinitionTable definitions;
  return abi_mangle::mangle_target(target, records, definitions);
}

void Lowerer::AddThreadLocalWrapperDeclaration(
    const std::string& target, const std::string& object,
    bool internal_linkage)
{
  if (target.empty() || target[0] != '@')
    Unsupported("a thread-local wrapper without a target symbol");
  const std::string base = "@__cppgm_tls_wrapper__" + target.substr(1);
  const std::string name = TopLevelName(base, object);
  lowir_model::FunctionDeclaration declaration;
  declaration.name = name;
  declaration.return_type = PtrType();
  declaration.metadata.binding = internal_linkage ?
      lowir_model::SBM_INTERNAL : lowir_model::SBM_STRONG;
  if (!object.empty())
    declaration.metadata.object_symbol = object;
  declaration.metadata.tls_for_symbol = target;
  program_.function_declarations.push_back(declaration);
}

void Lowerer::AddThreadLocalInitializer(const GlobalSymbol& symbol,
                                        SemaId expression, TypeId type,
                                        bool constructor_action)
{
  if (expression == 0)
    Unsupported("a thread-local initializer without an expression");
  const std::string suffix = symbol.name.substr(1);
  const std::string guard = TopLevelName(
      "@__cppgm_tls_guard__" + suffix, std::string());
  const std::string function = TopLevelName(
      "@__cppgm_tls_init__" + suffix, std::string());
  AddThreadLocalWrapperDeclaration(guard, std::string(), true);

  lowir_model::GlobalDefinition guard_global;
  guard_global.name = guard;
  guard_global.type = I64Type();
  guard_global.storage = lowir_model::GSM_THREAD_LOCAL;
  guard_global.init_kind = lowir_model::GlobalDefinition::INIT_ZERO;
  guard_global.metadata.binding = lowir_model::SBM_INTERNAL;
  program_.globals.push_back(guard_global);
  thread_local_initializers_.push_back(ThreadLocalInitializer(
      function, symbol.name, guard, expression, type, constructor_action));
}

void Lowerer::BuildGlobalArrayDefinition(
    const GlobalSymbol& symbol, const Binding& binding,
    const std::vector<SemaId>& initializer,
    lowir_model::GlobalDefinition& global)
{
  typedef lowir_model::GlobalDefinition::DataItem DataItem;
  global.structured = true;
  const TypeId object_type = types_.Unqualified(binding.type);
  const TypeId element = types_.At(object_type).base;
  const std::size_t element_size = types_.SizeOf(element);
  const std::size_t bound = types_.At(object_type).array_bound;
  std::vector<SemaId> elements;
  if (!initializer.empty() &&
      tree_.At(initializer[0]).kind == SEMA_BRACED_INIT_LIST)
    elements = Children(initializer[0]);

  if (TryBuildRuntimeClassAggregate(symbol, binding, element, elements,
                                    global))
    return;
  // Consecutive zero elements, whether written, missing, or awaiting a
  // startup store, merge into one zero run.
  std::size_t pending_zero = 0;
  for (std::size_t index = 0; index < elements.size() && index < bound;
       ++index) {
    const TypeId element_unqualified = types_.Unqualified(element);
    if (types_.Kind(element_unqualified) == TYPE_CLASS &&
        tree_.At(elements[index]).kind == SEMA_BRACED_INIT_LIST) {
      if (pending_zero != 0) {
        DataItem zero;
        zero.kind = DataItem::ITEM_ZERO;
        zero.zero_bytes = pending_zero;
        global.data_items.push_back(zero);
        pending_zero = 0;
      }
      const ClassEntity& class_entity = model_.ClassAt(
          types_.At(element_unqualified).entity);
      const std::vector<SemaId> values = Children(elements[index]);
      std::size_t value_index = 0;
      std::size_t cursor = 0;
      for (std::size_t field_index = 0;
           field_index < class_entity.fields.size(); ++field_index) {
        const ClassField& field = class_entity.fields[field_index];
        if (field.static_member || field.binding == 0)
          continue;
        if (field.offset > cursor) {
          DataItem zero;
          zero.kind = DataItem::ITEM_ZERO;
          zero.zero_bytes = field.offset - cursor;
          global.data_items.push_back(zero);
        }
        const TypeId value_type = field.type;
        DataItem field_item;
        if (value_index < values.size() &&
            ConstantGlobalItem(values[value_index], value_type,
                               field_item)) {
          global.data_items.push_back(field_item);
        } else if (value_index < values.size()) {
          DynamicInitializer dynamic;
          dynamic.expression = values[value_index];
          dynamic.symbol = symbol.name;
          dynamic.type = value_type;
          dynamic.aggregate_type = binding.type;
          dynamic.aggregate_path.push_back(index);
          dynamic.aggregate_path.push_back(field_index);
          dynamic.aggregate_subobject = true;
          dynamic_initializers_.push_back(dynamic);
          DataItem zero;
          zero.kind = DataItem::ITEM_ZERO;
          zero.zero_bytes = types_.SizeOf(value_type);
          global.data_items.push_back(zero);
        } else {
          DataItem zero;
          zero.kind = DataItem::ITEM_ZERO;
          zero.zero_bytes = types_.SizeOf(value_type);
          global.data_items.push_back(zero);
        }
        ++value_index;
        cursor = field.offset + types_.SizeOf(value_type);
      }
      if (cursor < element_size) {
        DataItem zero;
        zero.kind = DataItem::ITEM_ZERO;
        zero.zero_bytes = element_size - cursor;
        global.data_items.push_back(zero);
      }
      continue;
    }
    if (types_.Kind(element_unqualified) == TYPE_CLASS &&
        tree_.At(elements[index]).kind == SEMA_CONSTRUCTOR_ACTION) {
      std::vector<DataItem> folded;
      if (FoldConstructorAction(elements[index], element_unqualified,
                                folded)) {
        if (pending_zero != 0) {
          DataItem zero;
          zero.kind = DataItem::ITEM_ZERO;
          zero.zero_bytes = pending_zero;
          global.data_items.push_back(zero);
          pending_zero = 0;
        }
        global.data_items.insert(global.data_items.end(),
                                 folded.begin(), folded.end());
        continue;
      }
      DynamicInitializer dynamic;
      dynamic.expression = elements[index];
      dynamic.symbol = symbol.name;
      dynamic.byte_offset = index * element_size;
      dynamic.element_index = index;
      dynamic.type = element;
      dynamic.constructor_action = true;
      dynamic_initializers_.push_back(dynamic);
      pending_zero += element_size;
      continue;
    }
    DataItem item;
    if (!ConstantGlobalItem(elements[index], element, item)) {
      DynamicInitializer dynamic;
      dynamic.expression = elements[index];
      dynamic.symbol = symbol.name;
      dynamic.byte_offset = index * element_size;
      dynamic.type = element;
      dynamic_initializers_.push_back(dynamic);
      pending_zero += element_size;
      continue;
    }
    if (item.kind == DataItem::ITEM_ZERO) {
      pending_zero += element_size;
      continue;
    }
    if (pending_zero != 0) {
      DataItem zero;
      zero.kind = DataItem::ITEM_ZERO;
      zero.zero_bytes = pending_zero;
      global.data_items.push_back(zero);
      pending_zero = 0;
    }
    global.data_items.push_back(item);
  }
  if (bound > elements.size())
    pending_zero += (bound - elements.size()) * element_size;
  if (pending_zero != 0 || global.data_items.empty()) {
    DataItem zero;
    zero.kind = DataItem::ITEM_ZERO;
    zero.zero_bytes = pending_zero;
    global.data_items.push_back(zero);
  }
}

// Every reference to a function symbol passes through here, so a function
// that is only declared receives its `declare function` exactly when the
// program names it, whether by call, address, or initializer.
const std::string& Lowerer::FunctionSymbolName(FunctionEntityId id)
{
  const std::map<FunctionEntityId, FunctionSymbol>::iterator found =
      functions_.find(id);
  if (found == functions_.end())
    Unsupported("a function the unit never declares");
  found->second.referenced = true;
  return found->second.name;
}

const std::string& Lowerer::FunctionBaseSymbolName(FunctionEntityId id)
{
  const std::map<FunctionEntityId, FunctionSymbol>::iterator found =
      functions_.find(id);
  if (found == functions_.end() || found->second.base_name.empty())
    Unsupported("a base constructor or destructor without a symbol");
  found->second.base_required = true;
  return found->second.base_name;
}

// Static address forms an initializer may take: an object or function
// name (possibly through `&`, casts, or array/function decay), a string
// literal, and a pointer or array plus a constant element offset.
bool Lowerer::GlobalAddress(SemaId node, std::string& symbol,
                            long long& addend)
{
  if (node == 0)
    return false;
  const SemaNode& value = tree_.At(node);
  if (value.kind == SEMA_ID_EXPRESSION) {
    if (value.binding != 0 &&
        model_.BindingAt(value.binding).kind == BINDING_VARIABLE) {
      const GlobalSymbol* global = GlobalFor(value.binding);
      if (global == 0)
        return false;
      symbol = global->name;
      return true;
    }
    if (value.function != 0) {
      symbol = FunctionSymbolName(value.function);
      return true;
    }
    return false;
  }
  if (value.kind == SEMA_MEMBER && value.binding != 0 &&
      model_.BindingAt(value.binding).static_member) {
    const GlobalSymbol* global = GlobalFor(value.binding);
    if (global == 0)
      return false;
    symbol = global->name;
    return true;
  }
  if (value.kind == SEMA_LITERAL && value.HasSpan() &&
      value.first < tokens_.size() &&
      tokens_[value.first].kind == PA6_LITERAL_TOKEN &&
      !tokens_[value.first].lit_scalar) {
    symbol = RegisterStringLiteral(node, value);
    return true;
  }
  if ((value.kind == SEMA_UNARY && value.op == OP_AMP) ||
      value.kind == SEMA_CAST) {
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
    const TypeId element = types_.At(left_type).base;
    addend += (value.op == OP_PLUS ? 1 : -1) *
        tree_.At(children[1]).value *
        static_cast<long long>(types_.SizeOf(element));
    return true;
  }
  // A subscript lvalue keeps its projection instruction so its address has
  // the same ownership and bounds semantics as a local subscript; it is
  // materialized by the initialization function instead.
  return false;
}

// A translation-time constant initializer item: an address, an integral
// constant (a folded expression or a const object's value), or the null
// pointer.  Anything else is initialized at startup.
bool Lowerer::ConstantGlobalItem(SemaId node, TypeId type,
                                 lowir_model::GlobalDefinition::DataItem& item)
{
  typedef lowir_model::GlobalDefinition::DataItem DataItem;
  if (node == 0)
    return false;
  std::string symbol;
  long long addend = 0;
  const bool pointer_target = types_.IsPointer(type) ||
      types_.IsNullPointerType(type);
  if (pointer_target && GlobalAddress(node, symbol, addend)) {
    item.kind = DataItem::ITEM_ADDR;
    item.type = LowTypeOf(type);
    item.symbol = symbol;
    item.addr_addend = addend;
    return true;
  }
  if (pointer_target && IsNullptrLiteral(node)) {
    item.kind = DataItem::ITEM_ZERO;
    item.zero_bytes = types_.SizeOf(type);
    return true;
  }
  const SemaNode& value = tree_.At(node);
  long long constant = 0;
  if (value.has_value)
    constant = value.value;
  else if (value.kind == SEMA_ID_EXPRESSION && value.binding != 0 &&
           model_.BindingAt(value.binding).has_const_value)
    constant = model_.BindingAt(value.binding).const_value;
  else
    return false;
  if (pointer_target) {
    // 4.10: an integral constant zero is a null pointer constant; any other
    // integer becomes a pointer only through a startup conversion.
    if (constant != 0)
      return false;
    item.kind = DataItem::ITEM_ZERO;
    item.zero_bytes = types_.SizeOf(type);
    return true;
  }
  item.kind = DataItem::ITEM_INTEGER;
  item.type = LowTypeOf(type);
  item.literal_operand = Immediate(constant);
  item.literal_operand.literal_type = item.type;
  return true;
}

// A braced element of a non-aggregate class array names a constructor.
// When that constructor's body is empty and each mem-initializer stores one
// parameter or constant into a field, the element is constant data, which
// is how the fixtures present it; anything else constructs the element at
// startup.  The folded constructor is still odr-used by the element.
bool Lowerer::FoldConstructorAction(
    SemaId action, TypeId class_type,
    std::vector<lowir_model::GlobalDefinition::DataItem>& items)
{
  typedef lowir_model::GlobalDefinition::DataItem DataItem;
  const FunctionEntityId function = tree_.At(action).function;
  const std::vector<SemaId> action_children = Children(action);
  if (function == 0 || action_children.size() != 1 ||
      tree_.At(action_children[0]).kind != SEMA_CALL)
    return false;
  const std::vector<SemaId> call_children = Children(action_children[0]);
  const FunctionEntity& entity = model_.FunctionAt(function);
  const ClassEntity& owner = model_.ClassAt(entity.member_class);
  const std::map<FunctionEntityId, FunctionSymbol>::const_iterator found =
      functions_.find(function);
  if (found == functions_.end() || found->second.definition == 0 ||
      !owner.bases.empty() || owner.is_union ||
      !entity.default_member_initializers.empty())
    return false;
  const SemaId definition = found->second.definition;
  const SemaId body = FunctionBody(definition);
  if (body == 0 || tree_.At(body).first_child != 0)
    return false;
  std::vector<SemaId> parameters;
  CollectParameters(definition, parameters);
  std::map<BindingId, std::size_t> parameter_positions;
  for (std::size_t i = 0; i < parameters.size(); ++i)
    parameter_positions[tree_.At(parameters[i]).binding] = i;

  std::vector<DataItem> field_items(owner.fields.size());
  std::vector<bool> field_written(owner.fields.size(), false);
  for (SemaId child = tree_.At(definition).first_child; child != 0;
       child = tree_.At(child).next_sibling) {
    const SemaNode& member = tree_.At(child);
    if (member.kind != SEMA_MEMBER_INITIALIZER)
      continue;
    if (member.binding == 0 || member.function != 0)
      return false;
    const ClassField* field = model_.FieldFor(member.binding);
    const std::size_t field_index = model_.BindingAt(member.binding).field_index;
    if (field == 0 || field->bit_width != 0 || field_written[field_index] ||
        types_.Kind(types_.Unqualified(field->type)) == TYPE_REFERENCE)
      return false;
    const std::vector<SemaId> arguments = Children(child);
    if (arguments.size() != 1)
      return false;
    SemaId value = arguments[0];
    const SemaNode& argument = tree_.At(value);
    if (argument.kind == SEMA_ID_EXPRESSION && argument.binding != 0 &&
        model_.BindingAt(argument.binding).kind == BINDING_PARAMETER) {
      const std::map<BindingId, std::size_t>::const_iterator position =
          parameter_positions.find(argument.binding);
      // Parameter k of the constructor (`this` is 0) is call operand k
      // (the callee is 0).
      if (position == parameter_positions.end() || position->second == 0 ||
          position->second >= call_children.size())
        return false;
      value = call_children[position->second];
    }
    if (!ConstantGlobalItem(value, field->type, field_items[field_index]))
      return false;
    field_written[field_index] = true;
  }

  const std::size_t element_size = types_.SizeOf(class_type);
  std::size_t cursor = 0;
  for (std::size_t i = 0; i < owner.fields.size(); ++i) {
    const ClassField& field = owner.fields[i];
    if (field.static_member || field.binding == 0)
      continue;
    if (field.offset > cursor) {
      DataItem zero;
      zero.kind = DataItem::ITEM_ZERO;
      zero.zero_bytes = field.offset - cursor;
      items.push_back(zero);
    }
    if (field_written[i])
      items.push_back(field_items[i]);
    else {
      DataItem zero;
      zero.kind = DataItem::ITEM_ZERO;
      zero.zero_bytes = types_.SizeOf(field.type);
      items.push_back(zero);
    }
    cursor = field.offset + types_.SizeOf(field.type);
  }
  if (cursor < element_size) {
    DataItem zero;
    zero.kind = DataItem::ITEM_ZERO;
    zero.zero_bytes = element_size - cursor;
    items.push_back(zero);
  }
  (void)FunctionSymbolName(function);
  return true;
}

lowir_model::SymbolMetadata Lowerer::GlobalMetadata(
    const GlobalSymbol& symbol) const
{
  lowir_model::SymbolMetadata metadata;
  metadata.binding = symbol.internal_linkage ? lowir_model::SBM_INTERNAL :
      lowir_model::SBM_STRONG;
  metadata.linkage = symbol.c_linkage ? lowir_model::LLM_C :
      lowir_model::LLM_DEFAULT;
  metadata.object_symbol = symbol.object;
  return metadata;
}

lowir_model::GlobalDeclaration Lowerer::BuildGlobalDeclaration(
    const GlobalSymbol& symbol) const
{
  const TypeId declared = types_.Unqualified(
      model_.BindingAt(symbol.binding).type);
  lowir_model::GlobalDeclaration declaration;
  declaration.name = symbol.name;
  declaration.storage = symbol.thread_local_storage ?
      lowir_model::GSM_THREAD_LOCAL : lowir_model::GSM_DEFAULT;
  // An array of unknown bound has no LowIR storage type, and a thread-local
  // class object is reached only through its wrapper.
  declaration.has_type = (!symbol.thread_local_storage ||
                          types_.Kind(declared) != TYPE_CLASS) &&
      (types_.Kind(declared) != TYPE_ARRAY ||
       types_.At(declared).array_bound != 0);
  if (declaration.has_type)
    declaration.type = LowTypeOf(model_.BindingAt(symbol.binding).type);
  declaration.metadata = GlobalMetadata(symbol);
  return declaration;
}

// Definitions and thread-local wrappers are emitted here; a symbol the unit
// only declares is declared with the functions, once its uses are known.
void Lowerer::BuildGlobalDefinitions()
{
  typedef lowir_model::GlobalDefinition::DataItem DataItem;
  for (std::size_t i = 0; i < global_order_.size(); ++i) {
    const GlobalSymbol& symbol = globals_[global_order_[i]];
    if (symbol.thread_local_storage)
      AddThreadLocalWrapperDeclaration(
          symbol.name, ThreadLocalWrapperObjectName(symbol),
          symbol.internal_linkage);
    if (symbol.definition == 0)
      continue;
    const lowir_model::SymbolMetadata metadata = GlobalMetadata(symbol);

    const SemaNode& variable = tree_.At(symbol.definition);
    const Binding& binding = model_.BindingAt(variable.binding);
    const TypeId object_type = types_.Unqualified(binding.type);
    const std::vector<SemaId> initializer = Children(symbol.definition);
    lowir_model::GlobalDefinition global;
    global.name = symbol.name;
    global.storage = symbol.thread_local_storage ?
        lowir_model::GSM_THREAD_LOCAL : lowir_model::GSM_DEFAULT;
    global.metadata = metadata;

    if (types_.Kind(object_type) == TYPE_ARRAY) {
      BuildGlobalArrayDefinition(symbol, binding, initializer, global);
      program_.globals.push_back(global);
      continue;
    }

    if (types_.Kind(object_type) == TYPE_CLASS) {
      global.structured = true;
      DataItem zero;
      zero.kind = DataItem::ITEM_ZERO;
      zero.zero_bytes = types_.SizeOf(binding.type);
      global.data_items.push_back(zero);
      program_.globals.push_back(global);
      if (initializer.size() == 1 &&
          tree_.At(initializer[0]).kind == SEMA_CONSTRUCTOR_ACTION) {
        const std::vector<SemaId> action_children =
            Children(initializer[0]);
        if (action_children.size() == 1 &&
            tree_.At(action_children[0]).kind == SEMA_CALL) {
          const FunctionEntityId constructor =
              tree_.At(action_children[0]).function;
          if (constructor != 0) {
            const bool trivial =
                model_.ClassAt(model_.FunctionAt(constructor).member_class)
                    .trivial_default_constructor;
            if (symbol.thread_local_storage) {
              if (!trivial)
                AddThreadLocalInitializer(symbol, initializer[0],
                                          binding.type, true);
            } else {
              shared_.needs_init_function_ = true;
              if (!trivial) {
                DynamicInitializer dynamic;
                dynamic.expression = action_children[0];
                dynamic.symbol = symbol.name;
                dynamic.type = binding.type;
                dynamic.constructor_action = true;
                dynamic_initializers_.push_back(dynamic);
              }
            }
          }
        }
      }
      continue;
    }

    global.type = LowTypeOf(binding.type);
    global.init_kind = lowir_model::GlobalDefinition::INIT_ZERO;
    DataItem item;
    bool dynamic = false;
    if (initializer.empty()) {
      // zero-initialized
    } else if (!ConstantGlobalItem(initializer[0], binding.type, item)) {
      dynamic = true;
    } else if (item.kind == DataItem::ITEM_ADDR) {
      global.init_kind = lowir_model::GlobalDefinition::INIT_ADDR;
      global.init_operand = GlobalOperand(item.symbol);
      global.addr_addend = item.addr_addend;
    } else if (item.kind == DataItem::ITEM_INTEGER) {
      global.init_kind = lowir_model::GlobalDefinition::INIT_INTEGER;
      global.init_operand = item.literal_operand;
    }
    program_.globals.push_back(global);
    if (dynamic) {
      if (symbol.thread_local_storage)
        AddThreadLocalInitializer(symbol, initializer[0], binding.type, false);
      else {
        DynamicInitializer initializer_record;
        initializer_record.expression = initializer[0];
        initializer_record.symbol = symbol.name;
        initializer_record.type = binding.type;
        dynamic_initializers_.push_back(initializer_record);
      }
    }
  }
}

void Lowerer::BuildThreadLocalInitializers()
{
  for (std::size_t i = 0; i < thread_local_initializers_.size(); ++i) {
    const ThreadLocalInitializer& initializer = thread_local_initializers_[i];
    ResetFunction(initializer.function, VoidType());
    function_.metadata.binding = lowir_model::SBM_INTERNAL;
    StartBlock("^entry");

    const std::string run = NewBlockLabel("local_static_ctor_run");
    const std::string done = NewBlockLabel("local_static_ctor_done");
    lowir_model::Instruction guard;
    guard.kind = lowir_model::Instruction::IK_LOAD;
    guard.dest = NewTemp();
    guard.type = I64Type();
    guard.first = GlobalOperand(initializer.guard);
    Emit(guard);

    lowir_model::Instruction initialized;
    initialized.kind = lowir_model::Instruction::IK_CMP;
    initialized.dest = NewTemp();
    initialized.op = "ne";
    initialized.type = I64Type();
    initialized.first = TempOperand(guard.dest);
    initialized.second = Immediate(0);
    Emit(initialized);
    EmitBranch(TempOperand(initialized.dest), done, run);

    StartBlock(run);
    if (initializer.constructor_action) {
      const std::vector<SemaId> action_children =
          Children(initializer.expression);
      if (action_children.size() != 1 ||
          tree_.At(action_children[0]).kind != SEMA_CALL)
        Unsupported("a thread-local constructor action");
      (void)LowerRValue(action_children[0], types_.Fundamental(FT_VOID));
    } else {
      const Value value = LowerRValue(initializer.expression,
                                      initializer.type);
      EmitStore(LowTypeOf(initializer.type), value.operand,
                GlobalOperand(initializer.symbol));
    }
    EmitStore(I64Type(), Immediate(1), GlobalOperand(initializer.guard));
    EmitJump(done);

    StartBlock(done);
    EmitReturn(0);
    program_.functions.push_back(std::move(function_));
  }
}

bool Lowerer::TryBuildRuntimeClassAggregate(
    const GlobalSymbol& symbol, const Binding& binding, TypeId element,
    const std::vector<SemaId>& elements,
    lowir_model::GlobalDefinition& global)
{
  typedef lowir_model::GlobalDefinition::DataItem DataItem;
  if (!binding.static_member ||
      model_.ScopeAt(binding.scope).kind != SCOPE_CLASS)
    return false;
  const TypeId element_unqualified = types_.Unqualified(element);
  if (types_.Kind(element_unqualified) != TYPE_CLASS)
    return false;
  const ClassEntity& class_entity = model_.ClassAt(
      types_.At(element_unqualified).entity);
  bool has_dynamic_leaf = false;
  for (std::size_t index = 0; index < elements.size(); ++index) {
    if (tree_.At(elements[index]).kind != SEMA_BRACED_INIT_LIST)
      return false;
    const std::vector<SemaId> values = Children(elements[index]);
    std::size_t value_index = 0;
    for (std::size_t field_index = 0;
         field_index < class_entity.fields.size(); ++field_index) {
      const ClassField& field = class_entity.fields[field_index];
      if (field.static_member || field.binding == 0)
        continue;
      if (value_index < values.size()) {
        DataItem ignored;
        if (!ConstantGlobalItem(values[value_index], field.type, ignored))
          has_dynamic_leaf = true;
        ++value_index;
      }
    }
  }
  if (!has_dynamic_leaf)
    return false;

  DataItem zero;
  zero.kind = DataItem::ITEM_ZERO;
  zero.zero_bytes = types_.SizeOf(binding.type);
  global.data_items.push_back(zero);
  for (std::size_t index = 0; index < elements.size(); ++index) {
    const std::vector<SemaId> values = Children(elements[index]);
    std::size_t value_index = 0;
    for (std::size_t field_index = 0;
         field_index < class_entity.fields.size(); ++field_index) {
      const ClassField& field = class_entity.fields[field_index];
      if (field.static_member || field.binding == 0)
        continue;
      if (value_index < values.size()) {
        DynamicInitializer dynamic;
        dynamic.expression = values[value_index];
        dynamic.symbol = symbol.name;
        dynamic.type = field.type;
        dynamic.aggregate_type = binding.type;
        dynamic.aggregate_path.push_back(index);
        dynamic.aggregate_path.push_back(field_index);
        dynamic.aggregate_subobject = true;
        dynamic_initializers_.push_back(dynamic);
        ++value_index;
      }
    }
  }
  return true;
}

// The program's startup initializer stores every non-constant initializer
// in declaration order, unit by unit.
void Lowerer::BuildGlobalInitializers()
{
  if (dynamic_initializers_.empty() && !shared_.needs_init_function_)
    return;
  ResumeInitFunction();
  for (std::size_t i = 0; i < dynamic_initializers_.size(); ++i) {
    const DynamicInitializer& dynamic = dynamic_initializers_[i];
    if (dynamic.constructor_action) {
      if (tree_.At(dynamic.expression).kind == SEMA_CONSTRUCTOR_ACTION) {
        // An array element constructed at startup, in place.
        lowir_model::Instruction base;
        base.kind = lowir_model::Instruction::IK_ADDR;
        base.dest = NewTemp();
        base.type = PtrType();
        base.first = GlobalOperand(dynamic.symbol);
        Emit(base);
        LowerAggregateConstructor(
            dynamic.expression, dynamic.type,
            ProjectArrayElement(TempOperand(base.dest), dynamic.type,
                                dynamic.element_index));
      } else
        (void)LowerRValue(dynamic.expression, types_.Fundamental(FT_VOID));
      continue;
    }
    const Value value = LowerRValue(dynamic.expression, dynamic.type);
    lowir_model::Operand destination = GlobalOperand(dynamic.symbol);
    if (dynamic.aggregate_subobject) {
      Value aggregate;
      aggregate.type = dynamic.aggregate_type;
      aggregate.lvalue = true;
      aggregate.operand = destination;
      destination = AggregateDestination(aggregate, dynamic.aggregate_path);
      EmitStore(LowTypeOf(dynamic.type), value.operand, destination);
      continue;
    }
    if (dynamic.byte_offset != 0) {
      lowir_model::Instruction base;
      base.kind = lowir_model::Instruction::IK_ADDR;
      base.dest = NewTemp();
      base.type = PtrType();
      base.first = destination;
      Emit(base);
      lowir_model::Instruction projection;
      projection.kind = lowir_model::Instruction::IK_INDEX;
      projection.dest = NewTemp();
      projection.type = I8Type();
      projection.first = TempOperand(base.dest);
      projection.second = Immediate(
          static_cast<long long>(dynamic.byte_offset));
      Emit(projection);
      destination = TempOperand(projection.dest);
    }
    EmitStore(LowTypeOf(dynamic.type), value.operand, destination);
  }
  SuspendInitFunction();
}

void Lowerer::BuildGlobalFinalizers()
{
  bool has_destructor = false;
  for (std::size_t i = 0; i < global_order_.size(); ++i) {
    const GlobalSymbol& global = globals_[global_order_[i]];
    if (global.definition == 0)
      continue;
    TypeId type = types_.Unqualified(model_.BindingAt(global.binding).type);
    if (types_.Kind(type) == TYPE_ARRAY)
      type = types_.Unqualified(types_.At(type).base);
    if (types_.Kind(type) == TYPE_CLASS &&
        NeedsDestructor(types_.At(type).entity)) {
      has_destructor = true;
      break;
    }
  }
  if (!has_destructor)
    return;

  ResumeFiniFunction();
  for (std::size_t i = global_order_.size(); i != 0; --i) {
    const GlobalSymbol& global = globals_[global_order_[i - 1]];
    if (global.definition == 0)
      continue;
    TypeId type = types_.Unqualified(model_.BindingAt(global.binding).type);
    bool array = false;
    if (types_.Kind(type) == TYPE_ARRAY) {
      array = true;
      type = types_.Unqualified(types_.At(type).base);
    }
    if (types_.Kind(type) != TYPE_CLASS ||
        !NeedsDestructor(types_.At(type).entity))
      continue;
    const FunctionEntityId destructor =
        model_.ClassAt(types_.At(type).entity).destructor;
    if (destructor == 0)
      continue;
    if (array) {
      Value object;
      object.type = model_.BindingAt(global.binding).type;
      object.lvalue = true;
      object.operand = GlobalOperand(global.name);
      const std::size_t bound = types_.At(
          types_.Unqualified(object.type)).array_bound;
      for (std::size_t index = 0; index < bound; ++index) {
        const std::string& symbol = FunctionSymbolName(destructor);
        EmitVoidCall(symbol, std::vector<lowir_model::Operand>(
            1, LowerArrayElementAddress(object, type, index)));
      }
    } else {
      Value object;
      object.type = model_.BindingAt(global.binding).type;
      object.lvalue = true;
      object.operand = GlobalOperand(global.name);
      const std::string& symbol = FunctionSymbolName(destructor);
      EmitVoidCall(symbol, std::vector<lowir_model::Operand>(
          1, AddressValue(object).operand));
    }
  }
  SuspendFiniFunction();
}

// A declaration-only object is declared unless it is a constant the unit
// only folded: a static member with an in-class initializer needs storage
// only where it is odr-used, and its thread-local wrapper keeps its target.
void Lowerer::BuildDeclarations()
{
  for (std::size_t i = 0; i < global_order_.size(); ++i) {
    const GlobalSymbol& symbol = globals_[global_order_[i]];
    if (symbol.definition != 0)
      continue;
    if (!symbol.referenced && !symbol.thread_local_storage &&
        model_.BindingAt(symbol.binding).has_const_value)
      continue;
    program_.global_declarations.push_back(BuildGlobalDeclaration(symbol));
  }
  for (std::size_t i = 0; i < function_order_.size(); ++i) {
    const FunctionSymbol& symbol = functions_[function_order_[i]];
    if (symbol.definition != 0)
      continue;
    if (symbol.base_required)
      program_.function_declarations.push_back(
          BuildFunctionDeclaration(function_order_[i], symbol, true));
    if (symbol.referenced)
      program_.function_declarations.push_back(
          BuildFunctionDeclaration(function_order_[i], symbol, false));
  }
}

lowir_model::FunctionDeclaration Lowerer::BuildFunctionDeclaration(
    FunctionEntityId id, const FunctionSymbol& symbol, bool base_variant)
{
  lowir_model::FunctionDeclaration result;
  result.name = base_variant ? symbol.base_name : symbol.name;
  const FunctionEntity& entity = model_.FunctionAt(id);
  const TypeNode& type = types_.At(types_.Unqualified(entity.type));
  result.return_type = LowTypeOf(type.result);
  result.boundary.arity = type.variadic ? lowir_model::CAM_VARIADIC :
      lowir_model::CAM_FIXED;
  result.metadata.binding = base_variant ? lowir_model::SBM_STRONG :
      entity.internal_linkage ?
      lowir_model::SBM_INTERNAL : entity.in_class_definition ?
      lowir_model::SBM_WEAK : lowir_model::SBM_STRONG;
  result.metadata.linkage = entity.c_linkage ? lowir_model::LLM_C :
      lowir_model::LLM_DEFAULT;
  if (!entity.c_linkage)
    result.metadata.object_symbol = base_variant ? symbol.base_object :
        symbol.object;
  std::vector<SemaId> parameters;
  CollectParameters(symbol.declaration, parameters);
  for (std::size_t i = 0; i < type.parameters.size(); ++i) {
    lowir_model::Parameter parameter;
    parameter.name = "%arg" + std::to_string(i);
    parameter.type = LowTypeOf(type.parameters[i]);
    if (types_.Kind(types_.Unqualified(type.parameters[i])) == TYPE_REFERENCE)
      parameter.metadata.passing = lowir_model::PPM_REFERENCE;
    if (i < parameters.size()) {
      const Binding& binding =
          model_.BindingAt(tree_.At(parameters[i]).binding);
      if (!binding.name.empty())
        parameter.name = "%" + binding.name;
    }
    result.params.push_back(parameter);
  }
  return result;
}

}  // namespace lowir_lowering
