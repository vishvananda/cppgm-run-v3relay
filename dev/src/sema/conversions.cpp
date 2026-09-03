#include "sema/conversions.h"

#include <algorithm>

#include "sema/overload.h"

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

bool IsVolatile(const TypeTable& types, TypeId type)
{
  return type != 0 && types.Kind(type) == TYPE_CV &&
      types.At(type).is_volatile;
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
  const bool source_volatile = IsVolatile(types, source);
  const bool target_volatile = IsVolatile(types, target);
  if ((source_const && !target_const) ||
      (source_volatile && !target_volatile))
    return false;
  if (target_const && !source_const)
    added = true;
  if (target_volatile && !source_volatile)
    added = true;
  TypeId s = types.Unqualified(source);
  TypeId t = types.Unqualified(target);
  if (s == t)
    return true;
  if (types.Kind(s) == TYPE_ARRAY && types.Kind(t) == TYPE_ARRAY &&
      types.At(s).array_bound == types.At(t).array_bound)
  {
    bool nested_added = false;
    if (!QualificationCompatible(types, types.At(s).base,
                                 types.At(t).base, nested_added))
      return false;
    added = added || nested_added;
    return true;
  }
  if (types.Kind(s) != TYPE_POINTER || types.Kind(t) != TYPE_POINTER)
    return false;

  bool nested_added = false;
  if (!QualificationCompatible(types, types.At(s).base, types.At(t).base,
                               nested_added))
    return false;
  if (nested_added && !target_const && !target_volatile &&
      !IsConst(types, types.At(t).base) &&
      !IsVolatile(types, types.At(t).base))
    return false;
  added = added || nested_added;
  return true;
}

bool SameUnqualified(const TypeTable& types, TypeId left, TypeId right)
{
  return types.Unqualified(left) == types.Unqualified(right);
}

bool IsScopedEnum(const TypeTable& types, TypeId type)
{
  type = types.Unqualified(type);
  return types.Kind(type) == TYPE_ENUM && types.At(type).scoped;
}

bool PointerToVoidCompatible(const TypeTable& types, TypeId source,
                             TypeId target, bool& added)
{
  added = false;
  source = types.Unqualified(source);
  target = types.Unqualified(target);
  if (types.Kind(source) != TYPE_POINTER ||
      types.Kind(target) != TYPE_POINTER)
    return false;
  const TypeId source_pointee = types.At(source).base;
  const TypeId target_pointee = types.At(target).base;
  const TypeId target_unqualified = types.Unqualified(target_pointee);
  if (types.Kind(target_unqualified) != TYPE_FUNDAMENTAL ||
      types.At(target_unqualified).fundamental != FT_VOID ||
      types.Kind(types.Unqualified(source_pointee)) == TYPE_FUNCTION)
    return false;
  if (IsConst(types, source_pointee) &&
      !IsConst(types, target_pointee))
    return false;
  if (IsVolatile(types, source_pointee) &&
      !IsVolatile(types, target_pointee))
    return false;
  added = (IsConst(types, target_pointee) &&
           !IsConst(types, source_pointee)) ||
      (IsVolatile(types, target_pointee) &&
       !IsVolatile(types, source_pointee));
  return true;
}

unsigned& UserDefinedConversionDepth()
{
  static thread_local unsigned depth = 0;
  return depth;
}

class UserDefinedConversionGuard
{
public:
  UserDefinedConversionGuard()
  {
    ++UserDefinedConversionDepth();
  }

  ~UserDefinedConversionGuard()
  {
    --UserDefinedConversionDepth();
  }
};

ImplicitConversion ClassifyValue(TypeTable& types, TypeId source,
                                 ValueCategory source_category,
                                 bool is_null_literal, TypeId target)
{
  ImplicitConversion result;
  if (source == 0 || target == 0)
    return result;

  TypeId source_value = source;
  if (types.Kind(source_value) == TYPE_REFERENCE)
    source_value = types.Referent(source_value);
  const bool source_array = types.Kind(source_value) == TYPE_ARRAY;
  const bool source_function = types.Kind(source_value) == TYPE_FUNCTION;

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
    // The lvalue-to-rvalue conversion discards cv-qualification on the
    // value itself, but not qualification nested in a pointer or aggregate
    // type.  Normalize that one outer layer before checking the target so a
    // const pointer member can still select a const-pointee overload.
    source_value = types.Unqualified(source_value);
  }

  if (is_null_literal && !types.IsNullPointerType(source_value) &&
      types.IsNullPointerType(target))
  {
    result.rank = RANK_CONVERSION;
    result.kind = CONV_NULL_TO_NULLPTR;
    return result;
  }
  if (is_null_literal && types.IsPointer(target))
  {
    result.rank = RANK_CONVERSION;
    result.kind = CONV_NULL_TO_POINTER;
    return result;
  }
  if (types.IsNullPointerType(source_value) && types.IsPointer(target))
  {
    result.rank = RANK_CONVERSION;
    result.kind = CONV_NULL_TO_POINTER;
    return result;
  }

  if (SameUnqualified(types, source_value, target))
  {
    bool qualification = false;
    if (!QualificationCompatible(types, source_value, target,
                                 qualification))
    {
      // Top-level cv is discarded by the lvalue-to-rvalue conversion when a
      // value is passed by value.  It is not a qualification conversion (and
      // the reference path above deliberately does not use this fallback).
      if (types.Unqualified(source_value) != types.Unqualified(target))
        return ImplicitConversion();
      qualification = false;
    }
    result.rank = RANK_EXACT;
    result.qualification = qualification;
    if (qualification)
      result.kind = CONV_QUALIFICATION;
    return result;
  }

  if (types.IsPointer(source_value) && types.IsPointer(target))
  {
    bool qualification = false;
    if (QualificationCompatible(types, source_value, target,
                                qualification))
    {
      result.rank = RANK_EXACT;
      result.kind = qualification ? CONV_QUALIFICATION : CONV_POINTER;
      result.qualification = qualification;
      return result;
    }
    if (PointerToVoidCompatible(types, source_value, target, qualification))
    {
      result.rank = RANK_CONVERSION;
      result.kind = CONV_POINTER;
      result.qualification = qualification;
      return result;
    }
  }
  if (types.IsPointer(source_value) &&
      types.Kind(types.Unqualified(target)) == TYPE_FUNDAMENTAL &&
      types.At(types.Unqualified(target)).fundamental == FT_BOOL)
  {
    result.rank = RANK_CONVERSION;
    result.kind = CONV_POINTER_TO_BOOL;
    return result;
  }

  const bool target_enum =
      types.Kind(types.Unqualified(target)) == TYPE_ENUM;
  // A scoped enumeration is arithmetic for its own operators, but it does
  // not participate in the implicit integral conversions of an unscoped
  // enumeration.  Identity above still permits E -> E.
  if (!IsScopedEnum(types, source_value) &&
      !IsScopedEnum(types, target) &&
      types.IsArithmetic(source_value) && types.IsArithmetic(target))
  {
    if (types.IsIntegral(source_value) && types.IsIntegral(target))
    {
      const TypeId promoted = types.Promote(source_value);
      if (promoted == types.Unqualified(target) && !target_enum)
      {
        result.rank = RANK_PROMOTION;
        result.kind = CONV_INTEGRAL_PROMOTION;
      }
      else
      {
        result.rank = RANK_CONVERSION;
        result.kind = CONV_INTEGRAL_CONVERSION;
      }
      return result;
    }
    if (IsFloating(types, source_value) && IsFloating(types, target))
    {
      const std::size_t source_size = FundamentalSize(
          types.At(types.Unqualified(source_value)).fundamental);
      const std::size_t target_size = FundamentalSize(
          types.At(types.Unqualified(target)).fundamental);
      result.rank = source_size < target_size ? RANK_PROMOTION :
          RANK_CONVERSION;
      result.kind = source_size < target_size ? CONV_FLOATING_PROMOTION :
          CONV_FLOATING_CONVERSION;
      return result;
    }
    result.rank = RANK_CONVERSION;
    result.kind = IsFloating(types, target) ? CONV_FLOATING_CONVERSION :
        CONV_INTEGRAL_CONVERSION;
    return result;
  }
  if (!IsScopedEnum(types, source_value) && types.IsScalar(source_value) &&
      types.Kind(types.Unqualified(target)) == TYPE_FUNDAMENTAL &&
      types.At(types.Unqualified(target)).fundamental == FT_BOOL)
  {
    result.rank = RANK_CONVERSION;
    result.kind = CONV_BOOLEAN;
    return result;
  }
  return ImplicitConversion();
}

} // namespace

void MemberObjectQualifiers(const TypeTable& types, TypeId object_type,
                            ETokenType access_operator, bool& is_const,
                            bool& is_volatile)
{
  is_const = false;
  is_volatile = false;
  if (types.Kind(object_type) == TYPE_REFERENCE)
    object_type = types.Referent(object_type);
  if (access_operator == OP_ARROW) {
    if (!types.IsPointer(object_type))
      return;
    object_type = types.At(types.Unqualified(object_type)).base;
  }
  if (types.Kind(object_type) == TYPE_CV) {
    is_const = types.At(object_type).is_const;
    is_volatile = types.At(object_type).is_volatile;
  }
}

TypeId MemberAccessType(TypeTable& types, TypeId member_type,
                        bool static_member, bool mutable_member,
                        TypeId object_type, ETokenType access_operator)
{
  const bool reference_member =
      types.Kind(types.Unqualified(member_type)) == TYPE_REFERENCE;
  if (static_member || reference_member)
    return reference_member ? types.Referent(member_type) : member_type;
  if (mutable_member)
    return member_type;
  bool is_const = false;
  bool is_volatile = false;
  MemberObjectQualifiers(types, object_type, access_operator, is_const,
                         is_volatile);
  return is_const || is_volatile ?
      types.Cv(member_type, is_const, is_volatile) : member_type;
}

ImplicitConversion::ImplicitConversion()
    : rank(RANK_NONE), kind(CONV_IDENTITY), qualification(false),
      reference(REFERENCE_NONE), rvalue_ref_to_rvalue(false),
      function_lvalue_to_lvalue_ref(false), implicit_object(false),
      source_type(0), target_type(0)
{
}

ImplicitConversion Classify(TypeTable& types, TypeId source,
                            ValueCategory source_category,
                            bool is_null_literal,
                            bool is_function_lvalue, TypeId target)
{
  ImplicitConversion result;
  if (source == 0 || target == 0)
    return result;

  const bool target_reference = types.Kind(target) == TYPE_REFERENCE;
  TypeId target_value = target_reference ? types.Referent(target) : target;
  const bool target_rvalue_reference = target_reference &&
      !types.At(target).lvalue_reference;

  TypeId source_value = source;
  if (types.Kind(source_value) == TYPE_REFERENCE)
    source_value = types.Referent(source_value);
  const bool source_function = types.Kind(source_value) == TYPE_FUNCTION;

  if (target_reference)
  {
    bool qualification = false;
    const bool related = QualificationCompatible(types, source_value,
                                                  target_value, qualification);
    if (related && source_category == VC_LVALUE && !target_rvalue_reference)
    {
      result.rank = RANK_EXACT;
      result.kind = qualification ? CONV_QUALIFICATION : CONV_IDENTITY;
      result.qualification = qualification;
      result.reference = REFERENCE_DIRECT;
      result.function_lvalue_to_lvalue_ref = source_function ||
          is_function_lvalue;
      return result;
    }
    if (related && source_category == VC_LVALUE && target_rvalue_reference &&
        !source_function && !is_function_lvalue)
      return result;
    if (related && source_category == VC_XVALUE && !target_rvalue_reference &&
        !(IsConst(types, target_value) || IsVolatile(types, target_value)))
      return result;
    if (related && source_category == VC_XVALUE)
    {
      result.rank = RANK_EXACT;
      result.kind = qualification ? CONV_QUALIFICATION : CONV_IDENTITY;
      result.qualification = qualification;
      result.reference = REFERENCE_DIRECT;
      result.rvalue_ref_to_rvalue = target_rvalue_reference;
      return result;
    }
    if (related && source_category == VC_PRVALUE &&
        (target_rvalue_reference ||
         IsConst(types, target_value) || IsVolatile(types, target_value)))
    {
      result.rank = RANK_EXACT;
      result.kind = qualification ? CONV_QUALIFICATION : CONV_IDENTITY;
      result.qualification = qualification;
      result.reference = REFERENCE_DIRECT;
      result.rvalue_ref_to_rvalue = target_rvalue_reference;
      return result;
    }

    // A converted temporary can bind a const lvalue reference or an rvalue
    // reference.  Same-type lvalues were rejected above for rvalue refs, so
    // this path cannot silently turn `int` into `const int&&`.
    const bool drops_source_qualification =
        (IsConst(types, source_value) && !IsConst(types, target_value)) ||
        (IsVolatile(types, source_value) &&
         !IsVolatile(types, target_value));
    if (drops_source_qualification)
      return result;
    ImplicitConversion converted = ClassifyValue(
        types, source_value, source_category, is_null_literal, target_value);
    if (converted.Viable() &&
        (target_rvalue_reference || IsConst(types, target_value) ||
         IsVolatile(types, target_value)))
    {
      converted.reference = REFERENCE_TEMPORARY;
      converted.rvalue_ref_to_rvalue = target_rvalue_reference;
      return converted;
    }
    return result;
  }
  return ClassifyValue(types, source, source_category, is_null_literal,
                       target_value);
}

namespace
{

bool ClassEntityForType(const TypeTable& types, TypeId type,
                        ClassEntityId& entity)
{
  if (type == 0)
    return false;
  if (types.Kind(type) == TYPE_REFERENCE)
    type = types.Referent(type);
  type = types.Unqualified(type);
  if (types.Kind(type) != TYPE_CLASS)
    return false;
  entity = static_cast<ClassEntityId>(types.At(type).entity);
  return entity != 0;
}

bool CVCompatibleForBase(const TypeTable& types, TypeId source, TypeId target)
{
  return !(IsConst(types, source) && !IsConst(types, target)) &&
      !(IsVolatile(types, source) && !IsVolatile(types, target));
}

bool HasConvertingConstructor(const SemaModel& model, TypeTable& types,
                              TypeId source, ValueCategory source_category,
                              bool is_null_literal,
                              bool is_function_lvalue, TypeId target)
{
  TypeId target_value = target;
  if (types.Kind(target_value) == TYPE_REFERENCE)
    target_value = types.Referent(target_value);
  const TypeId class_type = types.Unqualified(target_value);
  if (types.Kind(class_type) != TYPE_CLASS)
    return false;

  TypeId source_value = source;
  if (types.Kind(source_value) == TYPE_REFERENCE)
    source_value = types.Referent(source_value);
  if (types.Unqualified(source_value) == class_type)
    return false;

  const ClassEntityId class_entity =
      static_cast<ClassEntityId>(types.At(class_type).entity);
  const ClassEntity& owner = model.ClassAt(class_entity);
  if (owner.class_scope == 0 || owner.constructors.empty())
    return false;

  std::vector<BindingId> bindings;
  model.DirectBindings(owner.class_scope,
                       model.ScopeAt(owner.class_scope).name,
                       LOOKUP_FUNCTIONS, bindings);
  std::vector<BindingId> converting;
  for (std::size_t i = 0; i < bindings.size(); ++i)
  {
    const Binding& binding = model.BindingAt(bindings[i]);
    if (binding.function == 0)
      continue;
    const FunctionEntity& function = model.FunctionAt(binding.function);
    // An implicit conversion is a copy-initialization of the target class;
    // explicit constructors are therefore not candidates here.
    if (function.special_member == SPECIAL_MEMBER_CONSTRUCTOR &&
        !function.deleted && !function.explicit_constructor)
      converting.push_back(bindings[i]);
  }
  if (converting.empty())
    return false;

  std::vector<OverloadArgument> arguments;
  arguments.push_back(OverloadArgument(
      types.Pointer(class_type), VC_PRVALUE, false, false, true));
  arguments.push_back(OverloadArgument(
      source, source_category, is_null_literal, is_function_lvalue));
  UserDefinedConversionGuard guard;
  return SelectBestOverload(model, types, converting, arguments, true) != 0;
}

} // namespace

ImplicitConversion Classify(const SemaModel& model, TypeTable& types,
                            TypeId source, ValueCategory source_category,
                            bool is_null_literal,
                            bool is_function_lvalue, TypeId target)
{
  const ImplicitConversion standard = Classify(
      types, source, source_category, is_null_literal,
      is_function_lvalue, target);
  if (standard.Viable())
    return standard;

  const bool target_reference = types.Kind(target) == TYPE_REFERENCE;
  TypeId source_value = source;
  if (types.Kind(source_value) == TYPE_REFERENCE)
    source_value = types.Referent(source_value);
  TypeId target_value = target_reference ? types.Referent(target) : target;

  ClassEntityId source_class = 0;
  ClassEntityId target_class = 0;
  if (ClassEntityForType(types, source_value, source_class) &&
      ClassEntityForType(types, target_value, target_class) &&
      model.IsDerivedFrom(source_class, target_class) &&
      (!target_reference || CVCompatibleForBase(
          types, source_value, target_value)))
  {
    ImplicitConversion result;
    result.rank = RANK_CONVERSION;
    result.kind = CONV_DERIVED_TO_BASE;
    if (target_reference)
    {
      const bool target_rvalue = !types.At(target).lvalue_reference;
      if (target_rvalue) {
        if (source_category != VC_PRVALUE && source_category != VC_XVALUE)
          return ImplicitConversion();
        result.reference = source_category == VC_PRVALUE ?
            REFERENCE_TEMPORARY : REFERENCE_DIRECT;
        result.rvalue_ref_to_rvalue = true;
      } else {
        if (source_category == VC_LVALUE) {
          result.reference = REFERENCE_DIRECT;
        } else if (source_category == VC_PRVALUE ||
                   source_category == VC_XVALUE) {
          if (!IsConst(types, target_value) &&
              !IsVolatile(types, target_value))
            return ImplicitConversion();
          result.reference = REFERENCE_TEMPORARY;
        } else
          return ImplicitConversion();
      }
    }
    return result;
  }

  const TypeId source_unqualified = types.Unqualified(source_value);
  const TypeId target_unqualified = types.Unqualified(target_value);
  if (types.Kind(source_unqualified) == TYPE_POINTER &&
      types.Kind(target_unqualified) == TYPE_POINTER)
  {
    const TypeId source_pointee = types.At(source_unqualified).base;
    const TypeId target_pointee = types.At(target_unqualified).base;
    if (ClassEntityForType(types, source_pointee, source_class) &&
        ClassEntityForType(types, target_pointee, target_class) &&
        model.IsDerivedFrom(source_class, target_class) &&
        CVCompatibleForBase(types, source_pointee, target_pointee))
    {
      ImplicitConversion result;
      result.rank = RANK_CONVERSION;
      result.kind = CONV_DERIVED_TO_BASE;
      result.qualification = IsConst(types, target_pointee) &&
          !IsConst(types, source_pointee);
      if (target_reference)
      {
        const bool target_rvalue = !types.At(target).lvalue_reference;
        if (!target_rvalue && !IsConst(types, target_value) &&
            !IsVolatile(types, target_value))
          return ImplicitConversion();
        result.reference = REFERENCE_TEMPORARY;
        result.rvalue_ref_to_rvalue = target_rvalue;
        result.qualification = result.qualification ||
            (IsConst(types, target_value) &&
             !IsConst(types, source_value)) ||
            (IsVolatile(types, target_value) &&
             !IsVolatile(types, source_value));
      }
      return result;
    }
  }

  // 13.3.3.1.2: a converting constructor contributes one user-defined
  // conversion sequence after the standard sequence has been ruled out.
  // The guard prevents constructor-parameter ranking from recursively
  // introducing a second user-defined conversion into the same sequence.
  if (UserDefinedConversionDepth() == 0 &&
      (!target_reference ||
       !types.At(target).lvalue_reference ||
       IsConst(types, target_value) || IsVolatile(types, target_value)) &&
      HasConvertingConstructor(model, types, source, source_category,
                               is_null_literal, is_function_lvalue, target))
  {
    ImplicitConversion result;
    result.rank = RANK_CONVERSION;
    result.kind = CONV_USER_DEFINED;
    if (target_reference)
    {
      result.reference = REFERENCE_TEMPORARY;
      result.rvalue_ref_to_rvalue = !types.At(target).lvalue_reference;
    }
    return result;
  }
  return standard;
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
  if ((left.kind == CONV_POINTER_TO_BOOL) !=
      (right.kind == CONV_POINTER_TO_BOOL))
    return left.kind == CONV_POINTER_TO_BOOL ? CONVERSION_WORSE :
        CONVERSION_BETTER;
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

TypeId CompositePointer(const SemaModel& model, TypeTable& types,
                        TypeId left, TypeId right, bool& ok)
{
  const TypeId left_pointer = types.Decay(left);
  const TypeId right_pointer = types.Decay(right);
  const TypeId composite = types.CompositePointer(
      left_pointer, right_pointer, ok);
  if (ok)
    return composite;
  if (!types.IsPointer(left_pointer) || !types.IsPointer(right_pointer))
    return 0;

  const ImplicitConversion left_to_right = Classify(
      model, types, left_pointer, VC_PRVALUE, false, false, right_pointer);
  if (left_to_right.Viable() &&
      left_to_right.kind == CONV_DERIVED_TO_BASE)
  {
    ok = true;
    return right_pointer;
  }
  const ImplicitConversion right_to_left = Classify(
      model, types, right_pointer, VC_PRVALUE, false, false, left_pointer);
  if (right_to_left.Viable() &&
      right_to_left.kind == CONV_DERIVED_TO_BASE)
  {
    ok = true;
    return left_pointer;
  }
  ok = false;
  return 0;
}
