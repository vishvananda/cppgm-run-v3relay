#include "sema/const_eval.h"

#include <limits>
#include <stdexcept>

namespace
{

bool IsIntegral(EFundamentalType type)
{
  switch (type)
  {
  case FT_SIGNED_CHAR: case FT_SHORT_INT: case FT_INT: case FT_LONG_INT:
  case FT_LONG_LONG_INT: case FT_UNSIGNED_CHAR: case FT_UNSIGNED_SHORT_INT:
  case FT_UNSIGNED_INT: case FT_UNSIGNED_LONG_INT:
  case FT_UNSIGNED_LONG_LONG_INT: case FT_WCHAR_T: case FT_CHAR:
  case FT_CHAR16_T: case FT_CHAR32_T: case FT_BOOL:
    return true;
  case FT_FLOAT: case FT_DOUBLE: case FT_LONG_DOUBLE: case FT_VOID:
  case FT_NULLPTR_T:
    return false;
  }
  return false;
}

bool IsUnsigned(EFundamentalType type)
{
  return type == FT_UNSIGNED_CHAR || type == FT_UNSIGNED_SHORT_INT ||
      type == FT_UNSIGNED_INT || type == FT_UNSIGNED_LONG_INT ||
      type == FT_UNSIGNED_LONG_LONG_INT;
}

unsigned IntegerBits(EFundamentalType type)
{
  switch (type)
  {
  case FT_SIGNED_CHAR: case FT_UNSIGNED_CHAR: case FT_CHAR: return 8;
  case FT_SHORT_INT: case FT_UNSIGNED_SHORT_INT: return 16;
  case FT_INT: case FT_UNSIGNED_INT: case FT_FLOAT: return 32;
  case FT_LONG_INT: case FT_LONG_LONG_INT: case FT_UNSIGNED_LONG_INT:
  case FT_UNSIGNED_LONG_LONG_INT: case FT_DOUBLE: return 64;
  case FT_WCHAR_T: case FT_CHAR16_T: return 16;
  case FT_CHAR32_T: return 32;
  case FT_BOOL: return 1;
  case FT_LONG_DOUBLE: case FT_VOID: case FT_NULLPTR_T: break;
  }
  return 0;
}

bool IsBuiltin(ETokenType token)
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

} // namespace

ConstEvaluator::ConstEvaluator(const std::vector<Pa6Token>& tokens,
                               const AstArena& arena)
    : tokens_(tokens), arena_(arena), model_(0), types_(0)
{
}

ConstEvaluator::ConstEvaluator(const std::vector<Pa6Token>& tokens,
                               const AstArena& arena, const SemaModel& model,
                               TypeTable& types)
    : tokens_(tokens), arena_(arena), model_(&model), types_(&types)
{
}

long long ConstEvaluator::Evaluate(AstId expression, ScopeId scope) const
{
  if (expression == 0)
    throw std::runtime_error("missing constant expression");
  return EvaluateNode(expression, scope);
}

long long ConstEvaluator::Checked(__int128 value)
{
  const __int128 minimum =
      static_cast<__int128>(std::numeric_limits<long long>::min());
  const __int128 maximum =
      static_cast<__int128>(std::numeric_limits<long long>::max());
  if (value < minimum || value > maximum)
    throw std::runtime_error("constant expression overflows signed integer");
  return static_cast<long long>(value);
}

std::vector<std::string> ConstEvaluator::NameComponents(
    std::size_t first, std::size_t last) const
{
  std::vector<std::string> result;
  std::string component;
  for (std::size_t i = first; i < last && i < tokens_.size(); ++i)
  {
    const Pa6Token& token = tokens_[i];
    if (token.kind == PA6_IDENTIFIER_TOKEN)
    {
      component += token.spelling;
      continue;
    }
    if (token.IsSimple(OP_COLON2))
    {
      if (!component.empty())
      {
        result.push_back(component);
        component.clear();
      }
      continue;
    }
    if (token.IsRshiftPart())
      component += ">";
    else if (token.IsSimple(OP_LT) || token.IsSimple(OP_GT))
      component += token.spelling;
  }
  if (!component.empty())
    result.push_back(component);
  return result;
}

std::vector<std::string> ConstEvaluator::NameComponents(AstId node) const
{
  if (node == 0)
    return std::vector<std::string>();
  const AstNode& value = arena_.At(node);
  if (value.kind == AST_ENUM_SPECIFIER && !value.text.empty())
  {
    std::vector<std::string> result;
    std::string component;
    for (std::size_t i = 0; i < value.text.size(); ++i)
    {
      if (i + 1 < value.text.size() && value.text[i] == ':' &&
          value.text[i + 1] == ':')
      {
        if (!component.empty())
        {
          result.push_back(component);
          component.clear();
        }
        ++i;
      }
      else
        component += value.text[i];
    }
    if (!component.empty())
      result.push_back(component);
    return result;
  }
  if (value.first < value.last)
    return NameComponents(value.first, value.last);
  if (!value.text.empty())
    return std::vector<std::string>(1, value.text);
  return std::vector<std::string>();
}

TypeId ConstEvaluator::BuildFundamental(
    const std::vector<ETokenType>& tokens) const
{
  if (types_ == 0)
    throw std::runtime_error("type table unavailable during constant evaluation");
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
  return types_->Fundamental(result);
}

TypeId ConstEvaluator::ResolveTypeSpecifier(AstId node, ScopeId scope) const
{
  const AstNode& value = arena_.At(node);
  if (value.kind == AST_ENUM_SPECIFIER || value.kind == AST_CLASS_SPECIFIER ||
      value.kind == AST_CLASS_FORWARD_DECLARATION)
  {
    const std::vector<std::string> name = NameComponents(node);
    if (model_ == 0 || name.empty())
      throw std::runtime_error("constant expression has no named type");
    const BindingId binding = name.size() == 1 ?
        model_->LookupTypeName(scope, name[0]) :
        model_->LookupQualified(scope, name, LOOKUP_TYPES);
    if (binding == 0 || model_->BindingAt(binding).type == 0)
      throw std::runtime_error("unknown type in constant expression");
    return model_->BindingAt(binding).type;
  }
  if ((value.kind == AST_DECL_SPECIFIER || value.kind == AST_DECLTYPE_SPECIFIER) &&
      !value.children.empty())
    return ResolveExpressionType(value.children[0], scope);
  if (value.first >= tokens_.size())
    throw std::runtime_error("invalid type in constant expression");
  const std::vector<std::string> name = NameComponents(node);
  if (name.empty())
    throw std::runtime_error("invalid type in constant expression");
  const Pa6Token& token = tokens_[value.first];
  if (value.last == value.first + 1 && token.kind == PA6_SIMPLE_TOKEN &&
      IsBuiltin(token.simple_type))
    return BuildFundamental(std::vector<ETokenType>(1, token.simple_type));
  const BindingId binding = name.size() == 1 ?
      model_->LookupTypeName(scope, name[0]) :
      model_->LookupQualified(scope, name, LOOKUP_TYPES);
  if (binding == 0 || model_->BindingAt(binding).type == 0)
    throw std::runtime_error("unknown type in constant expression");
  return model_->BindingAt(binding).type;
}

TypeId ConstEvaluator::ResolveTypeSequence(AstId node, ScopeId scope) const
{
  const AstNode& sequence = arena_.At(node);
  std::vector<ETokenType> fundamental;
  TypeId result = 0;
  bool is_const = false;
  bool is_volatile = false;
  for (std::size_t i = 0; i < sequence.children.size(); ++i)
  {
    const AstId child = sequence.children[i];
    const AstNode& value = arena_.At(child);
    if (value.kind == AST_CV_QUALIFIER)
    {
      if (value.first < tokens_.size() &&
          tokens_[value.first].IsSimple(KW_CONST))
        is_const = true;
      if (value.first < tokens_.size() &&
          tokens_[value.first].IsSimple(KW_VOLATILE))
        is_volatile = true;
      continue;
    }
    if ((value.kind == AST_TYPE_SPECIFIER || value.kind == AST_DECL_SPECIFIER) &&
        value.first < tokens_.size() && value.last == value.first + 1 &&
        tokens_[value.first].kind == PA6_SIMPLE_TOKEN &&
        IsBuiltin(tokens_[value.first].simple_type))
    {
      fundamental.push_back(tokens_[value.first].simple_type);
      continue;
    }
    const TypeId child_type = ResolveTypeSpecifier(child, scope);
    if (result != 0)
      throw std::runtime_error("multiple types in constant expression");
    result = child_type;
  }
  if (result == 0 && !fundamental.empty())
    result = BuildFundamental(fundamental);
  if (result == 0)
    throw std::runtime_error("constant expression has no type");
  if (is_const || is_volatile)
    result = types_->Cv(result, is_const, is_volatile);
  return result;
}

TypeId ConstEvaluator::ResolveAbstractDeclarator(AstId node, TypeId base,
                                                 ScopeId scope) const
{
  if (node == 0)
    return base;
  const AstNode& value = arena_.At(node);
  TypeId result = base;
  for (std::size_t i = 0; i < value.children.size(); ++i)
  {
    const AstId child = value.children[i];
    const AstNode& part = arena_.At(child);
    if (part.kind == AST_PTR_OPERATOR && part.first < tokens_.size())
    {
      if (tokens_[part.first].IsSimple(OP_STAR))
        result = types_->Pointer(result);
      else if (tokens_[part.first].IsSimple(OP_AMP))
        result = types_->Reference(result, true);
      else if (tokens_[part.first].IsSimple(OP_LAND))
        result = types_->Reference(result, false);
      else
        throw std::runtime_error("unsupported abstract declarator");
    }
    else if (part.kind == AST_CV_QUALIFIER)
    {
      const bool is_const = part.first < tokens_.size() &&
          tokens_[part.first].IsSimple(KW_CONST);
      result = types_->Cv(result, is_const, !is_const);
    }
    else if (part.kind == AST_ARRAY_SUFFIX)
    {
      if (part.children.empty() || part.children[0] == 0)
        throw std::runtime_error("incomplete array type");
      const long long bound = EvaluateNode(part.children[0], scope);
      if (bound <= 0)
        throw std::runtime_error("array bound must be positive");
      result = types_->Array(result, static_cast<std::size_t>(bound));
    }
    else if (part.kind == AST_NESTED_DECLARATOR ||
             part.kind == AST_ABSTRACT_DECLARATOR ||
             part.kind == AST_DECLARATOR)
    {
      if (part.children.size() != 1)
        throw std::runtime_error("unsupported abstract declarator");
      result = ResolveAbstractDeclarator(part.children[0], result, scope);
    }
  }
  return result;
}

TypeId ConstEvaluator::ResolveType(AstId node, ScopeId scope) const
{
  if (node == 0)
    throw std::runtime_error("missing type in constant expression");
  const AstNode& value = arena_.At(node);
  if (value.kind != AST_TYPE_ID)
    return ResolveTypeSpecifier(node, scope);
  if (value.children.empty())
    throw std::runtime_error("invalid type-id in constant expression");
  TypeId result = ResolveTypeSequence(value.children[0], scope);
  if (value.children.size() > 1 && value.children[1] != 0)
    result = ResolveAbstractDeclarator(value.children[1], result, scope);
  return result;
}

TypeId ConstEvaluator::ResolveExpressionType(AstId node, ScopeId scope) const
{
  if (node == 0)
    throw std::runtime_error("missing expression type");
  const AstNode& value = arena_.At(node);
  if (value.kind == AST_PARENTHESIZED_EXPRESSION)
  {
    if (value.children.size() != 1)
      throw std::runtime_error("invalid parenthesized expression");
    return ResolveExpressionType(value.children[0], scope);
  }
  if (value.kind == AST_TYPE_ID)
    return ResolveType(node, scope);
  if (value.kind == AST_ID_EXPRESSION || value.kind == AST_IDENTIFIER)
  {
    const std::vector<std::string> name = NameComponents(node);
    if (name.empty() || model_ == 0)
      throw std::runtime_error("invalid expression name");
    const BindingId binding = name.size() == 1 ?
        model_->LookupUnqualified(scope, name[0], LOOKUP_ANY) :
        model_->LookupQualified(scope, name, LOOKUP_ANY);
    if (binding == 0 || model_->BindingAt(binding).type == 0)
      throw std::runtime_error("unknown expression name");
    return model_->BindingAt(binding).type;
  }
  if (value.kind == AST_LITERAL)
  {
    if (value.first >= tokens_.size() || !tokens_[value.first].lit_scalar)
      throw std::runtime_error("invalid literal type");
    return types_->Fundamental(tokens_[value.first].lit_type);
  }
  if (value.kind == AST_KEYWORD_LITERAL)
    return types_->Fundamental(FT_BOOL);
  if (value.kind == AST_CAST_EXPRESSION)
  {
    if (value.children.empty())
      throw std::runtime_error("invalid cast expression");
    return ResolveType(value.children[0], scope);
  }
  if (value.kind == AST_SIZEOF_EXPRESSION ||
      value.kind == AST_TYPE_TRAIT_EXPRESSION ||
      value.kind == AST_BINARY_EXPRESSION ||
      value.kind == AST_ASSIGNMENT_EXPRESSION ||
      value.kind == AST_UNARY_EXPRESSION ||
      value.kind == AST_CONDITIONAL_EXPRESSION)
    return types_->Fundamental(FT_INT);
  throw std::runtime_error("unsupported expression type");
}

long long ConstEvaluator::EvaluateNode(AstId expression, ScopeId scope) const
{
  const AstNode& node = arena_.At(expression);
  switch (node.kind)
  {
  case AST_PARENTHESIZED_EXPRESSION:
    if (node.children.size() != 1)
      throw std::runtime_error("invalid parenthesized constant expression");
    return EvaluateNode(node.children[0], scope);

  case AST_LITERAL:
    if (node.first >= tokens_.size() || !tokens_[node.first].lit_scalar ||
        !IsIntegral(tokens_[node.first].lit_type) ||
        tokens_[node.first].lit_value >
            static_cast<unsigned long long>(
                std::numeric_limits<long long>::max()))
      throw std::runtime_error("constant expression is not a signed integer");
    return static_cast<long long>(tokens_[node.first].lit_value);

  case AST_KEYWORD_LITERAL:
    if (node.first >= tokens_.size())
      throw std::runtime_error("invalid keyword literal");
    if (tokens_[node.first].IsSimple(KW_TRUE))
      return 1;
    if (tokens_[node.first].IsSimple(KW_FALSE))
      return 0;
    throw std::runtime_error("keyword is not an integer constant");

  case AST_ID_EXPRESSION:
  case AST_IDENTIFIER:
  {
    if (model_ == 0)
      throw std::runtime_error("scope model unavailable during constant evaluation");
    const std::vector<std::string> name = NameComponents(expression);
    if (name.empty())
      throw std::runtime_error("invalid constant expression name");
    const BindingId binding = name.size() == 1 ?
        model_->LookupUnqualified(scope, name[0], LOOKUP_VALUES) :
        model_->LookupQualified(scope, name, LOOKUP_VALUES);
    if (binding == 0 || !model_->BindingAt(binding).has_const_value)
      throw std::runtime_error("name is not a constant expression");
    return model_->BindingAt(binding).const_value;
  }

  case AST_UNARY_EXPRESSION:
    return EvaluateUnary(node, scope);
  case AST_BINARY_EXPRESSION:
    return EvaluateBinary(node, scope);
  case AST_ASSIGNMENT_EXPRESSION:
    throw std::runtime_error("assignment is not a constant expression");
  case AST_CONDITIONAL_EXPRESSION:
    return EvaluateConditional(node, scope);
  case AST_CAST_EXPRESSION:
    return EvaluateCast(node, scope);
  case AST_SIZEOF_EXPRESSION:
  case AST_TYPE_TRAIT_EXPRESSION:
    return EvaluateSizeOf(node, scope);
  default:
    throw std::runtime_error("unsupported constant expression");
  }
}

long long ConstEvaluator::EvaluateUnary(const AstNode& node, ScopeId scope) const
{
  if (node.children.size() != 1 || node.first >= tokens_.size())
    throw std::runtime_error("invalid unary constant expression");
  const long long value = EvaluateNode(node.children[0], scope);
  const ETokenType op = tokens_[node.first].simple_type;
  switch (op)
  {
  case OP_PLUS: return value;
  case OP_MINUS: return Checked(-static_cast<__int128>(value));
  case OP_LNOT: return value == 0 ? 1 : 0;
  case OP_COMPL: return Checked(~static_cast<__int128>(value));
  default: throw std::runtime_error("unsupported unary constant expression");
  }
}

long long ConstEvaluator::EvaluateBinary(const AstNode& node, ScopeId scope) const
{
  if (node.children.size() != 2 || node.first >= tokens_.size())
    throw std::runtime_error("invalid binary constant expression");
  const ETokenType op = tokens_[node.first].IsRshiftPart() ? OP_RSHIFT :
      tokens_[node.first].simple_type;
  const long long left = EvaluateNode(node.children[0], scope);
  if (op == OP_LAND && left == 0)
    return 0;
  if (op == OP_LOR && left != 0)
    return 1;
  const long long right = EvaluateNode(node.children[1], scope);
  switch (op)
  {
  case OP_PLUS: return Checked(static_cast<__int128>(left) + right);
  case OP_MINUS: return Checked(static_cast<__int128>(left) - right);
  case OP_STAR: return Checked(static_cast<__int128>(left) * right);
  case OP_DIV:
    if (right == 0 || (left == std::numeric_limits<long long>::min() &&
                       right == -1))
      throw std::runtime_error("invalid constant division");
    return left / right;
  case OP_MOD:
    if (right == 0 || (left == std::numeric_limits<long long>::min() &&
                       right == -1))
      throw std::runtime_error("invalid constant remainder");
    return left % right;
  case OP_LSHIFT:
    if (right < 0 || right >= 64 || left < 0)
      throw std::runtime_error("invalid constant shift");
    return Checked(static_cast<__int128>(left) << right);
  case OP_RSHIFT:
    if (right < 0 || right >= 64)
      throw std::runtime_error("invalid constant shift");
    if (left >= 0)
      return left >> right;
    return -static_cast<long long>((static_cast<unsigned long long>(
        -(left + 1)) >> right)) - 1;
  case OP_AMP: return left & right;
  case OP_BOR: return left | right;
  case OP_XOR: return left ^ right;
  case OP_EQ: return left == right ? 1 : 0;
  case OP_NE: return left != right ? 1 : 0;
  case OP_LT: return left < right ? 1 : 0;
  case OP_GT: return left > right ? 1 : 0;
  case OP_LE: return left <= right ? 1 : 0;
  case OP_GE: return left >= right ? 1 : 0;
  case OP_LAND: return (left != 0 && right != 0) ? 1 : 0;
  case OP_LOR: return (left != 0 || right != 0) ? 1 : 0;
  case OP_COMMA: return right;
  default: throw std::runtime_error("unsupported binary constant expression");
  }
}

long long ConstEvaluator::EvaluateConditional(const AstNode& node,
                                              ScopeId scope) const
{
  if (node.children.size() != 3)
    throw std::runtime_error("invalid conditional constant expression");
  return EvaluateNode(node.children[0], scope) != 0 ?
      EvaluateNode(node.children[1], scope) :
      EvaluateNode(node.children[2], scope);
}

long long ConstEvaluator::Convert(long long value, TypeId type) const
{
  while (types_->Kind(type) == TYPE_CV)
    type = types_->At(type).base;
  unsigned bits = 0;
  bool is_unsigned = false;
  if (types_->Kind(type) == TYPE_ENUM)
  {
    TypeId underlying = types_->At(type).base;
    while (types_->Kind(underlying) == TYPE_CV)
      underlying = types_->At(underlying).base;
    if (types_->Kind(underlying) != TYPE_FUNDAMENTAL ||
        !IsIntegral(types_->At(underlying).fundamental))
      throw std::runtime_error("invalid enum conversion");
    bits = IntegerBits(types_->At(underlying).fundamental);
    is_unsigned = IsUnsigned(types_->At(underlying).fundamental);
  }
  else if (types_->Kind(type) == TYPE_FUNDAMENTAL &&
           IsIntegral(types_->At(type).fundamental))
  {
    bits = IntegerBits(types_->At(type).fundamental);
    is_unsigned = IsUnsigned(types_->At(type).fundamental);
  }
  else
    throw std::runtime_error("cast target is not an integer type");
  if (bits == 1)
    return value == 0 ? 0 : 1;
  if (bits >= 64)
    return value;
  const unsigned long long modulus = 1ULL << bits;
  const unsigned long long raw =
      static_cast<unsigned long long>(value) & (modulus - 1);
  if (is_unsigned)
  {
    if (raw > static_cast<unsigned long long>(
        std::numeric_limits<long long>::max()))
      throw std::runtime_error("unsigned constant is not representable");
    return static_cast<long long>(raw);
  }
  const unsigned long long sign = 1ULL << (bits - 1);
  return raw >= sign ? static_cast<long long>(raw - modulus) :
      static_cast<long long>(raw);
}

long long ConstEvaluator::EvaluateCast(const AstNode& node, ScopeId scope) const
{
  if (node.children.size() != 2)
    throw std::runtime_error("invalid cast constant expression");
  return Convert(EvaluateNode(node.children[1], scope),
                 ResolveType(node.children[0], scope));
}

long long ConstEvaluator::EvaluateSizeOf(const AstNode& node,
                                         ScopeId scope) const
{
  if (node.children.size() != 1 || types_ == 0)
    throw std::runtime_error("invalid sizeof expression");
  const AstId operand = node.children[0];
  const TypeId type = arena_.At(operand).kind == AST_TYPE_ID ?
      ResolveType(operand, scope) : ResolveExpressionType(operand, scope);
  if (node.kind == AST_SIZEOF_EXPRESSION ||
      (node.first < tokens_.size() &&
       tokens_[node.first].IsSimple(KW_SIZEOF)))
    return Checked(static_cast<__int128>(types_->SizeOf(type)));
  if (node.first >= tokens_.size() ||
      !tokens_[node.first].IsSimple(KW_ALIGNOF))
    throw std::runtime_error("unsupported type trait constant expression");
  return Checked(static_cast<__int128>(types_->AlignOf(type)));
}
