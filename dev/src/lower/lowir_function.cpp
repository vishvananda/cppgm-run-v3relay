#include "lower/lowir_lowering.h"

#include <algorithm>
#include <stdexcept>

namespace lowir_lowering {

namespace {

lowir_model::Operand NamedOperand(lowir_model::Operand::Kind kind,
                                  const std::string& text)
{
  lowir_model::Operand result;
  result.kind = kind;
  result.text = text;
  return result;
}

}  // namespace

Lowerer::Lowerer(const std::vector<Pa6Token>& tokens, const AstArena& arena,
                 const SemaModel& model, const SemaTree& tree)
    : tokens_(tokens), arena_(arena), model_(model), tree_(tree),
      types_(model.Types()), active_switch_labels_(0), temp_counter_(0),
      label_counter_(0), generated_slot_counter_(0),
      function_return_type_id_(0)
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

const lowir_model::Block* Lowerer::CurrentBlock() const
{
  for (std::size_t i = 0; i < function_.blocks.size(); ++i)
    if (function_.blocks[i].label == current_label_)
      return &function_.blocks[i];
  return 0;
}

lowir_model::Block* Lowerer::CurrentBlock()
{
  for (std::size_t i = 0; i < function_.blocks.size(); ++i)
    if (function_.blocks[i].label == current_label_)
      return &function_.blocks[i];
  return 0;
}

void Lowerer::AddBlock(const std::string& label)
{
  for (std::size_t i = 0; i < function_.blocks.size(); ++i)
    if (function_.blocks[i].label == label)
      return;
  lowir_model::Block block;
  block.label = label;
  function_.blocks.push_back(block);
}

void Lowerer::AddBlockIfMissing(const std::string& label)
{
  AddBlock(label);
}

void Lowerer::SetCurrent(const std::string& label)
{
  AddBlockIfMissing(label);
  current_label_ = label;
}

std::string Lowerer::NewBlockLabel(const std::string& stem)
{
  return "^" + stem + "_" + std::to_string(++label_counter_);
}

std::string Lowerer::NewTemp()
{
  return "%t" + std::to_string(++temp_counter_);
}

std::string Lowerer::NewGeneratedSlot(const std::string& stem,
                                      const lowir_model::LowType& type)
{
  const std::string base = "$" + stem;
  std::string candidate = base;
  const std::size_t marker = stem.rfind("__");
  const std::string prefix = marker == std::string::npos ? stem :
      stem.substr(0, marker);
  while (true) {
    bool used = false;
    for (std::size_t i = 0; i < function_.slots.size(); ++i)
      if (function_.slots[i].first == candidate)
        used = true;
    if (!used) {
      function_.slots.push_back(std::make_pair(candidate, type));
      return candidate;
    }
    candidate = "$" + prefix + "__" +
        std::to_string(++generated_slot_counter_);
  }
}

void Lowerer::Emit(const lowir_model::Instruction& instruction)
{
  lowir_model::Block* block = CurrentBlock();
  if (block == 0)
    throw std::logic_error("internal LowIR error: no current block");
  block->instructions.push_back(instruction);
}

bool Lowerer::Terminated() const
{
  const lowir_model::Block* block = CurrentBlock();
  if (block == 0 || block->instructions.empty())
    return false;
  const lowir_model::Instruction::Kind kind = block->instructions.back().kind;
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
  instruction.first = NamedOperand(lowir_model::Operand::OP_LABEL, label);
  Emit(instruction);
}

void Lowerer::EmitBranch(const lowir_model::Operand& condition,
                         const std::string& true_label,
                         const std::string& false_label)
{
  lowir_model::Instruction instruction;
  instruction.kind = lowir_model::Instruction::IK_BRANCH;
  instruction.first = condition;
  instruction.second = NamedOperand(lowir_model::Operand::OP_LABEL, true_label);
  instruction.third = NamedOperand(lowir_model::Operand::OP_LABEL, false_label);
  Emit(instruction);
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

void Lowerer::CollectSlots(SemaId node, std::set<BindingId>& seen)
{
  if (node == 0)
    return;
  const SemaNode& value = tree_.At(node);
  if (value.kind == SEMA_FUNCTION_DEFINITION ||
      value.kind == SEMA_FUNCTION_DECLARATION)
    return;
  if (value.kind == SEMA_VARIABLE && value.binding != 0 &&
      seen.insert(value.binding).second)
    AddSourceSlot(value.binding);
  for (SemaId child = value.first_child; child != 0;
       child = tree_.At(child).next_sibling)
    CollectSlots(child, seen);
}

std::string Lowerer::SlotFor(BindingId binding) const
{
  const std::map<BindingId, std::string>::const_iterator found =
      slots_.find(binding);
  if (found == slots_.end())
    throw std::logic_error("unsupported in CP1: object has no storage slot");
  return found->second;
}

void Lowerer::AddSourceSlot(BindingId binding)
{
  const Binding& value = model_.BindingAt(binding);
  std::string name = value.name.empty() ?
      "$__local" + std::to_string(++generated_slot_counter_) : "$" + value.name;
  std::string base = name;
  unsigned suffix = 1;
  while (slots_.find(binding) == slots_.end()) {
    bool collision = false;
    for (std::size_t i = 0; i < function_.slots.size(); ++i)
      if (function_.slots[i].first == name)
        collision = true;
    if (!collision) {
      function_.slots.push_back(std::make_pair(name, LowTypeOf(value.type)));
      slots_[binding] = name;
      return;
    }
    name = base + "__shadow" + std::to_string(++suffix);
  }
}

void Lowerer::AddParameterSlots(SemaId function_node)
{
  const FunctionEntity& entity = model_.FunctionAt(tree_.At(function_node).function);
  const TypeNode& type = types_.At(types_.Unqualified(entity.type));
  function_return_type_id_ = type.result;
  std::vector<SemaId> parameters;
  CollectParameters(function_node, parameters);
  for (std::size_t i = 0; i < type.parameters.size(); ++i) {
    std::string name = "$__param" + std::to_string(i);
    BindingId binding = 0;
    if (i < parameters.size()) {
      binding = tree_.At(parameters[i]).binding;
      if (binding != 0 && !model_.BindingAt(binding).name.empty())
        name = "$" + model_.BindingAt(binding).name;
    }
    std::string base = name;
    unsigned suffix = 1;
    while (true) {
      bool collision = false;
      for (std::size_t j = 0; j < function_.slots.size(); ++j)
        if (function_.slots[j].first == name)
          collision = true;
      if (!collision) break;
      name = base + "__" + std::to_string(++suffix);
    }
    function_.slots.push_back(std::make_pair(name, LowTypeOf(type.parameters[i])));
    if (binding != 0)
      slots_[binding] = name;
  }
}

lowir_model::Function Lowerer::BuildFunction(SemaId node)
{
  function_ = lowir_model::Function();
  slots_.clear();
  controls_.clear();
  temp_counter_ = 0;
  label_counter_ = 0;
  generated_slot_counter_ = 0;
  const FunctionEntityId id = tree_.At(node).function;
  const FunctionEntity& entity = model_.FunctionAt(id);
  function_.name = function_names_[id];
  const TypeNode& type = types_.At(types_.Unqualified(entity.type));
  function_.return_type = LowTypeOf(type.result);
  function_.boundary.arity = type.variadic ? lowir_model::CAM_VARIADIC :
      lowir_model::CAM_FIXED;
  function_.metadata.binding = entity.internal_linkage ?
      lowir_model::SBM_INTERNAL : lowir_model::SBM_STRONG;
  function_.metadata.linkage = entity.c_linkage ? lowir_model::LLM_C :
      lowir_model::LLM_DEFAULT;
  if (entity.noexcept_qualifier)
    function_.boundary.unwind = lowir_model::CUM_NO;
  if (entity.name == "main" && entity.scope == model_.GlobalScope()) {
    function_.metadata.role = lowir_model::SR_ENTRY;
    function_.metadata.keep_internal_alias = true;
  } else if (!entity.c_linkage) {
    function_.metadata.object_symbol = object_names_[id];
  }

  std::vector<SemaId> parameters;
  CollectParameters(node, parameters);
  for (std::size_t i = 0; i < type.parameters.size(); ++i) {
    lowir_model::Parameter parameter;
    parameter.name = "%__param" + std::to_string(i);
    if (i < parameters.size()) {
      const BindingId binding = tree_.At(parameters[i]).binding;
      if (binding != 0 && !model_.BindingAt(binding).name.empty())
        parameter.name = "%" + model_.BindingAt(binding).name;
      if (types_.Kind(types_.Unqualified(type.parameters[i])) == TYPE_REFERENCE)
        parameter.metadata.passing = lowir_model::PPM_REFERENCE;
    }
    parameter.type = LowTypeOf(type.parameters[i]);
    function_.params.push_back(parameter);
  }

  AddParameterSlots(node);
  const SemaId body = FunctionBody(node);
  if (body == 0)
    Unsupported("function without a body");
  std::set<BindingId> source_slots;
  CollectSlots(body, source_slots);
  AddBlock("^entry");
  current_label_ = "^entry";
  for (std::size_t i = 0; i < function_.params.size(); ++i) {
    lowir_model::Instruction store;
    store.kind = lowir_model::Instruction::IK_STORE;
    store.type = function_.params[i].type;
    store.first = NamedOperand(lowir_model::Operand::OP_TEMP,
                               function_.params[i].name);
    store.second = NamedOperand(lowir_model::Operand::OP_SLOT,
                                function_.slots[i].first);
    Emit(store);
  }
  LowerSequence(body);
  if (!Terminated()) {
    if (function_.return_type.text == "void")
      EmitReturn(0);
    else
      Unsupported("falling off a non-void function");
  }
  return function_;
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
  for (std::vector<ControlTarget>::const_reverse_iterator i = controls_.rbegin();
       i != controls_.rend(); ++i)
    if (!i->continue_label.empty())
      return i->continue_label;
  return std::string();
}

void Lowerer::Unsupported(const std::string& feature) const
{
  throw std::logic_error("unsupported in CP1: " + feature);
}

}  // namespace lowir_lowering
