#include "lower/lowir_lowering.h"

#include <functional>
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

lowir_model::Operand IntegerOperand(long long value)
{
  lowir_model::Operand result;
  result.kind = lowir_model::Operand::OP_INTEGER;
  result.text = std::to_string(value);
  result.int_value = value;
  return result;
}

bool IsFloat(const TypeTable& types, TypeId type)
{
  type = types.Unqualified(type);
  if (types.Kind(type) != TYPE_FUNDAMENTAL)
    return false;
  return types.At(type).fundamental == FT_FLOAT ||
      types.At(type).fundamental == FT_DOUBLE ||
      types.At(type).fundamental == FT_LONG_DOUBLE;
}

bool IsLogical(SemaKind kind, ETokenType op, const SemaTree& tree,
               SemaId node)
{
  (void)kind;
  (void)tree;
  (void)node;
  return op == OP_LAND || op == OP_LOR;
}

bool IsKnownIntegralLiteral(const SemaNode& node, const TypeTable& types)
{
  if (node.kind != SEMA_LITERAL || !node.has_value)
    return false;
  const TypeId type = types.Unqualified(node.type);
  if (types.Kind(type) != TYPE_FUNDAMENTAL)
    return false;
  const EFundamentalType fundamental = types.At(type).fundamental;
  return fundamental != FT_FLOAT && fundamental != FT_DOUBLE &&
      fundamental != FT_LONG_DOUBLE;
}

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

bool Lowerer::DefinitelyTerminates(SemaId node) const
{
  if (node == 0)
    return false;
  const SemaNode& value = tree_.At(node);
  switch (value.kind) {
  case SEMA_RETURN_STATEMENT:
  case SEMA_BREAK_STATEMENT:
  case SEMA_CONTINUE_STATEMENT:
    return true;
  case SEMA_THEN:
  case SEMA_ELSE:
  case SEMA_COMPOUND_STATEMENT:
  case SEMA_FOR_INIT_STATEMENT:
  case SEMA_ITERATION:
  case SEMA_CASE_STATEMENT:
  case SEMA_DEFAULT_STATEMENT:
    {
      std::vector<SemaId> children = Children(node);
      if ((value.kind == SEMA_CASE_STATEMENT && children.size() <= 1) ||
          (value.kind == SEMA_DEFAULT_STATEMENT && children.empty()) ||
          (value.kind != SEMA_CASE_STATEMENT &&
           value.kind != SEMA_DEFAULT_STATEMENT && children.empty()))
        return false;
      return DefinitelyTerminates(children.back());
    }
  case SEMA_IF_STATEMENT:
    {
      std::vector<SemaId> children = Children(node);
      if (children.size() < 3) return false;
      return DefinitelyTerminates(children[1]) &&
          DefinitelyTerminates(children[2]);
    }
  default:
    return false;
  }
}

void Lowerer::PrepareConditionLabels(SemaId node)
{
  if (node == 0)
    return;
  const SemaNode& value = tree_.At(node);
  if (value.kind == SEMA_CONDITION || value.kind == SEMA_CONDITION_DECLARATION) {
    for (SemaId child = value.first_child; child != 0;
         child = tree_.At(child).next_sibling)
      PrepareConditionLabels(child);
    return;
  }
  if (value.kind == SEMA_BINARY && IsLogical(value.kind, value.op, tree_, node)) {
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
}

void Lowerer::LowerSimpleCondition(SemaId node,
                                   const std::string& true_label,
                                   const std::string& false_label)
{
  Value value = LowerRValue(node);
  if (IsFloat(types_, value.type)) {
    lowir_model::Instruction compare;
    compare.kind = lowir_model::Instruction::IK_CMP;
    compare.dest = NewTemp();
    compare.op = "ne";
    compare.type = LowTypeOf(value.type);
    compare.first = value.operand;
    compare.second = ZeroOperand(value.type);
    Emit(compare);
    value.operand = NamedOperand(lowir_model::Operand::OP_TEMP, compare.dest);
  }
  EmitBranch(value.operand, true_label, false_label);
}

void Lowerer::LowerCondition(SemaId node, const std::string& true_label,
                             const std::string& false_label)
{
  if (node == 0)
    Unsupported("missing condition");
  const SemaNode& value = tree_.At(node);
  if (value.kind == SEMA_CONDITION) {
    const std::vector<SemaId> children = Children(node);
    if (children.size() != 1)
      Unsupported("condition wrapper");
    return LowerCondition(children[0], true_label, false_label);
  }
  if (value.kind == SEMA_CONDITION_DECLARATION) {
    (void)LowerConditionDeclaration(node, true_label, false_label);
    return;
  }
  if (value.kind == SEMA_BINARY && IsLogical(value.kind, value.op, tree_, node)) {
    const std::vector<SemaId> children = Children(node);
    if (children.size() != 2)
      Unsupported("logical condition arity");
    const SemaNode& left = tree_.At(children[0]);
    if (IsKnownIntegralLiteral(left, types_)) {
      const bool left_true = left.value != 0;
      const bool decisive = (value.op == OP_LOR && left_true) ||
          (value.op == OP_LAND && !left_true);
      if (decisive) {
        EmitBranch(IntegerOperand(left_true ? 1 : 0), true_label, false_label);
        return;
      }
      LowerCondition(children[1], true_label, false_label);
      return;
    }
    std::map<SemaId, std::string>::const_iterator found =
        condition_labels_.find(node);
    if (found == condition_labels_.end()) {
      condition_labels_[node] = NewBlockLabel(value.op == OP_LOR ?
          "lor_rhs" : "land_rhs");
      found = condition_labels_.find(node);
    }
    const std::string rhs = found->second;
    if (value.op == OP_LOR)
      LowerCondition(children[0], true_label, rhs);
    else
      LowerCondition(children[0], rhs, false_label);
    AddBlock(rhs);
    SetCurrent(rhs);
    LowerCondition(children[1], true_label, false_label);
    return;
  }
  if (value.kind == SEMA_UNARY && value.op == OP_LNOT) {
    // Lowering `!x` through the comparison path preserves a typed comparison
    // (and therefore the source expression's NaN/zero policy) before branch.
    LowerSimpleCondition(node, true_label, false_label);
    return;
  }
  LowerSimpleCondition(node, true_label, false_label);
}

bool Lowerer::LowerConditionDeclaration(SemaId node,
                                        const std::string& true_label,
                                        const std::string& false_label)
{
  const std::vector<SemaId> children = Children(node);
  if (children.size() != 1 || tree_.At(children[0]).kind != SEMA_VARIABLE)
    Unsupported("condition declaration shape");
  LowerVariableDeclaration(children[0]);
  const BindingId binding = tree_.At(children[0]).binding;
  lowir_model::Instruction load;
  load.kind = lowir_model::Instruction::IK_LOAD;
  load.dest = NewTemp();
  load.type = LowTypeOf(model_.BindingAt(binding).type);
  load.first.kind = lowir_model::Operand::OP_SLOT;
  load.first.text = SlotFor(binding);
  Emit(load);
  Value condition;
  condition.type = model_.BindingAt(binding).type;
  condition.operand = NamedOperand(lowir_model::Operand::OP_TEMP, load.dest);
  if (IsFloat(types_, condition.type)) {
    lowir_model::Instruction compare;
    compare.kind = lowir_model::Instruction::IK_CMP;
    compare.dest = NewTemp();
    compare.op = "ne";
    compare.type = LowTypeOf(condition.type);
    compare.first = condition.operand;
    compare.second = ZeroOperand(condition.type);
    Emit(compare);
    condition.operand = NamedOperand(lowir_model::Operand::OP_TEMP, compare.dest);
  }
  EmitBranch(condition.operand, true_label, false_label);
  return true;
}

void Lowerer::LowerVariableDeclaration(SemaId node)
{
  if (tree_.At(node).kind == SEMA_VARIABLE) {
    const SemaNode& variable = tree_.At(node);
    const std::vector<SemaId> initializer = Children(node);
    if (initializer.empty())
      return;
    Value value = LowerRValue(initializer[0], variable.type);
    lowir_model::Instruction store;
    store.kind = lowir_model::Instruction::IK_STORE;
    store.type = LowTypeOf(variable.type);
    store.first = value.operand;
    store.second.kind = lowir_model::Operand::OP_SLOT;
    store.second.text = SlotFor(variable.binding);
    Emit(store);
    return;
  }
  const std::vector<SemaId> children = Children(node);
  for (std::size_t i = 0; i < children.size(); ++i) {
    if (tree_.At(children[i]).kind != SEMA_VARIABLE)
      continue;
    const SemaNode& variable = tree_.At(children[i]);
    const std::vector<SemaId> initializer = Children(children[i]);
    if (initializer.empty())
      continue;
    Value value = LowerRValue(initializer[0], variable.type);
    lowir_model::Instruction store;
    store.kind = lowir_model::Instruction::IK_STORE;
    store.type = LowTypeOf(variable.type);
    store.first = value.operand;
    store.second.kind = lowir_model::Operand::OP_SLOT;
    store.second.text = SlotFor(variable.binding);
    Emit(store);
  }
}

bool Lowerer::LowerIf(SemaId node)
{
  const std::vector<SemaId> children = Children(node);
  if (children.size() < 2)
    Unsupported("if statement shape");
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

  AddBlock(then_label);
  SetCurrent(then_label);
  bool then_done = LowerSequence(children[1]);
  if (!then_done && needs_end)
    EmitJump(end_label);

  AddBlock(else_label);
  SetCurrent(else_label);
  bool else_done = false;
  if (has_else)
    else_done = LowerSequence(children[2]);
  if (!else_done && needs_end)
    EmitJump(end_label);

  if (needs_end) {
    AddBlock(end_label);
    SetCurrent(end_label);
    return false;
  }
  return then_done && else_done;
}

bool Lowerer::LowerLoop(SemaId node, SemaKind kind)
{
  const std::vector<SemaId> children = Children(node);
  if (children.size() < 2)
    Unsupported("loop statement shape");
  const bool is_do = kind == SEMA_DO_STATEMENT;
  const std::string first_label = NewBlockLabel(is_do ? "do_body" : "while_cond");
  const std::string second_label = NewBlockLabel(is_do ? "do_cond" : "while_body");
  const std::string end_label = NewBlockLabel(is_do ? "do_end" : "while_end");
  const SemaId condition = is_do ? children[1] : children[0];
  const SemaId body = is_do ? children[0] : children[1];
  if (is_do) {
    EmitJump(first_label);
    AddBlock(first_label);
    SetCurrent(first_label);
    PushControl(end_label, second_label);
    const bool body_done = LowerSequence(body);
    if (!body_done) EmitJump(second_label);
    PopControl();
    AddBlock(second_label);
    SetCurrent(second_label);
    PrepareConditionLabels(condition);
    LowerCondition(condition, first_label, end_label);
  } else {
    EmitJump(first_label);
    AddBlock(first_label);
    SetCurrent(first_label);
    PrepareConditionLabels(condition);
    LowerCondition(condition, second_label, end_label);
    AddBlock(second_label);
    SetCurrent(second_label);
    PushControl(end_label, first_label);
    const bool body_done = LowerSequence(body);
    if (!body_done) EmitJump(first_label);
    PopControl();
  }
  AddBlock(end_label);
  SetCurrent(end_label);
  return false;
}

bool Lowerer::LowerFor(SemaId node)
{
  const std::vector<SemaId> children = Children(node);
  if (children.size() < 4)
    Unsupported("for statement shape");
  const std::string condition_label = NewBlockLabel("for_cond");
  const std::string body_label = NewBlockLabel("for_body");
  const std::string iteration_label = NewBlockLabel("for_iter");
  const std::string end_label = NewBlockLabel("for_end");
  LowerSequence(children[0]);
  EmitJump(condition_label);
  AddBlock(condition_label);
  SetCurrent(condition_label);
  PrepareConditionLabels(children[1]);
  LowerCondition(children[1], body_label, end_label);
  AddBlock(body_label);
  SetCurrent(body_label);
  PushControl(end_label, iteration_label);
  const bool body_done = LowerSequence(children[3]);
  if (!body_done) EmitJump(iteration_label);
  PopControl();
  AddBlock(iteration_label);
  SetCurrent(iteration_label);
  (void)LowerStatement(children[2]);
  if (!Terminated()) EmitJump(condition_label);
  AddBlock(end_label);
  SetCurrent(end_label);
  return false;
}

void Lowerer::LowerSwitchCase(
    SemaId node, const std::map<SemaId, std::string>& labels)
{
  const std::map<SemaId, std::string>::const_iterator found = labels.find(node);
  if (found == labels.end())
    Unsupported("switch case label");
  AddBlock(found->second);
  SetCurrent(found->second);
  const std::vector<SemaId> children = Children(node);
  std::size_t start = 0;
  if (!children.empty() && tree_.At(children[0]).kind == SEMA_LITERAL)
    start = 1;
  for (std::size_t i = start; i < children.size(); ++i) {
    if (tree_.At(children[i]).kind == SEMA_CASE_STATEMENT ||
        tree_.At(children[i]).kind == SEMA_DEFAULT_STATEMENT) {
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
    Unsupported("switch statement shape");
  const SemaId condition = children[0];
  const SemaId body = children[1];
  const std::string dispatch_label = NewBlockLabel("switch_dispatch");
  const std::string end_label = NewBlockLabel("switch_end");
  std::map<SemaId, std::string> labels;
  std::vector<SemaId> case_nodes;
  SemaId default_node = 0;
  std::function<void(SemaId)> collect = [&](SemaId current) {
    if (current == 0) return;
    const SemaKind kind = tree_.At(current).kind;
    if (kind == SEMA_CASE_STATEMENT) {
      if (labels.find(current) == labels.end()) {
        labels[current] = NewBlockLabel("switch_case");
        case_nodes.push_back(current);
      }
    } else if (kind == SEMA_DEFAULT_STATEMENT) {
      if (default_node == 0) {
        default_node = current;
        labels[current] = NewBlockLabel("switch_default");
      }
    }
    if (kind == SEMA_SWITCH_STATEMENT)
      return;
    for (SemaId child = tree_.At(current).first_child; child != 0;
         child = tree_.At(child).next_sibling)
      collect(child);
  };
  const std::vector<SemaId> body_children = Children(body);
  for (std::size_t i = 0; i < body_children.size(); ++i)
    if (tree_.At(body_children[i]).kind == SEMA_CASE_STATEMENT ||
        tree_.At(body_children[i]).kind == SEMA_DEFAULT_STATEMENT)
      collect(body_children[i]);
  if (default_node == 0) {
    default_node = 0;
    // A no-default switch still needs a destination for unmatched values.
    labels[0] = NewBlockLabel("switch_default");
  }
  bool switch_terminates = default_node != 0 &&
      !HasSwitchBreak(body, tree_) && DefinitelyTerminates(default_node);
  for (std::size_t i = 0; i < case_nodes.size() && switch_terminates; ++i)
    switch_terminates = DefinitelyTerminates(case_nodes[i]);

  // A condition declaration belongs to the dispatch predecessor and is not
  // itself a branch condition.
  SemaId condition_value = condition;
  if (tree_.At(condition_value).kind == SEMA_CONDITION) {
    const std::vector<SemaId> wrapped = Children(condition_value);
    if (wrapped.size() != 1) Unsupported("switch condition wrapper");
    condition_value = wrapped[0];
  }
  Value selector;
  if (tree_.At(condition_value).kind == SEMA_CONDITION_DECLARATION) {
    const std::vector<SemaId> declaration = Children(condition_value);
    if (declaration.size() != 1) Unsupported("switch condition declaration");
    LowerVariableDeclaration(declaration[0]);
    const BindingId binding = tree_.At(declaration[0]).binding;
    lowir_model::Instruction load;
    load.kind = lowir_model::Instruction::IK_LOAD;
    load.dest = NewTemp();
    load.type = LowTypeOf(model_.BindingAt(binding).type);
    load.first.kind = lowir_model::Operand::OP_SLOT;
    load.first.text = SlotFor(binding);
    Emit(load);
    selector.type = model_.BindingAt(binding).type;
    selector.operand = NamedOperand(lowir_model::Operand::OP_TEMP, load.dest);
  } else {
    selector = LowerRValue(condition_value);
  }
  EmitJump(dispatch_label);
  AddBlock(dispatch_label);
  SetCurrent(dispatch_label);
  lowir_model::Instruction dispatch;
  dispatch.kind = lowir_model::Instruction::IK_SWITCH;
  dispatch.first = selector.operand;
  dispatch.second = NamedOperand(lowir_model::Operand::OP_LABEL,
                                labels[default_node == 0 ? 0 : default_node]);
  for (std::size_t i = 0; i < case_nodes.size(); ++i) {
    const std::vector<SemaId> case_children = Children(case_nodes[i]);
    if (case_children.empty() || !tree_.At(case_children[0]).has_value)
      Unsupported("nonconstant switch case");
    dispatch.args.push_back(IntegerOperand(tree_.At(case_children[0]).value));
    dispatch.args.push_back(NamedOperand(lowir_model::Operand::OP_LABEL,
                                         labels[case_nodes[i]]));
  }
  Emit(dispatch);

  const std::map<SemaId, std::string>* previous_switch_labels =
      active_switch_labels_;
  active_switch_labels_ = &labels;
  PushControl(end_label, std::string());
  std::size_t current_case = 0;
  for (std::size_t i = 0; i < body_children.size(); ++i) {
    const SemaKind kind = tree_.At(body_children[i]).kind;
    if (kind == SEMA_CASE_STATEMENT || kind == SEMA_DEFAULT_STATEMENT) {
      if (current_case != 0 && !Terminated()) {
        // Fall through to the next label in source order.
        EmitJump(labels[body_children[i]]);
      }
      LowerSwitchCase(body_children[i], labels);
      current_case = i + 1;
    } else if (!Terminated()) {
      (void)LowerStatement(body_children[i]);
    }
  }
  if (!Terminated()) EmitJump(end_label);
  PopControl();
  active_switch_labels_ = previous_switch_labels;
  if (switch_terminates && previous_switch_labels == 0)
    return true;
  if (default_node == 0) {
    AddBlock(labels[0]);
    SetCurrent(labels[0]);
    EmitJump(end_label);
  }
  AddBlock(end_label);
  SetCurrent(end_label);
  return false;
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
  case SEMA_FOR_INIT_STATEMENT:
    return LowerSequence(node);
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
    if (!children.empty()) LowerDiscard(children[0]);
    return false;
  }
  case SEMA_RETURN_STATEMENT: {
    const std::vector<SemaId> children = Children(node);
    if (children.empty()) {
      EmitReturn(0);
    } else {
      Value result = LowerRValue(children[0]);
      const bool unsigned_widening_literal =
          IsUnsigned(function_return_type_id_) &&
          TypeBits(function_return_type_id_) == 64 &&
          tree_.At(children[0]).kind != SEMA_BRACED_INIT_LIST;
      result = unsigned_widening_literal ?
          ConvertExpression(result, function_return_type_id_) :
          Convert(result, function_return_type_id_);
      EmitReturn(&result);
    }
    return true;
  }
  case SEMA_IF_STATEMENT: return LowerIf(node);
  case SEMA_WHILE_STATEMENT: return LowerLoop(node, SEMA_WHILE_STATEMENT);
  case SEMA_DO_STATEMENT: return LowerLoop(node, SEMA_DO_STATEMENT);
  case SEMA_FOR_STATEMENT: return LowerFor(node);
  case SEMA_SWITCH_STATEMENT: return LowerSwitch(node);
  case SEMA_BREAK_STATEMENT:
    if (CurrentBreak().empty()) Unsupported("break outside a breakable statement");
    EmitJump(CurrentBreak());
    return true;
  case SEMA_CONTINUE_STATEMENT:
    if (CurrentContinue().empty()) Unsupported("continue outside a loop");
    EmitJump(CurrentContinue());
    return true;
  case SEMA_CASE_STATEMENT:
  case SEMA_DEFAULT_STATEMENT: {
    if (active_switch_labels_ == 0)
      Unsupported("case label outside a switch");
    const std::map<SemaId, std::string>::const_iterator found =
        active_switch_labels_->find(node);
    if (found == active_switch_labels_->end())
      Unsupported("switch case label");
    EmitJump(found->second);
    LowerSwitchCase(node, *active_switch_labels_);
    return true;
  }
  case SEMA_CONDITION:
  case SEMA_CONDITION_DECLARATION:
    Unsupported("condition outside a selection");
    break;
  default:
    Unsupported("statement node");
  }
  return false;
}

bool Lowerer::LowerSequence(SemaId node)
{
  bool terminated = false;
  const std::vector<SemaId> children = Children(node);
  for (std::size_t i = 0; i < children.size() && !terminated; ++i)
    terminated = LowerStatement(children[i]);
  return terminated;
}

lowir_model::Program LowerTranslationUnit(
    const std::vector<Pa6Token>& tokens, const AstArena& arena, AstId root,
    const SemaModel& model, const SemaTree& tree)
{
  return Lowerer(tokens, arena, model, tree).Run(root);
}

lowir_model::Program Lowerer::Run(AstId root)
{
  if (root == 0 || arena_.At(root).kind != AST_TRANSLATION_UNIT)
    throw std::logic_error("invalid translation unit for LowIR lowering");
  std::vector<FunctionUse> uses;
  std::set<FunctionEntityId> seen;
  CollectFunctions(tree_.Root(), uses, seen);
  BuildFunctionNames(uses);
  std::vector<lowir_model::Function> functions;
  for (std::size_t i = 0; i < uses.size(); ++i) {
    if (!uses[i].definition)
      continue;
    functions.push_back(BuildFunction(uses[i].node));
  }
  BuildDeclarations(uses);
  program_.functions.swap(functions);
  return program_;
}

}  // namespace lowir_lowering
