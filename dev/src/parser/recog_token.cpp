#include "recog_token.h"

#include <cctype>
#include <cstring>
#include <limits>

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
		flags(TokenFlags(token_kind, token_spelling)), lit_scalar(false),
		lit_type(FT_INT), lit_count(0), lit_value(0), pack_alignment(0)
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

Pa6TokenCollector::Pa6TokenCollector()
	: active_pack_alignment_(0), pack_stack_()
{
}

void Pa6TokenCollector::emit_invalid(const string& source)
{
	throw Pa6LexError("invalid posttoken: " + source);
}

void Pa6TokenCollector::append_token(const Pa6Token& token)
{
	Pa6Token stamped = token;
	stamped.pack_alignment = active_pack_alignment_;
	tokens.push_back(stamped);
}

void Pa6TokenCollector::emit_pragma(const string& text)
{
	string compact;
	for (size_t i = 0; i < text.size(); ++i)
		if (!isspace(static_cast<unsigned char>(text[i])))
			compact += text[i];
	if (compact.size() < 6 || compact.compare(0, 5, "pack(") != 0 ||
		compact[compact.size() - 1] != ')')
		return;

	const string arguments = compact.substr(5, compact.size() - 6);
	vector<string> parts;
	string current;
	for (size_t i = 0; i <= arguments.size(); ++i)
	{
		if (i == arguments.size() || arguments[i] == ',')
		{
			parts.push_back(current);
			current.clear();
		}
		else
			current += arguments[i];
	}

	const auto parse_alignment = [](const string& value,
		 size_t& alignment) -> bool
	{
		if (value.empty())
			return false;
		const size_t maximum = numeric_limits<size_t>::max();
		size_t parsed = 0;
		for (size_t i = 0; i < value.size(); ++i)
		{
			if (value[i] < '0' || value[i] > '9')
				return false;
			const size_t digit = static_cast<size_t>(value[i] - '0');
			if (parsed > (maximum - digit) / 10)
				return false;
			parsed = parsed * 10 + digit;
		}
		if (parsed == 0)
			return false;
		alignment = parsed;
		return true;
	};

	if (parts.size() == 1 && parts[0].empty())
	{
		active_pack_alignment_ = 0;
		return;
	}
	if (parts.size() == 1 && parts[0] == "pop")
	{
		if (!pack_stack_.empty())
		{
			active_pack_alignment_ = pack_stack_.back();
			pack_stack_.pop_back();
		}
		return;
	}
	if (parts.size() >= 1 && parts[0] == "push")
	{
		pack_stack_.push_back(active_pack_alignment_);
		if (parts.size() == 2)
		{
			size_t alignment = 0;
			if (parse_alignment(parts[1], alignment))
				active_pack_alignment_ = alignment;
		}
		return;
	}
	if (parts.size() == 1)
	{
		size_t alignment = 0;
		if (parse_alignment(parts[0], alignment))
			active_pack_alignment_ = alignment;
	}
}

void Pa6TokenCollector::emit_simple(const string& source,
	ETokenType token_type)
{
	if (token_type == OP_RSHIFT)
	{
		append_token(Pa6Token(PA6_RSHIFT_1_TOKEN, ">", OP_GT));
		append_token(Pa6Token(PA6_RSHIFT_2_TOKEN, ">", OP_GT));
		return;
	}
	append_token(Pa6Token(PA6_SIMPLE_TOKEN, source, token_type));
}

void Pa6TokenCollector::emit_identifier(const string& source)
{
	append_token(Pa6Token(PA6_IDENTIFIER_TOKEN, source));
}

void Pa6TokenCollector::append_literal(const string& source)
{
	append_token(Pa6Token(PA6_LITERAL_TOKEN, source));
}

void Pa6TokenCollector::emit_literal(const string& source,
	EFundamentalType type, const void* data, size_t nbytes)
{
	Pa6Token token(PA6_LITERAL_TOKEN, source);
	token.lit_scalar = true;
	token.lit_type = type;
	const size_t width = nbytes < sizeof(token.lit_value) ? nbytes :
		sizeof(token.lit_value);
	if (data != 0 && width != 0)
		memcpy(&token.lit_value, data, width);
	append_token(token);
}

void Pa6TokenCollector::emit_literal_array(const string& source,
	size_t num_elements, EFundamentalType type, const void* data, size_t nbytes)
{
	Pa6Token token(PA6_LITERAL_TOKEN, source);
	token.lit_type = type;
	token.lit_count = num_elements;
	if (data != 0 && nbytes != 0)
	{
		const unsigned char* bytes = static_cast<const unsigned char*>(data);
		token.lit_bytes.assign(bytes, bytes + nbytes);
	}
	append_token(token);
}

void Pa6TokenCollector::emit_user_defined_literal_character(
	const string& source, const string&, EFundamentalType, const void*, size_t)
{
	append_literal(source);
}

void Pa6TokenCollector::emit_user_defined_literal_string_array(
	const string& source, const string&, size_t num_elements,
	EFundamentalType type, const void* data, size_t nbytes)
{
	Pa6Token token(PA6_LITERAL_TOKEN, source);
	token.lit_type = type;
	token.lit_count = num_elements;
	if (data != 0 && nbytes != 0)
	{
		const unsigned char* bytes = static_cast<const unsigned char*>(data);
		token.lit_bytes.assign(bytes, bytes + nbytes);
	}
	append_token(token);
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
	append_token(Pa6Token(PA6_EOF_TOKEN, "", KW_AUTO));
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
