#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace std;

#include "IPPTokenStream.h"
#include "posttoken_stream.h"
#include "unicode.h"

namespace
{

enum EPPNumberKind
{
	PP_NUMBER_INVALID,
	PP_NUMBER_INTEGER,
	PP_NUMBER_FLOATING,
	PP_NUMBER_UD_INTEGER,
	PP_NUMBER_UD_FLOATING
};

struct IntegerSuffix
{
	bool has_unsigned;
	int long_rank;
};

struct PPNumberClassification
{
	EPPNumberKind kind;
	size_t body_end;
	string suffix;
	IntegerSuffix integer_suffix;
	int base;
};

bool IsAsciiDigit(char c)
{
	return c >= '0' && c <= '9';
}

bool IsHexDigit(char c)
{
	return (c >= '0' && c <= '9') ||
		(c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int HexDigitValue(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return c - 'A' + 10;
}

bool IsIdentifierContinuationByte(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

bool IsUDSuffix(const string& suffix)
{
	if (suffix.empty() || suffix[0] != '_')
		return false;

	for (size_t i = 1; i < suffix.size(); ++i)
		if (!IsIdentifierContinuationByte(
			static_cast<unsigned char>(suffix[i])))
			return false;
	return true;
}

bool ParseIntegerSuffix(const string& suffix, IntegerSuffix& result)
{
	result.has_unsigned = false;
	result.long_rank = 0;
	if (suffix.empty())
		return true;

	int unsigned_count = 0;
	int long_count = 0;
	size_t last_long = 0;
	char first_long = '\0';
	for (size_t i = 0; i < suffix.size(); ++i)
	{
		char c = suffix[i];
		if (c == 'u' || c == 'U')
		{
			++unsigned_count;
			continue;
		}
		if (c == 'l' || c == 'L')
		{
			// ll-suffix is two adjacent same-case characters (2.14.2)
			if (first_long == '\0')
				first_long = c;
			else if (c != first_long || i != last_long + 1)
				return false;
			last_long = i;
			++long_count;
			continue;
		}
		return false;
	}

	if (unsigned_count > 1 || long_count > 2 ||
		static_cast<size_t>(unsigned_count + long_count) != suffix.size())
		return false;

	result.has_unsigned = unsigned_count == 1;
	result.long_rank = long_count;
	return true;
}

bool IsFloatingSuffix(const string& suffix)
{
	return suffix.size() == 1 &&
		(suffix[0] == 'f' || suffix[0] == 'F' ||
		 suffix[0] == 'l' || suffix[0] == 'L');
}

bool ParseIntegerBody(const string& source, size_t& end, int& base)
{
	if (source.empty() || !IsAsciiDigit(source[0]))
		return false;

	if (source[0] == '0' && source.size() >= 2 &&
		(source[1] == 'x' || source[1] == 'X'))
	{
		size_t p = 2;
		size_t first_digit = p;
		while (p < source.size() && IsHexDigit(source[p]))
			++p;
		if (p == first_digit)
			return false;
		end = p;
		base = 16;
		return true;
	}

	if (source[0] == '0')
	{
		size_t p = 1;
		while (p < source.size() && source[p] >= '0' && source[p] <= '7')
			++p;
		end = p;
		base = 8;
		return true;
	}

	size_t p = 0;
	while (p < source.size() && IsAsciiDigit(source[p]))
		++p;
	end = p;
	base = 10;
	return p != 0;
}

bool ParseExponent(const string& source, size_t& p)
{
	if (p >= source.size() || (source[p] != 'e' && source[p] != 'E'))
		return false;

	++p;
	if (p < source.size() && (source[p] == '+' || source[p] == '-'))
		++p;
	size_t first_digit = p;
	while (p < source.size() && IsAsciiDigit(source[p]))
		++p;
	return p != first_digit;
}

bool ParseFloatingBody(const string& source, size_t& end)
{
	if (source.empty())
		return false;

	size_t p = 0;
	bool has_dot = false;
	bool has_exponent = false;

	if (source[p] == '.')
	{
		has_dot = true;
		++p;
		size_t first_digit = p;
		while (p < source.size() && IsAsciiDigit(source[p]))
			++p;
		if (p == first_digit)
			return false;
	}
	else
	{
		if (!IsAsciiDigit(source[p]))
			return false;
		while (p < source.size() && IsAsciiDigit(source[p]))
			++p;

		if (p < source.size() && source[p] == '.')
		{
			has_dot = true;
			++p;
			while (p < source.size() && IsAsciiDigit(source[p]))
				++p;
		}
	}

	if (p < source.size() && (source[p] == 'e' || source[p] == 'E'))
	{
		has_exponent = true;
		if (!ParseExponent(source, p))
			return false;
	}

	if (!has_dot && !has_exponent)
		return false;
	end = p;
	return true;
}

PPNumberClassification ClassifyPPNumber(const string& source)
{
	PPNumberClassification result;
	result.kind = PP_NUMBER_INVALID;
	result.body_end = 0;
	result.integer_suffix.has_unsigned = false;
	result.integer_suffix.long_rank = 0;
	result.base = 0;

	size_t body_end;
	int base;
	if (ParseIntegerBody(source, body_end, base))
	{
		string suffix = source.substr(body_end);
		if (IsUDSuffix(suffix))
		{
			result.kind = PP_NUMBER_UD_INTEGER;
			result.body_end = body_end;
			result.suffix = suffix;
			result.base = base;
			return result;
		}
		if (ParseIntegerSuffix(suffix, result.integer_suffix))
		{
			result.kind = PP_NUMBER_INTEGER;
			result.body_end = body_end;
			result.suffix = suffix;
			result.base = base;
			return result;
		}
	}

	if (ParseFloatingBody(source, body_end))
	{
		string suffix = source.substr(body_end);
		if (IsUDSuffix(suffix))
		{
			result.kind = PP_NUMBER_UD_FLOATING;
			result.body_end = body_end;
			result.suffix = suffix;
			return result;
		}
		if (suffix.empty() || IsFloatingSuffix(suffix))
		{
			result.kind = PP_NUMBER_FLOATING;
			result.body_end = body_end;
			result.suffix = suffix;
			return result;
		}
	}

	return result;
}

bool AccumulateInteger(const string& source, size_t begin, size_t end,
	int base, uint64_t& value)
{
	value = 0;
	const uint64_t maximum = numeric_limits<uint64_t>::max();
	for (size_t i = begin; i < end; ++i)
	{
		int digit;
		if (base == 16)
			digit = HexDigitValue(source[i]);
		else
			digit = source[i] - '0';

		if (value > (maximum - static_cast<uint64_t>(digit)) /
			static_cast<uint64_t>(base))
			return false;
		value = value * static_cast<uint64_t>(base) +
			static_cast<uint64_t>(digit);
	}
	return true;
}

// ABI width in bytes of the scalar literal types (System V AMD64 3.1)
size_t ScalarWidth(EFundamentalType type)
{
	switch (type)
	{
	case FT_CHAR:
		return 1;
	case FT_CHAR16_T:
		return 2;
	case FT_INT:
	case FT_UNSIGNED_INT:
	case FT_CHAR32_T:
	case FT_WCHAR_T:
		return 4;
	case FT_LONG_INT:
	case FT_UNSIGNED_LONG_INT:
	case FT_LONG_LONG_INT:
	case FT_UNSIGNED_LONG_LONG_INT:
		return 8;
	default:
		return 0;
	}
}

struct ScalarBytes
{
	unsigned char bytes[8];
	size_t size;
};

// Render a scalar literal value into its ABI representation: multibyte
// types are stored byte-wise little-endian.
ScalarBytes EncodeScalar(EFundamentalType type, uint64_t value)
{
	ScalarBytes result;
	result.size = ScalarWidth(type);
	for (size_t i = 0; i < result.size; ++i)
		result.bytes[i] = static_cast<unsigned char>(value >> (8 * i));
	return result;
}

uint64_t CandidateMaximum(EFundamentalType type)
{
	switch (type)
	{
	case FT_INT:
		return static_cast<uint64_t>(numeric_limits<int>::max());
	case FT_UNSIGNED_INT:
		return static_cast<uint64_t>(numeric_limits<unsigned int>::max());
	case FT_LONG_INT:
		return static_cast<uint64_t>(numeric_limits<long>::max());
	case FT_UNSIGNED_LONG_INT:
		return static_cast<uint64_t>(numeric_limits<unsigned long>::max());
	case FT_LONG_LONG_INT:
		return static_cast<uint64_t>(numeric_limits<long long>::max());
	case FT_UNSIGNED_LONG_LONG_INT:
		return static_cast<uint64_t>(
			numeric_limits<unsigned long long>::max());
	default:
		return 0;
	}
}

struct IntegerCandidateList
{
	const EFundamentalType* types;
	size_t count;
};

// Candidate type lists of 2.14.2: decimal unsuffixed literals stay signed;
// octal/hex also try the unsigned type at each rank; a `u` suffix restricts
// to unsigned; `l`/`ll` set the starting rank.
IntegerCandidateList IntegerCandidates(int base, const IntegerSuffix& suffix)
{
	static const EFundamentalType Unsigned[] =
		{FT_UNSIGNED_INT, FT_UNSIGNED_LONG_INT, FT_UNSIGNED_LONG_LONG_INT};
	static const EFundamentalType SignedDecimal[] =
		{FT_INT, FT_LONG_INT, FT_LONG_LONG_INT};
	static const EFundamentalType EitherSign[] =
		{FT_INT, FT_UNSIGNED_INT, FT_LONG_INT, FT_UNSIGNED_LONG_INT,
		 FT_LONG_LONG_INT, FT_UNSIGNED_LONG_LONG_INT};

	IntegerCandidateList result;
	if (suffix.has_unsigned)
	{
		result.types = Unsigned + suffix.long_rank;
		result.count = 3 - suffix.long_rank;
	}
	else if (base == 10)
	{
		result.types = SignedDecimal + suffix.long_rank;
		result.count = 3 - suffix.long_rank;
	}
	else
	{
		result.types = EitherSign + 2 * suffix.long_rank;
		result.count = 6 - 2 * suffix.long_rank;
	}
	return result;
}

void EmitInteger(IPostTokenOutputStream& output, const string& source,
	const PPNumberClassification& classification)
{
	size_t value_begin = classification.base == 16 ? 2 : 0;
	uint64_t value;
	if (!AccumulateInteger(source, value_begin, classification.body_end,
		classification.base, value))
	{
		output.emit_invalid(source);
		return;
	}

	const IntegerCandidateList candidates =
		IntegerCandidates(classification.base, classification.integer_suffix);
	for (size_t i = 0; i < candidates.count; ++i)
	{
		const EFundamentalType type = candidates.types[i];
		if (value > CandidateMaximum(type))
			continue;
		const ScalarBytes bytes = EncodeScalar(type, value);
		output.emit_literal(source, type, bytes.bytes, bytes.size);
		return;
	}
	output.emit_invalid(source);
}

// use these 3 functions to scan `floating-literals` (see PA2).  The starter
// kit reads through istringstream, whose C++11 num_get stores the largest
// finite value on overflow; the reference implementation overflows to
// infinity.  strtof/strtod/strtold keep in-range results bit-identical and
// overflow to infinity.
float PA2Decode_float(const string& source)
{
	return strtof(source.c_str(), 0);
}

double PA2Decode_double(const string& source)
{
	return strtod(source.c_str(), 0);
}

long double PA2Decode_long_double(const string& source)
{
	return strtold(source.c_str(), 0);
}

void EmitFloating(IPostTokenOutputStream& output, const string& source,
	const PPNumberClassification& classification)
{
	const string body = source.substr(0, classification.body_end);
	if (classification.suffix == "f" || classification.suffix == "F")
	{
		float value = PA2Decode_float(body);
		output.emit_literal(source, FT_FLOAT, &value, sizeof(value));
	}
	else if (classification.suffix == "l" ||
		classification.suffix == "L")
	{
		// the 16-byte long double object holds an 80-bit x87 value; the
		// ABI leaves the top 6 bytes undefined, so zero them for
		// deterministic output
		const long double value = PA2Decode_long_double(body);
		unsigned char bytes[sizeof(value)] = {};
		memcpy(bytes, &value, 10);
		output.emit_literal(source, FT_LONG_DOUBLE, bytes, sizeof(bytes));
	}
	else
	{
		double value = PA2Decode_double(body);
		output.emit_literal(source, FT_DOUBLE, &value, sizeof(value));
	}
}

struct CharacterLiteralClassification
{
	string prefix;
	size_t after_quote;
	int codepoint;
	EFundamentalType type;
};

bool DecodeSimpleEscape(char escaped, int& codepoint)
{
	switch (escaped)
	{
	case '\'': codepoint = '\''; return true;
	case '"': codepoint = '"'; return true;
	case '?': codepoint = '?'; return true;
	case '\\': codepoint = '\\'; return true;
	case 'a': codepoint = '\a'; return true;
	case 'b': codepoint = '\b'; return true;
	case 'f': codepoint = '\f'; return true;
	case 'n': codepoint = '\n'; return true;
	case 'r': codepoint = '\r'; return true;
	case 't': codepoint = '\t'; return true;
	case 'v': codepoint = '\v'; return true;
	default: return false;
	}
}

struct EscapeValue
{
	uint64_t value;
	bool numeric_escape;
};

// Decode one escape-sequence (2.14.3.3): simple escapes name a character;
// octal and hex escapes are numeric.  Hex values that cannot fit any code
// unit (above 32 bits) fail the parse.
bool ParseEscape(const string& source, size_t& cursor, EscapeValue& result)
{
	if (cursor >= source.size() || source[cursor] != '\\' ||
		cursor + 1 >= source.size())
		return false;

	char escaped = source[cursor + 1];
	int simple_codepoint;
	if (DecodeSimpleEscape(escaped, simple_codepoint))
	{
		cursor += 2;
		result.value = static_cast<uint64_t>(simple_codepoint);
		result.numeric_escape = false;
		return true;
	}

	if (escaped == 'x')
	{
		size_t first_digit = cursor + 2;
		size_t end = first_digit;
		uint64_t value = 0;
		bool too_large = false;
		while (end < source.size() && IsHexDigit(source[end]))
		{
			int digit = HexDigitValue(source[end]);
			if (value > (0xFFFFFFFFULL - static_cast<uint64_t>(digit)) / 16)
				too_large = true;
			else
				value = value * 16 + static_cast<uint64_t>(digit);
			++end;
		}
		if (end == first_digit || too_large)
			return false;
		cursor = end;
		result.value = value;
		result.numeric_escape = true;
		return true;
	}

	if (escaped >= '0' && escaped <= '7')
	{
		size_t end = cursor + 1;
		uint32_t value = 0;
		for (int digits = 0; digits < 3 && end < source.size() &&
			source[end] >= '0' && source[end] <= '7'; ++digits, ++end)
			value = value * 8 + static_cast<uint32_t>(source[end] - '0');
		cursor = end;
		result.value = value;
		result.numeric_escape = true;
		return true;
	}

	return false;
}

struct StringLiteralClassification
{
	string prefix;
	string suffix;
	vector<StringSequenceElement> elements;
	size_t after_quote;
	bool raw;
	bool empty_ordinary;
};

bool ParseStringLiteral(const string& source,
	StringLiteralClassification& result)
{
	result.prefix.clear();
	result.suffix.clear();
	result.elements.clear();
	result.after_quote = 0;
	result.raw = false;
	result.empty_ordinary = false;

	size_t quote;
	if (source.compare(0, 4, "u8R\"") == 0)
	{
		result.prefix = "u8";
		result.raw = true;
		quote = 3;
	}
	else if (source.compare(0, 3, "uR\"") == 0)
	{
		result.prefix = "u";
		result.raw = true;
		quote = 2;
	}
	else if (source.compare(0, 3, "UR\"") == 0)
	{
		result.prefix = "U";
		result.raw = true;
		quote = 2;
	}
	else if (source.compare(0, 3, "LR\"") == 0)
	{
		result.prefix = "L";
		result.raw = true;
		quote = 2;
	}
	else if (source.compare(0, 2, "R\"") == 0)
	{
		result.raw = true;
		quote = 1;
	}
	else if (source.compare(0, 3, "u8\"") == 0)
	{
		result.prefix = "u8";
		quote = 2;
	}
	else if (source.compare(0, 2, "u\"") == 0)
	{
		result.prefix = "u";
		quote = 1;
	}
	else if (source.compare(0, 2, "U\"") == 0)
	{
		result.prefix = "U";
		quote = 1;
	}
	else if (source.compare(0, 2, "L\"") == 0)
	{
		result.prefix = "L";
		quote = 1;
	}
	else if (!source.empty() && source[0] == '"')
	{
		quote = 0;
	}
	else
		return false;

	if (quote >= source.size() || source[quote] != '"')
		return false;

	if (result.raw)
	{
		size_t opening_parenthesis = source.find('(', quote + 1);
		if (opening_parenthesis == string::npos)
			return false;

		const string delimiter = source.substr(quote + 1,
			opening_parenthesis - quote - 1);
		const string closing = ")" + delimiter + '"';
		size_t closing_position = source.find(closing,
			opening_parenthesis + 1);
		if (closing_position == string::npos)
			return false;

		result.after_quote = closing_position + closing.size();
		result.suffix = source.substr(result.after_quote);
		size_t cursor = opening_parenthesis + 1;
		while (cursor < closing_position)
		{
			size_t end;
			int codepoint = DecodeUtf8At(source, cursor, end);
			if (codepoint < 0 || end > closing_position)
				return false;
			StringSequenceElement element;
			element.codepoint = static_cast<uint32_t>(codepoint);
			element.numeric_escape = false;
			result.elements.push_back(element);
			cursor = end;
		}
		return true;
	}

	size_t cursor = quote + 1;
	while (cursor < source.size())
	{
		if (source[cursor] == '"')
		{
			result.after_quote = cursor + 1;
			result.suffix = source.substr(result.after_quote);
			result.empty_ordinary = result.prefix.empty() &&
				result.elements.empty();
			return true;
		}

		if (source[cursor] == '\\')
		{
			EscapeValue escape;
			if (!ParseEscape(source, cursor, escape))
				return false;
			StringSequenceElement element;
			element.codepoint = static_cast<uint32_t>(escape.value);
			element.numeric_escape = escape.numeric_escape;
			result.elements.push_back(element);
			continue;
		}

		size_t end;
		int codepoint = DecodeUtf8At(source, cursor, end);
		if (codepoint < 0)
			return false;
		StringSequenceElement element;
		element.codepoint = static_cast<uint32_t>(codepoint);
		element.numeric_escape = false;
		result.elements.push_back(element);
		cursor = end;
	}

	return false;
}

struct EncodedString
{
	EFundamentalType type;
	size_t num_elements;
	size_t element_size;
	vector<unsigned char> bytes;
};

void AppendCodeUnit(EncodedString& result, uint32_t value)
{
	for (size_t i = 0; i < result.element_size; ++i)
		result.bytes.push_back(static_cast<unsigned char>(value >> (8 * i)));
	result.num_elements++;
}

// Encode the sequence body once its prefix is known.  Code points encode
// per the target encoding; a numeric escape contributes exactly one code
// unit and must fit the element type's unsigned range.
bool EncodeStringElements(const vector<StringSequenceElement>& elements,
	const string& prefix, EncodedString& result)
{
	if (prefix.empty() || prefix == "u8")
		result.type = FT_CHAR;
	else if (prefix == "u")
		result.type = FT_CHAR16_T;
	else if (prefix == "U")
		result.type = FT_CHAR32_T;
	else if (prefix == "L")
		result.type = FT_WCHAR_T;
	else
		return false;

	result.num_elements = 0;
	result.element_size = ScalarWidth(result.type);
	result.bytes.clear();

	for (size_t i = 0; i < elements.size(); ++i)
	{
		const uint32_t value = elements[i].codepoint;
		if (elements[i].numeric_escape)
		{
			const uint64_t unit_maximum =
				(1ULL << (8 * result.element_size)) - 1;
			if (value > unit_maximum)
				return false;
			AppendCodeUnit(result, value);
		}
		else if (result.type == FT_CHAR)
		{
			const string utf8 = EncodeUtf8(static_cast<int>(value));
			for (size_t j = 0; j < utf8.size(); ++j)
				AppendCodeUnit(result, static_cast<unsigned char>(utf8[j]));
		}
		else if (result.type == FT_CHAR16_T && value > 0xFFFF)
		{
			const uint32_t adjusted = value - 0x10000;
			AppendCodeUnit(result, 0xD800 + (adjusted >> 10));
			AppendCodeUnit(result, 0xDC00 + (adjusted & 0x3FF));
		}
		else
		{
			AppendCodeUnit(result, value);
		}
	}

	AppendCodeUnit(result, 0);
	return true;
}

bool ParseCharacterLiteral(const string& source,
	CharacterLiteralClassification& result)
{
	size_t quote;
	if (source.compare(0, 2, "u'") == 0)
	{
		result.prefix = "u";
		quote = 1;
	}
	else if (source.compare(0, 2, "U'") == 0)
	{
		result.prefix = "U";
		quote = 1;
	}
	else if (source.compare(0, 2, "L'") == 0)
	{
		result.prefix = "L";
		quote = 1;
	}
	else if (!source.empty() && source[0] == '\'')
	{
		result.prefix.clear();
		quote = 0;
	}
	else
		return false;

	if (quote + 1 >= source.size())
		return false;

	size_t cursor = quote + 1;
	int codepoint;
	if (source[cursor] == '\\')
	{
		// numeric escapes are code points here, unlike in strings; values
		// past the Unicode range can never be a scalar value, so reject
		EscapeValue escape;
		if (!ParseEscape(source, cursor, escape) || escape.value > 0x10FFFF)
			return false;
		codepoint = static_cast<int>(escape.value);
	}
	else
	{
		size_t end;
		codepoint = DecodeUtf8At(source, cursor, end);
		if (codepoint < 0)
			return false;
		cursor = end;
	}

	if (cursor >= source.size() || source[cursor] != '\'')
		return false;

	result.after_quote = cursor + 1;
	result.codepoint = codepoint;
	return true;
}

bool SelectCharacterType(const string& prefix, int codepoint,
	EFundamentalType& type)
{
	if (!IsUnicodeScalarValue(codepoint))
		return false;

	if (prefix == "u")
	{
		if (static_cast<unsigned long long>(codepoint) >
			static_cast<unsigned long long>(numeric_limits<char16_t>::max()))
			return false;
		type = FT_CHAR16_T;
	}
	else if (prefix == "U")
	{
		if (static_cast<unsigned long long>(codepoint) >
			static_cast<unsigned long long>(numeric_limits<char32_t>::max()))
			return false;
		type = FT_CHAR32_T;
	}
	else if (prefix == "L")
	{
		if (static_cast<unsigned long long>(codepoint) >
			static_cast<unsigned long long>(numeric_limits<wchar_t>::max()))
			return false;
		type = FT_WCHAR_T;
	}
	else if (codepoint <= 127)
		type = FT_CHAR;
	else
		type = FT_INT;
	return true;
}

bool AnalyzeCharacterLiteral(const string& source,
	CharacterLiteralClassification& result)
{
	return ParseCharacterLiteral(source, result) &&
		SelectCharacterType(result.prefix, result.codepoint, result.type);
}

} // namespace

PostTokenStream::PostTokenStream(IPostTokenOutputStream& output)
	: output_(output),
		pending_string_token_count_(0),
		pending_string_sequence_active_(false),
		pending_string_invalid_(false),
		pending_string_has_ud_suffix_(false),
		pending_string_ud_suffix_valid_(true),
		pending_string_operator_candidate_(false),
		pending_string_empty_ordinary_(false),
		operator_pending_(false)
{
}

void PostTokenStream::reset_string_sequence()
{
	pending_string_source_.clear();
	pending_string_prefix_.clear();
	pending_string_ud_suffix_.clear();
	pending_string_elements_.clear();
	pending_string_token_count_ = 0;
	pending_string_sequence_active_ = false;
	pending_string_invalid_ = false;
	pending_string_has_ud_suffix_ = false;
	pending_string_ud_suffix_valid_ = true;
	pending_string_operator_candidate_ = false;
	pending_string_empty_ordinary_ = false;
}

void PostTokenStream::append_string_token(const string& data,
	bool user_defined)
{
	const bool first_token = !pending_string_sequence_active_;
	if (first_token)
	{
		pending_string_sequence_active_ = true;
		pending_string_source_ = data;
		pending_string_operator_candidate_ = operator_pending_;
		pending_string_empty_ordinary_ = false;
		operator_pending_ = false;
	}
	else
	{
		pending_string_source_ += " ";
		pending_string_source_ += data;
		if (pending_string_has_ud_suffix_ &&
			!pending_string_ud_suffix_valid_)
			pending_string_invalid_ = true;
	}
	++pending_string_token_count_;

	StringLiteralClassification classification;
	if (!ParseStringLiteral(data, classification))
	{
		pending_string_invalid_ = true;
		if (user_defined)
		{
			pending_string_has_ud_suffix_ = true;
			pending_string_ud_suffix_valid_ = false;
		}
		return;
	}

	if (first_token)
		pending_string_empty_ordinary_ = classification.empty_ordinary;

	if (!user_defined && !classification.suffix.empty())
		pending_string_invalid_ = true;

	if (user_defined)
	{
		const bool had_ud_suffix = pending_string_has_ud_suffix_;
		const bool valid_ud_suffix = !classification.suffix.empty() &&
			IsUDSuffix(classification.suffix);
		pending_string_has_ud_suffix_ = true;
		if (!valid_ud_suffix)
		{
			pending_string_ud_suffix_valid_ = false;
			if (!(first_token && pending_string_operator_candidate_ &&
				classification.empty_ordinary))
				pending_string_invalid_ = true;
			if (!had_ud_suffix)
				pending_string_ud_suffix_ = classification.suffix;
		}
		else
		{
			if (!had_ud_suffix)
				pending_string_ud_suffix_ = classification.suffix;
			else if (!pending_string_ud_suffix_valid_ ||
				pending_string_ud_suffix_ != classification.suffix)
				pending_string_invalid_ = true;
		}
	}

	if (!classification.prefix.empty())
	{
		if (pending_string_prefix_.empty())
			pending_string_prefix_ = classification.prefix;
		else if (pending_string_prefix_ != classification.prefix)
			pending_string_invalid_ = true;
	}

	pending_string_elements_.insert(pending_string_elements_.end(),
		classification.elements.begin(), classification.elements.end());
}

void PostTokenStream::flush_string_sequence()
{
	if (!pending_string_sequence_active_)
		return;

	const bool split_operator_suffix =
		pending_string_operator_candidate_ &&
		pending_string_token_count_ == 1 &&
		pending_string_empty_ordinary_ &&
		pending_string_has_ud_suffix_ &&
		!pending_string_ud_suffix_valid_ &&
		!pending_string_ud_suffix_.empty() &&
		!pending_string_invalid_;
	if (split_operator_suffix)
	{
		EncodedString encoded;
		if (!EncodeStringElements(pending_string_elements_,
			pending_string_prefix_, encoded))
		{
			output_.emit_invalid(pending_string_source_);
		}
		else
		{
			const size_t suffix_size = pending_string_ud_suffix_.size();
			const string literal_source = pending_string_source_.substr(0,
				pending_string_source_.size() - suffix_size);
			output_.emit_literal_array(literal_source,
				encoded.num_elements, encoded.type, encoded.bytes.data(),
				encoded.bytes.size());
			output_.emit_identifier(pending_string_ud_suffix_);
		}
		reset_string_sequence();
		return;
	}

	EncodedString encoded;
	if (pending_string_invalid_ ||
		!EncodeStringElements(pending_string_elements_,
			pending_string_prefix_, encoded))
	{
		output_.emit_invalid(pending_string_source_);
	}
	else if (pending_string_has_ud_suffix_)
	{
		output_.emit_user_defined_literal_string_array(
			pending_string_source_, pending_string_ud_suffix_,
			encoded.num_elements, encoded.type, encoded.bytes.data(),
			encoded.bytes.size());
	}
	else
	{
		output_.emit_literal_array(pending_string_source_,
			encoded.num_elements, encoded.type, encoded.bytes.data(),
			encoded.bytes.size());
	}
	reset_string_sequence();
}

void PostTokenStream::emit_whitespace_sequence()
{
}

void PostTokenStream::emit_new_line()
{
}

void PostTokenStream::emit_header_name(const string& data)
{
	flush_string_sequence();
	operator_pending_ = false;
	output_.emit_invalid(data);
}

void PostTokenStream::emit_identifier(const string& data)
{
	flush_string_sequence();
	operator_pending_ = false;
	unordered_map<string, ETokenType>::const_iterator it =
		StringToTokenTypeMap.find(data);
	if (it == StringToTokenTypeMap.end())
		output_.emit_identifier(data);
	else
		output_.emit_simple(data, it->second);
	if (data == "operator")
		operator_pending_ = true;
}

void PostTokenStream::emit_pp_number(const string& data)
{
	flush_string_sequence();
	operator_pending_ = false;
	PPNumberClassification classification = ClassifyPPNumber(data);
	switch (classification.kind)
	{
	case PP_NUMBER_INTEGER:
		EmitInteger(output_, data, classification);
		return;
	case PP_NUMBER_FLOATING:
		EmitFloating(output_, data, classification);
		return;
	case PP_NUMBER_UD_INTEGER:
		output_.emit_user_defined_literal_integer(data, classification.suffix,
			data.substr(0, classification.body_end));
		return;
	case PP_NUMBER_UD_FLOATING:
		output_.emit_user_defined_literal_floating(data, classification.suffix,
			data.substr(0, classification.body_end));
		return;
	case PP_NUMBER_INVALID:
		output_.emit_invalid(data);
		return;
	}
}

void PostTokenStream::emit_character_literal(const string& data)
{
	flush_string_sequence();
	operator_pending_ = false;
	CharacterLiteralClassification classification;
	if (!AnalyzeCharacterLiteral(data, classification) ||
		classification.after_quote != data.size())
	{
		output_.emit_invalid(data);
		return;
	}
	const ScalarBytes bytes = EncodeScalar(classification.type,
		static_cast<uint64_t>(classification.codepoint));
	output_.emit_literal(data, classification.type, bytes.bytes, bytes.size);
}

void PostTokenStream::emit_user_defined_character_literal(const string& data)
{
	flush_string_sequence();
	operator_pending_ = false;
	CharacterLiteralClassification classification;
	if (!AnalyzeCharacterLiteral(data, classification))
	{
		output_.emit_invalid(data);
		return;
	}

	const string suffix = data.substr(classification.after_quote);
	if (!IsUDSuffix(suffix))
	{
		output_.emit_invalid(data);
		return;
	}
	const ScalarBytes bytes = EncodeScalar(classification.type,
		static_cast<uint64_t>(classification.codepoint));
	output_.emit_user_defined_literal_character(data, suffix,
		classification.type, bytes.bytes, bytes.size);
}

void PostTokenStream::emit_string_literal(const string& data)
{
	append_string_token(data, false);
}

void PostTokenStream::emit_user_defined_string_literal(const string& data)
{
	append_string_token(data, true);
}

void PostTokenStream::emit_preprocessing_op_or_punc(const string& data)
{
	flush_string_sequence();
	operator_pending_ = false;
	unordered_map<string, ETokenType>::const_iterator it =
		StringToTokenTypeMap.find(data);
	if (it == StringToTokenTypeMap.end())
		output_.emit_invalid(data);
	else
		output_.emit_simple(data, it->second);
}

void PostTokenStream::emit_non_whitespace_char(const string& data)
{
	flush_string_sequence();
	operator_pending_ = false;
	output_.emit_invalid(data);
}

void PostTokenStream::emit_eof()
{
	flush_string_sequence();
	output_.emit_eof();
}
