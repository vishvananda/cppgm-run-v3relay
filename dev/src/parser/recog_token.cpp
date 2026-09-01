#include "recog_token.h"

using namespace std;

namespace
{

unsigned TokenFlags(Pa6TokenKind kind, const string& spelling)
{
	if (kind == PA6_IDENTIFIER_TOKEN)
	{
		unsigned flags = NameCategoryMask(spelling);
		if (spelling == "final")
			flags |= PA6_FINAL_FLAG;
		if (spelling == "override")
			flags |= PA6_OVERRIDE_FLAG;
		return flags;
	}
	if (kind == PA6_LITERAL_TOKEN)
	{
		unsigned flags = 0;
		if (spelling == "\"\"")
			flags |= PA6_EMPTY_STRING_FLAG;
		if (spelling == "0")
			flags |= PA6_ZERO_FLAG;
		return flags;
	}
	return 0;
}

} // namespace

Pa6Token::Pa6Token(Pa6TokenKind token_kind, const string& token_spelling,
	ETokenType token_type)
	: kind(token_kind), simple_type(token_type), spelling(token_spelling),
		flags(TokenFlags(token_kind, token_spelling))
{
}

bool Pa6Token::IsSimple(ETokenType type) const
{
	return kind == PA6_SIMPLE_TOKEN && simple_type == type;
}

bool Pa6Token::IsIdentifier() const
{
	return kind == PA6_IDENTIFIER_TOKEN;
}

bool Pa6Token::IsLiteral() const
{
	return kind == PA6_LITERAL_TOKEN;
}

bool Pa6Token::IsRshiftPart() const
{
	return kind == PA6_RSHIFT_1_TOKEN || kind == PA6_RSHIFT_2_TOKEN;
}

void Pa6TokenCollector::emit_invalid(const string& source)
{
	throw Pa6LexError("invalid posttoken: " + source);
}

void Pa6TokenCollector::emit_simple(const string& source,
	ETokenType token_type)
{
	if (token_type == OP_RSHIFT)
	{
		tokens.push_back(Pa6Token(PA6_RSHIFT_1_TOKEN, ">", OP_GT));
		tokens.push_back(Pa6Token(PA6_RSHIFT_2_TOKEN, ">", OP_GT));
		return;
	}
	tokens.push_back(Pa6Token(PA6_SIMPLE_TOKEN, source, token_type));
}

void Pa6TokenCollector::emit_identifier(const string& source)
{
	tokens.push_back(Pa6Token(PA6_IDENTIFIER_TOKEN, source));
}

void Pa6TokenCollector::append_literal(const string& source)
{
	tokens.push_back(Pa6Token(PA6_LITERAL_TOKEN, source));
}

void Pa6TokenCollector::emit_literal(const string& source,
	EFundamentalType, const void*, size_t)
{
	append_literal(source);
}

void Pa6TokenCollector::emit_literal_array(const string& source,
	size_t, EFundamentalType, const void*, size_t)
{
	append_literal(source);
}

void Pa6TokenCollector::emit_user_defined_literal_character(
	const string& source, const string&, EFundamentalType, const void*, size_t)
{
	append_literal(source);
}

void Pa6TokenCollector::emit_user_defined_literal_string_array(
	const string& source, const string&, size_t, EFundamentalType,
	const void*, size_t)
{
	append_literal(source);
}

void Pa6TokenCollector::emit_user_defined_literal_integer(
	const string& source, const string&, const string&)
{
	append_literal(source);
}

void Pa6TokenCollector::emit_user_defined_literal_floating(
	const string& source, const string&, const string&)
{
	append_literal(source);
}

void Pa6TokenCollector::emit_eof()
{
	tokens.push_back(Pa6Token(PA6_EOF_TOKEN, "", KW_AUTO));
}

unsigned NameCategoryMask(const string& spelling)
{
	unsigned mask = 0;
	if (spelling.find('C') != string::npos)
		mask |= PA6_NAME_CLASS_FLAG;
	if (spelling.find('T') != string::npos)
		mask |= PA6_NAME_TEMPLATE_FLAG;
	if (spelling.find('Y') != string::npos)
		mask |= PA6_NAME_TYPEDEF_FLAG;
	if (spelling.find('E') != string::npos)
		mask |= PA6_NAME_ENUM_FLAG;
	if (spelling.find('N') != string::npos)
		mask |= PA6_NAME_NAMESPACE_FLAG;
	return mask;
}
