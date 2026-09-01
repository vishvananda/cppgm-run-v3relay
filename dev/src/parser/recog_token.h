#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

using std::string;

#include "IPPTokenStream.h"
#include "posttoken_stream.h"

// The recognizer deliberately keeps the post-token spelling alongside the
// token category.  The grammar uses the category for terminals and the
// spelling for the PA6 mock name lookup.
enum Pa6TokenKind
{
	PA6_SIMPLE_TOKEN,
	PA6_IDENTIFIER_TOKEN,
	PA6_LITERAL_TOKEN,
	PA6_RSHIFT_1_TOKEN,
	PA6_RSHIFT_2_TOKEN,
	PA6_EOF_TOKEN
};

enum Pa6TokenFlags
{
	PA6_NAME_CLASS_FLAG = 1u << 0,
	PA6_NAME_TEMPLATE_FLAG = 1u << 1,
	PA6_NAME_TYPEDEF_FLAG = 1u << 2,
	PA6_NAME_ENUM_FLAG = 1u << 3,
	PA6_NAME_NAMESPACE_FLAG = 1u << 4,
	PA6_FINAL_FLAG = 1u << 5,
	PA6_OVERRIDE_FLAG = 1u << 6,
	PA6_EMPTY_STRING_FLAG = 1u << 7,
	PA6_ZERO_FLAG = 1u << 8
};

struct Pa6Token
{
	Pa6TokenKind kind;
	ETokenType simple_type;
	std::string spelling;
	unsigned flags;

	Pa6Token(Pa6TokenKind kind, const std::string& spelling,
		ETokenType simple_type = KW_AUTO);

	bool IsSimple(ETokenType type) const;
	bool IsIdentifier() const;
	bool IsLiteral() const;
	bool IsRshiftPart() const;
};

class Pa6LexError : public std::runtime_error
{
public:
	explicit Pa6LexError(const std::string& message)
		: std::runtime_error(message)
	{
	}
};

class Pa6TokenCollector : public IPostTokenOutputStream
{
public:
	std::vector<Pa6Token> tokens;

	void emit_invalid(const std::string& source) override;
	void emit_simple(const std::string& source, ETokenType token_type) override;
	void emit_identifier(const std::string& source) override;
	void emit_literal(const std::string& source, EFundamentalType type,
		const void* data, std::size_t nbytes) override;
	void emit_literal_array(const std::string& source,
		std::size_t num_elements, EFundamentalType type, const void* data,
		std::size_t nbytes) override;
	void emit_user_defined_literal_character(const std::string& source,
		const std::string& ud_suffix, EFundamentalType type, const void* data,
		std::size_t nbytes) override;
	void emit_user_defined_literal_string_array(
		const std::string& source, const std::string& ud_suffix,
		std::size_t num_elements, EFundamentalType type, const void* data,
		std::size_t nbytes) override;
	void emit_user_defined_literal_integer(const std::string& source,
		const std::string& ud_suffix, const std::string& prefix) override;
	void emit_user_defined_literal_floating(const std::string& source,
		const std::string& ud_suffix, const std::string& prefix) override;
	void emit_eof() override;

private:
	void append_literal(const std::string& source);
};

unsigned NameCategoryMask(const std::string& spelling);
