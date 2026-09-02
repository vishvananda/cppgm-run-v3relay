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
  // An overloaded function name as an argument: the entities it may denote
  // (13.4 selects among them per parameter type).
  std::vector<FunctionEntityId> function_candidates;

  OverloadArgument(TypeId type = 0, ValueCategory category = VC_PRVALUE,
                   bool is_null_literal = false,
                   bool is_function_lvalue = false,
                   bool is_implicit_object = false)
      : type(type), category(category), is_null_literal(is_null_literal),
        is_function_lvalue(is_function_lvalue),
        is_implicit_object(is_implicit_object) {}
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
