#include "nsdecl_parser.h"

#include <stdexcept>

using namespace std;

namespace
{

bool IsStorageSpecifier(ETokenType type)
{
	switch (type)
	{
	case KW_EXTERN:
	case KW_STATIC:
	case KW_THREAD_LOCAL:
	case KW_REGISTER:
	case KW_MUTABLE:
	case KW_INLINE:
	case KW_VIRTUAL:
	case KW_EXPLICIT:
	case KW_FRIEND:
	case KW_CONSTEXPR:
		return true;
	default:
		return false;
	}
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
	case KW_AUTO:
		return true;
	default:
		return false;
	}
}

bool IsDeclarationBoundary(const Pa6Token& token)
{
	return token.IsSimple(OP_SEMICOLON) || token.IsSimple(OP_COMMA) ||
		token.IsSimple(OP_RPAREN) || token.IsSimple(OP_RSQUARE) ||
		token.IsSimple(OP_DOTS) || token.IsSimple(OP_ASS) ||
		token.kind == PA6_EOF_TOKEN;
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

Pa7Parser::Pa7Parser(const vector<Pa6Token>& tokens, Pa7Namespace* global)
	: tokens_(tokens), pos_(0), global_(global)
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
	ConsumeSimple(OP_SEMICOLON);
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

	string first;
	ExpectIdentifier(&first);
	if (ConsumeSimple(OP_ASS))
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

	NamePath path;
	path.parts.push_back(first);
	while (ConsumeSimple(OP_COLON2))
	{
		string part;
		ExpectIdentifier(&part);
		path.parts.push_back(part);
	}
	ExpectEndOfDeclaration();
	if (path.parts.size() < 2)
		throw runtime_error("unqualified using-declaration");

	vector<string> namespace_parts(path.parts.begin(), path.parts.end() - 1);
	Pa7Namespace* target = ResolveRelativeNamespace(scopes_.back(),
		namespace_parts, path.absolute);
	if (!target)
		throw runtime_error("using-declaration target namespace not found");
	const Pa7Decl* source = target->FindDirect(path.parts.back(),
		PA7_FIND_ANY);
	if (!source)
		throw runtime_error("using-declaration target not found");
	map<string, Pa7Decl>::iterator existing =
		scopes_.back()->declarations.find(path.parts.back());
	if (existing == scopes_.back()->declarations.end())
		scopes_.back()->declarations[path.parts.back()] = *source;
}

void Pa7Parser::ParseSimpleDeclaration()
{
	DeclSpec spec = ParseDeclSpecifierSeq();
	if (IsSimple(OP_SEMICOLON))
	{
		ExpectEndOfDeclaration();
		return;
	}

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
			destination = ResolveRelativeNamespace(scopes_.back(),
				namespace_parts, declarator.path.absolute);
			if (!destination)
				throw runtime_error("qualified declarator namespace not found");
		}

		const string& name = declarator.path.parts.back();
		if (spec.is_typedef)
			destination->AddTypedef(name, declarator.type);
		else if (IsFunction(declarator.type))
			destination->AddOrMergeFunction(name, declarator.type);
		else
			destination->AddOrMergeVariable(name, declarator.type);

		if (ConsumeSimple(OP_ASS))
		{
			while (!IsDeclarationBoundary(Token(pos_)))
				++pos_;
		}
		if (!ConsumeSimple(OP_COMMA))
			break;
	}
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
		if (IsStorageSpecifier(type))
		{
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
		case KW_AUTO: fundamental.have_int = true; break;
		default: break;
		}
	}

	if (!consumed || (!fundamental.HasType() && !named_type))
		throw runtime_error("missing declaration specifier");
	result.type = named_type ? named_type : fundamental.Finish();
	if (cv != PA7_CV_NONE)
		result.type = ApplyCV(cv, result.type);
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
	Pa7TypePtr working = base;
	bool have_ptr_operator = false;
	while (IsSimple(OP_STAR) || IsSimple(OP_AMP) || IsSimple(OP_LAND))
	{
		have_ptr_operator = true;
		if (ConsumeSimple(OP_STAR))
		{
			unsigned cv = PA7_CV_NONE;
			while (IsSimple(KW_CONST) || IsSimple(KW_VOLATILE))
			{
				if (ConsumeSimple(KW_CONST))
					cv |= PA7_CV_CONST;
				else
				{
					ExpectSimple(KW_VOLATILE);
					cv |= PA7_CV_VOLATILE;
				}
			}
			working = MakePointer(working);
			working = ApplyCV(cv, working);
		}
		else if (ConsumeSimple(OP_AMP))
			working = MakeReference(false, working);
		else
		{
			ExpectSimple(OP_LAND);
			working = MakeReference(true, working);
		}
	}

	Declarator result;
	bool grouped = false;
	if (IsKind(PA6_IDENTIFIER_TOKEN) || IsSimple(OP_COLON2))
	{
		result.path = ParseNamePath();
		result.has_name = true;
	}
	else if (IsSimple(OP_LPAREN) && !IsParameterStart(pos_ + 1))
	{
		ExpectSimple(OP_LPAREN);
		Declarator nested = ParsePtrDeclarator(working, mode);
		ExpectSimple(OP_RPAREN);
		result = nested;
		working = nested.type;
		grouped = true;
	}
	else if (IsSimple(OP_LPAREN) || IsSimple(OP_LSQUARE))
	{
		// An abstract declarator can begin with a suffix.  A parenthesis
		// whose contents can start a parameter-declaration-clause is the
		// function suffix form required by 8.2p7.
	}
	else if (have_ptr_operator)
	{
		// `(*)` and `(&)` use the pointer operator as the abstract root.
	}
	else
		throw runtime_error("missing declarator root");

	bool have_suffix = false;
	while (IsSimple(OP_LPAREN) || IsSimple(OP_LSQUARE))
	{
		if (IsSimple(OP_LPAREN))
		{
			Parameters parameters = ParseParametersAndQualifiers();
			if (grouped || have_suffix)
				working = InsertFunctionSuffix(working, parameters);
			else
				working = MakeFunction(parameters.types, parameters.varargs,
					working);
		}
		else
		{
			Pa6Token literal(PA6_EOF_TOKEN, "");
			ExpectSimple(OP_LSQUARE);
			bool has_bound = !IsSimple(OP_RSQUARE);
			unsigned long long bound = 0;
			if (has_bound)
			{
				if (!ConsumeLiteral(&literal) || !literal.lit_scalar)
					throw runtime_error("array bound is not a scalar literal");
				bound = literal.lit_value;
			}
			ExpectSimple(OP_RSQUARE);
			if (grouped || have_suffix)
				working = InsertArraySuffix(working, has_bound, bound);
			else
				working = MakeArray(has_bound, bound, working);
		}
		have_suffix = true;
	}
	result.type = working;
	if (mode == DECL_NAMED && !result.has_name)
		throw runtime_error("named declarator is abstract");
	return result;
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

Pa7TypePtr Pa7Parser::InsertFunctionSuffix(const Pa7TypePtr& type,
	const Parameters& parameters)
{
	if (!type)
		return MakeFunction(parameters.types, parameters.varargs, type);
	switch (type->kind)
	{
	case PA7_TYPE_CV:
		if (type->children.size() == 1)
			return ApplyCV(type->cv, InsertFunctionSuffix(type->children[0],
				parameters));
		break;
	case PA7_TYPE_POINTER:
		if (type->children.size() == 1)
			return MakePointer(InsertFunctionSuffix(type->children[0],
				parameters));
		break;
	case PA7_TYPE_ARRAY:
		if (type->children.size() == 1)
			return MakeArray(type->has_bound, type->bound,
				InsertFunctionSuffix(type->children[0], parameters));
		break;
	case PA7_TYPE_FUNCTION:
		return MakeFunction(type->children, type->varargs,
			InsertFunctionSuffix(type->return_type, parameters));
	case PA7_TYPE_LVALUE_REFERENCE:
	case PA7_TYPE_RVALUE_REFERENCE:
		break;
	case PA7_TYPE_FUNDAMENTAL:
		break;
	}
	return MakeFunction(parameters.types, parameters.varargs, type);
}

Pa7TypePtr Pa7Parser::InsertArraySuffix(const Pa7TypePtr& type,
	bool has_bound, unsigned long long bound)
{
	if (!type)
		return MakeArray(has_bound, bound, type);
	switch (type->kind)
	{
	case PA7_TYPE_CV:
		if (type->children.size() == 1)
			return ApplyCV(type->cv, InsertArraySuffix(type->children[0],
				has_bound, bound));
		break;
	case PA7_TYPE_POINTER:
		if (type->children.size() == 1)
			return MakePointer(InsertArraySuffix(type->children[0],
				has_bound, bound));
		break;
	case PA7_TYPE_ARRAY:
		if (type->children.size() == 1)
			return MakeArray(type->has_bound, type->bound,
				InsertArraySuffix(type->children[0], has_bound, bound));
		break;
	case PA7_TYPE_FUNCTION:
		return MakeFunction(type->children, type->varargs,
			InsertArraySuffix(type->return_type, has_bound, bound));
	case PA7_TYPE_LVALUE_REFERENCE:
	case PA7_TYPE_RVALUE_REFERENCE:
	case PA7_TYPE_FUNDAMENTAL:
		break;
	}
	return MakeArray(has_bound, bound, type);
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
	Pa7Decl* decl = scope->FindDirect(path.parts.back(), PA7_FIND_TYPE);
	return decl && decl->typedef_entity ? decl->typedef_entity->type :
		Pa7TypePtr();
}

Pa7Namespace* Pa7Parser::ResolveNamespace(const NamePath& path) const
{
	return ResolveRelativeNamespace(scopes_.back(), path.parts,
		path.absolute);
}

Pa7Decl* Pa7Parser::LookupUnqualified(const string& name,
	unsigned filter) const
{
	for (Pa7Namespace* scope = scopes_.back(); scope;
		scope = scope->parent)
	{
		Pa7Decl* direct = const_cast<Pa7Decl*>(
			static_cast<const Pa7Namespace*>(scope)->FindDirect(name, filter));
		if (direct)
			return direct;
		vector<const Pa7Namespace*> visited;
		Pa7Decl* imported = LookupInUsing(scope, name, filter, visited);
		if (imported)
			return imported;
	}
	return 0;
}

Pa7Decl* Pa7Parser::LookupInUsing(const Pa7Namespace* scope,
	const string& name, unsigned filter,
	vector<const Pa7Namespace*>& visited) const
{
	for (size_t i = 0; i < visited.size(); ++i)
		if (visited[i] == scope)
			return 0;
	visited.push_back(scope);
	for (size_t i = 0; i < scope->using_directives.size(); ++i)
	{
		Pa7Namespace* nominated = scope->using_directives[i];
		if (!nominated)
			continue;
		Pa7Decl* direct = const_cast<Pa7Decl*>(
			static_cast<const Pa7Namespace*>(nominated)->FindDirect(name,
				filter));
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
			Pa7Decl* decl = const_cast<Pa7Decl*>(
				static_cast<const Pa7Namespace*>(candidate)->FindDirect(
					parts[0], PA7_FIND_NAMESPACE));
			if (decl && decl->namespace_entity)
			{
				current = decl->namespace_entity.get();
				index = 1;
				break;
			}
		}
	}
	if (!current)
		return 0;
	for (; index < parts.size(); ++index)
	{
		Pa7Decl* decl = current->FindDirect(parts[index],
			PA7_FIND_NAMESPACE);
		if (!decl || !decl->namespace_entity)
			return 0;
		current = decl->namespace_entity.get();
	}
	return current;
}

bool Pa7Parser::IsParameterStart(size_t at) const
{
	const Pa6Token& token = Token(at);
	if (token.kind == PA6_EOF_TOKEN || token.IsSimple(OP_RPAREN) ||
		token.IsSimple(OP_DOTS) || token.IsSimple(OP_COLON2))
		return true;
	if (token.kind == PA6_IDENTIFIER_TOKEN)
	{
		NamePath path;
		path.parts.push_back(token.spelling);
		return LookupTypedef(path) != 0;
	}
	return IsDeclSpecifierStart(at);
}

bool Pa7Parser::IsDeclSpecifierStart(size_t at) const
{
	const Pa6Token& token = Token(at);
	if (token.kind == PA6_IDENTIFIER_TOKEN)
	{
		NamePath path;
		path.parts.push_back(token.spelling);
		return LookupTypedef(path) != 0;
	}
	return IsCVSpecifier(token.simple_type) ||
		token.simple_type == KW_TYPEDEF ||
		IsStorageSpecifier(token.simple_type) ||
		IsFundamentalSpecifier(token.simple_type);
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
