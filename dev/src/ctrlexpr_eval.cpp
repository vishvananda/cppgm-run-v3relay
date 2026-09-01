#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <string>
#include <vector>

using namespace std;

#include "ctrlexpr_eval.h"
#include "macro_replace.h"

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

EvalResult ErrorResult(bool is_unsigned = false)
{
	return EvalResult(is_unsigned, 0, true);
}

class ControllingExpressionParser
{
public:
	ControllingExpressionParser(const vector<CtrlExprToken>& tokens,
		const CtrlExprIsDefined& is_defined)
		: tokens_(tokens), is_defined_(is_defined), position_(0),
		  syntax_error_(false) {}

	EvalResult Parse()
	{
		if (tokens_.empty())
			return ErrorResult();

		EvalResult result = ParseConditional(true);
		if (syntax_error_)
			result.error = true;
		return result;
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

	int BinaryPrecedence(ETokenType op) const
	{
		switch (op)
		{
		case OP_STAR:
		case OP_DIV:
		case OP_MOD:
			return 10;
		case OP_PLUS:
		case OP_MINUS:
			return 9;
		case OP_LSHIFT:
		case OP_RSHIFT:
			return 8;
		case OP_LT:
		case OP_GT:
		case OP_LE:
		case OP_GE:
			return 7;
		case OP_EQ:
		case OP_NE:
			return 6;
		case OP_AMP:
			return 5;
		case OP_XOR:
			return 4;
		case OP_BOR:
			return 3;
		case OP_LAND:
			return 2;
		case OP_LOR:
			return 1;
		default:
			return -1;
		}
	}

	EvalResult ParseConditional(bool evaluate)
	{
		EvalResult condition = ParseBinary(1, evaluate);
		if (!IsOperator(OP_QMARK))
			return condition;

		++position_;
		// The reference discards the condition's evaluation error: the
		// carried value still selects the arm, and only the selected arm's
		// error survives.  The result type is the OR of both arm types
		// regardless of which arm is selected.
		const bool condition_true = condition.value != 0;
		EvalResult true_value = ParseConditional(evaluate && condition_true);

		if (!IsOperator(OP_COLON))
		{
			syntax_error_ = true;
			return ErrorResult(true_value.is_unsigned);
		}
		++position_;

		EvalResult false_value = ParseConditional(evaluate && !condition_true);
		return EvalResult(true_value.is_unsigned || false_value.is_unsigned,
			condition_true ? true_value.value : false_value.value,
			evaluate &&
				(condition_true ? true_value.error : false_value.error));
	}

	EvalResult ParseBinary(int minimum_precedence, bool evaluate)
	{
		EvalResult left = ParseUnary(evaluate);
		for (;;)
		{
			if (position_ >= tokens_.size() ||
				tokens_[position_].kind != CtrlExprToken::KIND_OPERATOR)
				break;

			const ETokenType op = tokens_[position_].op;
			const int precedence = BinaryPrecedence(op);
			if (precedence < minimum_precedence)
				break;

			++position_;
			bool evaluate_right = evaluate;
			if (op == OP_LAND)
				evaluate_right = evaluate && !left.error &&
					left.value != 0;
			else if (op == OP_LOR)
				evaluate_right = evaluate && !left.error &&
					left.value == 0;

			EvalResult right = ParseBinary(precedence + 1, evaluate_right);
			left = ApplyBinary(op, left, right, evaluate);
		}
		return left;
	}

	EvalResult ApplyBinary(ETokenType op, const EvalResult& left,
		const EvalResult& right, bool evaluate)
	{
		if (op == OP_LAND || op == OP_LOR)
		{
			const bool value = op == OP_LAND
				? (left.value != 0 && right.value != 0)
				: (left.value != 0 || right.value != 0);
			return EvalResult(false, value ? 1 : 0,
				evaluate && (left.error || right.error));
		}

		const bool is_unsigned = left.is_unsigned || right.is_unsigned;
		bool error = evaluate && (left.error || right.error);
		std::uint64_t value = 0;

		switch (op)
		{
		case OP_STAR:
			value = left.value * right.value;
			break;
		case OP_PLUS:
			value = left.value + right.value;
			break;
		case OP_MINUS:
			value = left.value - right.value;
			break;
		case OP_AMP:
			value = left.value & right.value;
			break;
		case OP_XOR:
			value = left.value ^ right.value;
			break;
		case OP_BOR:
			value = left.value | right.value;
			break;
		case OP_DIV:
		case OP_MOD:
			// A failed operation carries its left operand's value; that
			// value is observable when a `?:` condition discards the error.
			if (right.value == 0 || (!is_unsigned &&
				left.value == (static_cast<std::uint64_t>(1) << 63) &&
				right.value == static_cast<std::uint64_t>(-1)))
			{
				if (evaluate)
					error = true;
				value = left.value;
			}
			else if (is_unsigned)
			{
				value = op == OP_DIV ? left.value / right.value
					: left.value % right.value;
			}
			else
			{
				const std::int64_t left_signed =
					static_cast<std::int64_t>(left.value);
				const std::int64_t right_signed =
					static_cast<std::int64_t>(right.value);
				value = static_cast<std::uint64_t>(op == OP_DIV
					? left_signed / right_signed
					: left_signed % right_signed);
			}
			break;
		case OP_LSHIFT:
		case OP_RSHIFT:
		{
			const bool negative_shift = !right.is_unsigned &&
				static_cast<std::int64_t>(right.value) < 0;
			if (negative_shift || right.value >= 64)
			{
				if (evaluate)
					error = true;
				value = left.value;
			}
			else
			{
				const unsigned int count =
					static_cast<unsigned int>(right.value);
				if (op == OP_LSHIFT)
					value = left.value << count;
				else if (left.is_unsigned)
					value = left.value >> count;
				else
					value = static_cast<std::uint64_t>(
						static_cast<std::int64_t>(left.value) >> count);
			}
			return EvalResult(left.is_unsigned, value, error);
		}
		case OP_LT:
		case OP_GT:
		case OP_LE:
		case OP_GE:
		case OP_EQ:
		case OP_NE:
		{
			bool comparison = false;
			if (is_unsigned)
			{
				switch (op)
				{
				case OP_LT: comparison = left.value < right.value; break;
				case OP_GT: comparison = left.value > right.value; break;
				case OP_LE: comparison = left.value <= right.value; break;
				case OP_GE: comparison = left.value >= right.value; break;
				case OP_EQ: comparison = left.value == right.value; break;
				case OP_NE: comparison = left.value != right.value; break;
				default: break;
				}
			}
			else
			{
				const std::int64_t left_signed =
					static_cast<std::int64_t>(left.value);
				const std::int64_t right_signed =
					static_cast<std::int64_t>(right.value);
				switch (op)
				{
				case OP_LT: comparison = left_signed < right_signed; break;
				case OP_GT: comparison = left_signed > right_signed; break;
				case OP_LE: comparison = left_signed <= right_signed; break;
				case OP_GE: comparison = left_signed >= right_signed; break;
				case OP_EQ: comparison = left_signed == right_signed; break;
				case OP_NE: comparison = left_signed != right_signed; break;
				default: break;
				}
			}
			return EvalResult(false, comparison ? 1 : 0, error);
		}
		default:
			syntax_error_ = true;
			return ErrorResult(is_unsigned);
		}

		return EvalResult(is_unsigned, value, error);
	}

	EvalResult ParseUnary(bool evaluate)
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
			EvalResult operand = ParseUnary(evaluate);
			const bool error = evaluate && operand.error;
			switch (op)
			{
			case OP_PLUS:
				return EvalResult(operand.is_unsigned, operand.value, error);
			case OP_MINUS:
				return EvalResult(operand.is_unsigned,
					static_cast<std::uint64_t>(0) - operand.value, error);
			case OP_LNOT:
				// The reference evaluator gives `!x` the signedness of its
				// operand (unlike ISO `!`, which yields signed int), and
				// later preprocessor stages are gated on that behavior.
				return EvalResult(operand.is_unsigned,
					operand.value == 0 ? 1 : 0, error);
			case OP_COMPL:
				return EvalResult(operand.is_unsigned, ~operand.value, error);
			default:
				return ErrorResult();
			}
		}
		return ParsePrimary(evaluate);
	}

	EvalResult ParsePrimary(bool evaluate)
	{
		if (position_ >= tokens_.size())
		{
			syntax_error_ = true;
			return ErrorResult();
		}

		const CtrlExprToken& token = tokens_[position_];
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
			EvalResult result = ParseConditional(evaluate);
			if (!IsOperator(OP_RPAREN))
			{
				syntax_error_ = true;
				return result;
			}
			++position_;
			return result;
		}

		++position_;
		syntax_error_ = true;
		return ErrorResult();
	}

	EvalResult ParseDefined()
	{
		++position_; // defined
		if (position_ >= tokens_.size())
		{
			syntax_error_ = true;
			return ErrorResult();
		}

		if (tokens_[position_].kind == CtrlExprToken::KIND_IDENTIFIER)
		{
			const string& name = tokens_[position_].text;
			++position_;
			return DefinedValue(name);
		}

		if (!IsOperator(OP_LPAREN))
		{
			syntax_error_ = true;
			return ErrorResult();
		}
		++position_;
		if (position_ >= tokens_.size() ||
			tokens_[position_].kind != CtrlExprToken::KIND_IDENTIFIER)
		{
			syntax_error_ = true;
			return ErrorResult();
		}

		const string& name = tokens_[position_].text;
		++position_;
		if (!IsOperator(OP_RPAREN))
		{
			syntax_error_ = true;
			return ErrorResult();
		}
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
	bool syntax_error_;
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
	// Keywords precede operators in ETokenType; every keyword is an
	// identifier_or_keyword in controlling-expression context.
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
	// The invalid-token check precedes parsing (pa3/README.md Features), so
	// an unparseable line never recurses; the reference errors here too.
	for (size_t i = 0; i < tokens.size(); ++i)
	{
		if (tokens[i].kind == CtrlExprToken::KIND_BAD)
			return ErrorResult();
	}

	ControllingExpressionParser parser(tokens, is_defined);
	EvalResult result = parser.Parse();
	if (parser.position() != tokens.size())
		result.error = true;
	return result;
}

bool IsIdentifierLikeDefinedOperand(const PPToken& token)
{
	if (token.kind == PP_TOKEN_IDENTIFIER)
		return true;
	if (token.kind != PP_TOKEN_PREPROCESSING_OP_OR_PUNC)
		return false;
	return token.data == "new" || token.data == "delete" ||
		token.data == "and" || token.data == "and_eq" ||
		token.data == "bitand" || token.data == "bitor" ||
		token.data == "compl" || token.data == "not" ||
		token.data == "not_eq" || token.data == "or" ||
		token.data == "or_eq" || token.data == "xor" ||
		token.data == "xor_eq";
}

bool ResolveDefinedOperands(vector<PPToken>& tokens,
	const CtrlExprIsDefined& is_defined)
{
	vector<PPToken> resolved;
	resolved.reserve(tokens.size());

	for (size_t i = 0; i < tokens.size(); ++i)
	{
		if (tokens[i].kind != PP_TOKEN_IDENTIFIER ||
			tokens[i].data != "defined")
		{
			resolved.push_back(tokens[i]);
			continue;
		}

		size_t operand = i + 1;
		size_t end = operand;
		if (operand < tokens.size() &&
			IsIdentifierLikeDefinedOperand(tokens[operand]))
		{
			end = operand + 1;
		}
		else if (operand + 2 < tokens.size() &&
			tokens[operand].data == "(" &&
			 IsIdentifierLikeDefinedOperand(tokens[operand + 1]) &&
			tokens[operand + 2].data == ")")
		{
			operand += 1;
			end = operand + 2;
		}
		else
		{
			return false;
		}

		const string& name = tokens[operand].data;
		PPToken replacement = tokens[i];
		replacement.kind = PP_TOKEN_PP_NUMBER;
		replacement.data = is_defined && is_defined(name) ? "1" : "0";
		replacement.noninvokable = false;
		resolved.push_back(replacement);
		i = end - 1;
	}

	tokens.swap(resolved);
	return true;
}

EvalResult EvaluateControllingExpression(const vector<PPToken>& tokens,
	const CtrlExprIsDefined& is_defined)
{
	vector<CtrlExprToken> converted;
	CtrlExprTokenCollector collector(converted);
	PostTokenStream posttoken_output(collector);

	for (size_t i = 0; i < tokens.size(); ++i)
	{
		const PPToken& token = tokens[i];
		switch (token.kind)
		{
		case PP_TOKEN_HEADER_NAME:
			posttoken_output.emit_header_name(token.data);
			break;
		case PP_TOKEN_IDENTIFIER:
			posttoken_output.emit_identifier(token.data);
			break;
		case PP_TOKEN_PP_NUMBER:
			posttoken_output.emit_pp_number(token.data);
			break;
		case PP_TOKEN_CHARACTER_LITERAL:
			posttoken_output.emit_character_literal(token.data);
			break;
		case PP_TOKEN_USER_DEFINED_CHARACTER_LITERAL:
			posttoken_output.emit_user_defined_character_literal(token.data);
			break;
		case PP_TOKEN_STRING_LITERAL:
			posttoken_output.emit_string_literal(token.data);
			break;
		case PP_TOKEN_USER_DEFINED_STRING_LITERAL:
			posttoken_output.emit_user_defined_string_literal(token.data);
			break;
		case PP_TOKEN_PREPROCESSING_OP_OR_PUNC:
			posttoken_output.emit_preprocessing_op_or_punc(token.data);
			break;
		case PP_TOKEN_NON_WHITESPACE_CHAR:
			posttoken_output.emit_non_whitespace_char(token.data);
			break;
		case PP_TOKEN_NEW_LINE:
			posttoken_output.emit_new_line();
			break;
		case PP_TOKEN_PLACEMARKER:
			collector.emit_invalid(string());
			break;
		}
	}
	posttoken_output.emit_eof();
	return EvaluateControllingExpression(converted, is_defined);
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
	if (line_.empty())
		return;

	vector<CtrlExprToken> tokens;
	CtrlExprTokenCollector collector(tokens);
	PostTokenStream posttoken_output(collector);
	for (size_t i = 0; i < line_.size(); ++i)
	{
		const RawEvent& event = line_[i];
		switch (event.kind)
		{
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
	// Whitespace-sequences are discarded (pa3/README.md Features); replaying
	// them would be a no-op in PostTokenStream anyway.
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
