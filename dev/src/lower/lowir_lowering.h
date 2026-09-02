#pragma once

// PA15: lowers the PA12 semantic trees of one invocation's translation units
// into one PA13 LowIR program.  The driver uses ProgramLowering; everything
// else is internal to dev/src/lower.

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "abi_mangle.h"
#include "lower/lowir_support.h"
#include "lowir_model.h"
#include "parser/recog_token.h"
#include "sema/sema_tree.h"
#include "sema/scope_model.h"

namespace lowir_lowering {

// Builds one LowIR program from the translation units of one invocation.
// Units are analyzed independently; the facts that span units live here:
// top-level name uniqueness, the identity of an external symbol declared by
// several units, and the single startup initializer.
class ProgramLowering
{
public:
  explicit ProgramLowering(lowir_model::Program& program);

  // `model` is mutable only because the type table interns the pointer and
  // decayed types that lowering names; semantic facts are never changed.
  void AddUnit(const std::vector<Pa6Token>& tokens, SemaModel& model,
               const SemaTree& tree);
  // Completes the startup initializer and drops every declaration that a
  // definition in the program satisfies.
  void Finish();

private:
  friend class Lowerer;

  lowir_model::Program& program_;
  std::map<std::string, unsigned> top_level_names_;
  std::map<std::string, std::string> external_names_; // object name -> LowIR
  lowir_model::Function init_function_;
  bool has_init_;
  unsigned init_temp_counter_;
  unsigned init_label_counter_;
  unsigned init_slot_counter_;
  unsigned string_literal_counter_; // @__strlit__N, first use across units
};

class Lowerer
{
public:
  Lowerer(ProgramLowering& shared, const std::vector<Pa6Token>& tokens,
          SemaModel& model, const SemaTree& tree);

  void Run();

private:
  struct Value
  {
    lowir_model::Operand operand;
    TypeId type;
    bool lvalue;

    Value() : type(0), lvalue(false) {}
  };

  struct ControlTarget
  {
    std::string break_label;
    std::string continue_label;
  };

  // One entry per function entity the unit names, keyed by the entity.
  struct FunctionSymbol
  {
    std::string name;   // LowIR symbol, unique in the program
    std::string object; // ABI object name; the source name for C linkage
    SemaId declaration; // first declaration or definition node
    SemaId definition;  // 0 when the unit has no body for it
    bool referenced;    // named by a call, an address, or an initializer

    FunctionSymbol() : declaration(0), definition(0), referenced(false) {}
  };

  // One entry per namespace-scope object, keyed by its first binding.
  struct GlobalSymbol
  {
    std::string name;
    std::string object;
    BindingId binding;  // first declaration
    SemaId definition;  // defining variable node; 0 when only declared
    bool internal_linkage;
    bool c_linkage;

    GlobalSymbol()
        : binding(0), definition(0), internal_linkage(false),
          c_linkage(false) {}
  };

  // A store the startup initializer performs because the initializer is
  // not a translation-time constant.
  struct DynamicInitializer
  {
    SemaId expression;
    std::string symbol;
    std::size_t byte_offset;
    TypeId type;

    DynamicInitializer() : expression(0), byte_offset(0), type(0) {}
  };

  // Symbols.
  void CollectSymbols(SemaId node);
  void CollectReferencedFunctions(SemaId node,
                                  std::set<FunctionEntityId>& result) const;
  void ComputeReferencedFunctions();
  void NameSymbols();
  std::string TopLevelName(const std::string& base,
                           const std::string& external_object);
  std::vector<std::string> NamespacePieces(ScopeId scope) const;
  std::string FunctionObjectName(FunctionEntityId id) const;
  std::string GlobalObjectName(const GlobalSymbol& symbol) const;
  const std::string& FunctionSymbolName(FunctionEntityId id);
  BindingId CanonicalBinding(BindingId id) const;
  const GlobalSymbol* GlobalFor(BindingId id) const;
  void BuildGlobalDefinitions();
  bool ConstantGlobalItem(SemaId node, TypeId type,
                          lowir_model::GlobalDefinition::DataItem& item);
  bool GlobalAddress(SemaId node, std::string& symbol, long long& addend);
  void BuildGlobalInitializers();
  void BuildDeclarations();
  lowir_model::Function BuildFunction(const FunctionSymbol& symbol);
  lowir_model::FunctionDeclaration BuildFunctionDeclaration(
      FunctionEntityId id, const FunctionSymbol& symbol);

  // Type and ABI ownership.
  LowInfo LowInfoOf(TypeId type) const;
  lowir_model::LowType LowTypeOf(TypeId type) const;
  bool IsUnsigned(TypeId type) const;
  unsigned TypeBits(TypeId type) const;
  abi_mangle::AbiType AbiTypeOf(TypeId type) const;
  std::string MangleFunction(FunctionEntityId id) const;

  // Per-function state.
  void ResetFunction(const std::string& name,
                     const lowir_model::LowType& return_type);
  void ResumeInitFunction();
  void SuspendInitFunction();
  lowir_model::Block& CurrentBlock();
  void StartBlock(const std::string& label);
  std::string NewBlockLabel(const std::string& stem);
  std::string NewTemp();
  std::string NewGeneratedSlot(const std::string& stem,
                               const lowir_model::LowType& type);
  void AddSlot(const std::string& name, const lowir_model::LowType& type);
  std::string GotoLabel(const SemaNode& node);
  void Emit(const lowir_model::Instruction& instruction);
  bool Terminated() const;
  void EmitJump(const std::string& label);
  void EmitBranch(const lowir_model::Operand& condition,
                  const std::string& true_label,
                  const std::string& false_label);
  void EmitStore(const lowir_model::LowType& type,
                 const lowir_model::Operand& value,
                 const lowir_model::Operand& destination);
  void EmitReturn(const Value* value);

  // Semantic traversal and slots.
  std::vector<SemaId> Children(SemaId node) const;
  void CollectSlots(SemaId node, std::set<BindingId>& seen);
  void CollectParameters(SemaId function_node,
                         std::vector<SemaId>& parameters) const;
  void AddParameterSlots(SemaId function_node);
  const std::string& SlotFor(BindingId binding) const;
  void AddSourceSlot(BindingId binding);
  SemaId FunctionBody(SemaId function_node) const;

  // Expressions and conversions.
  Value LowerRValue(SemaId node, TypeId expected = 0);
  Value LowerLValue(SemaId node);
  Value LowerLiteral(SemaId node, const SemaNode& value, TypeId expected);
  Value LowerArrayDecay(SemaId node);
  Value LowerSubscript(SemaId node, bool lvalue);
  Value LowerConditionalLValue(SemaId node);
  Value LowerReferenceArgument(SemaId node, TypeId parameter);
  Value LoadValue(const Value& lvalue);
  Value AddressValue(const Value& lvalue);
  Value Convert(Value value, TypeId target);
  Value ZeroValue(TypeId type);
  Value LowerBinary(SemaId node, TypeId expected);
  Value LowerPointerOffset(Value pointer, SemaId index_node, bool subtract,
                           TypeId result_type);
  Value LowerComparison(SemaId node, const std::vector<SemaId>& children,
                        TypeId expected);
  Value LowerScalarBinary(SemaId node, Value left, TypeId expected);
  Value LowerAssignment(SemaId node, Value* assigned_lvalue = 0);
  Value LowerConditional(SemaId node, TypeId expected);
  Value LowerCall(SemaId node, TypeId expected);
  Value LowerUnary(SemaId node, bool postfix, TypeId expected,
                   bool as_lvalue = false);
  Value LowerIncrement(SemaId node, SemaId operand_node, bool postfix,
                       TypeId expected, bool as_lvalue);
  Value LowerLogicalValue(SemaId node);
  void LowerDiscard(SemaId node);
  lowir_model::Operand ZeroOperand(TypeId type) const;
  lowir_model::Operand OneOperand(TypeId type) const;
  std::string BinaryName(ETokenType op, TypeId type) const;
  std::string CompareName(ETokenType op, TypeId type) const;
  std::string ConversionName(TypeId from, TypeId to) const;
  bool IsCharacterLiteral(SemaId node) const;
  bool IsNullptrLiteral(SemaId node) const;
  bool IsZeroLiteral(SemaId node) const;
  TypeId ReferentType(TypeId type) const;
  TypeId PointerElementType(TypeId type) const;
  TypeId DefaultArgumentPromotion(TypeId type);
  std::string RegisterStringLiteral(SemaId node, const SemaNode& value);

  // Conditions and statements.
  void LowerCondition(SemaId node, const std::string& true_label,
                      const std::string& false_label);
  void PrepareConditionLabels(SemaId node);
  void LowerTruthBranch(Value value, const std::string& true_label,
                        const std::string& false_label);
  bool LowerStatement(SemaId node);
  bool LowerSequence(SemaId node);
  bool DefinitelyTerminates(SemaId node) const;
  bool LowerIf(SemaId node);
  bool LowerLoop(SemaId node, SemaKind kind);
  bool LowerFor(SemaId node);
  bool LowerSwitch(SemaId node);
  void CollectSwitchLabels(SemaId node, std::map<SemaId, std::string>& labels,
                           std::vector<SemaId>& cases, SemaId& default_node);
  void LowerSwitchCase(SemaId node,
                       const std::map<SemaId, std::string>& labels);
  Value LowerConditionVariable(SemaId declaration);
  void LowerReturn(SemaId node);
  void LowerVariableDeclaration(SemaId node);
  void LowerVariable(SemaId variable_node);
  void PushControl(const std::string& break_label,
                   const std::string& continue_label);
  void PopControl();
  std::string CurrentBreak() const;
  std::string CurrentContinue() const;
  void Unsupported(const std::string& feature) const;

  ProgramLowering& shared_;
  const std::vector<Pa6Token>& tokens_;
  const SemaModel& model_;
  const SemaTree& tree_;
  TypeTable& types_;
  lowir_model::Program& program_;

  // Unit-level symbol state.
  std::map<FunctionEntityId, FunctionSymbol> functions_;
  std::vector<FunctionEntityId> function_order_;
  std::set<FunctionEntityId> referenced_functions_;
  std::map<BindingId, GlobalSymbol> globals_;
  std::vector<BindingId> global_order_;
  std::vector<DynamicInitializer> dynamic_initializers_;
  std::map<SemaId, std::string> string_symbols_;

  // Function-level state.
  lowir_model::Function function_;
  std::size_t current_block_;
  std::set<std::string> block_labels_;
  std::set<std::string> slot_names_;
  std::map<BindingId, std::string> slots_;
  std::vector<std::string> goto_labels_; // indexed by label ordinal
  std::map<SemaId, std::string> condition_labels_;
  const std::map<SemaId, std::string>* active_switch_labels_;
  std::vector<ControlTarget> controls_;
  unsigned temp_counter_;
  unsigned label_counter_;
  unsigned generated_slot_counter_;
  TypeId function_return_type_id_;
};

}  // namespace lowir_lowering
