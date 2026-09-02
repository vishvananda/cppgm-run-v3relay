#include "sema/overload.h"

#include <algorithm>

namespace
{

bool Contains(const std::vector<FunctionEntityId>& values,
              FunctionEntityId value)
{
  return std::find(values.begin(), values.end(), value) != values.end();
}

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

bool TargetMemberFunctionType(TypeTable& types, TypeId target,
                              TypeId& member_class, TypeId& function)
{
  if (target == 0)
    return false;
  if (types.Kind(target) == TYPE_REFERENCE)
    target = types.Referent(target);
  target = types.Unqualified(target);
  if (types.Kind(target) != TYPE_MEMBER_POINTER)
    return false;
  const TypeNode& member = types.At(target);
  if (types.Kind(types.Unqualified(member.base)) != TYPE_FUNCTION)
    return false;
  member_class = member.member_class;
  function = types.Unqualified(member.base);
  return true;
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

bool SelectBestOverload(const SemaModel& model, TypeTable& types,
                        const std::vector<BindingId>& bindings,
                        const std::vector<OverloadArgument>& arguments,
                        OverloadSelection& selection)
{
  struct Candidate
  {
    FunctionEntityId function;
    std::vector<ImplicitConversion> conversions;
  };

  std::vector<Candidate> viable;
  std::vector<FunctionEntityId> seen;
  for (std::size_t i = 0; i < bindings.size(); ++i)
  {
    const Binding& binding = model.BindingAt(bindings[i]);
    if (binding.kind != BINDING_FUNCTION || binding.function == 0 ||
        Contains(seen, binding.function) ||
        model.FunctionAt(binding.function).is_template)
      continue;
    seen.push_back(binding.function);

    const TypeId function_type = model.FunctionAt(binding.function).type;
    if (types.Kind(types.Unqualified(function_type)) != TYPE_FUNCTION)
      continue;
    const TypeNode& function = types.At(types.Unqualified(function_type));
    if ((!function.variadic && function.parameters.size() != arguments.size()) ||
        (function.variadic && arguments.size() < function.parameters.size()))
      continue;

    Candidate candidate;
    candidate.function = binding.function;
    candidate.conversions.reserve(arguments.size());
    bool viable_candidate = true;
    for (std::size_t argument = 0; argument < arguments.size(); ++argument)
    {
      if (argument >= function.parameters.size())
      {
        ImplicitConversion ellipsis;
        ellipsis.rank = RANK_ELLIPSIS;
        ellipsis.to = 0;
        candidate.conversions.push_back(ellipsis);
        continue;
      }
      OverloadArgument source = arguments[argument];
      TypeId target_function = 0;
      if (!source.function_candidates.empty() &&
          TargetFunctionType(types, function.parameters[argument],
                             target_function))
      {
        FunctionEntityId matched = 0;
        for (std::size_t candidate = 0;
             candidate < source.function_candidates.size(); ++candidate)
        {
          const FunctionEntityId candidate_function =
              source.function_candidates[candidate];
          if (model.FunctionAt(candidate_function).type == target_function)
          {
            if (matched != 0)
            {
              viable_candidate = false;
              break;
            }
            matched = candidate_function;
          }
        }
        if (!viable_candidate || matched == 0)
        {
          viable_candidate = false;
          break;
        }
        source.type = model.FunctionAt(matched).type;
        source.category = VC_LVALUE;
        source.is_function_lvalue = true;
      }
      const ImplicitConversion conversion = Classify(
          types, source.type, source.category, source.is_null_literal,
          source.is_function_lvalue, function.parameters[argument]);
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
    return false;

  std::vector<std::size_t> best;
  for (std::size_t i = 0; i < viable.size(); ++i)
  {
    bool dominated = false;
    for (std::size_t j = 0; j < viable.size(); ++j)
      if (i != j && BetterThan(viable[j].conversions,
                               viable[i].conversions))
      {
        dominated = true;
        break;
      }
    if (!dominated)
      best.push_back(i);
  }
  if (best.size() != 1)
    return false;
  selection.function = viable[best[0]].function;
  selection.conversions = viable[best[0]].conversions;
  return true;
}

bool SelectTargetFunction(const SemaModel& model, TypeTable& types,
                          const std::vector<BindingId>& bindings,
                          TypeId target, BindingId& binding,
                          FunctionEntityId& function)
{
  TypeId target_member_class = 0;
  TypeId target_member_function = 0;
  const bool member_target = TargetMemberFunctionType(
      types, target, target_member_class, target_member_function);
  (void)target_member_class;
  (void)target_member_function;
  TypeId target_function = 0;
  if (!member_target && !TargetFunctionType(types, target, target_function))
    return false;

  std::vector<FunctionEntityId> matches;
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
      if (!entity.is_member || entity.member_class == 0 ||
          !entity.member_type ||
          entity.member_pointer_type == 0 ||
          types.Unqualified(entity.member_pointer_type) !=
              types.Unqualified(target))
        continue;
    }
    else if (types.Unqualified(entity.type) != target_function)
      continue;
    if (!Contains(matches, candidate.function))
    {
      matches.push_back(candidate.function);
      matched_binding = bindings[i];
    }
  }
  if (matches.size() != 1)
    return false;
  binding = matched_binding;
  function = matches[0];
  return true;
}
