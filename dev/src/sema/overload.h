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
  std::vector<FunctionEntityId> function_candidates;

  OverloadArgument(TypeId type = 0, ValueCategory category = VC_PRVALUE,
                   bool is_null_literal = false,
                   bool is_function_lvalue = false)
      : type(type), category(category), is_null_literal(is_null_literal),
        is_function_lvalue(is_function_lvalue) {}
};

struct OverloadSelection
{
  FunctionEntityId function;
  std::vector<ImplicitConversion> conversions;

  OverloadSelection() : function(0) {}
};

// Resolve one overload set.  The first lookup level has already been chosen
// by SemaModel::LookupSet/LookupQualifiedSet; only function bindings in that
// set participate, and repeated declarations of one entity are deduplicated.
bool SelectBestOverload(const SemaModel& model, TypeTable& types,
                        const std::vector<BindingId>& bindings,
                        const std::vector<OverloadArgument>& arguments,
                        OverloadSelection& selection);

// Select the unique function entity denoted by an overloaded function name
// when the surrounding initialization supplies a pointer/reference-to-
// function target (13.4).
bool SelectTargetFunction(const SemaModel& model, TypeTable& types,
                          const std::vector<BindingId>& bindings,
                          TypeId target, BindingId& binding,
                          FunctionEntityId& function);

// Return the function type reached by the call operator's standard decay.
bool CallableFunctionType(TypeTable& types, TypeId type,
                          TypeId& function_type);
