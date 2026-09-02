#pragma once

#include <vector>

#include "sema/conversions.h"

// The expression analyzer supplies already-typed arguments.  Keeping this
// record independent of the AST lets overload resolution operate on the
// canonical entities and standard conversion sequences only.
struct OverloadArgument
{
  TypeId type;
  ValueCategory category;
  bool is_null_literal;
  bool is_function_lvalue;
  bool is_implicit_object;
  bool user_defined_conversion;
  // An overloaded function name as an argument: the entities it may denote
  // (13.4 selects among them per parameter type).
  std::vector<FunctionEntityId> function_candidates;

  OverloadArgument(TypeId type = 0, ValueCategory category = VC_PRVALUE,
                   bool is_null_literal = false,
                   bool is_function_lvalue = false,
                   bool is_implicit_object = false,
                   bool user_defined_conversion = false)
      : type(type), category(category), is_null_literal(is_null_literal),
        is_function_lvalue(is_function_lvalue),
        is_implicit_object(is_implicit_object),
        user_defined_conversion(user_defined_conversion) {}
};

// A single operator/call candidate may have a different argument view from
// its neighbours: a member operator consumes the left operand as an
// implicit object, while a non-member consumes that same operand explicitly.
// Keeping the view on the candidate preserves the standard conversion
// sequence for mixed member/non-member operator sets.
struct OverloadCandidate
{
  FunctionEntityId function;
  std::vector<OverloadArgument> arguments;

  OverloadCandidate(FunctionEntityId function = 0)
      : function(function), arguments() {}
};

// Resolve one overload set (13.3.3).  The lookup level has already been
// chosen by SemaModel::LookupSet/LookupQualifiedSet; only non-template
// function bindings in that set participate, and repeated declarations of
// one entity count once.  Returns 0 when no candidate is viable or no
// candidate is better than every other.
FunctionEntityId SelectBestOverload(
    const SemaModel& model, TypeTable& types,
    const std::vector<BindingId>& bindings,
    const std::vector<OverloadArgument>& arguments,
    bool has_implicit_object = false);

// Same ranking engine with candidate-specific argument lists.  This is the
// operator form of overload resolution (13.3.1.2), where member and
// non-member candidates share one best-function comparison but do not share
// one parameter mapping.
FunctionEntityId SelectBestOverloadCandidates(
    const SemaModel& model, TypeTable& types,
    const std::vector<OverloadCandidate>& candidates);

// Select the unique function entity denoted by an overloaded function name
// when the surrounding initialization supplies a pointer/reference-to-
// function or pointer-to-member-function target (13.4).
bool SelectTargetFunction(const SemaModel& model, TypeTable& types,
                          const std::vector<BindingId>& bindings,
                          TypeId target, BindingId& binding,
                          FunctionEntityId& function);

// Return the function type reached by the call operator's standard decay.
bool CallableFunctionType(TypeTable& types, TypeId type,
                          TypeId& function_type);
