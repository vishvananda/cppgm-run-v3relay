#include "sema/scope_builder.h"

#include <algorithm>
#include <stdexcept>

// The parser must choose a declaration before semantic lookup has populated
// inherited members.  Reclassify only the bounded zero-argument parameter
// shape used by an expression initializer; ordinary function declarators
// remain owned by the normal type builder.
bool ScopeBuilder::FindAmbiguousDirectInitializer(
    AstId declarator, ScopeId scope, std::vector<AstId>& arguments) const
{
  arguments.clear();
  if (declarator == 0)
    return false;
  const AstNode& value = arena_.At(declarator);
  const AstId clause = FindChild(declarator, AST_PARAMETER_CLAUSE);
  if (clause == 0 || arena_.At(clause).children.empty())
    return false;
  for (std::size_t i = 0; i < value.children.size(); ++i)
  {
    const AstId child = value.children[i];
    if (child != clause && arena_.At(child).kind != AST_IDENTIFIER)
      return false;
  }

  bool has_expression_item = false;
  const std::vector<AstId>& parameters = arena_.At(clause).children;
  for (std::size_t i = 0; i < parameters.size(); ++i)
  {
    const AstId parameter = parameters[i];
    if (arena_.At(parameter).kind != AST_PARAMETER_DECLARATION)
      return false;
    const AstId specifiers = FindChild(parameter, AST_DECL_SPECIFIER_SEQ);
    const AstId parameter_declarator =
        FindChild(parameter, AST_DECLARATOR);
    if (specifiers == 0 || parameter_declarator == 0)
      return false;
    const AstId parameter_clause =
        FindChild(parameter_declarator, AST_PARAMETER_CLAUSE);
    if (parameter_clause == 0 ||
        !arena_.At(parameter_clause).children.empty() ||
        arena_.At(parameter_declarator).children.size() != 1)
      return false;
    const std::vector<AstId>& specifier_nodes =
        arena_.At(specifiers).children;
    if (specifier_nodes.size() != 1)
      return false;
    const AstNode& specifier = arena_.At(specifier_nodes[0]);
    bool type_like = specifier.first < tokens_.size() &&
        specifier.last == specifier.first + 1 &&
        tokens_[specifier.first].kind == PA6_SIMPLE_TOKEN &&
        IsFundamentalTypeKeyword(tokens_[specifier.first].simple_type);
    const QualifiedName name = NodeName(specifier_nodes[0]);
    if (name.Empty())
      return false;
    if (!type_like)
    {
      const BindingId type = name.Qualified() ?
          model_.LookupQualified(scope, name, LOOKUP_TYPES) :
          model_.LookupTypeName(scope, name.Last());
      type_like = type != 0;
    }
    if (!type_like)
    {
      std::vector<BindingId> functions;
      if (name.Qualified())
        model_.LookupQualifiedSet(scope, name, LOOKUP_FUNCTIONS, functions);
      else
        model_.LookupSet(scope, name.Last(), LOOKUP_FUNCTIONS, functions);
      if (functions.empty())
        return false;
      has_expression_item = true;
    }
    arguments.push_back(parameter);
  }
  return has_expression_item;
}

bool ScopeBuilder::BuildAmbiguousDirectInitializer(
    TypeId type, BindingId binding, SemaId variable, ScopeId scope,
    bool static_member, const std::vector<AstId>& arguments)
{
  if (type == 0)
    return false;
  if (variable != 0)
  {
    EnsureDestructor(type);
    AddConstructorActionWithArguments(variable, scope, type, binding,
                                      arguments);
  }
  if (model_.ScopeAt(scope).kind == SCOPE_BLOCK && !static_member)
    RecordInitializedLocal(scope);
  return true;
}

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

// 11.3: a friend declaration in `granting` names the entity that owns
// `friend_of`.  A class may repeat the declaration; the relation is one entry.
void ScopeBuilder::RecordFriend(ClassEntityId granting,
                                std::vector<ClassEntityId>& friend_of)
{
  if (std::find(friend_of.begin(), friend_of.end(), granting) ==
      friend_of.end())
    friend_of.push_back(granting);
}
