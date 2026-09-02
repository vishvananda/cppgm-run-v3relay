// Translation-unit entry, condition lowering, and statement lowering.
#include "lower/lowir_lowering.h"

#include <stdexcept>
#include <utility>

namespace lowir_lowering {

namespace {

bool HasSwitchBreak(SemaId node, const SemaTree& tree)
{
  if (node == 0)
    return false;
  const SemaKind kind = tree.At(node).kind;
  if (kind == SEMA_BREAK_STATEMENT)
    return true;
  if (kind == SEMA_SWITCH_STATEMENT || kind == SEMA_WHILE_STATEMENT ||
      kind == SEMA_DO_STATEMENT || kind == SEMA_FOR_STATEMENT)
    return false;
  for (SemaId child = tree.At(node).first_child; child != 0;
       child = tree.At(child).next_sibling)
    if (HasSwitchBreak(child, tree))
      return true;
  return false;
}

}  // namespace

ProgramLowering::ProgramLowering(lowir_model::Program& program)
    : program_(program), has_init_(false), init_temp_counter_(0),
      init_label_counter_(0), init_slot_counter_(0),
      has_fini_(false), needs_init_function_(false),
      fini_temp_counter_(0), fini_label_counter_(0), fini_slot_counter_(0),
      string_literal_counter_(0)
{
}

void ProgramLowering::AddUnit(const std::vector<Pa6Token>& tokens,
                              SemaModel& model, const SemaTree& tree)
{
  Lowerer(*this, tokens, model, tree).Run();
}

// The initializer returns after the last unit's stores.  Declarations are
// reconciled last because a later unit may define what an earlier one only
// declared; the first declaration of a symbol survives.
void ProgramLowering::Finish()
{
  if (has_init_) {
    lowir_model::Instruction result;
    result.kind = lowir_model::Instruction::IK_RETURN;
    result.type = VoidType();
    init_function_.blocks.back().instructions.push_back(result);
    program_.functions.push_back(std::move(init_function_));
    has_init_ = false;
  }
  if (has_fini_) {
    lowir_model::Instruction result;
    result.kind = lowir_model::Instruction::IK_RETURN;
    result.type = VoidType();
    fini_function_.blocks.back().instructions.push_back(result);
    program_.functions.push_back(std::move(fini_function_));
    has_fini_ = false;
  }
  needs_init_function_ = false;
  std::set<std::string> defined;
  for (std::size_t i = 0; i < program_.globals.size(); ++i)
    defined.insert(program_.globals[i].name);
  for (std::size_t i = 0; i < program_.functions.size(); ++i)
    defined.insert(program_.functions[i].name);
  std::set<std::string> declared;
  std::size_t kept = 0;
  for (std::size_t i = 0; i < program_.global_declarations.size(); ++i) {
    const std::string& name = program_.global_declarations[i].name;
    if (defined.count(name) != 0 || !declared.insert(name).second)
      continue;
    if (kept != i)
      program_.global_declarations[kept] =
          std::move(program_.global_declarations[i]);
    ++kept;
  }
  program_.global_declarations.resize(kept);
  kept = 0;
  for (std::size_t i = 0; i < program_.function_declarations.size(); ++i) {
    const std::string& name = program_.function_declarations[i].name;
    if (defined.count(name) != 0 || !declared.insert(name).second)
      continue;
    if (kept != i)
      program_.function_declarations[kept] =
          std::move(program_.function_declarations[i]);
    ++kept;
  }
  program_.function_declarations.resize(kept);
}

// One unit, in order: symbols and names, global data, function bodies in
// source order, the unit's startup stores, then declarations for the
// referenced functions this unit never defines.
void Lowerer::Run()
{
  CollectSymbols(tree_.Root());
  ComputeReferencedFunctions();
  // Deferred in-class definitions are visited in declaration order, while a
  // call from a later body may refer to an earlier member.  Apply the complete
  // reference set after the walk before deciding which weak inline bodies are
  // ODR-used.
  for (std::size_t i = 0; i < function_order_.size(); ++i)
    functions_[function_order_[i]].referenced =
        model_.FunctionAt(function_order_[i]).in_class_definition &&
        referenced_functions_.count(function_order_[i]) != 0;
  NameSymbols();
  BuildGlobalDefinitions();
  // Finalization roots are outside the semantic call graph.  Mark their
  // complete destructor entries before deciding which weak bodies to emit.
  for (std::size_t i = 0; i < global_order_.size(); ++i) {
    const GlobalSymbol& global = globals_[global_order_[i]];
    if (global.definition == 0)
      continue;
    TypeId type = types_.Unqualified(model_.BindingAt(global.binding).type);
    if (types_.Kind(type) == TYPE_ARRAY)
      type = types_.Unqualified(types_.At(type).base);
    if (types_.Kind(type) != TYPE_CLASS)
      continue;
    const ClassEntityId entity = types_.At(type).entity;
    if (!NeedsDestructor(entity))
      continue;
    const FunctionEntityId destructor = model_.ClassAt(entity).destructor;
    if (destructor != 0)
      (void)FunctionSymbolName(destructor);
  }
  // Lowering a constructor or destructor can discover a base-subobject
  // entry, or can discover a member's complete entry.  Keep the emission
  // walk bounded by the unit's finite entity table while allowing those
  // references to request the matching variant before validation.
  std::set<FunctionEntityId> emitted;
  bool emitted_one = true;
  while (emitted_one) {
    emitted_one = false;
    for (std::size_t i = 0; i < function_order_.size(); ++i) {
      const FunctionEntityId id = function_order_[i];
      FunctionSymbol& symbol = functions_[id];
      const FunctionEntity& entity = model_.FunctionAt(id);
      if (symbol.definition != 0 && emitted.count(id) == 0 &&
          (!entity.in_class_definition || symbol.referenced)) {
        program_.functions.push_back(BuildFunction(symbol));
        emitted.insert(id);
        emitted_one = true;
        if (entity.special_member != SPECIAL_MEMBER_NONE &&
            !entity.c_linkage) {
          lowir_model::ObjectAlias alias;
          alias.object_symbol = symbol.base_object;
          alias.target = symbol.name;
          program_.object_aliases.push_back(alias);
        }
      }
      if (symbol.definition != 0 && symbol.base_required &&
          !symbol.base_emitted) {
        program_.functions.push_back(BuildFunctionVariant(symbol, true));
        symbol.base_emitted = true;
        emitted_one = true;
      }
    }
  }
  BuildGlobalInitializers();
  BuildGlobalFinalizers();
  BuildDeclarations();
}

bool Lowerer::DefinitelyTerminates(SemaId node) const
{
  if (node == 0)
    return false;
  const SemaNode& value = tree_.At(node);
  switch (value.kind) {
  case SEMA_RETURN_STATEMENT:
  case SEMA_BREAK_STATEMENT:
  case SEMA_CONTINUE_STATEMENT:
  case SEMA_GOTO_STATEMENT:
    return true;
  case SEMA_LABELED_STATEMENT: {
    const std::vector<SemaId> children = Children(node);
    return children.size() == 1 && DefinitelyTerminates(children[0]);
  }
  case SEMA_THEN:
  case SEMA_ELSE:
  case SEMA_COMPOUND_STATEMENT:
  case SEMA_FOR_INIT_STATEMENT:
  case SEMA_ITERATION:
  case SEMA_CASE_STATEMENT:
  case SEMA_DEFAULT_STATEMENT: {
    const std::vector<SemaId> children = Children(node);
    // A case statement's first child is its label value.
    const std::size_t statements = value.kind == SEMA_CASE_STATEMENT ?
        (children.empty() ? 0 : children.size() - 1) : children.size();
    return statements != 0 && DefinitelyTerminates(children.back());
  }
  case SEMA_IF_STATEMENT: {
    const std::vector<SemaId> children = Children(node);
    return children.size() >= 3 && DefinitelyTerminates(children[1]) &&
        DefinitelyTerminates(children[2]);
  }
  default:
    return false;
  }
}

// Branch-context `&&`/`||` allocate their right-operand labels before the
// condition is lowered so label numbers follow source order.
void Lowerer::PrepareConditionLabels(SemaId node)
{
  if (node == 0)
    return;
  const SemaNode& value = tree_.At(node);
  if (value.kind == SEMA_CONDITION ||
      value.kind == SEMA_CONDITION_DECLARATION) {
    for (SemaId child = value.first_child; child != 0;
         child = tree_.At(child).next_sibling)
      PrepareConditionLabels(child);
    return;
  }
  if (value.kind != SEMA_BINARY || !IsLogicalOperator(value.op))
    return;
  const std::vector<SemaId> children = Children(node);
  const SemaNode& left = tree_.At(children[0]);
  if (IsKnownIntegralLiteral(left, types_) &&
      ((value.op == OP_LOR && left.value != 0) ||
       (value.op == OP_LAND && left.value == 0)))
    return;
  if (condition_labels_.find(node) == condition_labels_.end())
    condition_labels_[node] = NewBlockLabel(value.op == OP_LOR ?
        "lor_rhs" : "land_rhs");
  for (std::size_t i = 0; i < children.size(); ++i)
    PrepareConditionLabels(children[i]);
}

// Integral, enumeration, and pointer values branch directly; a floating
// value is first compared against zero (4.12).
void Lowerer::LowerTruthBranch(Value value, const std::string& true_label,
                               const std::string& false_label)
{
  if (IsFloatingType(types_, value.type)) {
    lowir_model::Instruction compare;
    compare.kind = lowir_model::Instruction::IK_CMP;
    compare.dest = NewTemp();
    compare.op = "ne";
    compare.type = LowTypeOf(value.type);
    compare.first = value.operand;
    compare.second = ZeroOperand(value.type);
    Emit(compare);
    value.operand = TempOperand(compare.dest);
  }
  EmitBranch(value.operand, true_label, false_label);
}

void Lowerer::LowerCondition(SemaId node, const std::string& true_label,
                             const std::string& false_label)
{
  if (node == 0)
    Unsupported("a missing condition");
  const SemaNode& value = tree_.At(node);
  if (value.kind == SEMA_CONDITION) {
    const std::vector<SemaId> children = Children(node);
    if (children.size() != 1)
      Unsupported("this condition wrapper");
    LowerCondition(children[0], true_label, false_label);
    return;
  }
  if (value.kind == SEMA_CONDITION_DECLARATION) {
    LowerTruthBranch(LowerConditionVariable(node), true_label, false_label);
    return;
  }
  if (value.kind == SEMA_BINARY && IsLogicalOperator(value.op)) {
    const std::vector<SemaId> children = Children(node);
    if (children.size() != 2)
      Unsupported("this logical condition");
    const SemaNode& left = tree_.At(children[0]);
    if (IsKnownIntegralLiteral(left, types_)) {
      // A literal left operand decides the branch, or is simply skipped.
      const bool left_true = left.value != 0;
      const bool decisive = (value.op == OP_LOR && left_true) ||
          (value.op == OP_LAND && !left_true);
      if (decisive)
        EmitBranch(Immediate(left_true ? 1 : 0), true_label, false_label);
      else
        LowerCondition(children[1], true_label, false_label);
      return;
    }
    std::map<SemaId, std::string>::const_iterator found =
        condition_labels_.find(node);
    if (found == condition_labels_.end())
      found = condition_labels_.insert(std::make_pair(
          node, NewBlockLabel(value.op == OP_LOR ? "lor_rhs" :
                              "land_rhs"))).first;
    const std::string rhs = found->second;
    if (value.op == OP_LOR)
      LowerCondition(children[0], true_label, rhs);
    else
      LowerCondition(children[0], rhs, false_label);
    StartBlock(rhs);
    LowerCondition(children[1], true_label, false_label);
    return;
  }
  LowerTruthBranch(LowerRValue(node), true_label, false_label);
}

// `if (T x = e)` and `switch (T x = e)`: the object is initialized in the
// enclosing block and its stored value is the condition.
Lowerer::Value Lowerer::LowerConditionVariable(SemaId declaration)
{
  const std::vector<SemaId> children = Children(declaration);
  if (children.size() != 1 || tree_.At(children[0]).kind != SEMA_VARIABLE)
    Unsupported("this condition declaration");
  LowerVariable(children[0]);
  const BindingId binding = tree_.At(children[0]).binding;
  Value slot;
  slot.type = model_.BindingAt(binding).type;
  slot.operand = SlotOperand(SlotFor(binding));
  return LoadValue(slot);
}

void Lowerer::LowerVariableDeclaration(SemaId node)
{
  if (tree_.At(node).kind == SEMA_VARIABLE) {
    LowerVariable(node);
    return;
  }
  for (SemaId child = tree_.At(node).first_child; child != 0;
       child = tree_.At(child).next_sibling)
    if (tree_.At(child).kind == SEMA_VARIABLE)
      LowerVariable(child);
}

void Lowerer::LowerVariable(SemaId variable_node)
{
  const SemaNode& variable = tree_.At(variable_node);
  const std::vector<SemaId> initializer = Children(variable_node);
  const TypeId declared = variable.type;
  const TypeId unqualified = types_.Unqualified(declared);
  if (types_.Kind(unqualified) == TYPE_ARRAY) {
    const TypeId element = types_.Unqualified(types_.At(unqualified).base);
    if (types_.Kind(element) == TYPE_CLASS && initializer.empty()) {
      const ClassEntityId element_entity = types_.At(element).entity;
      const FunctionEntityId constructor = DefaultConstructor(element_entity);
      const ClassEntity& element_class = model_.ClassAt(element_entity);
      Value array;
      array.type = declared;
      array.lvalue = true;
      array.operand = SlotOperand(SlotFor(variable.binding));
      if (constructor != 0 && !element_class.trivial_default_constructor) {
        const std::size_t bound = types_.At(unqualified).array_bound;
        for (std::size_t i = 0; i < bound; ++i) {
          lowir_model::Instruction call;
          call.kind = lowir_model::Instruction::IK_CALL;
          call.type = VoidType();
          call.call_return_type = VoidType();
          call.call_returns_void = true;
          call.first = GlobalOperand(FunctionSymbolName(constructor));
          call.args.push_back(LowerArrayElementAddress(array, element, i));
          Emit(call);
        }
      }
      RegisterLiveObject(variable.binding, declared);
      return;
    }
  }
  if (types_.Kind(unqualified) == TYPE_CLASS)
  {
    Value object;
    object.type = declared;
    object.lvalue = true;
    object.operand = SlotOperand(SlotFor(variable.binding));
    // The constructor action owns the address expression.  Keeping that
    // address in the action also makes explicit and synthesized construction
    // use the same call lowering path.
    if (initializer.empty()) {
      RegisterLiveObject(variable.binding, declared);
      return;
    }
    if (initializer.size() == 1 &&
        tree_.At(initializer[0]).kind == SEMA_CONSTRUCTOR_ACTION) {
      const std::vector<SemaId> action_children = Children(initializer[0]);
      if (action_children.size() != 1 ||
          tree_.At(action_children[0]).kind != SEMA_CALL)
        Unsupported("this constructor action");
      const FunctionEntityId function =
          tree_.At(action_children[0]).function;
      if (function == 0)
        Unsupported("a constructor action without a function");
      if (model_.ClassAt(model_.FunctionAt(function).member_class)
              .trivial_default_constructor)
      {
        (void)AddressValue(object);
        RegisterLiveObject(variable.binding, declared);
        return;
      }
      (void)LowerCall(action_children[0], 0);
      RegisterLiveObject(variable.binding, declared);
      return;
    }
    if (initializer.size() == 1 &&
        tree_.At(initializer[0]).kind == SEMA_BRACED_INIT_LIST) {
      const ClassEntity& class_entity = model_.ClassAt(
          types_.At(unqualified).entity);
      // A braced aggregate still begins the lifetime of the complete class
      // object before its fields are initialized.  Preserve that address
      // operation even when no non-trivial constructor call is required.
      (void)AddressValue(object);
      const std::vector<SemaId> values = Children(initializer[0]);
      for (std::size_t i = 0; i < class_entity.fields.size(); ++i) {
        const Value address = AddressValue(object);
        lowir_model::Instruction projection;
        projection.kind = lowir_model::Instruction::IK_INDEX;
        projection.dest = NewTemp();
        projection.type = I8Type();
        projection.index_projection = lowir_model::IPK_FIELD;
        projection.first = address.operand;
        projection.second = Immediate(static_cast<long long>(
            class_entity.fields[i].offset));
        Emit(projection);
        const lowir_model::Operand value = i < values.size() ?
            LowerRValue(values[i], class_entity.fields[i].type).operand :
            ZeroOperand(class_entity.fields[i].type);
        EmitStore(LowTypeOf(class_entity.fields[i].type), value,
                  TempOperand(projection.dest));
      }
      RegisterLiveObject(variable.binding, declared);
      return;
    }
  }
  if (initializer.empty())
    return;
  if (types_.Kind(unqualified) == TYPE_REFERENCE) {
    const TypeId referent = types_.Referent(unqualified);
    const SemaId source = initializer[0];
    const SemaNode& source_node = tree_.At(source);
    Value address;
    if (source_node.category == VC_LVALUE ||
        source_node.category == VC_XVALUE ||
        source_node.kind == SEMA_CALL || source_node.kind == SEMA_BINARY) {
      address = AddressValue(LowerLValue(source));
    } else {
      // 8.5.3p5: a converted temporary gets its own slot.
      const Value materialized = LowerRValue(source, referent);
      Value storage;
      storage.type = referent;
      storage.operand = SlotOperand(
          NewGeneratedSlot("refarg", LowTypeOf(referent)));
      EmitStore(LowTypeOf(referent), materialized.operand, storage.operand);
      address = AddressValue(storage);
    }
    EmitStore(PtrType(), address.operand,
              SlotOperand(SlotFor(variable.binding)));
    return;
  }
  if (types_.Kind(unqualified) == TYPE_ARRAY &&
      tree_.At(initializer[0]).kind == SEMA_BRACED_INIT_LIST) {
    const TypeId element = types_.At(unqualified).base;
    const std::size_t bound = types_.At(unqualified).array_bound;
    Value array;
    array.type = declared;
    array.lvalue = true;
    array.operand = SlotOperand(SlotFor(variable.binding));
    const Value address = AddressValue(array);
    const std::vector<SemaId> elements = Children(initializer[0]);
    for (std::size_t i = 0; i < bound; ++i) {
      lowir_model::Operand destination = address.operand;
      if (i != 0) {
        lowir_model::Instruction projection;
        projection.kind = lowir_model::Instruction::IK_INDEX;
        projection.dest = NewTemp();
        projection.type = I8Type();
        projection.first = address.operand;
        projection.second = Immediate(
            static_cast<long long>(i * types_.SizeOf(element)));
        Emit(projection);
        destination = TempOperand(projection.dest);
      }
      // 8.5.1p7: elements without an initializer are zero-initialized.
      const lowir_model::Operand value = i < elements.size() ?
          LowerRValue(elements[i], element).operand : ZeroOperand(element);
      EmitStore(LowTypeOf(element), value, destination);
    }
    return;
  }
  const Value value = LowerRValue(initializer[0], variable.type);
  EmitStore(LowTypeOf(variable.type), value.operand,
            SlotOperand(SlotFor(variable.binding)));
}

lowir_model::Operand Lowerer::LowerArrayElementAddress(
    const Value& array, TypeId element, std::size_t index)
{
  const Value address = AddressValue(array);
  lowir_model::Instruction decay;
  decay.kind = lowir_model::Instruction::IK_UNARY;
  decay.dest = NewTemp();
  decay.op = "decay";
  decay.type = PtrType();
  decay.first = address.operand;
  Emit(decay);

  lowir_model::Operand offset = Immediate(static_cast<long long>(index));
  const std::size_t element_size = types_.SizeOf(element);
  const bool byte_element = types_.Kind(types_.Unqualified(element)) ==
      TYPE_CLASS;
  if (byte_element && element_size != 1) {
    lowir_model::Instruction scale;
    scale.kind = lowir_model::Instruction::IK_BINARY;
    scale.dest = NewTemp();
    scale.op = "mul";
    scale.type = I64Type();
    scale.first = offset;
    scale.second = Immediate(static_cast<long long>(element_size));
    Emit(scale);
    offset = TempOperand(scale.dest);
  }
  lowir_model::Instruction projection;
  projection.kind = lowir_model::Instruction::IK_INDEX;
  projection.dest = NewTemp();
  projection.type = byte_element ? I8Type() : LowTypeOf(element);
  projection.index_projection = lowir_model::IPK_ARRAY_ELEMENT;
  projection.first = TempOperand(decay.dest);
  projection.second = offset;
  Emit(projection);
  return TempOperand(projection.dest);
}

void Lowerer::LowerAggregateZero(
    TypeId type, const lowir_model::Operand& destination)
{
  const TypeId unqualified = types_.Unqualified(type);
  if (types_.Kind(unqualified) == TYPE_CLASS) {
    const ClassEntity& class_entity = model_.ClassAt(
        types_.At(unqualified).entity);
    for (std::size_t i = 0; i < class_entity.fields.size(); ++i) {
      const ClassField& field = class_entity.fields[i];
      if (field.static_member)
        continue;
      lowir_model::Instruction projection;
      projection.kind = lowir_model::Instruction::IK_INDEX;
      projection.dest = NewTemp();
      projection.type = I8Type();
      projection.index_projection = lowir_model::IPK_FIELD;
      projection.first = destination;
      projection.second = Immediate(static_cast<long long>(field.offset));
      Emit(projection);
      const TypeId field_type = types_.Unqualified(field.type);
      if (types_.Kind(field_type) == TYPE_CLASS ||
          types_.Kind(field_type) == TYPE_ARRAY)
        LowerAggregateZero(field.type, TempOperand(projection.dest));
      else
        EmitStore(LowTypeOf(field.type), ZeroOperand(field.type),
                  TempOperand(projection.dest));
    }
    return;
  }
  if (types_.Kind(unqualified) == TYPE_ARRAY) {
    const TypeId element = types_.At(unqualified).base;
    const std::size_t bound = types_.At(unqualified).array_bound;
    for (std::size_t i = 0; i < bound; ++i) {
      lowir_model::Instruction decay;
      decay.kind = lowir_model::Instruction::IK_UNARY;
      decay.dest = NewTemp();
      decay.op = "decay";
      decay.type = PtrType();
      decay.first = destination;
      Emit(decay);
      lowir_model::Operand offset = Immediate(static_cast<long long>(i));
      const std::size_t element_size = types_.SizeOf(element);
      const bool byte_element = types_.Kind(types_.Unqualified(element)) ==
          TYPE_CLASS;
      if (byte_element && element_size != 1) {
        lowir_model::Instruction scale;
        scale.kind = lowir_model::Instruction::IK_BINARY;
        scale.dest = NewTemp();
        scale.op = "mul";
        scale.type = I64Type();
        scale.first = offset;
        scale.second = Immediate(static_cast<long long>(element_size));
        Emit(scale);
        offset = TempOperand(scale.dest);
      }
      lowir_model::Instruction projection;
      projection.kind = lowir_model::Instruction::IK_INDEX;
      projection.dest = NewTemp();
      projection.type = byte_element ? I8Type() : LowTypeOf(element);
      projection.index_projection = lowir_model::IPK_ARRAY_ELEMENT;
      projection.first = TempOperand(decay.dest);
      projection.second = offset;
      Emit(projection);
      const TypeId element_unqualified = types_.Unqualified(element);
      if (types_.Kind(element_unqualified) == TYPE_CLASS ||
          types_.Kind(element_unqualified) == TYPE_ARRAY)
        LowerAggregateZero(element, TempOperand(projection.dest));
      else
        EmitStore(LowTypeOf(element), ZeroOperand(element),
                  TempOperand(projection.dest));
    }
    return;
  }
  EmitStore(LowTypeOf(type), ZeroOperand(type), destination);
}

void Lowerer::LowerAggregateInitializer(
    SemaId node, TypeId type, const lowir_model::Operand& destination)
{
  if (node == 0 || tree_.At(node).kind != SEMA_BRACED_INIT_LIST)
    Unsupported("a non-braced aggregate initializer");
  const TypeId unqualified = types_.Unqualified(type);
  const std::vector<SemaId> values = Children(node);
  if (types_.Kind(unqualified) == TYPE_CLASS) {
    const ClassEntity& class_entity = model_.ClassAt(
        types_.At(unqualified).entity);
    std::size_t value_index = 0;
    for (std::size_t i = 0; i < class_entity.fields.size(); ++i) {
      const ClassField& field = class_entity.fields[i];
      if (field.static_member)
        continue;
      lowir_model::Instruction projection;
      projection.kind = lowir_model::Instruction::IK_INDEX;
      projection.dest = NewTemp();
      projection.type = I8Type();
      projection.index_projection = lowir_model::IPK_FIELD;
      projection.first = destination;
      projection.second = Immediate(static_cast<long long>(field.offset));
      Emit(projection);
      const lowir_model::Operand field_destination =
          TempOperand(projection.dest);
      const TypeId field_unqualified = types_.Unqualified(field.type);
      if (value_index >= values.size()) {
        if (types_.Kind(field_unqualified) == TYPE_CLASS ||
            types_.Kind(field_unqualified) == TYPE_ARRAY)
          LowerAggregateZero(field.type, field_destination);
        else
          EmitStore(LowTypeOf(field.type), ZeroOperand(field.type),
                    field_destination);
      } else if (tree_.At(values[value_index]).kind ==
                 SEMA_BRACED_INIT_LIST &&
                 (types_.Kind(field_unqualified) == TYPE_CLASS ||
                  types_.Kind(field_unqualified) == TYPE_ARRAY)) {
        LowerAggregateInitializer(values[value_index], field.type,
                                  field_destination);
      } else {
        EmitStore(LowTypeOf(field.type),
                  LowerRValue(values[value_index], field.type).operand,
                  field_destination);
      }
      ++value_index;
    }
    return;
  }
  if (types_.Kind(unqualified) == TYPE_ARRAY) {
    const TypeId element = types_.At(unqualified).base;
    const std::size_t bound = types_.At(unqualified).array_bound;
    for (std::size_t i = 0; i < bound; ++i) {
      lowir_model::Instruction decay;
      decay.kind = lowir_model::Instruction::IK_UNARY;
      decay.dest = NewTemp();
      decay.op = "decay";
      decay.type = PtrType();
      decay.first = destination;
      Emit(decay);
      lowir_model::Operand offset = Immediate(static_cast<long long>(i));
      const std::size_t element_size = types_.SizeOf(element);
      const bool byte_element = types_.Kind(types_.Unqualified(element)) ==
          TYPE_CLASS;
      if (byte_element && element_size != 1) {
        lowir_model::Instruction scale;
        scale.kind = lowir_model::Instruction::IK_BINARY;
        scale.dest = NewTemp();
        scale.op = "mul";
        scale.type = I64Type();
        scale.first = offset;
        scale.second = Immediate(static_cast<long long>(element_size));
        Emit(scale);
        offset = TempOperand(scale.dest);
      }
      lowir_model::Instruction projection;
      projection.kind = lowir_model::Instruction::IK_INDEX;
      projection.dest = NewTemp();
      projection.type = byte_element ? I8Type() : LowTypeOf(element);
      projection.index_projection = lowir_model::IPK_ARRAY_ELEMENT;
      projection.first = TempOperand(decay.dest);
      projection.second = offset;
      Emit(projection);
      const lowir_model::Operand element_destination =
          TempOperand(projection.dest);
      if (i >= values.size()) {
        const TypeId element_unqualified = types_.Unqualified(element);
        if (types_.Kind(element_unqualified) == TYPE_CLASS ||
            types_.Kind(element_unqualified) == TYPE_ARRAY)
          LowerAggregateZero(element, element_destination);
        else
          EmitStore(LowTypeOf(element), ZeroOperand(element),
                    element_destination);
      } else if (tree_.At(values[i]).kind == SEMA_BRACED_INIT_LIST &&
                 (types_.Kind(types_.Unqualified(element)) == TYPE_CLASS ||
                  types_.Kind(types_.Unqualified(element)) == TYPE_ARRAY)) {
        LowerAggregateInitializer(values[i], element,
                                  element_destination);
      } else {
        EmitStore(LowTypeOf(element),
                  LowerRValue(values[i], element).operand,
                  element_destination);
      }
    }
    return;
  }
  if (values.size() > 1)
    Unsupported("a scalar aggregate with too many values");
  EmitStore(LowTypeOf(type), values.empty() ? ZeroOperand(type) :
            LowerRValue(values[0], type).operand, destination);
}

void Lowerer::LowerAggregateMemberInitializer(
    SemaId node, TypeId type, BindingId binding, FunctionEntityId owner)
{
  const std::vector<std::pair<bool, std::size_t> > path;
  LowerAggregateMemberLeaves(node, type, type, binding, owner, path);
}

void Lowerer::LowerAggregateMemberLeaves(
    SemaId node, TypeId type, TypeId root_type, BindingId binding,
    FunctionEntityId owner,
    const std::vector<std::pair<bool, std::size_t> >& path)
{
  if (node == 0 || tree_.At(node).kind != SEMA_BRACED_INIT_LIST)
    Unsupported("a non-braced aggregate member initializer");
  const std::vector<SemaId> values = Children(node);
  const TypeId unqualified = types_.Unqualified(type);
  if (types_.Kind(unqualified) == TYPE_CLASS) {
    const ClassEntity& class_entity = model_.ClassAt(
        types_.At(unqualified).entity);
    std::size_t value_index = 0;
    for (std::size_t i = 0; i < class_entity.fields.size(); ++i) {
      const ClassField& field = class_entity.fields[i];
      if (field.static_member)
        continue;
      std::vector<std::pair<bool, std::size_t> > nested_path(path);
      nested_path.push_back(std::make_pair(false, field.offset));
      const TypeId field_unqualified = types_.Unqualified(field.type);
      if (value_index < values.size() &&
          tree_.At(values[value_index]).kind == SEMA_BRACED_INIT_LIST &&
          (types_.Kind(field_unqualified) == TYPE_CLASS ||
           types_.Kind(field_unqualified) == TYPE_ARRAY))
        LowerAggregateMemberLeaves(values[value_index], field.type, root_type,
                                   binding, owner, nested_path);
      else {
        const SemaId value = value_index < values.size() ?
            values[value_index] : 0;
        lowir_model::Instruction load_this;
        load_this.kind = lowir_model::Instruction::IK_LOAD;
        load_this.dest = NewTemp();
        load_this.type = PtrType();
        load_this.first = SlotOperand("$this");
        Emit(load_this);
        lowir_model::Operand destination = TempOperand(load_this.dest);
        const ClassEntity& owner_class = model_.ClassAt(
            model_.FunctionAt(owner).member_class);
        bool found_root = false;
        for (std::size_t root_index = 0;
             root_index < owner_class.fields.size(); ++root_index)
          if (owner_class.fields[root_index].binding == binding) {
            lowir_model::Instruction projection;
            projection.kind = lowir_model::Instruction::IK_INDEX;
            projection.dest = NewTemp();
            projection.type = I8Type();
            projection.index_projection = lowir_model::IPK_FIELD;
            projection.first = destination;
            projection.second = Immediate(static_cast<long long>(
                owner_class.fields[root_index].offset));
            Emit(projection);
            destination = TempOperand(projection.dest);
            found_root = true;
            break;
          }
        if (!found_root)
          Unsupported("an aggregate member without its root field");
        TypeId current = root_type;
        for (std::size_t step = 0; step < nested_path.size(); ++step) {
          const bool array = nested_path[step].first;
          const std::size_t offset = nested_path[step].second;
          if (array) {
            const TypeId current_unqualified = types_.Unqualified(current);
            if (types_.Kind(current_unqualified) != TYPE_ARRAY)
              Unsupported("an aggregate member array path");
            const TypeId element = types_.At(current_unqualified).base;
            lowir_model::Instruction decay;
            decay.kind = lowir_model::Instruction::IK_UNARY;
            decay.dest = NewTemp();
            decay.op = "decay";
            decay.type = PtrType();
            decay.first = destination;
            Emit(decay);
            lowir_model::Operand index = Immediate(
                static_cast<long long>(offset));
            const std::size_t element_size = types_.SizeOf(element);
            const bool byte_element = types_.Kind(
                types_.Unqualified(element)) == TYPE_CLASS;
            if (byte_element && element_size != 1) {
              lowir_model::Instruction scale;
              scale.kind = lowir_model::Instruction::IK_BINARY;
              scale.dest = NewTemp();
              scale.op = "mul";
              scale.type = I64Type();
              scale.first = index;
              scale.second = Immediate(static_cast<long long>(element_size));
              Emit(scale);
              index = TempOperand(scale.dest);
            }
            lowir_model::Instruction projection;
            projection.kind = lowir_model::Instruction::IK_INDEX;
            projection.dest = NewTemp();
            projection.type = byte_element ? I8Type() : LowTypeOf(element);
            projection.index_projection = lowir_model::IPK_ARRAY_ELEMENT;
            projection.first = TempOperand(decay.dest);
            projection.second = index;
            Emit(projection);
            destination = TempOperand(projection.dest);
            current = element;
          } else {
            const TypeId current_unqualified = types_.Unqualified(current);
            if (types_.Kind(current_unqualified) != TYPE_CLASS)
              Unsupported("an aggregate member field path");
            const ClassEntity& current_class = model_.ClassAt(
                types_.At(current_unqualified).entity);
            TypeId next = 0;
            for (std::size_t field_index = 0;
                 field_index < current_class.fields.size(); ++field_index)
              if (!current_class.fields[field_index].static_member &&
                  current_class.fields[field_index].offset == offset) {
                next = current_class.fields[field_index].type;
                break;
              }
            if (next == 0)
              Unsupported("an aggregate member field path metadata");
            lowir_model::Instruction projection;
            projection.kind = lowir_model::Instruction::IK_INDEX;
            projection.dest = NewTemp();
            projection.type = I8Type();
            projection.index_projection = lowir_model::IPK_FIELD;
            projection.first = destination;
            projection.second = Immediate(static_cast<long long>(offset));
            Emit(projection);
            destination = TempOperand(projection.dest);
            current = next;
          }
        }
        if (value == 0)
          EmitStore(LowTypeOf(field.type), ZeroOperand(field.type),
                    destination);
        else
          EmitStore(LowTypeOf(field.type),
                    LowerRValue(value, field.type).operand, destination);
      }
      ++value_index;
    }
    return;
  }
  if (types_.Kind(unqualified) == TYPE_ARRAY) {
    LowerAggregateMemberArrayLeaves(node, type, root_type, binding, owner,
                                    path);
    return;
  }
  Unsupported("a scalar aggregate member initializer");
}

void Lowerer::LowerAggregateMemberArrayLeaves(
    SemaId node, TypeId type, TypeId root_type, BindingId binding,
    FunctionEntityId owner,
    const std::vector<std::pair<bool, std::size_t> >& path)
{
  const std::vector<SemaId> values = Children(node);
  const TypeId unqualified = types_.Unqualified(type);
  const TypeId element = types_.At(unqualified).base;
  const std::size_t bound = types_.At(unqualified).array_bound;
  for (std::size_t i = 0; i < bound; ++i) {
    std::vector<std::pair<bool, std::size_t> > nested_path(path);
    nested_path.push_back(std::make_pair(true, i));
    const TypeId element_unqualified = types_.Unqualified(element);
    if (i < values.size() &&
        tree_.At(values[i]).kind == SEMA_BRACED_INIT_LIST &&
        (types_.Kind(element_unqualified) == TYPE_CLASS ||
         types_.Kind(element_unqualified) == TYPE_ARRAY))
      LowerAggregateMemberLeaves(values[i], element, root_type, binding, owner,
                                 nested_path);
    else {
      const SemaId value = i < values.size() ? values[i] : 0;
      lowir_model::Instruction load_this;
      load_this.kind = lowir_model::Instruction::IK_LOAD;
      load_this.dest = NewTemp();
      load_this.type = PtrType();
      load_this.first = SlotOperand("$this");
      Emit(load_this);
      lowir_model::Operand destination = TempOperand(load_this.dest);
      const ClassEntity& owner_class = model_.ClassAt(
          model_.FunctionAt(owner).member_class);
      bool found_root = false;
      for (std::size_t root_index = 0;
           root_index < owner_class.fields.size(); ++root_index)
        if (owner_class.fields[root_index].binding == binding) {
          lowir_model::Instruction projection;
          projection.kind = lowir_model::Instruction::IK_INDEX;
          projection.dest = NewTemp();
          projection.type = I8Type();
          projection.index_projection = lowir_model::IPK_FIELD;
          projection.first = destination;
          projection.second = Immediate(static_cast<long long>(
              owner_class.fields[root_index].offset));
          Emit(projection);
          destination = TempOperand(projection.dest);
          found_root = true;
          break;
        }
      if (!found_root)
        Unsupported("an aggregate array member without its root field");
      TypeId current = root_type;
      for (std::size_t step = 0; step < nested_path.size(); ++step) {
        const bool array_path = nested_path[step].first;
        const std::size_t offset = nested_path[step].second;
        const TypeId current_unqualified = types_.Unqualified(current);
        if (array_path) {
          if (types_.Kind(current_unqualified) != TYPE_ARRAY)
            Unsupported("an aggregate array member path");
          const TypeId path_element = types_.At(current_unqualified).base;
          lowir_model::Instruction decay;
          decay.kind = lowir_model::Instruction::IK_UNARY;
          decay.dest = NewTemp();
          decay.op = "decay";
          decay.type = PtrType();
          decay.first = destination;
          Emit(decay);
          lowir_model::Operand index = Immediate(
              static_cast<long long>(offset));
          const std::size_t element_size = types_.SizeOf(path_element);
          const bool byte_element = types_.Kind(
              types_.Unqualified(path_element)) == TYPE_CLASS;
          if (byte_element && element_size != 1) {
            lowir_model::Instruction scale;
            scale.kind = lowir_model::Instruction::IK_BINARY;
            scale.dest = NewTemp();
            scale.op = "mul";
            scale.type = I64Type();
            scale.first = index;
            scale.second = Immediate(static_cast<long long>(element_size));
            Emit(scale);
            index = TempOperand(scale.dest);
          }
          lowir_model::Instruction projection;
          projection.kind = lowir_model::Instruction::IK_INDEX;
          projection.dest = NewTemp();
          projection.type = byte_element ? I8Type() : LowTypeOf(path_element);
          projection.index_projection = lowir_model::IPK_ARRAY_ELEMENT;
          projection.first = TempOperand(decay.dest);
          projection.second = index;
          Emit(projection);
          destination = TempOperand(projection.dest);
          current = path_element;
        } else {
          if (types_.Kind(current_unqualified) != TYPE_CLASS)
            Unsupported("an aggregate array member field path");
          const ClassEntity& current_class = model_.ClassAt(
              types_.At(current_unqualified).entity);
          TypeId next = 0;
          for (std::size_t field_index = 0;
               field_index < current_class.fields.size(); ++field_index)
            if (!current_class.fields[field_index].static_member &&
                current_class.fields[field_index].offset == offset) {
              next = current_class.fields[field_index].type;
              break;
            }
          if (next == 0)
            Unsupported("an aggregate array member field metadata");
          lowir_model::Instruction projection;
          projection.kind = lowir_model::Instruction::IK_INDEX;
          projection.dest = NewTemp();
          projection.type = I8Type();
          projection.index_projection = lowir_model::IPK_FIELD;
          projection.first = destination;
          projection.second = Immediate(static_cast<long long>(offset));
          Emit(projection);
          destination = TempOperand(projection.dest);
          current = next;
        }
      }
      if (value == 0)
        EmitStore(LowTypeOf(element), ZeroOperand(element), destination);
      else
        EmitStore(LowTypeOf(element),
                  LowerRValue(value, element).operand, destination);
    }
  }
}

void Lowerer::RegisterLiveObject(BindingId binding, TypeId type)
{
  if (binding == 0)
    return;
  const ScopeId scope = model_.BindingAt(binding).scope;
  if (scope == 0 || model_.ScopeAt(scope).kind != SCOPE_BLOCK)
    return;
  const LiveObject object(binding, type);
  live_objects_[scope].push_back(object);
  if (shared_return_cleanup_) {
    shared_cleanup_nodes_.push_back(SharedCleanupNode(
        NewBlockLabel("return_cleanup"), shared_cleanup_head_, object));
    shared_cleanup_head_ = shared_cleanup_nodes_.back().label;
  }
}

bool Lowerer::NeedsDestructor(ClassEntityId entity) const
{
  const ClassEntity& owner = model_.ClassAt(entity);
  if (owner.destructor != 0 &&
      !model_.FunctionAt(owner.destructor).synthesized)
    return true;
  for (std::size_t i = 0; i < owner.bases.size(); ++i)
    if (NeedsDestructor(owner.bases[i].entity))
      return true;
  for (std::size_t i = 0; i < owner.fields.size(); ++i) {
    if (owner.fields[i].static_member)
      continue;
    const TypeId field_type = types_.Unqualified(owner.fields[i].type);
    if (types_.Kind(field_type) == TYPE_CLASS &&
        NeedsDestructor(types_.At(field_type).entity))
      return true;
    if (types_.Kind(field_type) == TYPE_ARRAY) {
      const TypeId element = types_.Unqualified(types_.At(field_type).base);
      if (types_.Kind(element) == TYPE_CLASS &&
          NeedsDestructor(types_.At(element).entity))
        return true;
    }
  }
  return false;
}

bool Lowerer::HasSubobjectDestructors(ClassEntityId entity) const
{
  const ClassEntity& owner = model_.ClassAt(entity);
  for (std::size_t i = 0; i < owner.bases.size(); ++i)
    if (NeedsDestructor(owner.bases[i].entity))
      return true;
  for (std::size_t i = 0; i < owner.fields.size(); ++i) {
    if (owner.fields[i].static_member)
      continue;
    const TypeId field_type = types_.Unqualified(owner.fields[i].type);
    if (types_.Kind(field_type) == TYPE_CLASS &&
        NeedsDestructor(types_.At(field_type).entity))
      return true;
    if (types_.Kind(field_type) == TYPE_ARRAY) {
      const TypeId element = types_.Unqualified(types_.At(field_type).base);
      if (types_.Kind(element) == TYPE_CLASS &&
          NeedsDestructor(types_.At(element).entity))
        return true;
    }
  }
  return false;
}

void Lowerer::EmitObjectDestructor(const LiveObject& object)
{
  const TypeId unqualified = types_.Unqualified(object.type);
  if (types_.Kind(unqualified) == TYPE_ARRAY) {
    const TypeId element = types_.Unqualified(types_.At(unqualified).base);
    if (types_.Kind(element) != TYPE_CLASS)
      return;
    const ClassEntityId element_entity = types_.At(element).entity;
    if (!NeedsDestructor(element_entity))
      return;
    const FunctionEntityId destructor =
        model_.ClassAt(element_entity).destructor;
    if (destructor == 0)
      return;
    Value array;
    array.type = object.type;
    array.lvalue = true;
    array.operand = SlotOperand(SlotFor(object.binding));
    const std::size_t bound = types_.At(unqualified).array_bound;
    for (std::size_t i = 0; i < bound; ++i) {
      lowir_model::Instruction call;
      call.kind = lowir_model::Instruction::IK_CALL;
      call.type = VoidType();
      call.call_return_type = VoidType();
      call.call_returns_void = true;
      call.first = GlobalOperand(FunctionSymbolName(destructor));
      call.args.push_back(LowerArrayElementAddress(array, element, i));
      Emit(call);
    }
    return;
  }
  if (types_.Kind(unqualified) != TYPE_CLASS)
    return;
  const ClassEntityId class_entity = types_.At(unqualified).entity;
  if (!NeedsDestructor(class_entity))
    return;
  const FunctionEntityId destructor =
      model_.ClassAt(class_entity).destructor;
  if (destructor == 0)
    return;
  Value value;
  value.type = object.type;
  value.lvalue = true;
  value.operand = SlotOperand(SlotFor(object.binding));
  lowir_model::Instruction call;
  call.kind = lowir_model::Instruction::IK_CALL;
  call.type = VoidType();
  call.call_return_type = VoidType();
  call.call_returns_void = true;
  call.first = GlobalOperand(FunctionSymbolName(destructor));
  call.args.push_back(AddressValue(value).operand);
  Emit(call);
}

void Lowerer::EmitScopeDestructors(ScopeId scope)
{
  const std::map<ScopeId, std::vector<LiveObject> >::const_iterator found =
      live_objects_.find(scope);
  if (found == live_objects_.end())
    return;
  for (std::size_t i = found->second.size(); i != 0; --i)
    EmitObjectDestructor(found->second[i - 1]);
}

void Lowerer::EmitActiveDestructors()
{
  std::set<ScopeId> emitted_scopes;
  for (std::size_t i = lowering_scopes_.size(); i != 0; --i)
    if (emitted_scopes.insert(lowering_scopes_[i - 1]).second)
      EmitScopeDestructors(lowering_scopes_[i - 1]);
}

std::size_t Lowerer::CountReturnStatements(SemaId node) const
{
  if (node == 0)
    return 0;
  std::size_t count = tree_.At(node).kind == SEMA_RETURN_STATEMENT ? 1 : 0;
  for (SemaId child = tree_.At(node).first_child; child != 0;
       child = tree_.At(child).next_sibling)
    count += CountReturnStatements(child);
  return count;
}

void Lowerer::EmitSharedReturn(const Value* value)
{
  if (value != 0)
    EmitStore(function_.return_type, value->operand,
              SlotOperand(shared_return_slot_));
  EmitJump(shared_cleanup_head_.empty() ? shared_return_end_label_ :
           shared_cleanup_head_);
}

void Lowerer::EmitSharedReturnCleanups()
{
  if (!shared_return_cleanup_)
    return;
  for (std::size_t i = 0; i < shared_cleanup_nodes_.size(); ++i) {
    const SharedCleanupNode& cleanup = shared_cleanup_nodes_[i];
    StartBlock(cleanup.label);
    EmitObjectDestructor(cleanup.object);
    EmitJump(cleanup.next.empty() ? shared_return_end_label_ : cleanup.next);
  }
  StartBlock(shared_return_end_label_);
  if (shared_return_slot_.empty()) {
    EmitReturn(0);
    return;
  }
  lowir_model::Instruction load;
  load.kind = lowir_model::Instruction::IK_LOAD;
  load.dest = NewTemp();
  load.type = LowTypeOf(function_return_type_id_);
  load.first = SlotOperand(shared_return_slot_);
  Emit(load);
  Value result;
  result.type = function_return_type_id_;
  result.operand = TempOperand(load.dest);
  EmitReturn(&result);
}

void Lowerer::EmitSubobjectDestructors(ClassEntityId entity)
{
  const ClassEntity& owner = model_.ClassAt(entity);
  for (std::size_t i = owner.fields.size(); i != 0; --i) {
    const ClassField& field = owner.fields[i - 1];
    if (field.static_member)
      continue;
    const TypeId field_type = types_.Unqualified(field.type);
    TypeId element = field_type;
    bool array = false;
    if (types_.Kind(field_type) == TYPE_ARRAY) {
      element = types_.Unqualified(types_.At(field_type).base);
      array = true;
    }
    if (types_.Kind(element) != TYPE_CLASS ||
        !NeedsDestructor(types_.At(element).entity))
      continue;
    const FunctionEntityId destructor =
        model_.ClassAt(types_.At(element).entity).destructor;
    if (destructor == 0)
      continue;
    lowir_model::Instruction load_this;
    load_this.kind = lowir_model::Instruction::IK_LOAD;
    load_this.dest = NewTemp();
    load_this.type = PtrType();
    load_this.first = SlotOperand("$this");
    Emit(load_this);
    lowir_model::Instruction projection;
    projection.kind = lowir_model::Instruction::IK_INDEX;
    projection.dest = NewTemp();
    projection.type = I8Type();
    projection.index_projection = lowir_model::IPK_FIELD;
    projection.first = TempOperand(load_this.dest);
    projection.second = Immediate(static_cast<long long>(field.offset));
    Emit(projection);
    if (array) {
      Value subobject;
      subobject.type = field.type;
      subobject.lvalue = true;
      subobject.operand = TempOperand(projection.dest);
      const std::size_t bound = types_.At(field_type).array_bound;
      for (std::size_t index = 0; index < bound; ++index) {
        lowir_model::Instruction call;
        call.kind = lowir_model::Instruction::IK_CALL;
        call.type = VoidType();
        call.call_return_type = VoidType();
        call.call_returns_void = true;
        call.first = GlobalOperand(FunctionSymbolName(destructor));
        call.args.push_back(LowerArrayElementAddress(subobject, element,
                                                     index));
        Emit(call);
      }
    } else {
      lowir_model::Instruction call;
      call.kind = lowir_model::Instruction::IK_CALL;
      call.type = VoidType();
      call.call_return_type = VoidType();
      call.call_returns_void = true;
      call.first = GlobalOperand(FunctionSymbolName(destructor));
      call.args.push_back(TempOperand(projection.dest));
      Emit(call);
    }
  }
  for (std::size_t i = owner.bases.size(); i != 0; --i) {
    const ClassBase& base = owner.bases[i - 1];
    if (!NeedsDestructor(base.entity))
      continue;
    const FunctionEntityId destructor =
        model_.ClassAt(base.entity).destructor;
    if (destructor == 0)
      continue;
    lowir_model::Instruction load_this;
    load_this.kind = lowir_model::Instruction::IK_LOAD;
    load_this.dest = NewTemp();
    load_this.type = PtrType();
    load_this.first = SlotOperand("$this");
    Emit(load_this);
    lowir_model::Instruction projection;
    projection.kind = lowir_model::Instruction::IK_INDEX;
    projection.dest = NewTemp();
    projection.type = I8Type();
    projection.index_projection = lowir_model::IPK_BASE_SUBOBJECT;
    projection.first = TempOperand(load_this.dest);
    projection.second = Immediate(static_cast<long long>(base.offset));
    Emit(projection);
    lowir_model::Instruction call;
    call.kind = lowir_model::Instruction::IK_CALL;
    call.type = VoidType();
    call.call_return_type = VoidType();
    call.call_returns_void = true;
    call.first = GlobalOperand(FunctionBaseSymbolName(destructor));
    call.args.push_back(TempOperand(projection.dest));
    Emit(call);
  }
}

void Lowerer::EmitDestructorBody(FunctionEntityId function,
                                 SemaId function_node)
{
  const FunctionEntity& entity = model_.FunctionAt(function);
  if (!HasSubobjectDestructors(entity.member_class)) {
    LowerSequence(FunctionBody(function_node));
    return;
  }
  const std::string cleanup_label = NewBlockLabel("destructor_cleanup");
  const std::string end_label = NewBlockLabel("destructor_end");
  lowir_model::Instruction cleanup;
  cleanup.kind = lowir_model::Instruction::IK_EH_CLEANUP;
  cleanup.first = LabelOperand(cleanup_label);
  Emit(cleanup);
  LowerSequence(FunctionBody(function_node));
  if (!Terminated()) {
    lowir_model::Instruction end;
    end.kind = lowir_model::Instruction::IK_EH_END;
    Emit(end);
    EmitSubobjectDestructors(entity.member_class);
    EmitJump(end_label);
  }
  StartBlock(cleanup_label);
  EmitSubobjectDestructors(entity.member_class);
  lowir_model::Instruction end;
  end.kind = lowir_model::Instruction::IK_EH_END;
  Emit(end);
  lowir_model::Instruction resume;
  resume.kind = lowir_model::Instruction::IK_RESUME;
  Emit(resume);
  StartBlock(end_label);
}

FunctionEntityId Lowerer::DefaultConstructor(ClassEntityId entity) const
{
  const ClassEntity& owner = model_.ClassAt(entity);
  if (owner.default_constructor != 0 &&
      !model_.FunctionAt(owner.default_constructor).deleted)
    return owner.default_constructor;
  for (std::size_t i = 0; i < owner.constructors.size(); ++i)
  {
    const FunctionEntity& candidate = model_.FunctionAt(owner.constructors[i]);
    if (candidate.deleted)
      continue;
    const TypeNode& type = types_.At(types_.Unqualified(candidate.type));
    bool viable = true;
    for (std::size_t parameter = 1; parameter < type.parameters.size();
         ++parameter)
      if (parameter >= candidate.default_arguments.size() ||
          candidate.default_arguments[parameter] == 0) {
        viable = false;
        break;
      }
    if (viable)
      return owner.constructors[i];
  }
  return 0;
}

void Lowerer::LowerMemberInitializer(SemaId node, FunctionEntityId owner)
{
  const SemaNode& initializer = tree_.At(node);
  const FunctionEntity& owner_function = model_.FunctionAt(owner);
  const ClassEntity& owner_class =
      model_.ClassAt(owner_function.member_class);
  const std::vector<SemaId> arguments = Children(node);
  const bool is_base = initializer.binding == 0 && initializer.function != 0;
  const bool aggregate_initializer = !is_base &&
      initializer.function == 0 && arguments.size() == 1 &&
      tree_.At(arguments[0]).kind == SEMA_BRACED_INIT_LIST &&
      (types_.Kind(types_.Unqualified(initializer.type)) == TYPE_CLASS ||
       types_.Kind(types_.Unqualified(initializer.type)) == TYPE_ARRAY);
  std::vector<lowir_model::Operand> lowered_arguments;
  bool arguments_lowered = false;
  if (!is_base && !aggregate_initializer) {
    if (initializer.function != 0) {
      const FunctionEntity& constructor =
          model_.FunctionAt(initializer.function);
      const TypeNode& type =
          types_.At(types_.Unqualified(constructor.type));
      for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::size_t parameter = i + 1;
        if (parameter >= type.parameters.size())
          Unsupported("a mem-initializer with too many arguments");
        if (types_.Kind(types_.Unqualified(type.parameters[parameter])) ==
            TYPE_REFERENCE)
          lowered_arguments.push_back(LowerReferenceArgument(
              arguments[i], type.parameters[parameter]).operand);
        else
          lowered_arguments.push_back(LowerRValue(
              arguments[i], type.parameters[parameter]).operand);
      }
    } else {
      if (arguments.size() != 1)
        Unsupported("a scalar mem-initializer with the wrong arity");
      lowered_arguments.push_back(
          LowerRValue(arguments[0], initializer.type).operand);
    }
    arguments_lowered = true;
  }
  if (aggregate_initializer) {
    LowerAggregateMemberInitializer(arguments[0], initializer.type,
                                    initializer.binding, owner);
    return;
  }
  lowir_model::Instruction load_this;
  load_this.kind = lowir_model::Instruction::IK_LOAD;
  load_this.dest = NewTemp();
  load_this.type = PtrType();
  load_this.first = SlotOperand("$this");

  lowir_model::Operand destination = TempOperand(load_this.dest);
  if (is_base) {
    const ClassEntityId target_entity = model_.FunctionAt(
        initializer.function).member_class;
    bool found = false;
    for (std::size_t i = 0; i < owner_class.bases.size(); ++i)
      if (owner_class.bases[i].entity == target_entity) {
        Emit(load_this);
        lowir_model::Instruction projection;
        projection.kind = lowir_model::Instruction::IK_INDEX;
        projection.dest = NewTemp();
        projection.type = I8Type();
        projection.index_projection = lowir_model::IPK_BASE_SUBOBJECT;
        projection.first = destination;
        projection.second = Immediate(static_cast<long long>(
            owner_class.bases[i].offset));
        Emit(projection);
        destination = TempOperand(projection.dest);
        found = true;
        break;
      }
    if (!found)
      Unsupported("a base mem-initializer without layout metadata");
  }
  else {
    ClassField field;
    bool found = false;
    for (std::size_t i = 0; i < owner_class.fields.size(); ++i)
      if (owner_class.fields[i].binding == initializer.binding) {
        field = owner_class.fields[i];
        found = true;
        break;
      }
    if (!found)
      Unsupported("a field mem-initializer without layout metadata");
    Emit(load_this);
    lowir_model::Instruction projection;
    projection.kind = lowir_model::Instruction::IK_INDEX;
    projection.dest = NewTemp();
    projection.type = I8Type();
    projection.index_projection = lowir_model::IPK_FIELD;
    projection.first = destination;
    projection.second = Immediate(static_cast<long long>(field.offset));
    Emit(projection);
    destination = TempOperand(projection.dest);
  }

  if (initializer.function != 0) {
    const FunctionEntity& constructor = model_.FunctionAt(initializer.function);
    if (!is_base && constructor.synthesized && arguments.empty() &&
        types_.SizeOf(initializer.type) == 8) {
      lowir_model::Instruction zero;
      zero.kind = lowir_model::Instruction::IK_STORE;
      zero.type = I64Type();
      zero.first = Immediate(0);
      zero.second = destination;
      Emit(zero);
    }
    const TypeNode& type = types_.At(types_.Unqualified(constructor.type));
    lowir_model::Instruction call;
    call.kind = lowir_model::Instruction::IK_CALL;
    call.type = VoidType();
    call.call_return_type = VoidType();
    call.call_returns_void = true;
    call.first = GlobalOperand(is_base ? FunctionBaseSymbolName(
        initializer.function) : FunctionSymbolName(initializer.function));
    call.args.push_back(destination);
    if (!arguments_lowered) {
      for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::size_t parameter = i + 1;
        if (parameter >= type.parameters.size())
          Unsupported("a mem-initializer with too many arguments");
        if (types_.Kind(types_.Unqualified(type.parameters[parameter])) ==
            TYPE_REFERENCE)
          lowered_arguments.push_back(LowerReferenceArgument(
              arguments[i], type.parameters[parameter]).operand);
        else
          lowered_arguments.push_back(LowerRValue(
              arguments[i], type.parameters[parameter]).operand);
      }
    }
    call.args.insert(call.args.end(), lowered_arguments.begin(),
                     lowered_arguments.end());
    Emit(call);
    return;
  }
  EmitStore(LowTypeOf(initializer.type), lowered_arguments[0], destination);
}

void Lowerer::LowerConstructorInitializers(FunctionEntityId function,
                                           SemaId function_node)
{
  const FunctionEntity& entity = model_.FunctionAt(function);
  const ClassEntity& owner = model_.ClassAt(entity.member_class);
  std::set<ClassEntityId> initialized_bases;
  std::vector<SemaId> member_initializers;
  for (SemaId child = tree_.At(function_node).first_child; child != 0;
       child = tree_.At(child).next_sibling)
    if (tree_.At(child).kind == SEMA_MEMBER_INITIALIZER) {
      member_initializers.push_back(child);
      if (tree_.At(child).binding == 0 && tree_.At(child).function != 0)
        initialized_bases.insert(model_.FunctionAt(
            tree_.At(child).function).member_class);
    }

  // Source order in a mem-initializer list does not control construction
  // order: direct bases are initialized before fields.  Lower arguments in
  // that order as well, so their side effects and projections agree with the
  // object lifetime sequence.
  for (std::size_t pass = 0; pass < 1; ++pass)
    for (std::size_t i = 0; i < member_initializers.size(); ++i) {
      const SemaNode& member = tree_.At(member_initializers[i]);
      const bool is_base = member.binding == 0 && member.function != 0;
      if ((pass == 0) != is_base)
        continue;
      LowerMemberInitializer(member_initializers[i], function);
    }

  // Synthesized and partially specified constructors perform the standard
  // default initialization for each omitted direct subobject.  A trivial
  // subobject has no call, but remains part of the canonical layout.
  for (std::size_t i = 0; i < owner.bases.size(); ++i) {
    const ClassBase& base = owner.bases[i];
    if (initialized_bases.count(base.entity) != 0)
      continue;
    const FunctionEntityId constructor = DefaultConstructor(base.entity);
    if (constructor == 0)
      continue;
    const ClassEntity& base_class = model_.ClassAt(base.entity);
    if (base_class.trivial_default_constructor)
      continue;
    lowir_model::Instruction load_this;
    load_this.kind = lowir_model::Instruction::IK_LOAD;
    load_this.dest = NewTemp();
    load_this.type = PtrType();
    load_this.first = SlotOperand("$this");
    Emit(load_this);
    lowir_model::Instruction projection;
    projection.kind = lowir_model::Instruction::IK_INDEX;
    projection.dest = NewTemp();
    projection.type = I8Type();
    projection.index_projection = lowir_model::IPK_BASE_SUBOBJECT;
    projection.first = TempOperand(load_this.dest);
    projection.second = Immediate(static_cast<long long>(base.offset));
    Emit(projection);
    lowir_model::Instruction call;
    call.kind = lowir_model::Instruction::IK_CALL;
    call.type = VoidType();
    call.call_return_type = VoidType();
    call.call_returns_void = true;
    call.first = GlobalOperand(FunctionBaseSymbolName(constructor));
    call.args.push_back(TempOperand(projection.dest));
    const FunctionEntity& constructor_entity = model_.FunctionAt(constructor);
    const TypeNode& constructor_type = types_.At(types_.Unqualified(
        constructor_entity.type));
    for (std::size_t parameter = 1;
         parameter < constructor_type.parameters.size(); ++parameter) {
      if (parameter >= constructor_entity.default_semantic_arguments.size() ||
          constructor_entity.default_semantic_arguments[parameter] == 0)
        Unsupported("a default base constructor argument without semantics");
      call.args.push_back(LowerRValue(
          constructor_entity.default_semantic_arguments[parameter],
          constructor_type.parameters[parameter]).operand);
    }
    Emit(call);
  }
  for (std::size_t i = 0; i < owner.fields.size(); ++i) {
    const ClassField& field = owner.fields[i];
    if (field.static_member)
      continue;
    SemaId explicit_initializer = 0;
    for (std::size_t j = 0; j < member_initializers.size(); ++j)
      if (tree_.At(member_initializers[j]).binding == field.binding) {
        explicit_initializer = member_initializers[j];
        break;
      }
    if (explicit_initializer != 0) {
      LowerMemberInitializer(explicit_initializer, function);
      continue;
    }
    const TypeId field_type = types_.Unqualified(field.type);
    SemaId default_initializer = 0;
    for (std::size_t j = 0; j < entity.default_member_initializers.size();
         ++j)
      if (entity.default_member_initializers[j].first == field.binding) {
        default_initializer = entity.default_member_initializers[j].second;
        break;
      }
    if (default_initializer != 0) {
      if (tree_.At(default_initializer).kind == SEMA_MEMBER_INITIALIZER) {
        LowerMemberInitializer(default_initializer, function);
        continue;
      }
      if (tree_.At(default_initializer).kind == SEMA_BRACED_INIT_LIST &&
          (types_.Kind(field_type) == TYPE_CLASS ||
           types_.Kind(field_type) == TYPE_ARRAY)) {
        LowerAggregateMemberInitializer(default_initializer, field.type,
                                        field.binding, function);
        continue;
      }
      lowir_model::Instruction load_this;
      load_this.kind = lowir_model::Instruction::IK_LOAD;
      load_this.dest = NewTemp();
      load_this.type = PtrType();
      load_this.first = SlotOperand("$this");
      Emit(load_this);
      lowir_model::Instruction projection;
      projection.kind = lowir_model::Instruction::IK_INDEX;
      projection.dest = NewTemp();
      projection.type = I8Type();
      projection.index_projection = lowir_model::IPK_FIELD;
      projection.first = TempOperand(load_this.dest);
      projection.second = Immediate(static_cast<long long>(field.offset));
      Emit(projection);
      const lowir_model::Operand destination = TempOperand(projection.dest);
      EmitStore(LowTypeOf(field.type),
                LowerRValue(default_initializer, field.type).operand,
                destination);
      continue;
    }
    if (types_.Kind(field_type) != TYPE_CLASS)
      continue;
    const ClassEntity& field_class = model_.ClassAt(types_.At(field_type).entity);
    if (field_class.trivial_default_constructor)
      continue;
    const FunctionEntityId constructor = DefaultConstructor(
        types_.At(field_type).entity);
    if (constructor == 0)
      Unsupported("a member without a default constructor");
    lowir_model::Instruction load_this;
    load_this.kind = lowir_model::Instruction::IK_LOAD;
    load_this.dest = NewTemp();
    load_this.type = PtrType();
    load_this.first = SlotOperand("$this");
    Emit(load_this);
    lowir_model::Instruction projection;
    projection.kind = lowir_model::Instruction::IK_INDEX;
    projection.dest = NewTemp();
    projection.type = I8Type();
    projection.index_projection = lowir_model::IPK_FIELD;
    projection.first = TempOperand(load_this.dest);
    projection.second = Immediate(static_cast<long long>(field.offset));
    Emit(projection);
    lowir_model::Instruction call;
    call.kind = lowir_model::Instruction::IK_CALL;
    call.type = VoidType();
    call.call_return_type = VoidType();
    call.call_returns_void = true;
    call.first = GlobalOperand(FunctionSymbolName(constructor));
    call.args.push_back(TempOperand(projection.dest));
    const FunctionEntity& constructor_entity = model_.FunctionAt(constructor);
    const TypeNode& constructor_type = types_.At(types_.Unqualified(
        constructor_entity.type));
    for (std::size_t parameter = 1;
         parameter < constructor_type.parameters.size(); ++parameter) {
      if (parameter >= constructor_entity.default_semantic_arguments.size() ||
          constructor_entity.default_semantic_arguments[parameter] == 0)
        Unsupported("a default member constructor argument without semantics");
      call.args.push_back(LowerRValue(
          constructor_entity.default_semantic_arguments[parameter],
          constructor_type.parameters[parameter]).operand);
    }
    Emit(call);
  }
}

bool Lowerer::LowerIf(SemaId node)
{
  const std::vector<SemaId> children = Children(node);
  if (children.size() < 2)
    Unsupported("this if statement");
  const bool has_else = children.size() >= 3 &&
      tree_.At(children[2]).kind == SEMA_ELSE;
  const std::string then_label = NewBlockLabel("if_then");
  const std::string else_label = NewBlockLabel("if_else");
  const bool then_terminates = DefinitelyTerminates(children[1]);
  const bool else_terminates = has_else && DefinitelyTerminates(children[2]);
  const bool needs_end = !has_else || !then_terminates || !else_terminates;
  const std::string end_label = needs_end ? NewBlockLabel("if_end") :
      std::string();
  PrepareConditionLabels(children[0]);
  LowerCondition(children[0], then_label, else_label);

  StartBlock(then_label);
  const bool then_done = LowerStatement(children[1]);
  if (!then_done && needs_end)
    EmitJump(end_label);

  StartBlock(else_label);
  bool else_done = false;
  if (has_else)
    else_done = LowerStatement(children[2]);
  if (!else_done && needs_end)
    EmitJump(end_label);

  if (needs_end) {
    StartBlock(end_label);
    return false;
  }
  return then_done && else_done;
}

bool Lowerer::LowerLoop(SemaId node, SemaKind kind)
{
  const std::vector<SemaId> children = Children(node);
  if (children.size() < 2)
    Unsupported("this loop statement");
  const bool is_do = kind == SEMA_DO_STATEMENT;
  const std::string first_label =
      NewBlockLabel(is_do ? "do_body" : "while_cond");
  const std::string second_label =
      NewBlockLabel(is_do ? "do_cond" : "while_body");
  const std::string end_label = NewBlockLabel(is_do ? "do_end" : "while_end");
  const SemaId condition = is_do ? children[1] : children[0];
  const SemaId body = is_do ? children[0] : children[1];
  EmitJump(first_label);
  StartBlock(first_label);
  if (is_do) {
    PushControl(end_label, second_label);
    if (!LowerStatement(body))
      EmitJump(second_label);
    PopControl();
    StartBlock(second_label);
    PrepareConditionLabels(condition);
    LowerCondition(condition, first_label, end_label);
  } else {
    PrepareConditionLabels(condition);
    LowerCondition(condition, second_label, end_label);
    StartBlock(second_label);
    PushControl(end_label, first_label);
    if (!LowerStatement(body))
      EmitJump(first_label);
    PopControl();
  }
  StartBlock(end_label);
  return false;
}

// The semantic for-statement carries only the parts the source wrote; each
// is recognized by kind, and a missing condition loops unconditionally.
bool Lowerer::LowerFor(SemaId node)
{
  SemaId init = 0;
  SemaId condition = 0;
  SemaId iteration = 0;
  SemaId body = 0;
  for (SemaId child = tree_.At(node).first_child; child != 0;
       child = tree_.At(child).next_sibling) {
    switch (tree_.At(child).kind) {
    case SEMA_FOR_INIT_STATEMENT: init = child; break;
    case SEMA_CONDITION: condition = child; break;
    case SEMA_ITERATION: iteration = child; break;
    default: body = child; break;
    }
  }
  const std::string condition_label = NewBlockLabel("for_cond");
  const std::string body_label = NewBlockLabel("for_body");
  const std::string iteration_label = NewBlockLabel("for_iter");
  const std::string end_label = NewBlockLabel("for_end");
  if (init != 0)
    (void)LowerStatement(init);
  EmitJump(condition_label);
  StartBlock(condition_label);
  if (condition != 0) {
    PrepareConditionLabels(condition);
    LowerCondition(condition, body_label, end_label);
  } else {
    EmitJump(body_label);
  }
  StartBlock(body_label);
  PushControl(end_label, iteration_label);
  const bool body_done = body != 0 && LowerStatement(body);
  if (!body_done)
    EmitJump(iteration_label);
  PopControl();
  StartBlock(iteration_label);
  if (iteration != 0)
    (void)LowerStatement(iteration);
  EmitJump(condition_label);
  StartBlock(end_label);
  return false;
}

// Case and default labels of one switch in source order, without entering
// nested switches whose labels belong to them.
void Lowerer::CollectSwitchLabels(SemaId node,
                                  std::map<SemaId, std::string>& labels,
                                  std::vector<SemaId>& cases,
                                  SemaId& default_node)
{
  if (node == 0)
    return;
  const SemaKind kind = tree_.At(node).kind;
  if (kind == SEMA_CASE_STATEMENT) {
    if (labels.find(node) == labels.end()) {
      labels[node] = NewBlockLabel("switch_case");
      cases.push_back(node);
    }
  } else if (kind == SEMA_DEFAULT_STATEMENT && default_node == 0) {
    default_node = node;
    labels[node] = NewBlockLabel("switch_default");
  }
  if (kind == SEMA_SWITCH_STATEMENT)
    return;
  for (SemaId child = tree_.At(node).first_child; child != 0;
       child = tree_.At(child).next_sibling)
    CollectSwitchLabels(child, labels, cases, default_node);
}

void Lowerer::LowerSwitchCase(
    SemaId node, const std::map<SemaId, std::string>& labels)
{
  const std::map<SemaId, std::string>::const_iterator found =
      labels.find(node);
  if (found == labels.end())
    Unsupported("this switch label");
  StartBlock(found->second);
  const std::vector<SemaId> children = Children(node);
  std::size_t start = 0;
  if (!children.empty() && tree_.At(children[0]).kind == SEMA_LITERAL)
    start = 1;
  for (std::size_t i = start; i < children.size(); ++i) {
    const SemaKind kind = tree_.At(children[i]).kind;
    if (kind == SEMA_CASE_STATEMENT || kind == SEMA_DEFAULT_STATEMENT) {
      EmitJump(labels.find(children[i])->second);
      LowerSwitchCase(children[i], labels);
    } else if (!Terminated()) {
      (void)LowerStatement(children[i]);
    }
  }
}

bool Lowerer::LowerSwitch(SemaId node)
{
  const std::vector<SemaId> children = Children(node);
  if (children.size() < 2)
    Unsupported("this switch statement");
  const SemaId condition = children[0];
  const SemaId body = children[1];
  const std::string dispatch_label = NewBlockLabel("switch_dispatch");
  const std::string end_label = NewBlockLabel("switch_end");
  std::map<SemaId, std::string> labels;
  std::vector<SemaId> case_nodes;
  SemaId default_node = 0;
  const std::vector<SemaId> body_children = Children(body);
  for (std::size_t i = 0; i < body_children.size(); ++i) {
    const SemaKind kind = tree_.At(body_children[i]).kind;
    if (kind == SEMA_CASE_STATEMENT || kind == SEMA_DEFAULT_STATEMENT)
      CollectSwitchLabels(body_children[i], labels, case_nodes,
                          default_node);
  }
  // Without a default arm an unmatched selector leaves through the join
  // block; the join label doubles as the dispatch default.
  if (default_node == 0)
    labels[0] = end_label;
  bool switch_terminates = default_node != 0 &&
      !HasSwitchBreak(body, tree_) && DefinitelyTerminates(default_node);
  for (std::size_t i = 0; i < case_nodes.size() && switch_terminates; ++i)
    switch_terminates = DefinitelyTerminates(case_nodes[i]);

  SemaId condition_value = condition;
  if (tree_.At(condition_value).kind == SEMA_CONDITION) {
    const std::vector<SemaId> wrapped = Children(condition_value);
    if (wrapped.size() != 1)
      Unsupported("this switch condition");
    condition_value = wrapped[0];
  }
  const Value selector =
      tree_.At(condition_value).kind == SEMA_CONDITION_DECLARATION ?
      LowerConditionVariable(condition_value) : LowerRValue(condition_value);
  EmitJump(dispatch_label);
  StartBlock(dispatch_label);
  lowir_model::Instruction dispatch;
  dispatch.kind = lowir_model::Instruction::IK_SWITCH;
  dispatch.first = selector.operand;
  dispatch.second = LabelOperand(labels[default_node]);
  for (std::size_t i = 0; i < case_nodes.size(); ++i) {
    const std::vector<SemaId> case_children = Children(case_nodes[i]);
    if (case_children.empty() || !tree_.At(case_children[0]).has_value)
      Unsupported("a non-constant case label");
    dispatch.args.push_back(Immediate(tree_.At(case_children[0]).value));
    dispatch.args.push_back(LabelOperand(labels[case_nodes[i]]));
  }
  Emit(dispatch);

  const std::map<SemaId, std::string>* previous_switch_labels =
      active_switch_labels_;
  active_switch_labels_ = &labels;
  PushControl(end_label, std::string());
  bool in_case = false;
  for (std::size_t i = 0; i < body_children.size(); ++i) {
    const SemaKind kind = tree_.At(body_children[i]).kind;
    if (kind == SEMA_CASE_STATEMENT || kind == SEMA_DEFAULT_STATEMENT) {
      // Fall through to the next label in source order.
      if (in_case && !Terminated())
        EmitJump(labels[body_children[i]]);
      LowerSwitchCase(body_children[i], labels);
      in_case = true;
    } else if (!Terminated()) {
      (void)LowerStatement(body_children[i]);
    }
  }
  if (!Terminated())
    EmitJump(end_label);
  PopControl();
  active_switch_labels_ = previous_switch_labels;
  if (switch_terminates && previous_switch_labels == 0)
    return true;
  StartBlock(end_label);
  return false;
}

void Lowerer::LowerReturn(SemaId node)
{
  const std::vector<SemaId> children = Children(node);
  if (children.empty()) {
    if (shared_return_cleanup_)
      EmitSharedReturn(0);
    else {
      EmitActiveDestructors();
      EmitReturn(0);
    }
    return;
  }
  if (types_.Kind(function_return_type_id_) == TYPE_REFERENCE) {
    // A reference result is the address of the selected object.
    const SemaNode& expression = tree_.At(children[0]);
    Value result = expression.kind == SEMA_CALL ?
        LowerCall(children[0], 0) : LowerLValue(children[0]);
    if (result.operand.kind == lowir_model::Operand::OP_SLOT ||
        result.operand.kind == lowir_model::Operand::OP_GLOBAL)
      result = AddressValue(result);
    result.type = function_return_type_id_;
    if (shared_return_cleanup_)
      EmitSharedReturn(&result);
    else {
      EmitActiveDestructors();
      EmitReturn(&result);
    }
    return;
  }
  const Value result = Convert(LowerRValue(children[0]),
                               function_return_type_id_);
  if (shared_return_cleanup_)
    EmitSharedReturn(&result);
  else {
    EmitActiveDestructors();
    EmitReturn(&result);
  }
}

bool Lowerer::LowerStatement(SemaId node)
{
  if (node == 0)
    return false;
  const SemaNode& value = tree_.At(node);
  switch (value.kind) {
  case SEMA_COMPOUND_STATEMENT:
  case SEMA_THEN:
  case SEMA_ELSE:
    return LowerSequence(node);
  case SEMA_FOR_INIT_STATEMENT: {
    const std::vector<SemaId> children = Children(node);
    if (children.empty())
      return false;
    if (children.size() != 1)
      Unsupported("this for-init statement");
    if (tree_.At(children[0]).kind == SEMA_SIMPLE_DECLARATION)
      LowerVariableDeclaration(children[0]);
    else
      LowerDiscard(children[0]);
    return false;
  }
  case SEMA_ITERATION: {
    const std::vector<SemaId> children = Children(node);
    if (children.size() == 1 &&
        tree_.At(children[0]).kind != SEMA_EXPRESSION_STATEMENT)
      LowerDiscard(children[0]);
    else
      (void)LowerSequence(node);
    return false;
  }
  case SEMA_SIMPLE_DECLARATION:
    LowerVariableDeclaration(node);
    return false;
  case SEMA_EXPRESSION_STATEMENT: {
    const std::vector<SemaId> children = Children(node);
    if (!children.empty())
      LowerDiscard(children[0]);
    return false;
  }
  case SEMA_RETURN_STATEMENT:
    LowerReturn(node);
    return true;
  case SEMA_IF_STATEMENT: return LowerIf(node);
  case SEMA_WHILE_STATEMENT: return LowerLoop(node, SEMA_WHILE_STATEMENT);
  case SEMA_DO_STATEMENT: return LowerLoop(node, SEMA_DO_STATEMENT);
  case SEMA_FOR_STATEMENT: return LowerFor(node);
  case SEMA_SWITCH_STATEMENT: return LowerSwitch(node);
  case SEMA_BREAK_STATEMENT:
    if (CurrentBreak().empty())
      Unsupported("a break outside a breakable statement");
    EmitJump(CurrentBreak());
    return true;
  case SEMA_CONTINUE_STATEMENT:
    if (CurrentContinue().empty())
      Unsupported("a continue outside a loop");
    EmitJump(CurrentContinue());
    return true;
  case SEMA_LABELED_STATEMENT: {
    const std::vector<SemaId> children = Children(node);
    if (children.size() != 1)
      Unsupported("this labeled statement");
    const std::string label = GotoLabel(value);
    EmitJump(label);
    StartBlock(label);
    return LowerStatement(children[0]);
  }
  case SEMA_GOTO_STATEMENT:
    EmitJump(GotoLabel(value));
    return true;
  case SEMA_CASE_STATEMENT:
  case SEMA_DEFAULT_STATEMENT: {
    if (active_switch_labels_ == 0)
      Unsupported("a case label outside a switch");
    const std::map<SemaId, std::string>::const_iterator found =
        active_switch_labels_->find(node);
    if (found == active_switch_labels_->end())
      Unsupported("this switch label");
    EmitJump(found->second);
    LowerSwitchCase(node, *active_switch_labels_);
    return true;
  }
  case SEMA_CONDITION:
  case SEMA_CONDITION_DECLARATION:
    Unsupported("a condition outside a selection");
    break;
  default:
    Unsupported("this statement");
  }
  return false;
}

bool Lowerer::LowerSequence(SemaId node)
{
  const SemaKind kind = tree_.At(node).kind;
  const bool tracks_scope =
      (kind == SEMA_COMPOUND_STATEMENT || kind == SEMA_THEN ||
       kind == SEMA_ELSE) && tree_.At(node).scope != 0;
  if (tracks_scope) {
    lowering_scopes_.push_back(tree_.At(node).scope);
    if (shared_return_cleanup_)
      shared_cleanup_scope_heads_.push_back(shared_cleanup_head_);
  }
  bool terminated = false;
  for (SemaId child = tree_.At(node).first_child; child != 0;
       child = tree_.At(child).next_sibling) {
    // A goto terminates the current block, but a later source label starts
    // a new reachable block and must still be lowered for that edge.
    if (terminated && tree_.At(child).kind != SEMA_LABELED_STATEMENT)
      continue;
    terminated = LowerStatement(child);
  }
  if (tracks_scope) {
    if (!terminated)
      EmitScopeDestructors(tree_.At(node).scope);
    if (shared_return_cleanup_) {
      shared_cleanup_head_ = shared_cleanup_scope_heads_.back();
      shared_cleanup_scope_heads_.pop_back();
    }
    lowering_scopes_.pop_back();
  }
  return terminated;
}

}  // namespace lowir_lowering
