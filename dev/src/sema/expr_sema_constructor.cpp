#include "sema/expr_sema.h"

#include <stdexcept>

#include "sema/scope_builder.h"

using std::size_t;
using std::vector;

SemaId ExpressionAnalyzer::BuildConstructorTemporary(
    AstId source, TypeId target, ScopeId scope,
    const vector<SemaId>& arguments, bool list_initialization,
    bool copy_initialization, FunctionEntityId selected,
    BindingId destination_binding)
{
  const TypeId class_type = types_.Unqualified(target);
  if (types_.Kind(class_type) != TYPE_CLASS)
    throw std::runtime_error("constructor temporary target is not a class");
  // A conversion sequence that already selected its converting constructor
  // supplies it; every other form selects here.
  const FunctionEntityId constructor = selected != 0 ? selected :
      builder_.ResolveConstructor(
          class_type, arguments, scope, copy_initialization);
  const FunctionEntity& entity = model_.FunctionAt(constructor);
  const TypeNode& callable = types_.At(types_.Unqualified(entity.type));
  if (arguments.size() + 1 > callable.parameters.size())
    throw std::runtime_error("constructor temporary has too many arguments");

  vector<SemaId> converted;
  converted.reserve(callable.parameters.size() - 1);
  for (size_t i = 0; i < arguments.size(); ++i) {
    // 12.8p32 selects a move constructor in the return context by treating
    // the named local as an xvalue.  Preserve that selected value category
    // while binding the constructor's rvalue-reference parameter; otherwise
    // the later materialization would retry the argument as an lvalue and
    // reject the already-selected move operation.
    if (entity.move_constructor && tree_.At(arguments[i]).category == VC_LVALUE)
      tree_.At(arguments[i]).category = VC_XVALUE;
    converted.push_back(Initialize(arguments[i], callable.parameters[i + 1],
                                   false, list_initialization));
  }
  for (size_t parameter = arguments.size() + 1;
       parameter < callable.parameters.size(); ++parameter)
  {
    if (parameter >= entity.default_arguments.size() ||
        entity.default_arguments[parameter] == 0)
      throw std::runtime_error("missing constructor argument");
    converted.push_back(AnalyzeInitializer(
        entity.default_arguments[parameter], entity.scope,
        callable.parameters[parameter]));
  }

  const SemaId action = MakeExpression(SEMA_CONSTRUCTOR_ACTION, source,
                                       class_type, VC_XVALUE, scope);
  tree_.At(action).binding = destination_binding;
  tree_.At(action).function = constructor;
  const SemaId call = MakeExpression(
      SEMA_CALL, 0, types_.Fundamental(FT_VOID), VC_PRVALUE, scope);
  tree_.At(call).function = constructor;
  const SemaId callee = tree_.Make(SEMA_CALLEE);
  SemaNode& callee_node = tree_.At(callee);
  callee_node.scope = scope;
  callee_node.type = entity.type;
  callee_node.function = constructor;
  Append(action, call);
  Append(call, callee);
  if (destination_binding != 0)
  {
    const SemaId address = MakeExpression(
        SEMA_UNARY, 0, types_.Pointer(types_.Unqualified(target)),
        VC_PRVALUE, scope, OP_AMP);
    const SemaId object = MakeExpression(
        SEMA_ID_EXPRESSION, 0, target, VC_LVALUE, scope);
    tree_.At(object).binding = destination_binding;
    Append(address, object);
    Append(call, address);
  }
  for (size_t i = 0; i < converted.size(); ++i)
    Append(call, converted[i]);
  return action;
}

TypeId ExpressionAnalyzer::BracedArgumentTarget(
    TypeId target, std::size_t index, bool copy_initialization) const
{
  const TypeId class_type = types_.Unqualified(target);
  if (types_.Kind(class_type) != TYPE_CLASS)
    return 0;
  const ClassEntity& owner = model_.ClassAt(types_.At(class_type).entity);
  std::vector<BindingId> candidates;
  ConstructorCandidates(model_, owner, copy_initialization, candidates);
  TypeId common = 0;
  for (std::size_t i = 0; i < candidates.size(); ++i)
  {
    const FunctionEntityId function = model_.BindingAt(candidates[i]).function;
    const TypeNode& callable = types_.At(
        types_.Unqualified(model_.FunctionAt(function).type));
    if (index + 1 >= callable.parameters.size())
      continue;
    TypeId parameter = callable.parameters[index + 1];
    if (types_.Kind(types_.Unqualified(parameter)) == TYPE_REFERENCE)
      parameter = types_.Referent(parameter);
    parameter = types_.Unqualified(parameter);
    if (common == 0)
      common = parameter;
    else if (types_.Kind(parameter) != types_.Kind(common) ||
             (types_.Kind(parameter) == TYPE_CLASS &&
              types_.At(parameter).entity != types_.At(common).entity))
      return 0;
  }
  return common;
}

SemaId ExpressionAnalyzer::MaterializeBracedArgument(AstId argument,
                                                     TypeId target,
                                                     ScopeId scope)
{
  TypeId value_target = target;
  if (types_.Kind(types_.Unqualified(value_target)) == TYPE_REFERENCE)
    value_target = types_.Referent(value_target);
  value_target = types_.Unqualified(value_target);
  if (types_.Kind(value_target) == TYPE_CLASS)
  {
    const ClassEntity& owner = model_.ClassAt(types_.At(value_target).entity);
    std::vector<SemaId> values;
    if (owner.aggregate)
    {
      const SemaId aggregate = AnalyzeBraced(argument, scope, value_target);
      for (SemaId child = tree_.At(aggregate).first_child; child != 0;
           child = tree_.At(child).next_sibling)
        values.push_back(child);
    }
    else
    {
      const std::vector<AstId>& clauses = arena_.At(argument).children;
      for (std::size_t i = 0; i < clauses.size(); ++i)
        values.push_back(Analyze(clauses[i], scope));
    }
    return BuildConstructorTemporary(argument, value_target, scope, values,
                                     true, false);
  }
  return AnalyzeInitializer(argument, scope, value_target);
}

SemaId ExpressionAnalyzer::BuildConstructorCall(
    AstId source, TypeId target, ScopeId scope,
    const std::vector<AstId>& arguments, bool list_initialization,
    bool copy_initialization, BindingId destination_binding)
{
  std::vector<SemaId> analyzed;
  analyzed.reserve(arguments.size());
  for (std::size_t i = 0; i < arguments.size(); ++i)
  {
    if (arena_.At(arguments[i]).kind == AST_BRACED_INIT_LIST)
    {
      const TypeId argument_target = BracedArgumentTarget(
          target, i, copy_initialization);
      if (argument_target == 0)
        throw std::runtime_error("braced constructor argument has no target");
      analyzed.push_back(MaterializeBracedArgument(
          arguments[i], argument_target, scope));
    }
    else
      analyzed.push_back(Analyze(arguments[i], scope));
  }
  return BuildConstructorTemporary(source, target, scope, analyzed,
                                   list_initialization, copy_initialization,
                                   0, destination_binding);
}
