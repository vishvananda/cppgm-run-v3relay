#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

#include "IPPTokenStream.h"
#include "posttoken_stream.h"

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

struct PPNumberClassification
{
	EPPNumberKind kind;
	size_t body_end;
	string suffix;
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

struct IntegerSuffix
{
	bool has_unsigned;
	int long_rank;
};

bool ParseIntegerSuffix(const string& suffix, IntegerSuffix& result)
{
	result.has_unsigned = false;
	result.long_rank = 0;
	if (suffix.empty())
		return true;

	int unsigned_count = 0;
	int long_count = 0;
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
			if (first_long == '\0')
				first_long = c;
			else if (c != first_long)
				return false;
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
	result.base = 0;

	size_t body_end;
	int base;
	if (ParseIntegerBody(source, body_end, base))
	{
		string suffix = source.substr(body_end);
		IntegerSuffix parsed_suffix;
		if (IsUDSuffix(suffix))
		{
			result.kind = PP_NUMBER_UD_INTEGER;
			result.body_end = body_end;
			result.suffix = suffix;
			result.base = base;
			return result;
		}
		if (ParseIntegerSuffix(suffix, parsed_suffix))
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

struct IntegerCandidate
{
	EFundamentalType type;
	uint64_t maximum;
};

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

void AddIntegerCandidate(vector<IntegerCandidate>& candidates,
	EFundamentalType type)
{
	IntegerCandidate candidate;
	candidate.type = type;
	candidate.maximum = CandidateMaximum(type);
	candidates.push_back(candidate);
}

vector<IntegerCandidate> IntegerCandidates(int base,
	const IntegerSuffix& suffix)
{
	vector<IntegerCandidate> candidates;
	const bool is_decimal = base == 10;

	if (is_decimal)
	{
		if (suffix.has_unsigned)
		{
			if (suffix.long_rank == 0)
			{
				AddIntegerCandidate(candidates, FT_UNSIGNED_INT);
				AddIntegerCandidate(candidates, FT_UNSIGNED_LONG_INT);
				AddIntegerCandidate(candidates, FT_UNSIGNED_LONG_LONG_INT);
			}
			else if (suffix.long_rank == 1)
			{
				AddIntegerCandidate(candidates, FT_UNSIGNED_LONG_INT);
				AddIntegerCandidate(candidates, FT_UNSIGNED_LONG_LONG_INT);
			}
			else
			{
				AddIntegerCandidate(candidates, FT_UNSIGNED_LONG_LONG_INT);
			}
		}
		else if (suffix.long_rank == 0)
		{
			AddIntegerCandidate(candidates, FT_INT);
			AddIntegerCandidate(candidates, FT_LONG_INT);
			AddIntegerCandidate(candidates, FT_LONG_LONG_INT);
		}
		else if (suffix.long_rank == 1)
		{
			AddIntegerCandidate(candidates, FT_LONG_INT);
			AddIntegerCandidate(candidates, FT_LONG_LONG_INT);
		}
		else
		{
			AddIntegerCandidate(candidates, FT_LONG_LONG_INT);
		}
		return candidates;
	}

	if (suffix.long_rank == 0 && !suffix.has_unsigned)
	{
		AddIntegerCandidate(candidates, FT_INT);
		AddIntegerCandidate(candidates, FT_UNSIGNED_INT);
		AddIntegerCandidate(candidates, FT_LONG_INT);
		AddIntegerCandidate(candidates, FT_UNSIGNED_LONG_INT);
		AddIntegerCandidate(candidates, FT_LONG_LONG_INT);
		AddIntegerCandidate(candidates, FT_UNSIGNED_LONG_LONG_INT);
	}
	else if (suffix.long_rank == 0)
	{
		AddIntegerCandidate(candidates, FT_UNSIGNED_INT);
		AddIntegerCandidate(candidates, FT_UNSIGNED_LONG_INT);
		AddIntegerCandidate(candidates, FT_UNSIGNED_LONG_LONG_INT);
	}
	else if (suffix.long_rank == 1 && !suffix.has_unsigned)
	{
		AddIntegerCandidate(candidates, FT_LONG_INT);
		AddIntegerCandidate(candidates, FT_UNSIGNED_LONG_INT);
		AddIntegerCandidate(candidates, FT_LONG_LONG_INT);
		AddIntegerCandidate(candidates, FT_UNSIGNED_LONG_LONG_INT);
	}
	else if (suffix.long_rank == 1)
	{
		AddIntegerCandidate(candidates, FT_UNSIGNED_LONG_INT);
		AddIntegerCandidate(candidates, FT_UNSIGNED_LONG_LONG_INT);
	}
	else if (!suffix.has_unsigned)
	{
		AddIntegerCandidate(candidates, FT_LONG_LONG_INT);
		AddIntegerCandidate(candidates, FT_UNSIGNED_LONG_LONG_INT);
	}
	else
	{
		AddIntegerCandidate(candidates, FT_UNSIGNED_LONG_LONG_INT);
	}
	return candidates;
}

template<typename T>
void EmitIntegerValue(IPostTokenOutputStream& output, const string& source,
	EFundamentalType type, uint64_t value)
{
	T typed_value = static_cast<T>(value);
	output.emit_literal(source, type, &typed_value, sizeof(typed_value));
}

void EmitInteger(IPostTokenOutputStream& output, const string& source,
	const PPNumberClassification& classification)
{
	IntegerSuffix suffix;
	if (!ParseIntegerSuffix(classification.suffix, suffix))
	{
		output.emit_invalid(source);
		return;
	}

	const string body = source.substr(0, classification.body_end);
	size_t value_begin = classification.base == 16 ? 2 : 0;
	uint64_t value;
	if (!AccumulateInteger(body, value_begin, body.size(),
		classification.base, value))
	{
		output.emit_invalid(source);
		return;
	}

	const vector<IntegerCandidate> candidates =
		IntegerCandidates(classification.base, suffix);
	for (size_t i = 0; i < candidates.size(); ++i)
	{
		if (value > candidates[i].maximum)
			continue;

		switch (candidates[i].type)
		{
		case FT_INT:
			EmitIntegerValue<int>(output, source, FT_INT, value);
			return;
		case FT_UNSIGNED_INT:
			EmitIntegerValue<unsigned int>(output, source, FT_UNSIGNED_INT,
				value);
			return;
		case FT_LONG_INT:
			EmitIntegerValue<long>(output, source, FT_LONG_INT, value);
			return;
		case FT_UNSIGNED_LONG_INT:
			EmitIntegerValue<unsigned long>(output, source,
				FT_UNSIGNED_LONG_INT, value);
			return;
		case FT_LONG_LONG_INT:
			EmitIntegerValue<long long>(output, source, FT_LONG_LONG_INT,
				value);
			return;
		case FT_UNSIGNED_LONG_LONG_INT:
			EmitIntegerValue<unsigned long long>(output, source,
				FT_UNSIGNED_LONG_LONG_INT, value);
			return;
		default:
			break;
		}
	}
	output.emit_invalid(source);
}

// use these 3 functions to scan `floating-literals` (see PA2)
float PA2Decode_float(const string& source)
{
	istringstream iss(source);
	float value;
	iss >> value;
	return value;
}

double PA2Decode_double(const string& source)
{
	istringstream iss(source);
	double value;
	iss >> value;
	return value;
}

long double PA2Decode_long_double(const string& source)
{
	istringstream iss(source);
	long double value;
	iss >> value;
	return value;
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
		long double value = PA2Decode_long_double(body);
		output.emit_literal(source, FT_LONG_DOUBLE, &value, sizeof(value));
	}
	else
	{
		double value = PA2Decode_double(body);
		output.emit_literal(source, FT_DOUBLE, &value, sizeof(value));
	}
}

} // namespace

PostTokenStream::PostTokenStream(IPostTokenOutputStream& output)
	: output_(output)
{
}

void PostTokenStream::emit_whitespace_sequence()
{
}

void PostTokenStream::emit_new_line()
{
}

void PostTokenStream::emit_header_name(const string& data)
{
	output_.emit_invalid(data);
}

void PostTokenStream::emit_identifier(const string& data)
{
	unordered_map<string, ETokenType>::const_iterator it =
		StringToTokenTypeMap.find(data);
	if (it == StringToTokenTypeMap.end())
		output_.emit_identifier(data);
	else
		output_.emit_simple(data, it->second);
}

void PostTokenStream::emit_pp_number(const string& data)
{
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
	output_.emit_invalid(data);
}

void PostTokenStream::emit_user_defined_character_literal(const string& data)
{
	output_.emit_invalid(data);
}

void PostTokenStream::emit_string_literal(const string& data)
{
	output_.emit_invalid(data);
}

void PostTokenStream::emit_user_defined_string_literal(const string& data)
{
	output_.emit_invalid(data);
}

void PostTokenStream::emit_preprocessing_op_or_punc(const string& data)
{
	unordered_map<string, ETokenType>::const_iterator it =
		StringToTokenTypeMap.find(data);
	if (it == StringToTokenTypeMap.end())
		output_.emit_invalid(data);
	else
		output_.emit_simple(data, it->second);
}

void PostTokenStream::emit_non_whitespace_char(const string& data)
{
	output_.emit_invalid(data);
}

void PostTokenStream::emit_eof()
{
	output_.emit_eof();
}
