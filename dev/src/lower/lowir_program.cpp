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
  NameSymbols();
  BuildGlobalDefinitions();
  for (std::size_t i = 0; i < function_order_.size(); ++i) {
    const FunctionSymbol& symbol =
        functions_.find(function_order_[i])->second;
    if (symbol.definition != 0)
      program_.functions.push_back(BuildFunction(symbol));
  }
  BuildGlobalInitializers();
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
  if (initializer.empty())
    return;
  const TypeId declared = variable.type;
  const TypeId unqualified = types_.Unqualified(declared);
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
    EmitReturn(0);
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
    EmitReturn(&result);
    return;
  }
  const Value result = Convert(LowerRValue(children[0]),
                               function_return_type_id_);
  EmitReturn(&result);
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
  bool terminated = false;
  for (SemaId child = tree_.At(node).first_child; child != 0;
       child = tree_.At(child).next_sibling) {
    // A goto terminates the current block, but a later source label starts
    // a new reachable block and must still be lowered for that edge.
    if (terminated && tree_.At(child).kind != SEMA_LABELED_STATEMENT)
      continue;
    terminated = LowerStatement(child);
  }
  return terminated;
}

}  // namespace lowir_lowering
