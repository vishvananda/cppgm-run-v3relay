#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "abi_mangle.h"
#include "lowir_model.h"
#include "parser/ast_model.h"
#include "parser/recog_token.h"
#include "sema/sema_tree.h"
#include "sema/scope_model.h"

namespace lowir_lowering {

lowir_model::Program LowerTranslationUnit(
    const std::vector<Pa6Token>& tokens, const AstArena& arena, AstId root,
    const SemaModel& model, const SemaTree& tree);

class Lowerer
{
public:
  Lowerer(const std::vector<Pa6Token>& tokens, const AstArena& arena,
          const SemaModel& model, const SemaTree& tree);

  lowir_model::Program Run(AstId root);

private:
  struct Value
  {
    lowir_model::Operand operand;
    TypeId type;
    bool lvalue;
    BindingId binding;

    Value() : type(0), lvalue(false), binding(0) {}
  };

  struct ControlTarget
  {
    std::string break_label;
    std::string continue_label;
  };

  struct FunctionUse
  {
    FunctionEntityId id;
    SemaId node;
    bool definition;

    FunctionUse(FunctionEntityId id = 0, SemaId node = 0,
                bool definition = false)
        : id(id), node(node), definition(definition) {}
  };

  // Collection and symbols.
  void CollectFunctions(SemaId node, std::vector<FunctionUse>& result,
                        std::set<FunctionEntityId>& seen) const;
  void BuildFunctionNames(const std::vector<FunctionUse>& uses);
  std::string QualifiedFunctionName(FunctionEntityId id) const;
  std::string FunctionBaseName(FunctionEntityId id) const;
  std::string FunctionObjectName(FunctionEntityId id) const;
  void BuildDeclarations(const std::vector<FunctionUse>& uses);
  lowir_model::Function BuildFunction(SemaId node);
  lowir_model::FunctionDeclaration BuildFunctionDeclaration(
      FunctionEntityId id, SemaId node);

  // Type and ABI ownership.
  lowir_model::LowType LowTypeOf(TypeId type) const;
  lowir_model::LowType LowTypeOfUnqualified(TypeId type) const;
  bool IsUnsigned(TypeId type) const;
  unsigned TypeBits(TypeId type) const;
  bool IsScalar(TypeId type) const;
  abi_mangle::AbiType AbiTypeOf(TypeId type) const;
  std::string NamedType(TypeId type) const;
  std::string MangleFunction(FunctionEntityId id) const;
  std::string OperatorName(const std::string& name) const;

  // Per-function state.
  lowir_model::Function function_;
  const lowir_model::Block* CurrentBlock() const;
  lowir_model::Block* CurrentBlock();
  void AddBlock(const std::string& label);
  void AddBlockIfMissing(const std::string& label);
  void SetCurrent(const std::string& label);
  std::string NewBlockLabel(const std::string& stem);
  std::string NewTemp();
  std::string NewGeneratedSlot(const std::string& stem,
                               const lowir_model::LowType& type);
  std::string LabelFor(SemaId node);
  void Emit(const lowir_model::Instruction& instruction);
  bool Terminated() const;
  void EmitJump(const std::string& label);
  void EmitBranch(const lowir_model::Operand& condition,
                  const std::string& true_label,
                  const std::string& false_label);
  void EmitReturn(const Value* value);

  // Semantic traversal and slots.
  std::vector<SemaId> Children(SemaId node) const;
  void CollectSlots(SemaId node, std::set<BindingId>& seen);
  void CollectParameters(SemaId function_node,
                         std::vector<SemaId>& parameters) const;
  void AddParameterSlots(SemaId function_node);
  std::string SlotFor(BindingId binding) const;
  void AddSourceSlot(BindingId binding);
  SemaId FunctionBody(SemaId function_node) const;

  // Expressions and conversions.
  Value LowerExpression(SemaId node, TypeId expected = 0);
  Value LowerRValue(SemaId node, TypeId expected = 0);
  Value LowerLValue(SemaId node);
  Value LowerLiteral(const SemaNode& node, TypeId expected);
  Value Convert(Value value, TypeId target);
  Value ConvertExpression(Value value, TypeId target);
  Value LowerBinary(SemaId node, TypeId expected);
  Value LowerAssignment(SemaId node);
  Value LowerConditional(SemaId node, TypeId expected);
  Value LowerCall(SemaId node, TypeId expected);
  Value LowerUnary(SemaId node, bool postfix, TypeId expected);
  Value LowerLogicalValue(SemaId node);
  void LowerDiscard(SemaId node);
  lowir_model::Operand ZeroOperand(TypeId type) const;
  lowir_model::Operand OneOperand(TypeId type) const;
  std::string BinaryName(ETokenType op) const;
  std::string CompareName(ETokenType op, TypeId type) const;
  std::string ConversionName(TypeId from, TypeId to) const;
  Value MakeBoolValue(const lowir_model::Operand& operand);

  // Conditions and statements.
  void LowerCondition(SemaId node, const std::string& true_label,
                      const std::string& false_label);
  void PrepareConditionLabels(SemaId node);
  void LowerSimpleCondition(SemaId node, const std::string& true_label,
                            const std::string& false_label);
  bool LowerStatement(SemaId node);
  bool LowerSequence(SemaId node);
  bool DefinitelyTerminates(SemaId node) const;
  bool LowerIf(SemaId node);
  bool LowerLoop(SemaId node, SemaKind kind);
  bool LowerFor(SemaId node);
  bool LowerSwitch(SemaId node);
  void LowerSwitchCase(SemaId node,
                       const std::map<SemaId, std::string>& labels);
  bool LowerConditionDeclaration(SemaId node,
                                 const std::string& true_label,
                                 const std::string& false_label);
  void LowerVariableDeclaration(SemaId node);
  void PushControl(const std::string& break_label,
                   const std::string& continue_label);
  void PopControl();
  std::string CurrentBreak() const;
  std::string CurrentContinue() const;
  void Unsupported(const std::string& feature) const;

  const std::vector<Pa6Token>& tokens_;
  const AstArena& arena_;
  const SemaModel& model_;
  const SemaTree& tree_;
  const TypeTable& types_;
  lowir_model::Program program_;
  std::map<FunctionEntityId, std::string> function_names_;
  std::map<FunctionEntityId, std::string> object_names_;
  std::map<BindingId, std::string> slots_;
  std::map<std::string, std::string> labels_;
  std::map<SemaId, std::string> condition_labels_;
  std::set<FunctionEntityId> definitions_;
  std::set<FunctionEntityId> declarations_;
  std::set<FunctionEntityId> emitted_function_uses_;
  const std::map<SemaId, std::string>* active_switch_labels_;
  std::vector<ControlTarget> controls_;
  std::string current_label_;
  unsigned temp_counter_;
  unsigned label_counter_;
  unsigned generated_slot_counter_;
  TypeId function_return_type_id_;
};

}  // namespace lowir_lowering
