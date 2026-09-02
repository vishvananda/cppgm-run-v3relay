#include "sema/conversions.h"

#include <algorithm>

namespace
{

bool IsFloating(const TypeTable& types, TypeId type)
{
  type = types.Unqualified(type);
  if (types.Kind(type) != TYPE_FUNDAMENTAL)
    return false;
  const EFundamentalType value = types.At(type).fundamental;
  return value == FT_FLOAT || value == FT_DOUBLE || value == FT_LONG_DOUBLE;
}

bool IsConst(const TypeTable& types, TypeId type)
{
  return type != 0 && types.Kind(type) == TYPE_CV &&
      types.At(type).is_const;
}

// Qualification conversion for object pointers.  The recursive check keeps
// the C++ rule that adding qualification below a pointer also qualifies every
// intervening pointer level.
bool QualificationCompatible(const TypeTable& types, TypeId source,
                             TypeId target, bool& added)
{
  added = false;
  if (source == target)
    return true;
  const bool source_const = IsConst(types, source);
  const bool target_const = IsConst(types, target);
  if (target_const && !source_const)
    added = true;
  TypeId s = types.Unqualified(source);
  TypeId t = types.Unqualified(target);
  if (s == t)
    return !source_const || target_const;
  if (types.Kind(s) != TYPE_POINTER || types.Kind(t) != TYPE_POINTER)
    return false;

  bool nested_added = false;
  if (!QualificationCompatible(types, types.At(s).base, types.At(t).base,
                               nested_added))
    return false;
  if (nested_added && !target_const && !IsConst(types, types.At(t).base))
    return false;
  added = added || nested_added;
  return true;
}

bool SameUnqualified(const TypeTable& types, TypeId left, TypeId right)
{
  return types.Unqualified(left) == types.Unqualified(right);
}

} // namespace

ImplicitConversion::ImplicitConversion()
    : rank(RANK_NONE), kind(CONV_IDENTITY), qualification(false),
      reference(REFERENCE_NONE), rvalue_ref_to_rvalue(false),
      function_lvalue_to_lvalue_ref(false), to(0)
{
}

ImplicitConversion Classify(TypeTable& types, TypeId source,
                            ValueCategory source_category,
                            bool is_null_literal,
                            bool is_function_lvalue, TypeId target)
{
  ImplicitConversion result;
  result.to = target;
  if (source == 0 || target == 0)
    return result;

  const bool target_reference = types.Kind(target) == TYPE_REFERENCE;
  TypeId target_value = target_reference ? types.Referent(target) : target;
  const bool target_rvalue_reference = target_reference &&
      !types.At(target).lvalue_reference;

  TypeId source_value = source;
  if (types.Kind(source_value) == TYPE_REFERENCE)
    source_value = types.Referent(source_value);
  const bool source_array = types.Kind(source_value) == TYPE_ARRAY;
  const bool source_function = types.Kind(source_value) == TYPE_FUNCTION;

  if (target_reference)
  {
    if (source_category == VC_LVALUE &&
        (target_rvalue_reference || !source_array))
    {
      bool qualification = false;
      if (SameUnqualified(types, source_value, target_value) &&
          QualificationCompatible(types, source_value, target_value,
                                  qualification))
      {
        if (target_rvalue_reference && source_function)
          result.function_lvalue_to_lvalue_ref = true;
        else if (target_rvalue_reference)
          return result;
        result.rank = RANK_EXACT;
        result.kind = qualification ? CONV_QUALIFICATION : CONV_IDENTITY;
        result.qualification = qualification;
        result.reference = REFERENCE_DIRECT;
        return result;
      }
    }
    if (target_rvalue_reference && source_category == VC_LVALUE &&
        !source_function)
      return result;
    // A const reference can bind a converted temporary.  The caller still
    // checks the exact direct-binding restrictions for non-const references.
    if (types.Kind(target_value) == TYPE_CV &&
        types.At(target_value).is_const)
    {
      ImplicitConversion converted = Classify(
          types, source_value, VC_PRVALUE, is_null_literal,
          is_function_lvalue, target_value);
      if (converted.Viable())
      {
        converted.reference = REFERENCE_TEMPORARY;
        converted.to = target;
        return converted;
      }
    }
    return result;
  }

  if (source_array)
  {
    source_value = types.Decay(source_value);
    result.rank = RANK_EXACT;
    result.kind = CONV_ARRAY_TO_POINTER;
  }
  else if (source_function)
  {
    source_value = types.Decay(source_value);
    result.rank = RANK_EXACT;
    result.kind = CONV_FUNCTION_TO_POINTER;
  }
  else if (source_category == VC_LVALUE)
  {
    result.kind = CONV_LVALUE_TO_RVALUE;
    result.rank = RANK_EXACT;
  }

  if (is_null_literal && types.IsNullPointerType(target_value))
  {
    result.rank = RANK_CONVERSION;
    result.kind = CONV_NULL_TO_NULLPTR;
    return result;
  }
  if (is_null_literal && types.IsPointer(target_value))
  {
    result.rank = RANK_CONVERSION;
    result.kind = CONV_NULL_TO_POINTER;
    return result;
  }
  if (types.IsNullPointerType(source_value) && types.IsPointer(target_value))
  {
    result.rank = RANK_CONVERSION;
    result.kind = CONV_NULL_TO_POINTER;
    return result;
  }

  if (source_value == target_value)
  {
    if (result.kind == CONV_IDENTITY)
      result.kind = CONV_IDENTITY;
    result.rank = RANK_EXACT;
    return result;
  }
  bool qualification = false;
  if (types.IsPointer(source_value) && types.IsPointer(target_value) &&
      QualificationCompatible(types, source_value, target_value,
                               qualification))
  {
    result.rank = RANK_CONVERSION;
    result.kind = qualification ? CONV_QUALIFICATION : CONV_POINTER;
    result.qualification = qualification;
    return result;
  }
  if (types.IsPointer(source_value) &&
      types.Kind(types.Unqualified(target_value)) == TYPE_FUNDAMENTAL &&
      types.At(types.Unqualified(target_value)).fundamental == FT_BOOL)
  {
    result.rank = RANK_CONVERSION;
    result.kind = CONV_POINTER_TO_BOOL;
    return result;
  }

  const bool source_enum = types.Kind(types.Unqualified(source_value)) == TYPE_ENUM;
  const bool target_enum = types.Kind(types.Unqualified(target_value)) == TYPE_ENUM;
  if (types.IsArithmetic(source_value) && types.IsArithmetic(target_value))
  {
    if (types.IsIntegral(source_value) && types.IsIntegral(target_value))
    {
      const TypeId promoted = types.Promote(source_value);
      if (promoted == types.Unqualified(target_value) && !target_enum)
      {
        result.rank = RANK_PROMOTION;
        result.kind = CONV_INTEGRAL_PROMOTION;
      }
      else
      {
        result.rank = RANK_CONVERSION;
        result.kind = source_enum || target_enum ? CONV_INTEGRAL_CONVERSION :
            CONV_INTEGRAL_CONVERSION;
      }
      return result;
    }
    if (IsFloating(types, source_value) && IsFloating(types, target_value))
    {
      const std::size_t source_size = FundamentalSize(
          types.At(types.Unqualified(source_value)).fundamental);
      const std::size_t target_size = FundamentalSize(
          types.At(types.Unqualified(target_value)).fundamental);
      result.rank = source_size < target_size ? RANK_PROMOTION : RANK_CONVERSION;
      result.kind = source_size < target_size ? CONV_FLOATING_PROMOTION :
          CONV_FLOATING_CONVERSION;
      return result;
    }
    result.rank = RANK_CONVERSION;
    result.kind = IsFloating(types, target_value) ? CONV_FLOATING_CONVERSION :
        CONV_INTEGRAL_CONVERSION;
    return result;
  }
  if (types.IsScalar(source_value) &&
      types.Kind(types.Unqualified(target_value)) == TYPE_FUNDAMENTAL &&
      types.At(types.Unqualified(target_value)).fundamental == FT_BOOL)
  {
    result.rank = RANK_CONVERSION;
    result.kind = CONV_BOOLEAN;
    return result;
  }
  return ImplicitConversion();
}

ConversionComparison Compare(const ImplicitConversion& left,
                             const ImplicitConversion& right)
{
  if (left.rank < right.rank)
    return CONVERSION_BETTER;
  if (left.rank > right.rank)
    return CONVERSION_WORSE;
  if (left.rank == RANK_NONE)
    return CONVERSION_EQUAL;
  if (left.reference != right.reference)
  {
    if (left.reference == REFERENCE_DIRECT)
      return CONVERSION_BETTER;
    if (right.reference == REFERENCE_DIRECT)
      return CONVERSION_WORSE;
  }
  if (left.qualification != right.qualification)
    return left.qualification ? CONVERSION_WORSE : CONVERSION_BETTER;
  if (left.rvalue_ref_to_rvalue != right.rvalue_ref_to_rvalue)
    return left.rvalue_ref_to_rvalue ? CONVERSION_BETTER : CONVERSION_WORSE;
  if (left.function_lvalue_to_lvalue_ref != right.function_lvalue_to_lvalue_ref)
    return left.function_lvalue_to_lvalue_ref ? CONVERSION_BETTER :
        CONVERSION_WORSE;
  return CONVERSION_EQUAL;
}
