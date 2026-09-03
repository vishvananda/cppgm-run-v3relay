// Per-function state: blocks, temporaries, slots, terminators, and the
// function definition builder.
#include "lower/lowir_lowering.h"

#include <stdexcept>
#include <utility>

namespace lowir_lowering {

Lowerer::Lowerer(ProgramLowering& shared, const std::vector<Pa6Token>& tokens,
                 SemaModel& model, const SemaTree& tree)
    : shared_(shared), tokens_(tokens), model_(model), tree_(tree),
      types_(model.Types()), program_(shared.program_), current_block_(0),
      active_switch_labels_(0), shared_cleanup_scope_heads_(),
      shared_cleanup_nodes_(), shared_cleanup_head_(),
      shared_return_end_label_(), shared_return_slot_(),
      shared_return_cleanup_(false), exception_guard_depth_(0),
      defer_next_call_guard_(false), deferred_call_guard_active_(false),
      deferred_call_handler_reused_(false), deferred_call_cleanup_(),
      deferred_call_end_(),
      temp_counter_(0), label_counter_(0),
      generated_slot_counter_(0), function_return_type_id_(0),
      building_base_variant_(false), return_slot_binding_(0)
{
}

std::vector<SemaId> Lowerer::Children(SemaId node) const
{
  std::vector<SemaId> result;
  if (node == 0)
    return result;
  for (SemaId child = tree_.At(node).first_child; child != 0;
       child = tree_.At(child).next_sibling)
    result.push_back(child);
  return result;
}

void Lowerer::ResetFunction(const std::string& name,
                            const lowir_model::LowType& return_type)
{
  function_ = lowir_model::Function();
  function_.name = name;
  function_.return_type = return_type;
  current_block_ = 0;
  block_labels_.clear();
  slot_names_.clear();
  slots_.clear();
  parameter_addresses_.clear();
  temporaries_.clear();
  pending_temporaries_.clear();
  parameter_destructors_.clear();
  cleanup_handler_labels_.clear();
  initialized_bitfield_units_.clear();
  goto_labels_.clear();
  condition_labels_.clear();
  controls_.clear();
  lowering_scopes_.clear();
  live_objects_.clear();
  shared_cleanup_scope_heads_.clear();
  shared_cleanup_nodes_.clear();
  shared_cleanup_head_.clear();
  shared_return_end_label_.clear();
  shared_return_slot_.clear();
  shared_return_cleanup_ = false;
  exception_guard_depth_ = 0;
  defer_next_call_guard_ = false;
  deferred_call_guard_active_ = false;
  deferred_call_handler_reused_ = false;
  deferred_call_cleanup_.clear();
  deferred_call_end_.clear();
  temp_counter_ = 0;
  label_counter_ = 0;
  generated_slot_counter_ = 0;
  function_return_type_id_ = 0;
  return_slot_binding_ = 0;
}

// The startup initializer is one program-wide function: each unit resumes
// it with the counters the previous unit left, appends its stores, and hands
// it back.
void Lowerer::ResumeInitFunction()
{
  ResetFunction("@__cppgm_init", VoidType());
  if (!shared_.has_init_) {
    function_.metadata.role = lowir_model::SR_INIT;
    function_.metadata.binding = lowir_model::SBM_INTERNAL;
    StartBlock("^entry");
    return;
  }
  function_ = std::move(shared_.init_function_);
  temp_counter_ = shared_.init_temp_counter_;
  label_counter_ = shared_.init_label_counter_;
  generated_slot_counter_ = shared_.init_slot_counter_;
  for (std::size_t i = 0; i < function_.slots.size(); ++i)
    slot_names_.insert(function_.slots[i].first);
  for (std::size_t i = 0; i < function_.blocks.size(); ++i)
    block_labels_.insert(function_.blocks[i].label);
  current_block_ = function_.blocks.size() - 1;
}

void Lowerer::SuspendInitFunction()
{
  shared_.init_function_ = std::move(function_);
  shared_.has_init_ = true;
  shared_.init_temp_counter_ = temp_counter_;
  shared_.init_label_counter_ = label_counter_;
  shared_.init_slot_counter_ = generated_slot_counter_;
}

void Lowerer::ResumeFiniFunction()
{
  ResetFunction("@__cppgm_fini", VoidType());
  if (!shared_.has_fini_) {
    function_.metadata.role = lowir_model::SR_FINI;
    function_.metadata.binding = lowir_model::SBM_INTERNAL;
    StartBlock("^entry");
    return;
  }
  function_ = std::move(shared_.fini_function_);
  temp_counter_ = shared_.fini_temp_counter_;
  label_counter_ = shared_.fini_label_counter_;
  generated_slot_counter_ = shared_.fini_slot_counter_;
  for (std::size_t i = 0; i < function_.slots.size(); ++i)
    slot_names_.insert(function_.slots[i].first);
  for (std::size_t i = 0; i < function_.blocks.size(); ++i)
    block_labels_.insert(function_.blocks[i].label);
  current_block_ = function_.blocks.size() - 1;
}

void Lowerer::SuspendFiniFunction()
{
  shared_.fini_function_ = std::move(function_);
  shared_.has_fini_ = true;
  shared_.fini_temp_counter_ = temp_counter_;
  shared_.fini_label_counter_ = label_counter_;
  shared_.fini_slot_counter_ = generated_slot_counter_;
}

lowir_model::Block& Lowerer::CurrentBlock()
{
  if (current_block_ >= function_.blocks.size())
    throw std::logic_error("internal LowIR error: no current block");
  return function_.blocks[current_block_];
}

// Blocks appear in the order they are started; every label is started once.
void Lowerer::StartBlock(const std::string& label)
{
  if (!block_labels_.insert(label).second)
    throw std::logic_error("internal LowIR error: block started twice: " +
                           label);
  lowir_model::Block block;
  block.label = label;
  function_.blocks.push_back(block);
  current_block_ = function_.blocks.size() - 1;
}

std::string Lowerer::NewBlockLabel(const std::string& stem)
{
  return "^" + stem + "_" + std::to_string(++label_counter_);
}

std::string Lowerer::NewTemp()
{
  return "%t" + std::to_string(++temp_counter_);
}

void Lowerer::AddSlot(const std::string& name,
                      const lowir_model::LowType& type)
{
  function_.slots.push_back(std::make_pair(name, type));
  slot_names_.insert(name);
}

// Generated slots share one per-function counter and skip every number
// whose name a source declaration already owns.
std::string Lowerer::NewGeneratedSlot(const std::string& stem,
                                      const lowir_model::LowType& type)
{
  while (true) {
    const std::string candidate =
        "$" + stem + "__" + std::to_string(++generated_slot_counter_);
    if (slot_names_.count(candidate) == 0) {
      AddSlot(candidate, type);
      return candidate;
    }
  }
}

// Labels carry the per-function ordinal the semantic layer assigned; the
// block is named on the first mention, by a goto or by the label itself.
std::string Lowerer::GotoLabel(const SemaNode& node)
{
  if (!node.has_value || node.value <= 0)
    Unsupported("a label without a semantic ordinal");
  const std::size_t ordinal = static_cast<std::size_t>(node.value);
  if (goto_labels_.size() <= ordinal)
    goto_labels_.resize(ordinal + 1);
  if (goto_labels_[ordinal].empty())
    goto_labels_[ordinal] = NewBlockLabel("goto");
  return goto_labels_[ordinal];
}

void Lowerer::Emit(const lowir_model::Instruction& instruction)
{
  CurrentBlock().instructions.push_back(instruction);
}

bool Lowerer::Terminated() const
{
  if (current_block_ >= function_.blocks.size())
    return false;
  const lowir_model::Block& block = function_.blocks[current_block_];
  if (block.instructions.empty())
    return false;
  const lowir_model::Instruction::Kind kind = block.instructions.back().kind;
  return kind == lowir_model::Instruction::IK_RETURN ||
      kind == lowir_model::Instruction::IK_JUMP ||
      kind == lowir_model::Instruction::IK_BRANCH ||
      kind == lowir_model::Instruction::IK_SWITCH;
}

void Lowerer::EmitJump(const std::string& label)
{
  if (Terminated())
    return;
  lowir_model::Instruction instruction;
  instruction.kind = lowir_model::Instruction::IK_JUMP;
  instruction.first = LabelOperand(label);
  Emit(instruction);
}

void Lowerer::EmitBranch(const lowir_model::Operand& condition,
                         const std::string& true_label,
                         const std::string& false_label)
{
  lowir_model::Instruction instruction;
  instruction.kind = lowir_model::Instruction::IK_BRANCH;
  instruction.first = condition;
  instruction.second = LabelOperand(true_label);
  instruction.third = LabelOperand(false_label);
  Emit(instruction);
}

void Lowerer::EmitStore(const lowir_model::LowType& type,
                        const lowir_model::Operand& value,
                        const lowir_model::Operand& destination)
{
  lowir_model::Instruction store;
  store.kind = lowir_model::Instruction::IK_STORE;
  store.type = type;
  store.first = value;
  store.second = destination;
  Emit(store);
}

// A trivial object copy of `type`'s complete size from a pointer or an
// object-typed value into the object at `destination`.
void Lowerer::EmitCopyObject(TypeId type, const lowir_model::Operand& source,
                             const lowir_model::Operand& destination)
{
  EmitCopyObjectBytes(types_.SizeOf(type), types_.AlignOf(type), source,
                      destination);
}

void Lowerer::EmitCopyObjectBytes(
    std::size_t bytes, std::size_t alignment,
    const lowir_model::Operand& source,
    const lowir_model::Operand& destination)
{
  if (bytes == 0)
    return;
  lowir_model::Instruction copy;
  copy.kind = lowir_model::Instruction::IK_COPYOBJ;
  copy.byte_count = bytes;
  copy.byte_alignment = alignment == 0 ? 1 : alignment;
  copy.first = source;
  copy.second = destination;
  Emit(copy);
}

void Lowerer::EmitVoidCall(
    const std::string& symbol,
    const std::vector<lowir_model::Operand>& arguments)
{
  lowir_model::Instruction call;
  call.kind = lowir_model::Instruction::IK_CALL;
  call.type = VoidType();
  call.call_return_type = VoidType();
  call.call_returns_void = true;
  call.first = GlobalOperand(symbol);
  call.args = arguments;
  Emit(call);
}

void Lowerer::EmitReturn(const Value* value)
{
  lowir_model::Instruction instruction;
  instruction.kind = lowir_model::Instruction::IK_RETURN;
  instruction.type = function_.return_type;
  if (value != 0)
    instruction.first = value->operand;
  Emit(instruction);
}

SemaId Lowerer::FunctionBody(SemaId function_node) const
{
  for (SemaId child = tree_.At(function_node).first_child; child != 0;
       child = tree_.At(child).next_sibling)
    if (tree_.At(child).kind == SEMA_COMPOUND_STATEMENT)
      return child;
  return 0;
}

void Lowerer::CollectParameters(SemaId function_node,
                                std::vector<SemaId>& parameters) const
{
  if (function_node == 0)
    return;
  for (SemaId child = tree_.At(function_node).first_child; child != 0;
       child = tree_.At(child).next_sibling)
    if (tree_.At(child).kind == SEMA_PARAMETER)
      parameters.push_back(child);
}

// Source slots are declared in encounter order over the whole body, nested
// blocks included, so the frame layout is fixed before any statement is
// lowered.
void Lowerer::CollectSlots(SemaId node, std::set<BindingId>& seen,
                           bool in_class_initializer)
{
  if (node == 0)
    return;
  const SemaNode& value = tree_.At(node);
  if (value.kind == SEMA_FUNCTION_DEFINITION ||
      value.kind == SEMA_FUNCTION_DECLARATION)
    return;
  if (value.kind == SEMA_VARIABLE && value.binding != 0 &&
      !model_.BindingAt(value.binding).extern_declaration &&
      value.binding != return_slot_binding_ &&
      seen.insert(value.binding).second)
    AddSourceSlot(value.binding);
  if (value.kind == SEMA_CONSTRUCTOR_ACTION && value.binding == 0 &&
      value.function != 0 && value.user_defined_conversion &&
      in_class_initializer)
  {
    TemporaryObject& temporary = temporaries_[node];
    if (temporary.slot.empty())
    {
      temporary.slot = NewGeneratedSlot(
          ConstructorTemporaryIsObjectExpression(node) ? "tmpobj" : "arg",
          LowTypeOf(value.type));
    }
  }
  const bool child_in_class_initializer = in_class_initializer ||
      (value.kind == SEMA_VARIABLE &&
       types_.Kind(types_.Unqualified(value.type)) == TYPE_CLASS);
  for (SemaId child = value.first_child; child != 0;
       child = tree_.At(child).next_sibling)
    CollectSlots(child, seen, child_in_class_initializer);
}

const std::string& Lowerer::SlotFor(BindingId binding) const
{
  const std::map<BindingId, std::string>::const_iterator found =
      slots_.find(binding);
  if (found == slots_.end())
    Unsupported("an object without a storage slot");
  return found->second;
}

void Lowerer::AddSourceSlot(BindingId binding)
{
  if (slots_.find(binding) != slots_.end())
    return;
  const Binding& value = model_.BindingAt(binding);
  const std::string base = value.name.empty() ?
      "$__local" + std::to_string(++generated_slot_counter_) :
      "$" + value.name;
  std::string name = base;
  for (unsigned suffix = 2; slot_names_.count(name) != 0; ++suffix)
    name = base + "__shadow" + std::to_string(suffix);
  AddSlot(name, LowTypeOf(value.type));
  slots_[binding] = name;
}

void Lowerer::AddParameterSlots(SemaId function_node)
{
  const FunctionEntity& entity =
      model_.FunctionAt(tree_.At(function_node).function);
  const TypeNode& type = types_.At(types_.Unqualified(entity.type));
  std::vector<SemaId> parameters;
  CollectParameters(function_node, parameters);
  for (std::size_t i = 0; i < type.parameters.size(); ++i) {
    std::string base = "$__param" + std::to_string(i);
    BindingId binding = 0;
    if (i < parameters.size()) {
      binding = tree_.At(parameters[i]).binding;
      if (binding != 0 && !model_.BindingAt(binding).name.empty())
        base = "$" + model_.BindingAt(binding).name;
    }
    std::string name = base;
    for (unsigned suffix = 2; slot_names_.count(name) != 0; ++suffix)
      name = base + "__" + std::to_string(suffix);
    AddSlot(name, LowTypeOf(type.parameters[i]));
    if (binding != 0)
      slots_[binding] = name;
  }
}

lowir_model::Function Lowerer::BuildFunction(const FunctionSymbol& symbol)
{
  return BuildFunctionVariant(symbol, false);
}

lowir_model::Function Lowerer::BuildFunctionVariant(
    const FunctionSymbol& symbol, bool base_variant)
{
  const SemaId node = symbol.definition;
  const FunctionEntityId id = tree_.At(node).function;
  const FunctionEntity& entity = model_.FunctionAt(id);
  const TypeNode& type = types_.At(types_.Unqualified(entity.type));
  const ClassBoundaryMode result_boundary =
      ClassBoundary(type.result, true);
  building_base_variant_ = base_variant;
  ResetFunction(base_variant ? symbol.base_name : symbol.name,
                result_boundary == CBM_INDIRECT_RESULT ? VoidType() :
                LowTypeOf(type.result));
  function_return_type_id_ = type.result;
  function_.boundary.arity = type.variadic ? lowir_model::CAM_VARIADIC :
      lowir_model::CAM_FIXED;
  function_.metadata.binding = entity.internal_linkage ?
      lowir_model::SBM_INTERNAL : entity.in_class_definition ?
      lowir_model::SBM_WEAK : lowir_model::SBM_STRONG;
  function_.metadata.linkage = entity.c_linkage ? lowir_model::LLM_C :
      lowir_model::LLM_DEFAULT;
  if (entity.noexcept_qualifier)
    function_.boundary.unwind = lowir_model::CUM_NO;
  if (entity.special_member != SPECIAL_MEMBER_NONE &&
      entity.member_class != 0) {
    const ClassEntity& owner = model_.ClassAt(entity.member_class);
    const bool trivial = entity.special_member == SPECIAL_MEMBER_CONSTRUCTOR ?
        owner.trivial_default_constructor : owner.trivial_destructor;
    if (trivial) {
      function_.boundary.unwind = lowir_model::CUM_NO;
      function_.metadata.object_trivial_lifecycle = true;
    }
  }
  const bool is_main = entity.name == "main" &&
      entity.scope == model_.GlobalScope();
  if (is_main) {
    function_.metadata.role = lowir_model::SR_ENTRY;
    function_.metadata.keep_internal_alias = true;
  } else if (!entity.c_linkage) {
    function_.metadata.object_symbol = base_variant ? symbol.base_object :
        symbol.object;
  }

  std::vector<SemaId> parameters;
  CollectParameters(node, parameters);
  const std::size_t parameter_offset =
      result_boundary == CBM_INDIRECT_RESULT ? 1 : 0;
  if (result_boundary == CBM_INDIRECT_RESULT) {
    lowir_model::Parameter result_parameter;
    result_parameter.name = "%ret";
    result_parameter.type = PtrType();
    result_parameter.metadata.passing = lowir_model::PPM_INDIRECT_RESULT;
    function_.params.push_back(result_parameter);
  }
  for (std::size_t i = 0; i < type.parameters.size(); ++i) {
    lowir_model::Parameter parameter;
    parameter.name = "%__param" + std::to_string(i);
    if (i < parameters.size()) {
      const BindingId binding = tree_.At(parameters[i]).binding;
      if (binding != 0 && !model_.BindingAt(binding).name.empty())
        parameter.name = "%" + model_.BindingAt(binding).name;
      if (types_.Kind(types_.Unqualified(type.parameters[i])) ==
          TYPE_REFERENCE)
        parameter.metadata.passing = lowir_model::PPM_REFERENCE;
    }
    if (ClassBoundary(type.parameters[i], false) == CBM_BY_ADDRESS) {
      parameter.type = PtrType();
      parameter.metadata.passing = lowir_model::PPM_BY_ADDRESS;
    } else {
      parameter.type = LowTypeOf(type.parameters[i]);
    }
    function_.params.push_back(parameter);
  }

  // Parameter names share the textual namespace of generated temporaries.
  // Reserve every %tN a parameter spells before the body allocates any.
  for (std::size_t i = 0; i < function_.params.size(); ++i) {
    const std::string& name = function_.params[i].name;
    if (name.size() <= 2 || name.compare(0, 2, "%t") != 0)
      continue;
    unsigned value = 0;
    bool digits = true;
    for (std::size_t j = 2; j < name.size() && digits; ++j) {
      digits = name[j] >= '0' && name[j] <= '9';
      value = value * 10 + static_cast<unsigned>(name[j] - '0');
    }
    if (digits && value > temp_counter_)
      temp_counter_ = value;
  }

  AddParameterSlots(node);
  for (std::size_t i = 0; i < type.parameters.size(); ++i) {
    // The implicit object of a member function is borrowed.  Explicit
    // by-address class parameters, however, are complete objects whose
    // destructor runs on every function exit, including an empty user body.
    if (entity.is_member && !entity.static_member && i == 0)
      continue;
    const TypeId parameter_type = types_.Unqualified(type.parameters[i]);
    if (types_.Kind(parameter_type) != TYPE_CLASS ||
        ClassBoundary(parameter_type, false) != CBM_BY_ADDRESS)
      continue;
    const ClassEntityId class_entity = types_.At(parameter_type).entity;
    const ClassEntity& owner = model_.ClassAt(class_entity);
    if (owner.destructor == 0)
      continue;
    const FunctionEntity& destructor = model_.FunctionAt(owner.destructor);
    // Class completion may have installed a synthesized placeholder before
    // the source destructor is visited.  Its `body` is the stable ownership
    // fact that distinguishes that later user definition from a genuinely
    // trivial implicit destructor.
    if (owner.trivial_destructor && destructor.body == 0)
      continue;
    parameter_destructors_.push_back(ParameterObject(
        parameter_type,
        TempOperand(function_.params[i + parameter_offset].name)));
  }
  const SemaId body = FunctionBody(node);
  if (body == 0)
    Unsupported("a function without a body");
  return_slot_binding_ = ReturnSlotBinding(body, type.result);
  if (return_slot_binding_ != 0)
    parameter_addresses_[return_slot_binding_] = TempOperand("%ret");
  // A return that destroys a long prefix of the same local-object stack at
  // every source exit would duplicate that suffix quadratically.  For large
  // return sets, route exits through one linked cleanup chain; small
  // functions retain the direct form used by the normal LowIR shape.
  shared_return_cleanup_ =
      CountReturnStatements(body) >= kInlineCleanupLimit;
  if (shared_return_cleanup_) {
    shared_return_end_label_ = NewBlockLabel("return_cleanup_end");
    if (result_boundary != CBM_INDIRECT_RESULT &&
        !IsVoidType(types_, type.result))
      shared_return_slot_ = NewGeneratedSlot("return", LowTypeOf(type.result));
  }
  std::set<BindingId> source_slots;
  CollectSlots(body, source_slots);
  StartBlock("^entry");
  for (std::size_t i = 0; i < type.parameters.size(); ++i) {
    const std::size_t parameter_index = i + parameter_offset;
    // An empty class parameter object has no value: its slot is declared
    // but never stored (the fixture shape).  Every other parameter,
    // including a non-empty class object, is stored to its slot on entry.
    const TypeId parameter_type = types_.Unqualified(type.parameters[i]);
    if (types_.Kind(parameter_type) == TYPE_CLASS) {
      if (ClassBoundary(parameter_type, false) == CBM_BY_ADDRESS) {
        if (i < parameters.size()) {
          const BindingId binding = tree_.At(parameters[i]).binding;
          if (binding != 0)
            parameter_addresses_[binding] =
                TempOperand(function_.params[parameter_index].name);
        }
        continue;
      }
      if (model_.ClassAt(types_.At(parameter_type).entity).empty)
        continue;
      Value destination;
      destination.type = parameter_type;
      destination.lvalue = true;
      destination.operand = SlotOperand(function_.slots[i].first);
      EmitCopyObject(parameter_type,
                     TempOperand(function_.params[parameter_index].name),
                     AddressValue(destination).operand);
      continue;
    }
    EmitStore(function_.params[parameter_index].type,
              TempOperand(function_.params[parameter_index].name),
              SlotOperand(function_.slots[i].first));
  }
  const bool implicit_special = (entity.synthesized || entity.defaulted) &&
      (entity.copy_constructor || entity.move_constructor ||
       entity.copy_assignment || entity.move_assignment);
  if (implicit_special)
    LowerImplicitSpecialMember(id, node);
  else {
    if (entity.special_member == SPECIAL_MEMBER_CONSTRUCTOR)
      LowerConstructorInitializers(id, node);
    if (entity.special_member == SPECIAL_MEMBER_DESTRUCTOR)
      EmitDestructorBody(id, node);
    else
      LowerSequence(body);
  }
  if (!Terminated()) {
    // 6.6.3p2/3.6.1p5: main returns 0 when it falls off the end.  Any other
    // value-returning function that can fall off the end gets the same
    // zero terminator: the block is well-formed and reaching it is the
    // program's undefined behaviour, not a compile-time error.
    if (IsVoidType(types_, type.result)) {
      EmitActiveDestructors();
      EmitParameterDestructors();
      EmitReturn(0);
    } else {
      EmitActiveDestructors();
      EmitParameterDestructors();
      const Value zero = ZeroValue(type.result);
      EmitReturn(&zero);
    }
  }
  EmitSharedReturnCleanups();
  return std::move(function_);
}

void Lowerer::PushControl(const std::string& break_label,
                          const std::string& continue_label)
{
  ControlTarget target;
  target.break_label = break_label;
  target.continue_label = continue_label;
  controls_.push_back(target);
}

void Lowerer::PopControl()
{
  if (!controls_.empty())
    controls_.pop_back();
}

std::string Lowerer::CurrentBreak() const
{
  if (controls_.empty())
    return std::string();
  return controls_.back().break_label;
}

std::string Lowerer::CurrentContinue() const
{
  for (std::vector<ControlTarget>::const_reverse_iterator i =
           controls_.rbegin(); i != controls_.rend(); ++i)
    if (!i->continue_label.empty())
      return i->continue_label;
  return std::string();
}

void Lowerer::Unsupported(const std::string& feature) const
{
  throw std::logic_error("LowIR lowering does not support " + feature);
}

}  // namespace lowir_lowering
