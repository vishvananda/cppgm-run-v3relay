#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <string>
#include <vector>

using namespace std;

#include "ctrlexpr_eval.h"

namespace
{

bool IsIntegralType(EFundamentalType type, bool& is_unsigned)
{
	switch (type)
	{
	case FT_BOOL:
	case FT_WCHAR_T:
	case FT_CHAR:
	case FT_SIGNED_CHAR:
	case FT_SHORT_INT:
	case FT_INT:
	case FT_LONG_INT:
	case FT_LONG_LONG_INT:
		is_unsigned = false;
		return true;
	case FT_UNSIGNED_CHAR:
	case FT_UNSIGNED_SHORT_INT:
	case FT_UNSIGNED_INT:
	case FT_UNSIGNED_LONG_INT:
	case FT_UNSIGNED_LONG_LONG_INT:
	case FT_CHAR16_T:
	case FT_CHAR32_T:
		is_unsigned = true;
		return true;
	default:
		return false;
	}
}

std::uint64_t ReadScalar(const void* data, std::size_t nbytes,
	bool is_unsigned)
{
	const unsigned char* bytes = static_cast<const unsigned char*>(data);
	std::uint64_t value = 0;
	for (std::size_t i = 0; i < nbytes; ++i)
		value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);

	if (!is_unsigned && nbytes < sizeof(std::uint64_t) && nbytes != 0 &&
		(value & (static_cast<std::uint64_t>(1) << (8 * nbytes - 1))) != 0)
		value |= (~static_cast<std::uint64_t>(0)) << (8 * nbytes);
	return value;
}

EvalResult ErrorResult()
{
	return EvalResult(false, 0, true);
}

class ControllingExpressionParser
{
public:
	ControllingExpressionParser(const vector<CtrlExprToken>& tokens,
		const CtrlExprIsDefined& is_defined)
		: tokens_(tokens), is_defined_(is_defined), position_(0) {}

	EvalResult Parse()
	{
		if (tokens_.empty())
			return ErrorResult();
		return ParseUnary();
	}

	std::size_t position() const
	{
		return position_;
	}

private:
	bool IsOperator(ETokenType op) const
	{
		return position_ < tokens_.size() &&
			tokens_[position_].kind == CtrlExprToken::KIND_OPERATOR &&
			tokens_[position_].op == op;
	}

	EvalResult ParseUnary()
	{
		if (position_ < tokens_.size() &&
			tokens_[position_].kind == CtrlExprToken::KIND_OPERATOR &&
			(tokens_[position_].op == OP_PLUS ||
			 tokens_[position_].op == OP_MINUS ||
			 tokens_[position_].op == OP_LNOT ||
			 tokens_[position_].op == OP_COMPL))
		{
			const ETokenType op = tokens_[position_].op;
			++position_;
			EvalResult operand = ParseUnary();
			switch (op)
			{
			case OP_PLUS:
				return operand;
			case OP_MINUS:
				operand.value = static_cast<std::uint64_t>(0) - operand.value;
				return operand;
			case OP_LNOT:
				operand.is_unsigned = false;
				operand.value = operand.value == 0 ? 1 : 0;
				return operand;
			case OP_COMPL:
				operand.value = ~operand.value;
				return operand;
			default:
				return ErrorResult();
			}
		}
		return ParsePrimary();
	}

	EvalResult ParsePrimary()
	{
		if (position_ >= tokens_.size())
			return ErrorResult();

		const CtrlExprToken& token = tokens_[position_];
		if (token.kind == CtrlExprToken::KIND_BAD)
		{
			++position_;
			return ErrorResult();
		}

		if (token.kind == CtrlExprToken::KIND_LITERAL)
		{
			++position_;
			return EvalResult(token.is_unsigned, token.value, false);
		}

		if (token.kind == CtrlExprToken::KIND_IDENTIFIER)
		{
			if (token.text == "defined")
				return ParseDefined();

			++position_;
			if (token.text == "true")
				return EvalResult(false, 1, false);
			if (token.text == "false")
				return EvalResult(false, 0, false);
			return EvalResult(false, 0, false);
		}

		if (IsOperator(OP_LPAREN))
		{
			++position_;
			EvalResult result = ParseUnary();
			if (!IsOperator(OP_RPAREN))
			{
				result.error = true;
				return result;
			}
			++position_;
			return result;
		}

		++position_;
		return ErrorResult();
	}

	EvalResult ParseDefined()
	{
		++position_; // defined
		if (position_ >= tokens_.size())
			return ErrorResult();

		if (tokens_[position_].kind == CtrlExprToken::KIND_IDENTIFIER)
		{
			const string name = tokens_[position_].text;
			++position_;
			return DefinedValue(name);
		}

		if (!IsOperator(OP_LPAREN))
			return ErrorResult();
		++position_;
		if (position_ >= tokens_.size() ||
			tokens_[position_].kind != CtrlExprToken::KIND_IDENTIFIER)
			return ErrorResult();

		const string name = tokens_[position_].text;
		++position_;
		if (!IsOperator(OP_RPAREN))
			return ErrorResult();
		++position_;
		return DefinedValue(name);
	}

	EvalResult DefinedValue(const string& name) const
	{
		const bool defined = is_defined_ ? is_defined_(name) : false;
		return EvalResult(false, defined ? 1 : 0, false);
	}

	const vector<CtrlExprToken>& tokens_;
	const CtrlExprIsDefined& is_defined_;
	std::size_t position_;
};

void PrintResult(ostream& output, const EvalResult& result)
{
	if (result.error)
	{
		output << "error\n";
		return;
	}

	if (result.is_unsigned)
		output << result.value << "u\n";
	else
		output << static_cast<std::int64_t>(result.value) << "\n";
}

} // namespace

CtrlExprToken::CtrlExprToken()
	: kind(KIND_BAD), is_unsigned(false), value(0), op(OP_COMMA)
{
}

CtrlExprToken CtrlExprToken::MakeLiteral(bool is_unsigned,
	std::uint64_t value)
{
	CtrlExprToken result;
	result.kind = KIND_LITERAL;
	result.is_unsigned = is_unsigned;
	result.value = value;
	return result;
}

CtrlExprToken CtrlExprToken::MakeIdentifier(const std::string& text)
{
	CtrlExprToken result;
	result.kind = KIND_IDENTIFIER;
	result.text = text;
	return result;
}

CtrlExprToken CtrlExprToken::MakeOperator(ETokenType op)
{
	CtrlExprToken result;
	result.kind = KIND_OPERATOR;
	result.op = op;
	return result;
}

CtrlExprToken CtrlExprToken::MakeBad()
{
	return CtrlExprToken();
}

CtrlExprTokenCollector::CtrlExprTokenCollector(vector<CtrlExprToken>& tokens)
	: tokens_(tokens)
{
}

void CtrlExprTokenCollector::emit_invalid(const string& source)
{
	(void)source;
	tokens_.push_back(CtrlExprToken::MakeBad());
}

void CtrlExprTokenCollector::emit_simple(const string& source,
	ETokenType token_type)
{
	if (token_type <= KW_WHILE)
		tokens_.push_back(CtrlExprToken::MakeIdentifier(source));
	else
		tokens_.push_back(CtrlExprToken::MakeOperator(token_type));
}

void CtrlExprTokenCollector::emit_identifier(const string& source)
{
	tokens_.push_back(CtrlExprToken::MakeIdentifier(source));
}

void CtrlExprTokenCollector::emit_literal(const string& source,
	EFundamentalType type, const void* data, size_t nbytes)
{
	(void)source;
	bool is_unsigned;
	if (!IsIntegralType(type, is_unsigned) || data == 0 || nbytes == 0 ||
		nbytes > sizeof(std::uint64_t))
	{
		tokens_.push_back(CtrlExprToken::MakeBad());
		return;
	}
	tokens_.push_back(CtrlExprToken::MakeLiteral(is_unsigned,
		ReadScalar(data, nbytes, is_unsigned)));
}

void CtrlExprTokenCollector::emit_literal_array(const string& source,
	size_t num_elements, EFundamentalType type, const void* data,
	size_t nbytes)
{
	(void)source;
	(void)num_elements;
	(void)type;
	(void)data;
	(void)nbytes;
	tokens_.push_back(CtrlExprToken::MakeBad());
}

void CtrlExprTokenCollector::emit_user_defined_literal_character(
	const string& source, const string& ud_suffix, EFundamentalType type,
	const void* data, size_t nbytes)
{
	(void)source;
	(void)ud_suffix;
	(void)type;
	(void)data;
	(void)nbytes;
	tokens_.push_back(CtrlExprToken::MakeBad());
}

void CtrlExprTokenCollector::emit_user_defined_literal_string_array(
	const string& source, const string& ud_suffix, size_t num_elements,
	EFundamentalType type, const void* data, size_t nbytes)
{
	(void)source;
	(void)ud_suffix;
	(void)num_elements;
	(void)type;
	(void)data;
	(void)nbytes;
	tokens_.push_back(CtrlExprToken::MakeBad());
}

void CtrlExprTokenCollector::emit_user_defined_literal_integer(
	const string& source, const string& ud_suffix, const string& prefix)
{
	(void)source;
	(void)ud_suffix;
	(void)prefix;
	tokens_.push_back(CtrlExprToken::MakeBad());
}

void CtrlExprTokenCollector::emit_user_defined_literal_floating(
	const string& source, const string& ud_suffix, const string& prefix)
{
	(void)source;
	(void)ud_suffix;
	(void)prefix;
	tokens_.push_back(CtrlExprToken::MakeBad());
}

void CtrlExprTokenCollector::emit_eof()
{
}

EvalResult::EvalResult()
	: is_unsigned(false), value(0), error(true)
{
}

EvalResult::EvalResult(bool is_unsigned, std::uint64_t value, bool error)
	: is_unsigned(is_unsigned), value(value), error(error)
{
}

EvalResult EvaluateControllingExpression(const vector<CtrlExprToken>& tokens,
	const CtrlExprIsDefined& is_defined)
{
	ControllingExpressionParser parser(tokens, is_defined);
	EvalResult result = parser.Parse();
	if (parser.position() != tokens.size())
		result.error = true;
	return result;
}

CtrlExprLineSplitter::CtrlExprLineSplitter(ostream& output,
	const CtrlExprIsDefined& is_defined)
	: output_(output), is_defined_(is_defined)
{
}

void CtrlExprLineSplitter::append(RawEventKind kind, const string& data)
{
	line_.push_back(RawEvent(kind, data));
}

void CtrlExprLineSplitter::flush_line()
{
	bool has_non_whitespace = false;
	for (size_t i = 0; i < line_.size(); ++i)
	{
		if (line_[i].kind != RAW_WHITESPACE)
		{
			has_non_whitespace = true;
			break;
		}
	}

	if (!has_non_whitespace)
	{
		line_.clear();
		return;
	}

	vector<CtrlExprToken> tokens;
	CtrlExprTokenCollector collector(tokens);
	PostTokenStream posttoken_output(collector);
	for (size_t i = 0; i < line_.size(); ++i)
	{
		const RawEvent& event = line_[i];
		switch (event.kind)
		{
		case RAW_WHITESPACE:
			posttoken_output.emit_whitespace_sequence();
			break;
		case RAW_HEADER_NAME:
			posttoken_output.emit_header_name(event.data);
			break;
		case RAW_IDENTIFIER:
			posttoken_output.emit_identifier(event.data);
			break;
		case RAW_PP_NUMBER:
			posttoken_output.emit_pp_number(event.data);
			break;
		case RAW_CHARACTER_LITERAL:
			posttoken_output.emit_character_literal(event.data);
			break;
		case RAW_USER_DEFINED_CHARACTER_LITERAL:
			posttoken_output.emit_user_defined_character_literal(event.data);
			break;
		case RAW_STRING_LITERAL:
			posttoken_output.emit_string_literal(event.data);
			break;
		case RAW_USER_DEFINED_STRING_LITERAL:
			posttoken_output.emit_user_defined_string_literal(event.data);
			break;
		case RAW_PREPROCESSING_OP_OR_PUNC:
			posttoken_output.emit_preprocessing_op_or_punc(event.data);
			break;
		case RAW_NON_WHITESPACE_CHAR:
			posttoken_output.emit_non_whitespace_char(event.data);
			break;
		}
	}
	posttoken_output.emit_eof();

	PrintResult(output_, EvaluateControllingExpression(tokens, is_defined_));
	line_.clear();
}

void CtrlExprLineSplitter::emit_whitespace_sequence()
{
	append(RAW_WHITESPACE, string());
}

void CtrlExprLineSplitter::emit_new_line()
{
	flush_line();
}

void CtrlExprLineSplitter::emit_header_name(const string& data)
{
	append(RAW_HEADER_NAME, data);
}

void CtrlExprLineSplitter::emit_identifier(const string& data)
{
	append(RAW_IDENTIFIER, data);
}

void CtrlExprLineSplitter::emit_pp_number(const string& data)
{
	append(RAW_PP_NUMBER, data);
}

void CtrlExprLineSplitter::emit_character_literal(const string& data)
{
	append(RAW_CHARACTER_LITERAL, data);
}

void CtrlExprLineSplitter::emit_user_defined_character_literal(
	const string& data)
{
	append(RAW_USER_DEFINED_CHARACTER_LITERAL, data);
}

void CtrlExprLineSplitter::emit_string_literal(const string& data)
{
	append(RAW_STRING_LITERAL, data);
}

void CtrlExprLineSplitter::emit_user_defined_string_literal(
	const string& data)
{
	append(RAW_USER_DEFINED_STRING_LITERAL, data);
}

void CtrlExprLineSplitter::emit_preprocessing_op_or_punc(const string& data)
{
	append(RAW_PREPROCESSING_OP_OR_PUNC, data);
}

void CtrlExprLineSplitter::emit_non_whitespace_char(const string& data)
{
	append(RAW_NON_WHITESPACE_CHAR, data);
}

void CtrlExprLineSplitter::emit_eof()
{
	flush_line();
	output_ << "eof\n";
}
