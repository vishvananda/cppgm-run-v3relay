#include "sema/overload.h"

#include <algorithm>

namespace
{

bool ConversionTargetClass(const TypeTable& types, TypeId type,
                           ClassEntityId& entity)
{
  if (type == 0)
    return false;
  if (types.Kind(type) == TYPE_REFERENCE)
    type = types.Referent(type);
  type = types.Unqualified(type);
  if (types.Kind(type) == TYPE_POINTER)
    type = types.Unqualified(types.At(type).base);
  if (types.Kind(type) != TYPE_CLASS)
    return false;
  entity = static_cast<ClassEntityId>(types.At(type).entity);
  return entity != 0;
}

// The standard conversion rank alone leaves two derived-to-base sequences
// tied.  [over.ics.rank] then prefers the nearer base; for pointer targets,
// a class-base conversion is also better than the competing conversion to
// void.  The endpoint metadata is attached while candidates are classified.
ConversionComparison CompareOverloadConversion(
    const SemaModel& model, const TypeTable& types,
    const ImplicitConversion& left, const ImplicitConversion& right)
{
  // A member operator's implicit object is represented as the canonical
  // pointer parameter, while a non-member candidate consumes the same
  // source expression through an ordinary reference parameter.  Reference
  // binding sub-ranks do not compare those two parameter kinds; the member
  // object's cv conversion still participates through rank/qualification.
  ImplicitConversion comparable_left = left;
  ImplicitConversion comparable_right = right;
  if (left.implicit_object || right.implicit_object) {
    comparable_left.reference = REFERENCE_NONE;
    comparable_right.reference = REFERENCE_NONE;
    comparable_left.rvalue_ref_to_rvalue = false;
    comparable_right.rvalue_ref_to_rvalue = false;
    comparable_left.function_lvalue_to_lvalue_ref = false;
    comparable_right.function_lvalue_to_lvalue_ref = false;
  }
  const ConversionComparison standard = Compare(comparable_left,
                                                comparable_right);
  if (standard != CONVERSION_EQUAL)
    return standard;
  const bool left_base = left.kind == CONV_DERIVED_TO_BASE;
  const bool right_base = right.kind == CONV_DERIVED_TO_BASE;
  if (left_base != right_base)
    return left_base ? CONVERSION_BETTER : CONVERSION_WORSE;

  ClassEntityId left_target = 0;
  ClassEntityId right_target = 0;
  if (!ConversionTargetClass(types, left.target_type, left_target) ||
      !ConversionTargetClass(types, right.target_type, right_target) ||
      left_target == right_target)
    return CONVERSION_EQUAL;
  if (model.IsDerivedFrom(left_target, right_target) &&
      !model.IsDerivedFrom(right_target, left_target))
    return CONVERSION_BETTER;
  if (model.IsDerivedFrom(right_target, left_target) &&
      !model.IsDerivedFrom(left_target, right_target))
    return CONVERSION_WORSE;
  return CONVERSION_EQUAL;
}

// 13.3.3p1: better on no argument worse and at least one strictly better.
bool BetterThan(const SemaModel& model, const TypeTable& types,
                const std::vector<ImplicitConversion>& left,
                const std::vector<ImplicitConversion>& right)
{
  bool strict = false;
  for (std::size_t i = 0; i < left.size(); ++i)
  {
    const ConversionComparison comparison = CompareOverloadConversion(
        model, types, left[i], right[i]);
    if (comparison == CONVERSION_WORSE)
      return false;
    if (comparison == CONVERSION_BETTER)
      strict = true;
  }
  return strict;
}

bool MemberRefQualifierViable(const FunctionEntity& entity,
                              ValueCategory object_category)
{
  if (!entity.is_member || entity.static_member)
    return true;
  if (entity.member_lvalue_ref_qualifier)
    return object_category == VC_LVALUE;
  if (entity.member_rvalue_ref_qualifier)
    return object_category != VC_LVALUE;
  return true;
}

// Function type denoted by a pointer/reference-to-function target.
bool TargetFunctionType(TypeTable& types, TypeId target, TypeId& function)
{
  if (target == 0)
    return false;
  if (types.Kind(target) == TYPE_REFERENCE)
    target = types.Referent(target);
  target = types.Unqualified(target);
  if (types.Kind(target) == TYPE_POINTER)
    target = types.Unqualified(types.At(target).base);
  if (types.Kind(target) != TYPE_FUNCTION)
    return false;
  function = target;
  return true;
}

bool IsMemberFunctionPointer(TypeTable& types, TypeId target)
{
  if (target == 0)
    return false;
  if (types.Kind(target) == TYPE_REFERENCE)
    target = types.Referent(target);
  target = types.Unqualified(target);
  return types.Kind(target) == TYPE_MEMBER_POINTER &&
      types.Kind(types.Unqualified(types.At(target).base)) == TYPE_FUNCTION;
}

// 13.4 applied to one argument: the unique candidate whose type equals the
// parameter's function type; 0 when none or several match.
FunctionEntityId MatchFunctionArgument(
    const SemaModel& model, const std::vector<FunctionEntityId>& candidates,
    TypeId function_type)
{
  FunctionEntityId matched = 0;
  for (std::size_t i = 0; i < candidates.size(); ++i)
  {
    if (model.FunctionAt(candidates[i]).type != function_type)
      continue;
    if (matched != 0)
      return 0;
    matched = candidates[i];
  }
  return matched;
}

} // namespace

bool CallableFunctionType(TypeTable& types, TypeId type,
                          TypeId& function_type)
{
  if (type == 0)
    return false;
  if (types.Kind(type) == TYPE_REFERENCE)
    type = types.Referent(type);
  type = types.Unqualified(type);
  if (types.Kind(type) == TYPE_FUNCTION)
  {
    function_type = type;
    return true;
  }
  if (types.Kind(type) == TYPE_POINTER)
  {
    const TypeId pointed = types.Unqualified(types.At(type).base);
    if (types.Kind(pointed) == TYPE_FUNCTION)
    {
      function_type = pointed;
      return true;
    }
  }
  return false;
}

void ConstructorCandidates(const SemaModel& model, const ClassEntity& owner,
                           bool copy_initialization,
                           std::vector<BindingId>& candidates)
{
  std::vector<BindingId> declared;
  model.DirectBindings(owner.class_scope,
                       model.ScopeAt(owner.class_scope).name,
                       LOOKUP_FUNCTIONS, declared);
  for (std::size_t i = 0; i < declared.size(); ++i)
  {
    const Binding& binding = model.BindingAt(declared[i]);
    if (binding.function == 0)
      continue;
    const FunctionEntity& function = model.FunctionAt(binding.function);
    if (function.special_member != SPECIAL_MEMBER_CONSTRUCTOR ||
        (copy_initialization && function.explicit_constructor))
      continue;
    candidates.push_back(declared[i]);
  }
}

FunctionEntityId SelectBestOverload(
    const SemaModel& model, TypeTable& types,
    const std::vector<BindingId>& bindings,
    const std::vector<OverloadArgument>& arguments,
    bool has_implicit_object)
{
  struct Candidate
  {
    FunctionEntityId function;
    std::vector<ImplicitConversion> conversions;
  };

  // Redeclarations bind one entity several times; the candidate set holds
  // each entity once (entity ids follow declaration order).
  std::vector<FunctionEntityId> entities;
  entities.reserve(bindings.size());
  for (std::size_t i = 0; i < bindings.size(); ++i)
  {
    const Binding& binding = model.BindingAt(bindings[i]);
    if (binding.kind == BINDING_FUNCTION && binding.function != 0 &&
        !model.FunctionAt(binding.function).is_template)
      entities.push_back(binding.function);
  }
  std::sort(entities.begin(), entities.end());
  entities.erase(std::unique(entities.begin(), entities.end()),
                 entities.end());

  std::vector<Candidate> viable;
  for (std::size_t i = 0; i < entities.size(); ++i)
  {
    const TypeId function_type = model.FunctionAt(entities[i]).type;
    if (types.Kind(types.Unqualified(function_type)) != TYPE_FUNCTION)
      continue;
    const TypeNode& function = types.At(types.Unqualified(function_type));
    const FunctionEntity& entity = model.FunctionAt(entities[i]);
    const bool member_object = entity.is_member && !entity.static_member;
    const bool supplied_object = has_implicit_object &&
        !arguments.empty() && arguments[0].is_implicit_object;
    if (member_object && !supplied_object)
      continue;
    const std::size_t parameter_start = member_object ? 1 : 0;
    if (member_object && supplied_object &&
        !MemberRefQualifierViable(entity,
            arguments[0].has_implicit_object_category ?
                arguments[0].implicit_object_category :
                arguments[0].category))
      continue;
    if (parameter_start > function.parameters.size())
      continue;
    const std::size_t explicit_parameters = function.parameters.size() -
        parameter_start;
    std::size_t required = explicit_parameters;
    while (required > 0 && parameter_start + required <=
           entity.default_arguments.size() &&
           entity.default_arguments[parameter_start + required - 1] != 0)
      --required;
    const std::size_t argument_start = supplied_object ? 1 : 0;
    if (arguments.size() < argument_start + required ||
        (!function.variadic && arguments.size() >
            argument_start + explicit_parameters))
      continue;

    Candidate candidate;
    candidate.function = entities[i];
    candidate.conversions.reserve(arguments.size());
    bool viable_candidate = true;
    if (supplied_object && !member_object)
    {
      // The same source call may be an overload set containing a static and
      // a non-static member.  Static candidates ignore the implicit object,
      // but keep an exact placeholder so conversion vectors remain aligned.
      ImplicitConversion ignored;
      ignored.rank = RANK_EXACT;
      ignored.kind = CONV_IDENTITY;
      candidate.conversions.push_back(ignored);
    }
    if (member_object)
    {
      const OverloadArgument& object = arguments[0];
      ImplicitConversion conversion = Classify(
          model, types, object.type, object.category, object.is_null_literal,
          object.is_function_lvalue, function.parameters[0]);
      conversion.source_type = object.type;
      conversion.target_type = function.parameters[0];
      conversion.implicit_object = object.is_implicit_object;
      if (!conversion.Viable())
        viable_candidate = false;
      else
        candidate.conversions.push_back(conversion);
    }
    for (std::size_t argument = argument_start;
         viable_candidate && argument < arguments.size(); ++argument)
    {
      const std::size_t explicit_argument = argument - argument_start;
      const std::size_t parameter = parameter_start + explicit_argument;
      if (parameter >= function.parameters.size())
      {
        ImplicitConversion ellipsis;
        ellipsis.rank = RANK_ELLIPSIS;
        candidate.conversions.push_back(ellipsis);
        continue;
      }
      const OverloadArgument& source = arguments[argument];
      const TypeId parameter_type = function.parameters[parameter];
      TypeId source_type = source.type;
      ValueCategory source_category = source.category;
      bool function_lvalue = source.is_function_lvalue;
      TypeId target_function = 0;
      if (!source.function_candidates.empty() &&
          TargetFunctionType(types, parameter_type, target_function))
      {
        const FunctionEntityId matched = MatchFunctionArgument(
            model, source.function_candidates, target_function);
        if (matched == 0)
        {
          viable_candidate = false;
          break;
        }
        source_type = model.FunctionAt(matched).type;
        source_category = VC_LVALUE;
        function_lvalue = true;
      }
      ImplicitConversion conversion = Classify(
          model, types, source_type, source_category, source.is_null_literal,
          function_lvalue, parameter_type, !source.standard_conversions_only);
      conversion.source_type = source_type;
      conversion.target_type = parameter_type;
      conversion.implicit_object = source.is_implicit_object;
      if (!conversion.Viable())
      {
        viable_candidate = false;
        break;
      }
      if (source.user_defined_conversion)
        conversion.rank = RANK_CONVERSION;
      candidate.conversions.push_back(conversion);
    }
    if (viable_candidate)
      viable.push_back(candidate);
  }

  if (viable.empty())
    return 0;

  // 13.3.3p2 in two linear passes: a candidate no later candidate beats,
  // then confirmation that it beats every other one.
  std::size_t best = 0;
  for (std::size_t i = 1; i < viable.size(); ++i)
    if (BetterThan(model, types, viable[i].conversions,
                   viable[best].conversions))
      best = i;
  for (std::size_t i = 0; i < viable.size(); ++i)
    if (i != best &&
        !BetterThan(model, types, viable[best].conversions,
                    viable[i].conversions))
      return 0;
  return viable[best].function;
}

FunctionEntityId SelectBestOverloadCandidates(
    const SemaModel& model, TypeTable& types,
    const std::vector<OverloadCandidate>& candidates)
{
  struct RankedCandidate
  {
    FunctionEntityId function;
    std::vector<ImplicitConversion> conversions;
  };

  std::vector<RankedCandidate> viable;
  std::vector<FunctionEntityId> seen;
  for (std::size_t i = 0; i < candidates.size(); ++i)
  {
    const FunctionEntityId id = candidates[i].function;
    if (id == 0 ||
        std::find(seen.begin(), seen.end(), id) != seen.end())
      continue;
    seen.push_back(id);
    const FunctionEntity& entity = model.FunctionAt(id);
    if (entity.is_template)
      continue;
    const TypeId function_type = entity.type;
    if (types.Kind(types.Unqualified(function_type)) != TYPE_FUNCTION)
      continue;
    const TypeNode& function = types.At(types.Unqualified(function_type));
    const std::vector<OverloadArgument>& arguments = candidates[i].arguments;
    const bool member_object = entity.is_member && !entity.static_member;
    if (member_object && !arguments.empty() &&
        arguments[0].is_implicit_object &&
        !MemberRefQualifierViable(entity,
            arguments[0].has_implicit_object_category ?
                arguments[0].implicit_object_category :
                arguments[0].category))
      continue;
    std::size_t required = function.parameters.size();
    while (required > 0 && required <= entity.default_arguments.size() &&
           entity.default_arguments[required - 1] != 0)
      --required;
    if (arguments.size() < required ||
        (!function.variadic && arguments.size() > function.parameters.size()))
      continue;

    RankedCandidate ranked;
    ranked.function = id;
    ranked.conversions.reserve(arguments.size());
    bool viable_candidate = true;
    for (std::size_t argument = 0;
         argument < arguments.size() && viable_candidate; ++argument)
    {
      if (argument >= function.parameters.size())
      {
        ImplicitConversion ellipsis;
        ellipsis.rank = RANK_ELLIPSIS;
        ranked.conversions.push_back(ellipsis);
        continue;
      }
      const OverloadArgument& source = arguments[argument];
      const TypeId parameter_type = function.parameters[argument];
      TypeId source_type = source.type;
      ValueCategory source_category = source.category;
      bool function_lvalue = source.is_function_lvalue;
      TypeId target_function = 0;
      if (!source.function_candidates.empty() &&
          TargetFunctionType(types, parameter_type, target_function))
      {
        const FunctionEntityId matched = MatchFunctionArgument(
            model, source.function_candidates, target_function);
        if (matched == 0)
        {
          viable_candidate = false;
          break;
        }
        source_type = model.FunctionAt(matched).type;
        source_category = VC_LVALUE;
        function_lvalue = true;
      }
      ImplicitConversion conversion = Classify(
          model, types, source_type, source_category, source.is_null_literal,
          function_lvalue, parameter_type, !source.standard_conversions_only);
      conversion.source_type = source_type;
      conversion.target_type = parameter_type;
      conversion.implicit_object = source.is_implicit_object;
      if (!conversion.Viable())
      {
        viable_candidate = false;
        break;
      }
      if (source.user_defined_conversion)
        conversion.rank = RANK_CONVERSION;
      ranked.conversions.push_back(conversion);
    }
    if (viable_candidate)
      viable.push_back(ranked);
  }
  if (viable.empty())
    return 0;

  std::size_t best = 0;
  for (std::size_t i = 1; i < viable.size(); ++i)
  {
    const bool better = BetterThan(model, types, viable[i].conversions,
                                   viable[best].conversions);
    if (better)
      best = i;
  }
  for (std::size_t i = 0; i < viable.size(); ++i)
    if (i != best &&
        !BetterThan(model, types, viable[best].conversions,
                    viable[i].conversions))
      return 0;
  return viable[best].function;
}

bool SelectTargetFunction(const SemaModel& model, TypeTable& types,
                          const std::vector<BindingId>& bindings,
                          TypeId target, BindingId& binding,
                          FunctionEntityId& function)
{
  const bool member_target = IsMemberFunctionPointer(types, target);
  TypeId target_function = 0;
  if (!member_target && !TargetFunctionType(types, target, target_function))
    return false;
  const TypeId target_unqualified = types.Unqualified(
      types.Kind(target) == TYPE_REFERENCE ? types.Referent(target) : target);

  FunctionEntityId matched = 0;
  BindingId matched_binding = 0;
  for (std::size_t i = 0; i < bindings.size(); ++i)
  {
    const Binding& candidate = model.BindingAt(bindings[i]);
    if (candidate.kind != BINDING_FUNCTION || candidate.function == 0 ||
        model.FunctionAt(candidate.function).is_template)
      continue;
    const FunctionEntity& entity = model.FunctionAt(candidate.function);
    if (member_target)
    {
      if (!entity.is_member || entity.member_pointer_type == 0 ||
          types.Unqualified(entity.member_pointer_type) != target_unqualified)
        continue;
    }
    else if (types.Unqualified(entity.type) != target_function)
      continue;
    if (matched != 0 && matched != candidate.function)
      return false;
    matched = candidate.function;
    matched_binding = bindings[i];
  }
  if (matched == 0)
    return false;
  binding = matched_binding;
  function = matched;
  return true;
}
