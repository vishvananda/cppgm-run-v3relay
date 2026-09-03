#include "sema/expr_sema.h"

#include <stdexcept>

#include "sema/scope_builder.h"

using std::size_t;
using std::vector;

// 8.5.1 aggregate initialization: a braced list is matched to the fields
// of a class or the elements of an array in order, with brace elision,
// string-literal array clauses, and class elements that name a constructor.

SemaId ExpressionAnalyzer::AnalyzeBraced(AstId expression, ScopeId scope,
                                         TypeId target)
{
  const vector<AstId>& children = arena_.At(expression).children;
  if (target == 0)
    throw std::runtime_error("braced initializer requires an array target");
  const TypeId target_unqualified = types_.Unqualified(target);
  if (types_.Kind(target_unqualified) == TYPE_CLASS) {
    const ClassEntity& class_entity = model_.ClassAt(
        types_.At(target_unqualified).entity);
    if (!class_entity.aggregate)
      throw std::runtime_error("braced initializer requires an aggregate");
    const SemaId result = MakeExpression(SEMA_BRACED_INIT_LIST, expression,
                                         target, VC_PRVALUE, scope);
    size_t index = 0;
    AnalyzeAggregateElements(children, index, scope, target, result);
    if (index != children.size())
      throw std::runtime_error("initializer list has the wrong bound");
    return result;
  }
  if (types_.Kind(target) != TYPE_ARRAY) {
    if (children.empty() || children.size() == 1)
    {
      const SemaId result = MakeExpression(SEMA_BRACED_INIT_LIST, expression,
                                           target, VC_PRVALUE, scope);
      if (children.size() == 1)
      {
        const SemaId child = Analyze(children[0], scope);
        Initialize(child, target, false, true);
        Append(result, child);
      }
      return result;
    }
    throw std::runtime_error("braced initializer target is not an array");
  }
  const TypeId element = types_.At(target).base;
  if (children.size() > types_.At(target).array_bound)
    throw std::runtime_error("initializer list has the wrong bound");
  const SemaId result = MakeExpression(SEMA_BRACED_INIT_LIST, expression,
                                       target, VC_LVALUE, scope);
  if (children.size() == 1 &&
      IsStringLiteralArrayClause(children[0], target)) {
    Append(result, Analyze(children[0], scope));
    return result;
  }
  size_t index = 0;
  while (index < children.size())
    Append(result, AnalyzeAggregateClause(children, children[index], scope,
                                          element, index));
  return result;
}

bool ExpressionAnalyzer::IsAggregateType(TypeId type) const
{
  const TypeId unqualified = types_.Unqualified(type);
  if (types_.Kind(unqualified) == TYPE_ARRAY)
    return true;
  return types_.Kind(unqualified) == TYPE_CLASS &&
      model_.ClassAt(types_.At(unqualified).entity).aggregate;
}

bool ExpressionAnalyzer::IsStringLiteralArrayClause(AstId clause,
                                                     TypeId target) const
{
  if (clause == 0 || arena_.At(clause).kind != AST_LITERAL ||
      arena_.At(clause).first >= tokens_.size())
    return false;
  const TypeId unqualified = types_.Unqualified(target);
  if (types_.Kind(unqualified) != TYPE_ARRAY)
    return false;
  const TypeId element = types_.Unqualified(types_.At(unqualified).base);
  if (types_.Kind(element) != TYPE_FUNDAMENTAL)
    return false;
  switch (types_.At(element).fundamental)
  {
  case FT_CHAR: case FT_SIGNED_CHAR: case FT_UNSIGNED_CHAR:
  case FT_WCHAR_T: case FT_CHAR16_T: case FT_CHAR32_T:
    break;
  default:
    return false;
  }
  return tokens_[arena_.At(clause).first].lit_count != 0;
}

SemaId ExpressionAnalyzer::AnalyzeAggregateClause(
    const vector<AstId>& clauses, AstId clause, ScopeId scope, TypeId target,
    std::size_t& index)
{
  if (IsStringLiteralArrayClause(clause, target)) {
    const Pa6Token& token = tokens_[arena_.At(clause).first];
    const TypeId array = types_.Unqualified(target);
    if (token.lit_count > types_.At(array).array_bound)
      throw std::runtime_error("string initializer exceeds array bound");
    const SemaId result = MakeExpression(SEMA_BRACED_INIT_LIST, 0, target,
                                         VC_PRVALUE, scope);
    Append(result, Analyze(clause, scope));
    ++index;
    return result;
  }
  if (arena_.At(clause).kind == AST_BRACED_INIT_LIST) {
    if (IsAggregateType(target)) {
      ++index;
      const SemaId aggregate = AnalyzeBraced(clause, scope, target);
      const TypeId unqualified = types_.Unqualified(target);
      // An aggregate element of an array is a class prvalue: retain its
      // canonical synthesized aggregate constructor action so lowering can
      // construct the element in place.  Arrays remain nested braced lists;
      // only class elements have a constructor entity to select here.
      if (types_.Kind(unqualified) == TYPE_CLASS &&
          model_.ScopeAt(scope).kind == SCOPE_BLOCK)
      {
        std::vector<SemaId> values;
        for (SemaId child = tree_.At(aggregate).first_child; child != 0;
             child = tree_.At(child).next_sibling)
          values.push_back(child);
        return BuildConstructorTemporary(clause, target, scope, values, true);
      }
      return aggregate;
    }
    const TypeId unqualified = types_.Unqualified(target);
    if (types_.Kind(unqualified) != TYPE_CLASS) {
      ++index;
      return AnalyzeBraced(clause, scope, target);
    }
    const vector<AstId>& arguments = arena_.At(clause).children;
    vector<SemaId> analyzed;
    analyzed.reserve(arguments.size());
    for (size_t i = 0; i < arguments.size(); ++i)
      analyzed.push_back(Analyze(arguments[i], scope));
    ++index;
    return BuildConstructorTemporary(clause, target, scope, analyzed, true);
  }
  if (IsAggregateType(target))
    return AnalyzeElidedAggregate(clauses, index, scope, target);

  const SemaId expression = Analyze(clause, scope);
  ++index;
  const TypeId unqualified = types_.Unqualified(target);
  if (types_.Kind(unqualified) == TYPE_CLASS)
    if (tree_.At(expression).kind == SEMA_CONSTRUCTOR_ACTION)
      return expression;
  if (types_.Kind(unqualified) == TYPE_CLASS)
    return BuildConstructorTemporary(clause, target, scope,
                                     std::vector<SemaId>(1, expression));
  Initialize(expression, target, false, true);
  return expression;
}

void ExpressionAnalyzer::AnalyzeAggregateElements(
    const vector<AstId>& clauses, size_t& index, ScopeId scope,
    TypeId target, SemaId result)
{
  const TypeId unqualified = types_.Unqualified(target);
  if (types_.Kind(unqualified) == TYPE_CLASS) {
    const ClassEntity& owner = model_.ClassAt(types_.At(unqualified).entity);
    for (size_t field_index = 0; field_index < owner.fields.size();
         ++field_index) {
      const ClassField& field = owner.fields[field_index];
      if (field.static_member || field.binding == 0)
        continue;
      if (index >= clauses.size())
        return;
      const AstId clause = clauses[index];
      const SemaId child = AnalyzeAggregateClause(
          clauses, clause, scope, field.type, index);
      if (child == 0)
        throw std::runtime_error("invalid aggregate initializer element");
      Append(result, child);
    }
    return;
  }
  if (types_.Kind(unqualified) == TYPE_ARRAY) {
    const TypeId element = types_.At(unqualified).base;
    const size_t bound = types_.At(unqualified).array_bound;
    for (size_t element_index = 0; element_index < bound; ++element_index) {
      if (index >= clauses.size())
        return;
      const AstId clause = clauses[index];
      const SemaId child = AnalyzeAggregateClause(
          clauses, clause, scope, element, index);
      Append(result, child);
    }
  }
}

SemaId ExpressionAnalyzer::AnalyzeElidedAggregate(
    const vector<AstId>& clauses, size_t& index, ScopeId scope,
    TypeId target)
{
  const SemaId result = MakeExpression(SEMA_BRACED_INIT_LIST, 0, target,
                                       VC_PRVALUE, scope);
  AnalyzeAggregateElements(clauses, index, scope, target, result);
  return result;
}
