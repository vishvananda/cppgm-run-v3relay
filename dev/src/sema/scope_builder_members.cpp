#include "sema/scope_builder.h"

#include <stdexcept>

// Namespace redeclarations and out-of-class member definitions share the
// same direct-binding search, but keep each declaration as a separate dump
// binding while recording its canonical entity here.
void ScopeBuilder::LinkRedeclaration(BindingId binding, ScopeId scope,
                                     const std::string& name, TypeId type)
{
  std::vector<BindingId> priors;
  model_.DirectBindings(scope, name, LOOKUP_VALUES, priors);
  for (std::size_t i = 0; i < priors.size(); ++i)
  {
    if (priors[i] == binding)
      continue;
    const Binding& prior = model_.BindingAt(priors[i]);
    if (prior.kind != BINDING_VARIABLE)
      continue;
    if (!CompatibleRedeclaration(prior.type, type))
      throw std::runtime_error("object redeclared with a different type");
    if (prior.thread_local_storage !=
        model_.BindingAt(binding).thread_local_storage)
      throw std::runtime_error(
          "thread_local does not agree across redeclarations");
    model_.BindingAt(binding).redeclared_binding =
        prior.redeclared_binding != 0 ? prior.redeclared_binding : priors[i];
    return;
  }
}

bool ScopeBuilder::CompatibleRedeclaration(TypeId prior, TypeId current) const
{
  if (prior == current)
    return true;
  // 8.3.4p3: an array of unknown bound is completed by a later declaration.
  if (types_.Kind(prior) != TYPE_ARRAY || types_.Kind(current) != TYPE_ARRAY)
    return false;
  const TypeNode& first = types_.At(prior);
  const TypeNode& second = types_.At(current);
  return first.base == second.base &&
      (first.array_bound == 0 || second.array_bound == 0 ||
       first.array_bound == second.array_bound);
}

BindingId ScopeBuilder::FindStaticMemberVariable(
    ScopeId scope, const std::string& name, TypeId type) const
{
  std::vector<BindingId> prior_members;
  model_.DirectBindings(scope, name, LOOKUP_VALUES, prior_members);
  for (std::size_t i = prior_members.size(); i != 0; --i)
  {
    const BindingId prior_id = prior_members[i - 1];
    const Binding& prior = model_.BindingAt(prior_id);
    if (prior.kind == BINDING_VARIABLE && prior.static_member &&
        CompatibleRedeclaration(prior.type, type))
      return prior.redeclared_binding != 0 ?
          prior.redeclared_binding : prior_id;
  }
  return 0;
}

bool ScopeBuilder::HasStaticMemberFunction(
    ScopeId scope, const std::string& name, TypeId member_type) const
{
  std::vector<BindingId> prior_members;
  model_.DirectBindings(scope, name,
                        LOOKUP_FUNCTIONS | LOOKUP_HIDDEN_FRIENDS,
                        prior_members);
  for (std::size_t i = prior_members.size(); i != 0; --i)
  {
    const Binding& prior = model_.BindingAt(prior_members[i - 1]);
    if (prior.kind != BINDING_FUNCTION || prior.function == 0)
      continue;
    const FunctionEntity& entity = model_.FunctionAt(prior.function);
    if (entity.is_member && entity.static_member &&
        entity.member_type == member_type)
      return true;
  }
  return false;
}

ScopeId ScopeBuilder::TypeScopeForDeclaration(ScopeId scope, AstId list) const
{
  if (list == 0 || arena_.At(list).children.empty())
    return scope;
  const AstId first = arena_.At(list).children[0];
  const AstId identifier = FindIdentifier(FindChild(first, AST_DECLARATOR));
  const QualifiedName name = NodeName(identifier);
  return name.Qualified() ? ResolveQualifierScope(scope, name.Prefix()) :
      scope;
}
