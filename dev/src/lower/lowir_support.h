#pragma once

// Vocabulary shared by the PA15 lowering units: the LowIR storage class of a
// C++ type, rendered LowIR types, operand constructors, and the semantic
// predicates every unit asks.  Defined in lowir_types.cpp.

#include <cstddef>
#include <string>

#include "lowir_model.h"
#include "sema/sema_tree.h"
#include "sema/type_table.h"

namespace lowir_lowering {

// LowIR storage class of a C++ type, computed once from the type table so
// widths and conversions never parse rendered type text.
struct LowInfo
{
  enum Kind { LK_VOID, LK_INTEGER, LK_FLOAT, LK_POINTER, LK_OBJECT };

  Kind kind;
  unsigned bits;         // integer and floating width
  bool is_unsigned;      // integers: the C++ signedness
  std::size_t bytes;     // objects
  std::size_t alignment; // objects

  LowInfo()
      : kind(LK_VOID), bits(0), is_unsigned(false), bytes(0), alignment(0) {}
  bool Integer() const { return kind == LK_INTEGER; }
  bool Floating() const { return kind == LK_FLOAT; }
  bool Pointer() const { return kind == LK_POINTER; }
};

// Two types share one LowIR storage type when they render the same text:
// LowIR spells every 64-bit integer `i64`, so signedness only separates
// narrower integers.
bool SameStorage(const LowInfo& left, const LowInfo& right);
lowir_model::LowType RenderLowType(const LowInfo& info);
lowir_model::LowType PtrType();
lowir_model::LowType I64Type();
lowir_model::LowType I8Type();
lowir_model::LowType VoidType();

lowir_model::Operand TempOperand(const std::string& name);
lowir_model::Operand SlotOperand(const std::string& name);
lowir_model::Operand GlobalOperand(const std::string& name);
lowir_model::Operand LabelOperand(const std::string& name);
lowir_model::Operand Immediate(long long value);
lowir_model::Operand NullptrImmediate();
lowir_model::Operand FloatImmediate(const std::string& spelling,
                                    long double value,
                                    const lowir_model::LowType& type);

bool IsLogicalOperator(ETokenType op);
bool IsScalarBinaryOperator(ETokenType op);
bool IsKnownIntegralLiteral(const SemaNode& node, const TypeTable& types);
bool IsFloatingType(const TypeTable& types, TypeId type);
bool IsVoidType(const TypeTable& types, TypeId type);
bool IsBoolType(const TypeTable& types, TypeId type);

}  // namespace lowir_lowering
