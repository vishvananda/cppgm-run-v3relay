#include "nsdecl_parser.h"

#include <stdexcept>

#include "nsinit_sema.h"

using namespace std;

namespace
{

bool IsStorageSpecifier(ETokenType type)
{
	return type == KW_STATIC || type == KW_THREAD_LOCAL ||
		type == KW_EXTERN;
}

bool IsCVSpecifier(ETokenType type)
{
	return type == KW_CONST || type == KW_VOLATILE;
}

bool IsFundamentalSpecifier(ETokenType type)
{
	switch (type)
	{
	case KW_CHAR:
	case KW_CHAR16_T:
	case KW_CHAR32_T:
	case KW_WCHAR_T:
	case KW_BOOL:
	case KW_SHORT:
	case KW_INT:
	case KW_LONG:
	case KW_SIGNED:
	case KW_UNSIGNED:
	case KW_FLOAT:
	case KW_DOUBLE:
	case KW_VOID:
		return true;
	default:
		return false;
	}
}

} // namespace

Pa7Parser::FundamentalSpec::FundamentalSpec()
	: have_char(false), have_char16(false), have_char32(false),
		have_wchar(false), have_bool(false), have_int(false),
		have_float(false), have_double(false), have_void(false),
		have_signed(false), have_unsigned(false), short_count(0),
		long_count(0)
{
}

bool Pa7Parser::FundamentalSpec::HasType() const
{
	return have_char || have_char16 || have_char32 || have_wchar ||
		have_bool || have_int || have_float || have_double || have_void ||
		have_signed || have_unsigned || short_count != 0 || long_count != 0;
}

bool Pa7Parser::FundamentalSpec::Valid() const
{
	const unsigned primary_count =
		(have_char ? 1u : 0u) + (have_char16 ? 1u : 0u) +
		(have_char32 ? 1u : 0u) + (have_wchar ? 1u : 0u) +
		(have_bool ? 1u : 0u) + (have_float ? 1u : 0u) +
		(have_double ? 1u : 0u) + (have_void ? 1u : 0u);
	if (primary_count > 1 || short_count > 1 || long_count > 2)
		return false;
	if (have_signed && have_unsigned)
		return false;
	if (have_float || have_void || have_bool || have_char16 ||
		have_char32 || have_wchar)
		return !have_signed && !have_unsigned && short_count == 0 &&
			long_count == 0;
	if (have_double)
		return !have_signed && !have_unsigned && short_count == 0 &&
			long_count <= 1;
	if (have_char)
		return short_count == 0 && long_count == 0;
	return short_count <= 1;
}

Pa7TypePtr Pa7Parser::FundamentalSpec::Finish() const
{
	if (have_char)
	{
		if (have_unsigned)
			return MakeFundamental(FT_UNSIGNED_CHAR);
		if (have_signed)
			return MakeFundamental(FT_SIGNED_CHAR);
		return MakeFundamental(FT_CHAR);
	}
	if (have_char16)
		return MakeFundamental(FT_CHAR16_T);
	if (have_char32)
		return MakeFundamental(FT_CHAR32_T);
	if (have_wchar)
		return MakeFundamental(FT_WCHAR_T);
	if (have_bool)
		return MakeFundamental(FT_BOOL);
	if (have_float)
		return MakeFundamental(FT_FLOAT);
	if (have_double)
		return MakeFundamental(long_count == 1 ? FT_LONG_DOUBLE : FT_DOUBLE);
	if (have_void)
		return MakeFundamental(FT_VOID);
	if (short_count != 0)
	{
		return MakeFundamental(have_unsigned ? FT_UNSIGNED_SHORT_INT :
			FT_SHORT_INT);
	}
	if (long_count >= 2)
	{
		return MakeFundamental(have_unsigned ? FT_UNSIGNED_LONG_LONG_INT :
			FT_LONG_LONG_INT);
	}
	if (long_count == 1)
	{
		return MakeFundamental(have_unsigned ? FT_UNSIGNED_LONG_INT :
			FT_LONG_INT);
	}
	if (have_unsigned)
		return MakeFundamental(FT_UNSIGNED_INT);
	return MakeFundamental(FT_INT);
}

Pa7Parser::Pa7Parser(const vector<Pa6Token>& tokens, Pa7Namespace* global,
	bool strict_mode)
	: tokens_(tokens), pos_(0), global_(global), strict_mode_(strict_mode),
		next_order_(0)
{
	if (!global_)
		throw runtime_error("null global namespace");
	scopes_.push_back(global_);
}

const Pa6Token& Pa7Parser::Token(size_t at) const
{
	static const Pa6Token end_token(PA6_EOF_TOKEN, "");
	return at < tokens_.size() ? tokens_[at] : end_token;
}

bool Pa7Parser::IsSimple(ETokenType type, size_t at) const
{
	return Token(at == static_cast<size_t>(-1) ? pos_ : at).IsSimple(type);
}

bool Pa7Parser::IsKind(Pa6TokenKind kind, size_t at) const
{
	return Token(at == static_cast<size_t>(-1) ? pos_ : at).kind == kind;
}

bool Pa7Parser::ConsumeSimple(ETokenType type)
{
	if (!IsSimple(type))
		return false;
	++pos_;
	return true;
}

bool Pa7Parser::ConsumeKind(Pa6TokenKind kind)
{
	if (!IsKind(kind))
		return false;
	++pos_;
	return true;
}

bool Pa7Parser::ConsumeIdentifier(string* spelling)
{
	if (!IsKind(PA6_IDENTIFIER_TOKEN))
		return false;
	if (spelling)
		*spelling = Token(pos_).spelling;
	++pos_;
	return true;
}

bool Pa7Parser::ConsumeLiteral(Pa6Token* literal)
{
	if (!IsKind(PA6_LITERAL_TOKEN))
		return false;
	if (literal)
		*literal = Token(pos_);
	++pos_;
	return true;
}

void Pa7Parser::ExpectSimple(ETokenType type)
{
	if (!ConsumeSimple(type))
		throw runtime_error("unexpected token in nsdecl parser");
}

void Pa7Parser::ExpectIdentifier(string* spelling)
{
	if (!ConsumeIdentifier(spelling))
		throw runtime_error("expected identifier in nsdecl parser");
}

void Pa7Parser::ExpectEndOfDeclaration()
{
	if (!ConsumeSimple(OP_SEMICOLON))
		throw runtime_error("expected semicolon in nsdecl parser");
}

bool Pa7Parser::ParseTranslationUnit()
{
	while (!IsKind(PA6_EOF_TOKEN))
		ParseDeclaration();
	return true;
}

void Pa7Parser::ParseDeclaration()
{
	if (ConsumeSimple(OP_SEMICOLON))
		return;
	if (IsSimple(KW_NAMESPACE) ||
		(IsSimple(KW_INLINE) && IsSimple(KW_NAMESPACE, pos_ + 1)))
	{
		size_t name_at = IsSimple(KW_INLINE) ? pos_ + 2 : pos_ + 1;
		if (IsKind(PA6_IDENTIFIER_TOKEN, name_at) &&
			IsSimple(OP_ASS, name_at + 1))
			ParseNamespaceAliasDefinition();
		else
			ParseNamespaceDefinition();
		return;
	}
	if (IsSimple(KW_USING))
	{
		ParseUsingDeclaration();
		return;
	}
	if (IsSimple(KW_STATIC_ASSERT))
	{
		ParseStaticAssertDeclaration();
		return;
	}
	ParseSimpleDeclaration();
}

void Pa7Parser::ParseNamespaceDefinition()
{
	bool is_inline = ConsumeSimple(KW_INLINE);
	ExpectSimple(KW_NAMESPACE);

	Pa7Namespace* child;
	if (IsKind(PA6_IDENTIFIER_TOKEN))
	{
		string name;
		ExpectIdentifier(&name);
		child = scopes_.back()->AddNamespace(name, is_inline);
	}
	else
		child = scopes_.back()->AddUnnamedNamespace(is_inline);

	ExpectSimple(OP_LBRACE);
	scopes_.push_back(child);
	while (!IsSimple(OP_RBRACE))
	{
		if (IsKind(PA6_EOF_TOKEN))
			throw runtime_error("unterminated namespace");
		ParseDeclaration();
	}
	ExpectSimple(OP_RBRACE);
	scopes_.pop_back();
}

void Pa7Parser::ParseNamespaceAliasDefinition()
{
	ExpectSimple(KW_NAMESPACE);
	string alias;
	ExpectIdentifier(&alias);
	ExpectSimple(OP_ASS);
	NamePath target_path = ParseNamePath();
	ExpectEndOfDeclaration();
	Pa7Namespace* target = ResolveNamespace(target_path);
	if (!target)
		throw runtime_error("namespace alias target not found");
	scopes_.back()->AddNamespaceAlias(alias, target);
}

void Pa7Parser::ParseUsingDeclaration()
{
	ExpectSimple(KW_USING);
	if (ConsumeSimple(KW_NAMESPACE))
	{
		NamePath path = ParseNamePath();
		ExpectEndOfDeclaration();
		Pa7Namespace* target = ResolveNamespace(path);
		if (!target)
			throw runtime_error("using-directive target not found");
		scopes_.back()->AddUsingDirective(target);
		return;
	}

	NamePath path;
	if (ConsumeSimple(OP_COLON2))
		path.absolute = true;
	string first;
	ExpectIdentifier(&first);
	path.parts.push_back(first);
	if (!path.absolute && ConsumeSimple(OP_ASS))
	{
		DeclSpec spec = ParseDeclSpecifierSeq();
		Pa7TypePtr type = spec.type;
		if (IsDeclaratorStart(pos_))
		{
			Declarator declarator = ParseDeclarator(type, DECL_ABSTRACT);
			if (declarator.has_name)
				throw runtime_error("named declarator in alias type");
			type = declarator.type;
		}
		ExpectEndOfDeclaration();
		scopes_.back()->AddTypedef(first, type);
		return;
	}

	while (ConsumeSimple(OP_COLON2))
	{
		string part;
		ExpectIdentifier(&part);
		path.parts.push_back(part);
	}
	ExpectEndOfDeclaration();
	if (!path.absolute && path.parts.size() < 2)
		throw runtime_error("unqualified using-declaration");

	vector<string> namespace_parts(path.parts.begin(), path.parts.end() - 1);
	Pa7Namespace* target = ResolveRelativeNamespace(scopes_.back(),
		namespace_parts, path.absolute);
	if (!target)
		throw runtime_error("using-declaration target namespace not found");
	Pa7Decl* source = LookupInNamespace(target, path.parts.back(),
		PA7_FIND_ANY);
	if (!source)
		throw runtime_error("using-declaration target not found");
	if (strict_mode_ && source->kind == PA7_DECL_NAMESPACE)
		throw runtime_error("using-declaration cannot name a namespace");
	scopes_.back()->AddUsingDeclaration(path.parts.back(), *source);
}

void Pa7Parser::ParseSimpleDeclaration()
{
	DeclSpec spec = ParseDeclSpecifierSeq();
	if (IsSimple(OP_SEMICOLON))
	{
		ExpectEndOfDeclaration();
		return;
	}

	bool saw_function_definition = false;
	for (;;)
	{
		Declarator declarator = ParseDeclarator(spec.type, DECL_NAMED);
		if (!declarator.has_name || declarator.path.parts.empty())
			throw runtime_error("declaration has no name");

		Pa7Namespace* destination = scopes_.back();
			if (declarator.path.absolute || declarator.path.parts.size() > 1)
			{
				vector<string> namespace_parts(declarator.path.parts.begin(),
					declarator.path.parts.end() - 1);
				if (strict_mode_ && !declarator.path.absolute &&
					scopes_.back() != global_ && !namespace_parts.empty())
				{
					bool enclosing = false;
					for (Pa7Namespace* enclosing_scope = scopes_.back();
						enclosing_scope && enclosing_scope != global_;
						enclosing_scope = enclosing_scope->parent)
						if (enclosing_scope->name == namespace_parts[0])
							enclosing = true;
					if (!enclosing)
						throw runtime_error(
							"qualified declarator namespace is not enclosing");
				}
				destination = ResolveRelativeNamespace(scopes_.back(),
				namespace_parts, declarator.path.absolute);
			if (!destination)
				throw runtime_error("qualified declarator namespace not found");
		}

		const string& name = declarator.path.parts.back();
			SetTypeExpressionScope(declarator.type, destination);
			if (spec.is_typedef)
			{
				destination->AddTypedef(name, declarator.type);
				if (ConsumeSimple(OP_COMMA))
					continue;
				break;
			}
			if (spec.is_constexpr)
			declarator.type = ApplyCV(PA7_CV_CONST, declarator.type);

		const bool function = IsFunction(declarator.type);
		const bool has_initializer = ConsumeSimple(OP_ASS);
		if (has_initializer && function)
			throw runtime_error("function declarator has an initializer");
		const bool function_definition = function &&
			IsSimple(OP_LBRACE);
		saw_function_definition = saw_function_definition || function_definition;
		if (function_definition && spec.has_named_type && strict_mode_)
			throw runtime_error("function typedef cannot define a function");
		const bool definition = function ? function_definition :
			has_initializer ||
			(spec.storage & PA7_STORAGE_EXTERN) == 0;
		if (strict_mode_ && spec.is_constexpr && !function &&
			!has_initializer)
			throw runtime_error("constexpr object needs an initializer");
		if (strict_mode_ && spec.is_const && !function &&
			!has_initializer &&
			(spec.storage & PA7_STORAGE_EXTERN) == 0)
			throw runtime_error("const object needs an initializer");
		if (strict_mode_ && !function &&
			(declarator.type->kind == PA7_TYPE_LVALUE_REFERENCE ||
			 declarator.type->kind == PA7_TYPE_RVALUE_REFERENCE) &&
			!has_initializer)
			throw runtime_error("reference needs an initializer");

		Pa7DeclAttributes attributes = MakeAttributes(spec, definition,
			next_order_++);
		const bool qualified = declarator.path.absolute ||
			declarator.path.parts.size() > 1;
		Pa7Decl* visible = qualified ? const_cast<Pa7Decl*>(
			destination->FindDirectOrInline(name, function ?
				PA7_FIND_FUNCTION : PA7_FIND_VARIABLE)) : 0;
		Pa7Namespace* merge_scope = destination;
		if (visible)
		{
			if (visible->kind == PA7_DECL_VARIABLE && visible->variable &&
				!function)
				merge_scope = visible->variable->owner;
			else if (visible->kind == PA7_DECL_FUNCTION && visible->function &&
				function)
				merge_scope = visible->function->owner;
			else
				throw runtime_error("declaration kind conflict");
		}
		if (function)
		{
			shared_ptr<Pa7Function> entity = merge_scope->AddOrMergeFunction(
				name, declarator.type, attributes, strict_mode_);
			if (function_definition)
				ParseFunctionBody();
			(void)entity;
		}
		else
		{
			shared_ptr<Pa7Variable> entity = merge_scope->AddOrMergeVariable(
				name, declarator.type, attributes, strict_mode_);
			if (has_initializer)
			{
				if (!strict_mode_)
					throw runtime_error("initializers unsupported in nsdecl");
				Pa8ExprPtr initializer = ParseExpression();
				SetExpressionScope(initializer, destination);
				entity->initializer_expression = initializer;
			}
		}

		if (!ConsumeSimple(OP_COMMA))
			break;
		if (function_definition)
			throw runtime_error("function definition cannot declare another entity");
	}
	if (saw_function_definition)
	{
		ConsumeSimple(OP_SEMICOLON);
		return;
	}
	if (IsSimple(OP_LBRACE))
		throw runtime_error("unexpected function body");
	ExpectEndOfDeclaration();
}

Pa7Parser::DeclSpec Pa7Parser::ParseDeclSpecifierSeq()
{
	DeclSpec result;
	FundamentalSpec fundamental;
	Pa7TypePtr named_type;
	unsigned cv = PA7_CV_NONE;
	bool consumed = false;

	for (;;)
	{
		ETokenType type = Token(pos_).simple_type;
		if (IsKind(PA6_IDENTIFIER_TOKEN) || IsSimple(OP_COLON2))
		{
			if (fundamental.HasType() || named_type)
				break;
			NamePath path = ParseNamePath();
			named_type = LookupTypedef(path);
			if (!named_type)
				throw runtime_error("unknown typedef name");
			result.has_named_type = true;
			consumed = true;
			continue;
		}
		if (IsCVSpecifier(type))
		{
			if (type == KW_CONST)
				cv |= PA7_CV_CONST;
			else
				cv |= PA7_CV_VOLATILE;
			++pos_;
			consumed = true;
			continue;
		}
		if (type == KW_TYPEDEF)
		{
			result.is_typedef = true;
			++pos_;
			consumed = true;
			continue;
		}
		if (type == KW_CONSTEXPR)
		{
			if (result.is_constexpr && strict_mode_)
				throw runtime_error("duplicate constexpr specifier");
			result.is_constexpr = true;
			++pos_;
			consumed = true;
			continue;
		}
		if (type == KW_INLINE)
		{
			result.is_inline = true;
			++pos_;
			consumed = true;
			continue;
		}
		if (IsStorageSpecifier(type))
		{
			if (type == KW_STATIC)
				result.storage |= PA7_STORAGE_STATIC;
			else if (type == KW_EXTERN)
				result.storage |= PA7_STORAGE_EXTERN;
			else
				result.storage |= PA7_STORAGE_THREAD_LOCAL;
			++pos_;
			consumed = true;
			continue;
		}
		if (!IsFundamentalSpecifier(type))
			break;
		++pos_;
		consumed = true;
		switch (type)
		{
		case KW_CHAR: fundamental.have_char = true; break;
		case KW_CHAR16_T: fundamental.have_char16 = true; break;
		case KW_CHAR32_T: fundamental.have_char32 = true; break;
		case KW_WCHAR_T: fundamental.have_wchar = true; break;
		case KW_BOOL: fundamental.have_bool = true; break;
		case KW_SHORT: ++fundamental.short_count; break;
		case KW_INT: fundamental.have_int = true; break;
		case KW_LONG: ++fundamental.long_count; break;
		case KW_SIGNED: fundamental.have_signed = true; break;
		case KW_UNSIGNED: fundamental.have_unsigned = true; break;
		case KW_FLOAT: fundamental.have_float = true; break;
		case KW_DOUBLE: fundamental.have_double = true; break;
		case KW_VOID: fundamental.have_void = true; break;
		default: break;
		}
	}

	if (!consumed || (!fundamental.HasType() && !named_type))
		throw runtime_error("missing declaration specifier");
	if (strict_mode_ && fundamental.HasType() && !fundamental.Valid())
		throw runtime_error("invalid fundamental type specifier sequence");
	result.type = named_type ? named_type : fundamental.Finish();
	if (cv != PA7_CV_NONE)
		result.type = ApplyCV(cv, result.type);
	result.is_const = (cv & PA7_CV_CONST) != 0;
	if (!result.is_const && result.type && result.type->kind == PA7_TYPE_CV)
		result.is_const = (result.type->cv & PA7_CV_CONST) != 0;
	return result;
}

Pa7TypePtr Pa7Parser::ParseTypeId()
{
	DeclSpec spec = ParseDeclSpecifierSeq();
	if (IsSimple(OP_STAR) || IsSimple(OP_AMP) || IsSimple(OP_LAND) ||
		IsSimple(OP_LPAREN) || IsSimple(OP_LSQUARE))
	{
		Declarator declarator = ParseDeclarator(spec.type, DECL_ABSTRACT);
		if (declarator.has_name)
			throw runtime_error("named declarator in type-id");
		return declarator.type;
	}
	return spec.type;
}

Pa7Parser::Declarator Pa7Parser::ParseDeclarator(const Pa7TypePtr& base,
	DeclaratorMode mode)
{
	Declarator result;
	if (TryRedundantParentheses(base, mode, result))
		return result;
	return ParsePtrDeclarator(base, mode);
}

Pa7Parser::Declarator Pa7Parser::ParsePtrDeclarator(const Pa7TypePtr& base,
	DeclaratorMode mode)
{
	Declarator result;
	vector<TypeWrapper> chain;
	ParseDeclaratorChain(chain, result, mode);

	Pa7TypePtr working = base;
	for (size_t i = 0; i < chain.size(); ++i)
	{
		const TypeWrapper& wrap = chain[i];
		switch (wrap.kind)
		{
		case PA7_TYPE_POINTER:
			working = ApplyCV(wrap.cv, MakePointer(working));
			break;
		case PA7_TYPE_LVALUE_REFERENCE:
			if (strict_mode_ && working &&
				working->kind == PA7_TYPE_LVALUE_REFERENCE)
				throw runtime_error("reference to reference is not accepted here");
			working = MakeReference(false, working);
			break;
		case PA7_TYPE_RVALUE_REFERENCE:
			if (strict_mode_ && working &&
				working->kind == PA7_TYPE_LVALUE_REFERENCE)
				throw runtime_error("reference to reference is not accepted here");
			working = MakeReference(true, working);
			break;
		case PA7_TYPE_ARRAY:
			working = MakeArray(wrap.has_bound, wrap.bound, working,
				wrap.bound_expression);
			break;
		default:
			working = MakeFunction(wrap.parameters.types,
				wrap.parameters.varargs, working);
			break;
		}
	}
	result.type = working;
	if (mode == DECL_NAMED && !result.has_name)
		throw runtime_error("named declarator is abstract");
	return result;
}

void Pa7Parser::ParseDeclaratorChain(vector<TypeWrapper>& chain,
	Declarator& result, DeclaratorMode mode)
{
	vector<TypeWrapper> ops;
	while (IsSimple(OP_STAR) || IsSimple(OP_AMP) || IsSimple(OP_LAND))
	{
		TypeWrapper op;
		if (ConsumeSimple(OP_STAR))
		{
			op.kind = PA7_TYPE_POINTER;
			while (IsSimple(KW_CONST) || IsSimple(KW_VOLATILE))
			{
				if (ConsumeSimple(KW_CONST))
					op.cv |= PA7_CV_CONST;
				else
				{
					ExpectSimple(KW_VOLATILE);
					op.cv |= PA7_CV_VOLATILE;
				}
			}
		}
		else if (ConsumeSimple(OP_AMP))
			op.kind = PA7_TYPE_LVALUE_REFERENCE;
		else
		{
			ExpectSimple(OP_LAND);
			op.kind = PA7_TYPE_RVALUE_REFERENCE;
		}
		ops.push_back(op);
	}

	vector<TypeWrapper> child;
	if (IsKind(PA6_IDENTIFIER_TOKEN) || IsSimple(OP_COLON2))
	{
		result.path = ParseNamePath();
		result.has_name = true;
	}
	else if (IsSimple(OP_LPAREN) && !IsParameterStart(pos_ + 1))
	{
		ExpectSimple(OP_LPAREN);
		ParseDeclaratorChain(child, result, mode);
		ExpectSimple(OP_RPAREN);
	}
	else if (IsSimple(OP_LPAREN) || IsSimple(OP_LSQUARE))
	{
		// An abstract declarator can begin with a suffix.  A parenthesis
		// whose contents can start a parameter-declaration-clause is the
		// function suffix form required by 8.2p7.
	}
	else if (!ops.empty())
	{
		// `(*)` and `(&)` use the pointer operator as the abstract root.
	}
	else
		throw runtime_error("missing declarator root");

	vector<TypeWrapper> suffixes;
	while (IsSimple(OP_LPAREN) || IsSimple(OP_LSQUARE))
	{
		TypeWrapper suffix;
		if (IsSimple(OP_LPAREN))
		{
			suffix.kind = PA7_TYPE_FUNCTION;
			suffix.parameters = ParseParametersAndQualifiers();
		}
		else
		{
			Pa6Token literal(PA6_EOF_TOKEN, "");
			ExpectSimple(OP_LSQUARE);
			suffix.kind = PA7_TYPE_ARRAY;
			suffix.has_bound = !IsSimple(OP_RSQUARE);
			if (suffix.has_bound)
			{
				if (strict_mode_)
				{
					suffix.bound_expression = ParseExpression();
					if (suffix.bound_expression->kind == PA8_EXPR_LITERAL &&
						suffix.bound_expression->literal.lit_scalar &&
						suffix.bound_expression->literal.lit_type <= FT_BOOL)
						suffix.bound = suffix.bound_expression->literal.lit_value;
				}
				else
				{
					if (!ConsumeLiteral(&literal) || !literal.lit_scalar ||
						literal.lit_type > FT_BOOL || literal.lit_value == 0)
						throw runtime_error(
							"array bound is not a positive integral literal");
					suffix.bound = literal.lit_value;
				}
			}
			ExpectSimple(OP_RSQUARE);
		}
		suffixes.push_back(suffix);
	}

	// 8.3: within one level, suffixes bind tighter than ptr-operators and
	// a grouped sub-declarator binds tightest, so the applied-first-to-last
	// wrapper order is ops, then suffixes reversed, then the child chain.
	chain.insert(chain.end(), ops.begin(), ops.end());
	chain.insert(chain.end(), suffixes.rbegin(), suffixes.rend());
	chain.insert(chain.end(), child.begin(), child.end());
}

Pa7Parser::Parameters Pa7Parser::ParseParametersAndQualifiers()
{
	Parameters result;
	ExpectSimple(OP_LPAREN);
	if (ConsumeSimple(OP_RPAREN))
		return result;
	if (ConsumeSimple(OP_DOTS))
	{
		result.varargs = true;
		ExpectSimple(OP_RPAREN);
		return result;
	}

	for (;;)
	{
		Declarator parameter = ParseParameterDeclaration();
		if (!parameter.has_name && result.types.empty() &&
			IsVoid(parameter.type) &&
			(IsSimple(OP_RPAREN) || IsSimple(OP_DOTS)))
		{
			if (ConsumeSimple(OP_DOTS))
				result.varargs = true;
			ExpectSimple(OP_RPAREN);
			return result;
		}
		result.types.push_back(AdjustParameter(parameter.type));
		if (ConsumeSimple(OP_DOTS))
		{
			result.varargs = true;
			ExpectSimple(OP_RPAREN);
			return result;
		}
		if (ConsumeSimple(OP_RPAREN))
			return result;
		ExpectSimple(OP_COMMA);
		if (ConsumeSimple(OP_DOTS))
		{
			result.varargs = true;
			ExpectSimple(OP_RPAREN);
			return result;
		}
	}
}

Pa7Parser::Declarator Pa7Parser::ParseParameterDeclaration()
{
	DeclSpec spec = ParseDeclSpecifierSeq();
	if (!IsDeclaratorStart(pos_))
	{
		Declarator result;
		result.type = spec.type;
		return result;
	}
	return ParseDeclarator(spec.type, DECL_EITHER);
}

Pa7Parser::NamePath Pa7Parser::ParseNamePath()
{
	NamePath result;
	if (ConsumeSimple(OP_COLON2))
		result.absolute = true;
	string part;
	ExpectIdentifier(&part);
	result.parts.push_back(part);
	while (ConsumeSimple(OP_COLON2))
	{
		ExpectIdentifier(&part);
		result.parts.push_back(part);
	}
	return result;
}

Pa7TypePtr Pa7Parser::LookupTypedef(const NamePath& path) const
{
	if (path.parts.empty())
		return Pa7TypePtr();
	if (path.parts.size() == 1 && !path.absolute)
	{
		Pa7Decl* decl = LookupUnqualified(path.parts[0], PA7_FIND_TYPE);
		return decl && decl->typedef_entity ? decl->typedef_entity->type :
			Pa7TypePtr();
	}
	vector<string> namespace_parts(path.parts.begin(), path.parts.end() - 1);
	Pa7Namespace* scope = ResolveRelativeNamespace(scopes_.back(),
		namespace_parts, path.absolute);
	if (!scope)
		return Pa7TypePtr();
	Pa7Decl* decl = LookupInNamespace(scope, path.parts.back(),
		PA7_FIND_TYPE);
	return decl && decl->typedef_entity ? decl->typedef_entity->type :
		Pa7TypePtr();
}

Pa7Namespace* Pa7Parser::ResolveNamespace(const NamePath& path) const
{
	return ResolveRelativeNamespace(scopes_.back(), path.parts,
		path.absolute);
}

Pa7Decl* Pa7Parser::LookupInNamespace(const Pa7Namespace* scope,
	const string& name, unsigned filter) const
{
	if (!scope)
		return 0;
	const Pa7Decl* direct = scope->FindDirectOrInline(name, filter);
	if (direct)
		return const_cast<Pa7Decl*>(direct);
	set<const Pa7Namespace*> visited;
	return LookupInUsing(scope, name, filter, visited);
}

Pa7Decl* Pa7Parser::LookupUnqualified(const string& name,
	unsigned filter) const
{
	for (const Pa7Namespace* scope = scopes_.back(); scope;
		scope = scope->parent)
	{
		Pa7Decl* found = LookupInNamespace(scope, name, filter);
		if (found)
			return found;
	}
	return 0;
}

Pa7Decl* Pa7Parser::LookupInUsing(const Pa7Namespace* scope,
	const string& name, unsigned filter,
	set<const Pa7Namespace*>& visited) const
{
	if (!scope || !visited.insert(scope).second)
		return 0;
	for (size_t i = 0; i < scope->using_directives.size(); ++i)
	{
		const Pa7Namespace* nominated = scope->using_directives[i];
		if (!nominated)
			continue;
		if (visited.find(nominated) != visited.end())
			continue;
		Pa7Decl* direct = const_cast<Pa7Decl*>(
			nominated->FindDirectOrInline(name, filter));
		if (direct)
			return direct;
		Pa7Decl* transitive = LookupInUsing(nominated, name, filter,
			visited);
		if (transitive)
			return transitive;
	}
	return 0;
}

Pa7Namespace* Pa7Parser::ResolveRelativeNamespace(const Pa7Namespace* scope,
	const vector<string>& parts, bool absolute) const
{
	if (parts.empty())
		return absolute ? global_ : const_cast<Pa7Namespace*>(scope);

	Pa7Namespace* current = 0;
	size_t index = 0;
	if (absolute)
	{
		current = global_;
	}
	else
	{
		for (const Pa7Namespace* candidate = scope; candidate;
			candidate = candidate->parent)
		{
			Pa7Decl* decl = LookupInNamespace(candidate, parts[0],
				PA7_FIND_NAMESPACE);
			if (decl && decl->namespace_entity)
			{
				current = decl->namespace_entity;
				index = 1;
				break;
			}
		}
	}
	if (!current)
		return 0;
	for (; index < parts.size(); ++index)
	{
		Pa7Decl* decl = LookupInNamespace(current, parts[index],
			PA7_FIND_NAMESPACE);
		if (!decl || !decl->namespace_entity)
			return 0;
		current = decl->namespace_entity;
	}
	return current;
}

bool Pa7Parser::IsParameterStart(size_t at) const
{
	const Pa6Token& token = Token(at);
	if (token.kind == PA6_EOF_TOKEN || token.IsSimple(OP_RPAREN) ||
		token.IsSimple(OP_DOTS))
		return true;
	if (token.IsSimple(OP_COLON2) || token.kind == PA6_IDENTIFIER_TOKEN)
		return QualifiedTypeNameStartsAt(at);
	return IsDeclSpecifierStart(at);
}

bool Pa7Parser::IsDeclSpecifierStart(size_t at) const
{
	const ETokenType type = Token(at).simple_type;
	return IsCVSpecifier(type) || type == KW_TYPEDEF ||
		type == KW_CONSTEXPR || type == KW_INLINE ||
		IsStorageSpecifier(type) || IsFundamentalSpecifier(type);
}

bool Pa7Parser::QualifiedTypeNameStartsAt(size_t at) const
{
	NamePath path;
	if (Token(at).IsSimple(OP_COLON2))
	{
		path.absolute = true;
		++at;
	}
	if (Token(at).kind != PA6_IDENTIFIER_TOKEN)
		return false;
	path.parts.push_back(Token(at).spelling);
	while (Token(at + 1).IsSimple(OP_COLON2) &&
		Token(at + 2).kind == PA6_IDENTIFIER_TOKEN)
	{
		path.parts.push_back(Token(at + 2).spelling);
		at += 2;
	}
	return LookupTypedef(path) != 0;
}

bool Pa7Parser::IsDeclaratorStart(size_t at) const
{
	const Pa6Token& token = Token(at);
	return token.kind == PA6_IDENTIFIER_TOKEN ||
		token.IsSimple(OP_COLON2) || token.IsSimple(OP_STAR) ||
		token.IsSimple(OP_AMP) || token.IsSimple(OP_LAND) ||
		token.IsSimple(OP_LPAREN) || token.IsSimple(OP_LSQUARE);
}

bool Pa7Parser::TryRedundantParentheses(const Pa7TypePtr& base,
	DeclaratorMode mode, Declarator& result)
{
	if (mode == DECL_ABSTRACT || !IsSimple(OP_LPAREN))
		return false;
	size_t cursor = pos_;
	size_t opens = 0;
	while (IsSimple(OP_LPAREN, cursor))
	{
		++opens;
		++cursor;
	}
	if (opens == 0 || !IsKind(PA6_IDENTIFIER_TOKEN, cursor))
		return false;
	NamePath candidate;
	candidate.parts.push_back(Token(cursor).spelling);
	if (LookupTypedef(candidate))
		return false;
	++cursor;
	for (size_t i = 0; i < opens; ++i)
	{
		if (!IsSimple(OP_RPAREN, cursor))
			return false;
		++cursor;
	}
	if (IsSimple(OP_LPAREN, cursor) || IsSimple(OP_LSQUARE, cursor))
		return false;
	while (pos_ < cursor)
		++pos_;
	result.type = base;
	result.has_name = true;
	result.path = candidate;
	return true;
}

Pa8ExprPtr Pa7Parser::MakeExpression(Pa8ExprKind kind)
{
	Pa8ExprPtr result(new Pa8Expr);
	result->kind = kind;
	result->lookup_scope = scopes_.back();
	result->token_index = pos_;
	return result;
}

Pa8ExprPtr Pa7Parser::ParsePrimaryExpression()
{
	Pa6Token literal(PA6_EOF_TOKEN, "");
	if (ConsumeLiteral(&literal))
	{
		Pa8ExprPtr result = MakeExpression(PA8_EXPR_LITERAL);
		result->literal = literal;
		return result;
	}
	if (IsSimple(KW_TRUE) || IsSimple(KW_FALSE) ||
		IsSimple(KW_NULLPTR))
	{
		const Pa6Token token = Token(pos_);
		++pos_;
		Pa8ExprPtr result = MakeExpression(PA8_EXPR_LITERAL);
		result->literal = token;
		return result;
	}
	if (IsKind(PA6_IDENTIFIER_TOKEN) || IsSimple(OP_COLON2))
	{
		NamePath path = ParseNamePath();
		Pa8ExprPtr result = MakeExpression(PA8_EXPR_IDENTIFIER);
		result->absolute = path.absolute;
		result->path = path.parts;
		return result;
	}
	if (ConsumeSimple(OP_LPAREN))
	{
		Pa8ExprPtr result = ParseExpression();
		ExpectSimple(OP_RPAREN);
		return result;
	}
	throw runtime_error("missing expression operand");
}

Pa8ExprPtr Pa7Parser::ParseUnaryExpression()
{
	if (IsSimple(OP_PLUS) || IsSimple(OP_MINUS) || IsSimple(OP_LNOT) ||
		IsSimple(OP_COMPL) || IsSimple(OP_AMP) || IsSimple(OP_STAR))
	{
		const ETokenType op = Token(pos_).simple_type;
		++pos_;
		Pa8ExprPtr result = MakeExpression(PA8_EXPR_UNARY);
		result->op = op;
		result->left = ParseUnaryExpression();
		return result;
	}
	return ParsePrimaryExpression();
}

int Pa7Parser::BinaryPrecedence(ETokenType type) const
{
	switch (type)
	{
	case OP_LOR: return 1;
	case OP_LAND: return 2;
	case OP_BOR: return 3;
	case OP_XOR: return 4;
	case OP_AMP: return 5;
	case OP_EQ:
	case OP_NE: return 6;
	case OP_LT:
	case OP_GT:
	case OP_LE:
	case OP_GE: return 7;
	case OP_LSHIFT:
	case OP_RSHIFT: return 8;
	case OP_PLUS:
	case OP_MINUS: return 9;
	case OP_STAR:
	case OP_DIV:
	case OP_MOD: return 10;
	default: return 0;
	}
}

Pa8ExprPtr Pa7Parser::ParseExpression(unsigned minimum_precedence)
{
	Pa8ExprPtr left = ParseUnaryExpression();
	for (;;)
	{
		const int precedence = BinaryPrecedence(Token(pos_).simple_type);
		if (precedence < static_cast<int>(minimum_precedence))
			break;
		const ETokenType op = Token(pos_).simple_type;
		++pos_;
		Pa8ExprPtr right = ParseExpression(static_cast<unsigned>(precedence + 1));
		Pa8ExprPtr combined = MakeExpression(PA8_EXPR_BINARY);
		combined->op = op;
		combined->left = left;
		combined->right = right;
		left = combined;
	}
	if (minimum_precedence == 1 && ConsumeSimple(OP_QMARK))
	{
		Pa8ExprPtr when_true = ParseExpression();
		ExpectSimple(OP_COLON);
		Pa8ExprPtr when_false = ParseExpression();
		Pa8ExprPtr result = MakeExpression(PA8_EXPR_CONDITIONAL);
		result->left = left;
		result->right = when_true;
		result->third = when_false;
		left = result;
	}
	return left;
}

bool Pa7Parser::IsExpressionStart(size_t at) const
{
	const Pa6Token& token = Token(at);
	if (token.kind == PA6_LITERAL_TOKEN ||
		token.kind == PA6_IDENTIFIER_TOKEN)
		return true;
	return token.IsSimple(OP_COLON2) || token.IsSimple(OP_LPAREN) ||
		token.IsSimple(KW_TRUE) || token.IsSimple(KW_FALSE) ||
		token.IsSimple(KW_NULLPTR) || token.IsSimple(OP_PLUS) ||
		token.IsSimple(OP_MINUS) || token.IsSimple(OP_LNOT) ||
		token.IsSimple(OP_COMPL) || token.IsSimple(OP_AMP) ||
		token.IsSimple(OP_STAR);
}

void Pa7Parser::ParseStaticAssertDeclaration()
{
	ExpectSimple(KW_STATIC_ASSERT);
	ExpectSimple(OP_LPAREN);
	Pa8ExprPtr condition = ParseExpression();
	SetExpressionScope(condition, scopes_.back());
	if (ConsumeSimple(OP_COMMA))
	{
		Pa6Token message(PA6_EOF_TOKEN, "");
		if (!ConsumeLiteral(&message) || message.lit_scalar)
			throw runtime_error("static_assert message is not a string literal");
	}
	ExpectSimple(OP_RPAREN);
	ExpectEndOfDeclaration();
	global_->static_assertions.push_back(condition);
}

void Pa7Parser::ParseFunctionBody()
{
	ExpectSimple(OP_LBRACE);
	if (!ConsumeSimple(OP_RBRACE))
		throw runtime_error("only an empty function body is supported");
}

Pa7DeclAttributes Pa7Parser::MakeAttributes(const DeclSpec& spec,
	bool defined, size_t order) const
{
	Pa7DeclAttributes result;
	result.storage = spec.storage;
	result.is_const = spec.is_const || spec.is_constexpr;
	result.is_constexpr = spec.is_constexpr;
	result.is_inline = spec.is_inline;
	result.defined = defined;
	result.order = order;
	result.linkage_explicit =
		(spec.storage & (PA7_STORAGE_STATIC | PA7_STORAGE_EXTERN)) != 0;
	if (spec.storage & PA7_STORAGE_STATIC)
		result.linkage = PA7_LINKAGE_INTERNAL;
	else if (spec.storage & PA7_STORAGE_EXTERN)
		result.linkage = PA7_LINKAGE_EXTERNAL;
	return result;
}

void Pa7Parser::SetExpressionScope(const Pa8ExprPtr& expression,
	Pa7Namespace* scope) const
{
	if (!expression)
		return;
	expression->lookup_scope = scope;
	SetExpressionScope(expression->left, scope);
	SetExpressionScope(expression->right, scope);
	SetExpressionScope(expression->third, scope);
}

void Pa7Parser::SetTypeExpressionScope(const Pa7TypePtr& type,
	Pa7Namespace* scope) const
{
	if (!type)
		return;
	SetExpressionScope(type->bound_expression, scope);
	for (size_t i = 0; i < type->children.size(); ++i)
		SetTypeExpressionScope(type->children[i], scope);
	SetTypeExpressionScope(type->return_type, scope);
}
