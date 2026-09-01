#pragma once

#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <vector>

// IPPTokenStream predates the rest of the namespaced interfaces and uses the
// unqualified std::string name.  Make that name available before including
// the shared interface, without changing the PA2 boundary itself.
using std::string;

#include "IPPTokenStream.h"
#include "posttoken_stream.h"

struct CtrlExprToken
{
	enum Kind
	{
		KIND_LITERAL,
		KIND_IDENTIFIER,
		KIND_OPERATOR,
		KIND_BAD
	};

	Kind kind;
	bool is_unsigned;
	std::uint64_t value;
	std::string text;
	ETokenType op;

	CtrlExprToken();

	static CtrlExprToken MakeLiteral(bool is_unsigned, std::uint64_t value);
	static CtrlExprToken MakeIdentifier(const std::string& text);
	static CtrlExprToken MakeOperator(ETokenType op);
	static CtrlExprToken MakeBad();
};

typedef std::function<bool(const std::string&)> CtrlExprIsDefined;

struct CtrlExprTokenCollector : IPostTokenOutputStream
{
	explicit CtrlExprTokenCollector(std::vector<CtrlExprToken>& tokens);

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
	std::vector<CtrlExprToken>& tokens_;
};

struct EvalResult
{
	bool is_unsigned;
	std::uint64_t value;
	bool error;

	EvalResult();
	EvalResult(bool is_unsigned, std::uint64_t value, bool error);
};

EvalResult EvaluateControllingExpression(
	const std::vector<CtrlExprToken>& tokens,
	const CtrlExprIsDefined& is_defined);

struct CtrlExprLineSplitter : IPPTokenStream
{
	CtrlExprLineSplitter(std::ostream& output,
		const CtrlExprIsDefined& is_defined);

	void emit_whitespace_sequence() override;
	void emit_new_line() override;
	void emit_header_name(const std::string& data) override;
	void emit_identifier(const std::string& data) override;
	void emit_pp_number(const std::string& data) override;
	void emit_character_literal(const std::string& data) override;
	void emit_user_defined_character_literal(const std::string& data) override;
	void emit_string_literal(const std::string& data) override;
	void emit_user_defined_string_literal(const std::string& data) override;
	void emit_preprocessing_op_or_punc(const std::string& data) override;
	void emit_non_whitespace_char(const std::string& data) override;
	void emit_eof() override;

private:
	enum RawEventKind
	{
		RAW_WHITESPACE,
		RAW_HEADER_NAME,
		RAW_IDENTIFIER,
		RAW_PP_NUMBER,
		RAW_CHARACTER_LITERAL,
		RAW_USER_DEFINED_CHARACTER_LITERAL,
		RAW_STRING_LITERAL,
		RAW_USER_DEFINED_STRING_LITERAL,
		RAW_PREPROCESSING_OP_OR_PUNC,
		RAW_NON_WHITESPACE_CHAR
	};

	struct RawEvent
	{
		RawEventKind kind;
		std::string data;

		RawEvent(RawEventKind kind, const std::string& data)
			: kind(kind), data(data) {}
	};

	void append(RawEventKind kind, const std::string& data);
	void flush_line();

	std::ostream& output_;
	CtrlExprIsDefined is_defined_;
	std::vector<RawEvent> line_;
};
