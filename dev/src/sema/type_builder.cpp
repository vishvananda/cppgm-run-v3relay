// Specifier- and declarator-derived type construction for the ScopeBuilder.
#include "sema/scope_builder.h"

#include "sema/expr_sema.h"

#include <stdexcept>

using std::string;
using std::vector;

ScopeBuilder::ScopeBuilder(const vector<Pa6Token>& tokens,
                           const AstArena& arena, SemaModel& model)
    : tokens_(tokens), arena_(arena), model_(model), types_(model.Types()),
      const_eval_(tokens, arena, model, *this), tree_(0), expression_(0),
      semantic_root_(0),
      pending_array_bound_(0), unnamed_local_enum_counter_(0),
      unnamed_local_class_counter_(0)
{
}

ScopeBuilder::ScopeBuilder(const vector<Pa6Token>& tokens,
                           const AstArena& arena, SemaModel& model,
                           SemaTree& tree)
    : tokens_(tokens), arena_(arena), model_(model), types_(model.Types()),
      const_eval_(tokens, arena, model, *this), tree_(&tree), expression_(0),
      semantic_root_(0),
      pending_array_bound_(0), unnamed_local_enum_counter_(0),
      unnamed_local_class_counter_(0)
{
  expression_ = new ExpressionAnalyzer(tokens_, arena_, model_, tree,
                                        *this);
}

ScopeBuilder::~ScopeBuilder()
{
  delete expression_;
}

TypeId ScopeBuilder::TypeOfTypeId(AstId type_id, ScopeId scope)
{
  return BuildTypeId(type_id, scope);
}

TypeId ScopeBuilder::TypeOfExpression(AstId expression, ScopeId scope)
{
  return BuildExpressionType(expression, scope).type;
}

TypeId ScopeBuilder::TypeIdForSemantics(AstId type_id, ScopeId scope)
{
  return BuildTypeId(type_id, scope);
}

TypeId ScopeBuilder::DecltypeForSemantics(AstId expression, ScopeId scope)
{
  return BuildDecltype(expression, scope);
}

SemaId ScopeBuilder::AnalyzeExpression(AstId expression, ScopeId scope)
{
  if (expression_ == 0)
    throw std::runtime_error("semantic expression analyzer is unavailable");
  return expression_->Analyze(expression, scope);
}

SemaId ScopeBuilder::AnalyzeInitializer(AstId initializer, ScopeId scope,
                                         TypeId target)
{
  if (expression_ == 0)
    throw std::runtime_error("semantic expression analyzer is unavailable");
  return expression_->AnalyzeInitializer(initializer, scope, target);
}

SemaId ScopeBuilder::InitializeExpression(SemaId expression, TypeId target,
                                          bool variable,
                                          bool constexpr_value,
                                          bool condition,
                                          bool return_value,
                                          bool argument)
{
  if (expression_ == 0)
    throw std::runtime_error("semantic expression analyzer is unavailable");
  return expression_->Initialize(expression, target,
      InitContext(variable, constexpr_value, condition, return_value, argument));
}

bool ScopeBuilder::TryConstant(SemaId expression, long long& value) const
{
  return expression_ != 0 && expression_->TryConstant(expression, value);
}

bool ScopeBuilder::IsIgnoredSpecifier(ETokenType token)
{
  switch (token)
  {
  case KW_REGISTER: case KW_STATIC: case KW_THREAD_LOCAL: case KW_EXTERN:
  case KW_MUTABLE: case KW_INLINE: case KW_VIRTUAL: case KW_EXPLICIT:
  case KW_FRIEND: case KW_TYPEDEF: case KW_CONSTEXPR:
    return true;
  default:
    return false;
  }
}

TypeId ScopeBuilder::BuildSpecifierType(AstId specifier_sequence,
                                        ScopeId lookup_scope,
                                        const string& anonymous_name)
{
  return BuildTypeSequence(specifier_sequence, lookup_scope, true,
                           anonymous_name);
}

// decl-specifier-seq or type-specifier-seq: fundamental keywords combine,
// cv-qualifiers accumulate, exactly one other specifier may name the type.
TypeId ScopeBuilder::BuildTypeSequence(AstId sequence, ScopeId lookup_scope,
                                       bool in_declaration,
                                       const string& anonymous_name)
{
  const AstNode& value = arena_.At(sequence);
  vector<ETokenType> fundamental;
  TypeId result = 0;
  bool is_const = false;
  bool is_volatile = false;
  for (std::size_t i = 0; i < value.children.size(); ++i)
  {
    const AstId child = value.children[i];
    const AstNode& node = arena_.At(child);
    const bool single_token = node.first < tokens_.size() &&
        node.last == node.first + 1 &&
        tokens_[node.first].kind == PA6_SIMPLE_TOKEN;
    if (single_token && (node.kind == AST_CV_QUALIFIER ||
                         node.kind == AST_DECL_SPECIFIER ||
                         node.kind == AST_TYPE_SPECIFIER))
    {
      const ETokenType token = tokens_[node.first].simple_type;
      if (token == KW_CONST)
        is_const = true;
      else if (token == KW_VOLATILE)
        is_volatile = true;
      else if (IsFundamentalTypeKeyword(token))
        fundamental.push_back(token);
      else if (!IsIgnoredSpecifier(token))
        throw std::runtime_error("unsupported declaration specifier");
      continue;
    }
    const TypeId child_type = BuildTypeNode(child, lookup_scope, in_declaration,
                                            anonymous_name);
    if (result != 0)
      throw std::runtime_error("multiple type specifiers");
    result = child_type;
  }
  if (result != 0 && !fundamental.empty())
    throw std::runtime_error("multiple type specifiers");
  if (result == 0 && !fundamental.empty())
    result = types_.FundamentalFromKeywords(fundamental);
  if (result == 0)
    throw std::runtime_error("declaration has no type");
  return types_.Cv(result, is_const, is_volatile);
}

TypeId ScopeBuilder::LookupType(ScopeId scope, const QualifiedName& name) const
{
  if (name.Empty())
    throw std::runtime_error("invalid type name");
  // Ordinary lookup for an unqualified type name honours 3.3.10 hiding;
  // qualified names ignore non-types at every step (3.4.3).
  const BindingId binding = name.Qualified() ?
      model_.LookupQualified(scope, name, LOOKUP_TYPES) :
      model_.LookupTypeName(scope, name.components[0]);
  if (binding == 0 || model_.BindingAt(binding).type == 0)
    throw std::runtime_error("unknown type name: " + name.Last());
  return model_.BindingAt(binding).type;
}

// One specifier that denotes a type: a class or enum specifier, an elaborated
// class specifier, decltype, a (possibly qualified) type name, or a keyword.
TypeId ScopeBuilder::BuildTypeNode(AstId node, ScopeId lookup_scope,
                                   bool in_declaration,
                                   const string& anonymous_name)
{
  const AstNode& value = arena_.At(node);
  switch (value.kind)
  {
  case AST_CLASS_SPECIFIER:
    return BuildClassDefinition(node, lookup_scope, anonymous_name);
  case AST_CLASS_FORWARD_DECLARATION:
    return BuildElaboratedClass(node, lookup_scope, in_declaration);
  case AST_ENUM_SPECIFIER: case AST_ENUM_DECLARATION:
    return BuildEnum(node, lookup_scope, anonymous_name);
  case AST_DECLTYPE_SPECIFIER:
    if (value.children.size() != 1)
      throw std::runtime_error("invalid decltype specifier");
    return BuildDecltype(value.children[0], lookup_scope);
  case AST_DECL_SPECIFIER:
    if (!value.children.empty())
      return BuildDecltype(value.children[0], lookup_scope);
    return LookupType(lookup_scope, NodeName(node));
  case AST_TYPE_NAME:
    return LookupType(lookup_scope, NodeName(node));
  case AST_TYPE_SPECIFIER:
    if (value.first < tokens_.size() &&
        tokens_[value.first].kind == PA6_SIMPLE_TOKEN)
      return types_.FundamentalFromKeywords(
          vector<ETokenType>(1, tokens_[value.first].simple_type));
    break;
  default:
    break;
  }
  throw std::runtime_error("unsupported type specifier");
}

TypeId ScopeBuilder::BuildTypeId(AstId node, ScopeId lookup_scope)
{
  const AstNode& value = arena_.At(node);
  if (value.kind != AST_TYPE_ID || value.children.empty())
    throw std::runtime_error("invalid type-id");
  const TypeId base = BuildTypeSequence(value.children[0], lookup_scope, false,
                                        string());
  if (value.children.size() == 1 || value.children[1] == 0)
    return base;
  return BuildDeclaratorType(value.children[1], base, lookup_scope);
}

// ptr-operators and their cv-qualifiers, left to right.
TypeId ScopeBuilder::ApplyPrefix(TypeId base, const vector<AstId>& prefix)
{
  TypeId result = base;
  for (std::size_t i = 0; i < prefix.size(); ++i)
  {
    const AstNode& node = arena_.At(prefix[i]);
    if (node.first >= tokens_.size())
      throw std::runtime_error("invalid declarator operator");
    const Pa6Token& token = tokens_[node.first];
    if (node.kind == AST_CV_QUALIFIER)
      result = types_.Cv(result, token.IsSimple(KW_CONST),
                         token.IsSimple(KW_VOLATILE));
    else if (token.IsSimple(OP_STAR))
      result = types_.Pointer(result);
    else if (token.IsSimple(OP_AMP))
      result = types_.Reference(result, true);
    else if (token.IsSimple(OP_LAND))
      result = types_.Reference(result, false);
    else
      throw std::runtime_error("unsupported pointer operator");
  }
  return result;
}

// Parameter clauses and array bounds, right to left onto the base (8.3).
TypeId ScopeBuilder::ApplySuffix(TypeId base, const vector<AstId>& suffix,
                                 ScopeId lookup_scope)
{
  TypeId result = base;
  for (std::size_t i = suffix.size(); i != 0; --i)
  {
    const AstNode& node = arena_.At(suffix[i - 1]);
    if (node.kind == AST_PARAMETER_CLAUSE)
    {
      vector<ParameterInfo> parameters;
      bool variadic = false;
      BuildParameters(suffix[i - 1], lookup_scope, parameters, variadic);
      vector<TypeId> types(parameters.size());
      for (std::size_t p = 0; p < parameters.size(); ++p)
        types[p] = parameters[p].type;
      result = types_.Function(result, types, variadic);
    }
    else if (node.kind == AST_ARRAY_SUFFIX)
    {
      long long bound = 0;
      if (node.children.empty() || node.children[0] == 0)
      {
        if (pending_array_bound_ == 0)
          throw std::runtime_error("incomplete array type");
        bound = static_cast<long long>(pending_array_bound_);
      }
      else
        bound = const_eval_.Evaluate(node.children[0], lookup_scope);
      if (bound <= 0)
        throw std::runtime_error("array bound must be positive");
      result = types_.Array(result, static_cast<std::size_t>(bound));
    }
    else if (node.kind == AST_TRAILING_RETURN_TYPE)
      throw std::runtime_error("trailing return types are unsupported");
    // function-qualifiers, attributes and virt-specifiers do not change the
    // PA11 type.
  }
  return result;
}

// 8.3: the ptr-operators before the declarator-id apply to the base first,
// then the suffixes after it, right to left; a parenthesised declarator then
// takes that type as its base (`int *(*p)[3]` is pointer to array of 3
// pointer to int).
TypeId ScopeBuilder::BuildDeclaratorType(AstId declarator, TypeId base,
                                         ScopeId lookup_scope)
{
  if (declarator == 0)
    return base;
  const AstNode& node = arena_.At(declarator);
  vector<AstId> prefix;
  vector<AstId> suffix;
  AstId nested = 0;
  bool seen_direct = false;
  for (std::size_t i = 0; i < node.children.size(); ++i)
  {
    const AstId child = node.children[i];
    const AstKind kind = arena_.At(child).kind;
    if (kind == AST_IDENTIFIER || kind == AST_NESTED_DECLARATOR)
    {
      seen_direct = true;
      if (kind == AST_NESTED_DECLARATOR)
        nested = child;
    }
    else if (!seen_direct &&
             (kind == AST_PTR_OPERATOR || kind == AST_CV_QUALIFIER))
      prefix.push_back(child);
    else if (kind != AST_PARAMETER_PACK)
      suffix.push_back(child);
  }
  const TypeId declared = ApplySuffix(ApplyPrefix(base, prefix), suffix,
                                      lookup_scope);
  if (nested == 0)
    return declared;
  const AstNode& inner = arena_.At(nested);
  if (inner.children.size() != 1)
    throw std::runtime_error("invalid nested declarator");
  return BuildDeclaratorType(inner.children[0], declared, lookup_scope);
}

void ScopeBuilder::BuildParameters(AstId clause, ScopeId lookup_scope,
                                   vector<ParameterInfo>& parameters,
                                   bool& variadic)
{
  const AstNode& value = arena_.At(clause);
  variadic = false;
  for (std::size_t i = 0; i < value.children.size(); ++i)
  {
    const AstId child = value.children[i];
    const AstNode& parameter = arena_.At(child);
    if (parameter.kind == AST_PARAMETER_PACK)
    {
      variadic = true;
      continue;
    }
    if (parameter.kind != AST_PARAMETER_DECLARATION)
      continue;
    if (parameter.children.empty())
      throw std::runtime_error("invalid parameter declaration");
    const TypeId base = BuildTypeSequence(parameter.children[0], lookup_scope,
                                          true, string());
    const AstId declarator = FindChild(child, AST_DECLARATOR);
    ParameterInfo info;
    info.type = BuildDeclaratorType(declarator, base, lookup_scope);
    info.name = IdentifierName(FindIdentifier(declarator));
    parameters.push_back(info);
  }
  // 8.3.5p4: a lone unnamed `void` parameter is an empty parameter list.
  if (!variadic && parameters.size() == 1 && parameters[0].name.empty() &&
      types_.Kind(parameters[0].type) == TYPE_FUNDAMENTAL &&
      types_.At(parameters[0].type).fundamental == FT_VOID)
    parameters.clear();
}

// Static type and value category of the operand forms PA11 supports.
ScopeBuilder::ExpressionType ScopeBuilder::BuildExpressionType(
    AstId expression, ScopeId lookup_scope)
{
  const AstNode& node = arena_.At(expression);
  ExpressionType result;
  result.type = 0;
  result.lvalue = false;
  result.names_type = false;
  switch (node.kind)
  {
  case AST_PARENTHESIZED_EXPRESSION:
    if (node.children.size() != 1)
      throw std::runtime_error("invalid parenthesized expression");
    return BuildExpressionType(node.children[0], lookup_scope);
  case AST_ID_EXPRESSION: case AST_IDENTIFIER:
  {
    const BindingId binding = model_.Lookup(lookup_scope, NodeName(expression),
                                           LOOKUP_ANY);
    if (binding == 0 || model_.BindingAt(binding).type == 0)
      throw std::runtime_error("unknown name in expression");
    const Binding& value = model_.BindingAt(binding);
    result.type = value.type;
    result.names_type = value.kind == BINDING_TYPE ||
        value.kind == BINDING_TYPE_ALIAS;
    // 5.1.1p8: a name of a variable, parameter or function is an lvalue;
    // an enumerator is a prvalue.
    result.lvalue = value.kind == BINDING_VARIABLE ||
        value.kind == BINDING_PARAMETER || value.kind == BINDING_FUNCTION;
    return result;
  }
  case AST_LITERAL:
    if (node.first >= tokens_.size() || !tokens_[node.first].lit_scalar)
      throw std::runtime_error("unsupported literal operand");
    result.type = types_.Fundamental(tokens_[node.first].lit_type);
    return result;
  case AST_KEYWORD_LITERAL:
    result.type = types_.Fundamental(FT_BOOL);
    return result;
  case AST_CAST_EXPRESSION:
    if (node.children.size() != 2)
      throw std::runtime_error("invalid cast expression");
    result.type = BuildTypeId(node.children[0], lookup_scope);
    result.lvalue = types_.Kind(result.type) == TYPE_REFERENCE;
    return result;
  case AST_SIZEOF_EXPRESSION: case AST_TYPE_TRAIT_EXPRESSION:
    // 5.3.3p6, 5.3.6p3: std::size_t, which is unsigned long on this target.
    result.type = types_.Fundamental(FT_UNSIGNED_LONG_INT);
    return result;
  default:
    throw std::runtime_error("unsupported expression operand");
  }
}

// 7.1.6.2p4: decltype(id) is the declared type; decltype((lvalue)) is an
// lvalue reference to it.
TypeId ScopeBuilder::BuildDecltype(AstId expression, ScopeId lookup_scope)
{
  if (expression_ != 0)
  {
    const SemaId analyzed = expression_->Analyze(expression, lookup_scope);
    const SemaNode& semantic = tree_->At(analyzed);
    if (arena_.At(expression).kind == AST_PARENTHESIZED_EXPRESSION &&
        semantic.category == VC_LVALUE)
      return types_.Reference(semantic.type, true);
    return semantic.type;
  }
  const ExpressionType operand = BuildExpressionType(expression, lookup_scope);
  if (operand.names_type)
    throw std::runtime_error("decltype operand names a type");
  const bool parenthesized =
      arena_.At(expression).kind == AST_PARENTHESIZED_EXPRESSION;
  if (parenthesized && operand.lvalue &&
      types_.Kind(operand.type) != TYPE_REFERENCE)
    return types_.Reference(operand.type, true);
  return operand.type;
}
