#pragma once

#include "sema/sema_tree.h"

enum ConversionRank
{
  RANK_EXACT,
  RANK_PROMOTION,
  RANK_CONVERSION,
  RANK_ELLIPSIS,
  RANK_NONE
};

enum ConversionKind
{
  CONV_IDENTITY,
  CONV_LVALUE_TO_RVALUE,
  CONV_ARRAY_TO_POINTER,
  CONV_FUNCTION_TO_POINTER,
  CONV_INTEGRAL_PROMOTION,
  CONV_INTEGRAL_CONVERSION,
  CONV_FLOATING_PROMOTION,
  CONV_FLOATING_CONVERSION,
  CONV_POINTER,
  CONV_POINTER_TO_BOOL,
  CONV_NULL_TO_POINTER,
  CONV_NULL_TO_NULLPTR,
  CONV_QUALIFICATION,
  CONV_BOOLEAN
};

enum ReferenceBinding
{
  REFERENCE_NONE,
  REFERENCE_DIRECT,
  REFERENCE_TEMPORARY
};

struct ImplicitConversion
{
  ConversionRank rank;
  ConversionKind kind;
  bool qualification;
  ReferenceBinding reference;
  bool rvalue_ref_to_rvalue;
  bool function_lvalue_to_lvalue_ref;

  ImplicitConversion();
  bool Viable() const { return rank != RANK_NONE; }
};

ImplicitConversion Classify(TypeTable& types, TypeId source,
                            ValueCategory source_category,
                            bool is_null_literal,
                            bool is_function_lvalue, TypeId target);

enum ConversionComparison
{
  CONVERSION_BETTER,
  CONVERSION_WORSE,
  CONVERSION_EQUAL
};

// Pure ranking comparison used by overload resolution.  It deliberately
// takes already-classified conversions; lookup and AST state stay out of the
// conversion layer.
ConversionComparison Compare(const ImplicitConversion& left,
                        const ImplicitConversion& right);
