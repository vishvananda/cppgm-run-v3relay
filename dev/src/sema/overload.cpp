#include "sema/overload.h"

#include <algorithm>

namespace
{

// 13.3.3p1: better on no argument worse and at least one strictly better.
bool BetterThan(const std::vector<ImplicitConversion>& left,
                const std::vector<ImplicitConversion>& right)
{
  bool strict = false;
  for (std::size_t i = 0; i < left.size(); ++i)
  {
    const ConversionComparison comparison = Compare(left[i], right[i]);
    if (comparison == CONVERSION_WORSE)
      return false;
    if (comparison == CONVERSION_BETTER)
      strict = true;
  }
  return strict;
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

FunctionEntityId SelectBestOverload(
    const SemaModel& model, TypeTable& types,
    const std::vector<BindingId>& bindings,
    const std::vector<OverloadArgument>& arguments)
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
    std::size_t required = function.parameters.size();
    while (required > 0 && required <= entity.default_arguments.size() &&
           entity.default_arguments[required - 1] != 0)
      --required;
    if (arguments.size() < required ||
        (!function.variadic && arguments.size() > function.parameters.size()))
      continue;

    Candidate candidate;
    candidate.function = entities[i];
    candidate.conversions.reserve(arguments.size());
    bool viable_candidate = true;
    for (std::size_t argument = 0; argument < arguments.size(); ++argument)
    {
      if (argument >= function.parameters.size())
      {
        ImplicitConversion ellipsis;
        ellipsis.rank = RANK_ELLIPSIS;
        candidate.conversions.push_back(ellipsis);
        continue;
      }
      const OverloadArgument& source = arguments[argument];
      const TypeId parameter = function.parameters[argument];
      TypeId source_type = source.type;
      ValueCategory source_category = source.category;
      bool function_lvalue = source.is_function_lvalue;
      TypeId target_function = 0;
      if (!source.function_candidates.empty() &&
          TargetFunctionType(types, parameter, target_function))
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
      const ImplicitConversion conversion = Classify(
          types, source_type, source_category, source.is_null_literal,
          function_lvalue, parameter);
      if (!conversion.Viable())
      {
        viable_candidate = false;
        break;
      }
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
    if (BetterThan(viable[i].conversions, viable[best].conversions))
      best = i;
  for (std::size_t i = 0; i < viable.size(); ++i)
    if (i != best &&
        !BetterThan(viable[best].conversions, viable[i].conversions))
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
