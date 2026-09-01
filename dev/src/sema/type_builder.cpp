#include "sema/scope_builder.h"

#include <sstream>
#include <stdexcept>

using std::string;
using std::vector;

ScopeBuilder::ScopeBuilder(const vector<Pa6Token>& tokens, AstArena& arena,
                           TypeTable& types, SemaModel& model)
    : tokens_(tokens), arena_(arena), types_(types), model_(model),
      const_eval_(tokens, arena)
{
}

TypeId ScopeBuilder::BuildSpecifierType(AstId specifier_sequence,
                                        ScopeId lookup_scope)
{
  return BuildTypeSequence(specifier_sequence, lookup_scope, true);
}

TypeId ScopeBuilder::BuildTypeSequence(AstId sequence, ScopeId lookup_scope,
                                       bool allow_elaborated_declaration)
{
  const AstNode& value = arena_.At(sequence);
  vector<ETokenType> fundamental_tokens;
  TypeId result = 0;
  bool is_const = false;
  bool is_volatile = false;
  for (std::size_t i = 0; i < value.children.size(); ++i)
  {
    const AstId child = value.children[i];
    const AstNode& node = arena_.At(child);
    if (node.kind == AST_CV_QUALIFIER)
    {
      if (node.first < tokens_.size() && tokens_[node.first].IsSimple(KW_CONST))
        is_const = true;
      if (node.first < tokens_.size() && tokens_[node.first].IsSimple(KW_VOLATILE))
        is_volatile = true;
      continue;
    }
    if ((node.kind == AST_DECL_SPECIFIER || node.kind == AST_TYPE_SPECIFIER) &&
        node.first < tokens_.size() && node.last == node.first + 1 &&
        tokens_[node.first].kind == PA6_SIMPLE_TOKEN)
    {
      const ETokenType token = tokens_[node.first].simple_type;
      if (token == KW_CONST)
      {
        is_const = true;
        continue;
      }
      if (token == KW_VOLATILE)
      {
        is_volatile = true;
        continue;
      }
      if (IsBuiltinToken(token))
      {
        fundamental_tokens.push_back(token);
        continue;
      }
      if (token == KW_CONSTEXPR || IsIgnoredSpecifier(token))
        continue;
    }
    const TypeId child_type = BuildTypeNode(child, lookup_scope,
                                            allow_elaborated_declaration);
    if (child_type != 0)
    {
      if (result != 0)
        throw std::runtime_error("multiple type specifiers");
      result = child_type;
    }
  }
  if (result == 0 && !fundamental_tokens.empty())
    result = BuildFundamental(fundamental_tokens);
  if (result == 0)
    throw std::runtime_error("declaration has no type");
  return AddCv(result, is_const, is_volatile);
}

bool ScopeBuilder::IsBuiltinToken(ETokenType token) const
{
  switch (token)
  {
  case KW_CHAR: case KW_CHAR16_T: case KW_CHAR32_T: case KW_WCHAR_T:
  case KW_BOOL: case KW_SHORT: case KW_INT: case KW_LONG: case KW_SIGNED:
  case KW_UNSIGNED: case KW_FLOAT: case KW_DOUBLE: case KW_VOID:
    return true;
  default:
    return false;
  }
}

bool ScopeBuilder::IsIgnoredSpecifier(ETokenType token) const
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

TypeId ScopeBuilder::BuildFundamental(const vector<ETokenType>& tokens) const
{
  bool is_unsigned = false;
  bool is_signed = false;
  bool is_short = false;
  unsigned long_count = 0;
  ETokenType named = static_cast<ETokenType>(-1);
  for (std::size_t i = 0; i < tokens.size(); ++i)
  {
    switch (tokens[i])
    {
    case KW_UNSIGNED: is_unsigned = true; break;
    case KW_SIGNED: is_signed = true; break;
    case KW_SHORT: is_short = true; break;
    case KW_LONG: ++long_count; break;
    default: named = tokens[i]; break;
    }
  }
  EFundamentalType result = FT_INT;
  if (named == KW_CHAR)
    result = is_unsigned ? FT_UNSIGNED_CHAR : is_signed ? FT_SIGNED_CHAR : FT_CHAR;
  else if (named == KW_CHAR16_T)
    result = FT_CHAR16_T;
  else if (named == KW_CHAR32_T)
    result = FT_CHAR32_T;
  else if (named == KW_WCHAR_T)
    result = FT_WCHAR_T;
  else if (named == KW_BOOL)
    result = FT_BOOL;
  else if (named == KW_FLOAT)
    result = FT_FLOAT;
  else if (named == KW_DOUBLE)
    result = long_count == 0 ? FT_DOUBLE : FT_LONG_DOUBLE;
  else if (named == KW_VOID)
    result = FT_VOID;
  else if (is_short)
    result = is_unsigned ? FT_UNSIGNED_SHORT_INT : FT_SHORT_INT;
  else if (long_count >= 2)
    result = is_unsigned ? FT_UNSIGNED_LONG_LONG_INT : FT_LONG_LONG_INT;
  else if (long_count == 1)
    result = is_unsigned ? FT_UNSIGNED_LONG_INT : FT_LONG_INT;
  else if (is_unsigned)
    result = FT_UNSIGNED_INT;
  return types_.Fundamental(result);
}

TypeId ScopeBuilder::BuildTypeNode(AstId node, ScopeId lookup_scope,
                                   bool allow_elaborated_declaration)
{
  const AstNode& value = arena_.At(node);
  if (value.kind == AST_CLASS_SPECIFIER)
    return BuildClassSpecifier(node, lookup_scope);
  if (value.kind == AST_CLASS_FORWARD_DECLARATION)
    return BuildForwardType(node, lookup_scope,
                            allow_elaborated_declaration);
  if (value.kind == AST_DECLTYPE_SPECIFIER ||
      (value.kind == AST_DECL_SPECIFIER && !value.children.empty()))
  {
    if (!value.children.empty())
      return BuildDecltype(value.children[0], lookup_scope);
  }
  if ((value.kind == AST_DECL_SPECIFIER || value.kind == AST_TYPE_NAME) &&
      value.first < tokens_.size())
  {
    const vector<string> name = NameComponents(value.first, value.last);
    if (name.empty())
      throw std::runtime_error("invalid type name");
    const BindingId binding = name.size() == 1 ?
        model_.LookupTypeName(lookup_scope, name[0]) :
        ResolveName(lookup_scope, name, LOOKUP_TYPES);
    if (binding == 0 || model_.BindingAt(binding).type == 0)
      throw std::runtime_error("unknown type name: " + name.back());
    return model_.BindingAt(binding).type;
  }
  if (value.kind == AST_TYPE_SPECIFIER && value.first < tokens_.size())
  {
    vector<ETokenType> token;
    token.push_back(tokens_[value.first].simple_type);
    return BuildFundamental(token);
  }
  throw std::runtime_error("unsupported type specifier");
}

TypeId ScopeBuilder::BuildTypeId(AstId node, ScopeId lookup_scope)
{
  const AstNode& value = arena_.At(node);
  if (value.kind != AST_TYPE_ID || value.children.empty())
    throw std::runtime_error("invalid type-id");
  const TypeId base = BuildTypeSequence(value.children[0], lookup_scope, false);
  if (value.children.size() == 1)
    return base;
  return BuildDeclaratorType(value.children[1], base, lookup_scope);
}

TypeId ScopeBuilder::AddCv(TypeId base, bool is_const, bool is_volatile)
{
  if (base == 0 || (!is_const && !is_volatile))
    return base;
  if (types_.Kind(base) == TYPE_CV)
  {
    const TypeNode& old = types_.At(base);
    return types_.Cv(old.base, old.is_const || is_const,
                     old.is_volatile || is_volatile);
  }
  return types_.Cv(base, is_const, is_volatile);
}

TypeId ScopeBuilder::ApplyPrefix(TypeId base, const vector<AstId>& prefix)
{
  TypeId result = base;
  for (std::size_t i = 0; i < prefix.size(); ++i)
  {
    const AstNode& node = arena_.At(prefix[i]);
    if (node.kind == AST_CV_QUALIFIER)
    {
      const bool is_const = node.first < tokens_.size() &&
          tokens_[node.first].IsSimple(KW_CONST);
      result = AddCv(result, is_const, !is_const);
      continue;
    }
    if (node.kind != AST_PTR_OPERATOR || node.first >= tokens_.size())
      continue;
    const Pa6Token& token = tokens_[node.first];
    if (token.IsSimple(OP_STAR))
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

TypeId ScopeBuilder::ApplySuffix(TypeId base, const vector<AstId>& suffix,
                                 ScopeId lookup_scope)
{
  TypeId result = base;
  for (std::size_t i = 0; i < suffix.size(); ++i)
  {
    const AstNode& node = arena_.At(suffix[i]);
    if (node.kind == AST_PARAMETER_CLAUSE)
    {
      vector<ParameterInfo> parameters;
      bool variadic = false;
      BuildParameters(suffix[i], lookup_scope, parameters, variadic);
      vector<TypeId> types;
      for (std::size_t p = 0; p < parameters.size(); ++p)
        types.push_back(parameters[p].type);
      result = types_.Function(result, types, variadic);
    }
    else if (node.kind == AST_ARRAY_SUFFIX)
    {
      if (node.children.empty() || node.children[0] == 0)
        throw std::runtime_error("incomplete array type");
      const long long bound = const_eval_.Evaluate(node.children[0]);
      if (bound <= 0)
        throw std::runtime_error("array bound must be positive");
      result = types_.Array(result, static_cast<std::size_t>(bound));
    }
  }
  return result;
}

TypeId ScopeBuilder::BuildDeclaratorType(AstId declarator, TypeId base,
                                         ScopeId lookup_scope)
{
  if (declarator == 0)
    return base;
  const AstNode& node = arena_.At(declarator);
  vector<AstId> prefix;
  vector<AstId> suffix;
  AstId direct = 0;
  for (std::size_t i = 0; i < node.children.size(); ++i)
  {
    const AstId child = node.children[i];
    const AstKind kind = arena_.At(child).kind;
    if (kind == AST_IDENTIFIER || kind == AST_NESTED_DECLARATOR)
    {
      direct = child;
      continue;
    }
    if (direct == 0 && (kind == AST_PTR_OPERATOR || kind == AST_CV_QUALIFIER))
      prefix.push_back(child);
    else if (kind == AST_PARAMETER_PACK)
      continue;
    else
      suffix.push_back(child);
  }
  if (direct != 0 && arena_.At(direct).kind == AST_NESTED_DECLARATOR)
  {
    const AstNode& nested = arena_.At(direct);
    if (nested.children.size() != 1)
      throw std::runtime_error("invalid nested declarator");
    const TypeId outer = ApplySuffix(base, suffix, lookup_scope);
    const TypeId inner = BuildDeclaratorType(nested.children[0], outer,
                                             lookup_scope);
    return ApplyPrefix(inner, prefix);
  }
  return ApplySuffix(ApplyPrefix(base, prefix), suffix, lookup_scope);
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
                                          false);
    AstId declarator = 0;
    for (std::size_t p = 1; p < parameter.children.size(); ++p)
      if (arena_.At(parameter.children[p]).kind == AST_DECLARATOR)
        declarator = parameter.children[p];
    ParameterInfo info;
    info.type = declarator == 0 ? base :
        BuildDeclaratorType(declarator, base, lookup_scope);
    info.name = IdentifierName(FindIdentifier(declarator));
    parameters.push_back(info);
  }
  if (!variadic && parameters.size() == 1 && parameters[0].name.empty() &&
      types_.Kind(parameters[0].type) == TYPE_FUNDAMENTAL &&
      types_.At(parameters[0].type).fundamental == FT_VOID)
    parameters.clear();
}

TypeId ScopeBuilder::BuildExpressionType(AstId expression,
                                         ScopeId lookup_scope, bool& lvalue)
{
  const AstNode& node = arena_.At(expression);
  if (node.kind == AST_PARENTHESIZED_EXPRESSION)
  {
    if (node.children.size() != 1)
      throw std::runtime_error("unsupported decltype expression");
    return BuildExpressionType(node.children[0], lookup_scope, lvalue);
  }
  if (node.kind == AST_ID_EXPRESSION || node.kind == AST_IDENTIFIER)
  {
    const vector<string> name = NameComponents(expression);
    const BindingId binding = ResolveName(lookup_scope, name, LOOKUP_ANY);
    if (binding == 0 || model_.BindingAt(binding).type == 0)
      throw std::runtime_error("unknown decltype name");
    const Binding& value = model_.BindingAt(binding);
    lvalue = value.kind != BINDING_FUNCTION;
    return value.type;
  }
  if (node.kind == AST_LITERAL && node.first < tokens_.size())
  {
    lvalue = false;
    return types_.Fundamental(tokens_[node.first].lit_type);
  }
  throw std::runtime_error("unsupported decltype expression");
}

TypeId ScopeBuilder::BuildDecltype(AstId expression, ScopeId lookup_scope)
{
  const AstNode& node = arena_.At(expression);
  if (node.kind == AST_PARENTHESIZED_EXPRESSION)
  {
    bool lvalue = false;
    const TypeId type = BuildExpressionType(expression, lookup_scope, lvalue);
    return lvalue ? types_.Reference(type, true) : type;
  }
  bool lvalue = false;
  return BuildExpressionType(expression, lookup_scope, lvalue);
}
