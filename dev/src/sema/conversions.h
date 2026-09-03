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
  CONV_BOOLEAN,
  CONV_DERIVED_TO_BASE
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
  bool implicit_object;
  // The owning overload comparison uses these original endpoints for the
  // derived-to-base tie-break.  They are metadata, not part of Compare's
  // context-free standard-sequence ordering.
  TypeId source_type;
  TypeId target_type;

  ImplicitConversion();
  bool Viable() const { return rank != RANK_NONE; }
};

ImplicitConversion Classify(TypeTable& types, TypeId source,
                            ValueCategory source_category,
                            bool is_null_literal,
                            bool is_function_lvalue, TypeId target);
// Model-aware classification adds standard derived-to-base object and
// pointer conversions.  The plain overload remains useful for contexts that
// intentionally have no class graph, while semantic overload resolution
// always supplies the owning model.
ImplicitConversion Classify(const SemaModel& model, TypeTable& types,
                            TypeId source, ValueCategory source_category,
                            bool is_null_literal,
                            bool is_function_lvalue, TypeId target);

// Member access applies cv-qualification from the selected object to a
// non-static data member, while preserving mutable and reference members.
void MemberObjectQualifiers(const TypeTable& types, TypeId object_type,
                            ETokenType access_operator, bool& is_const,
                            bool& is_volatile);
TypeId MemberAccessType(TypeTable& types, TypeId member_type,
                        bool static_member, bool mutable_member,
                        TypeId object_type, ETokenType access_operator);

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
