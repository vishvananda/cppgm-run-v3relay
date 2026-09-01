#include <stdexcept>
#include <string>

using namespace std;

#include "unicode.h"

namespace
{

const int EndOfInput = -1;

bool IsContinuationByte(unsigned char c)
{
	return (c & 0xC0) == 0x80;
}

int InvalidUtf8()
{
	throw runtime_error("Invalid utf-8 character");
}

int ReadContinuation(const string& input, size_t offset, size_t& end)
{
	if (offset >= input.size() ||
		!IsContinuationByte(static_cast<unsigned char>(input[offset])))
		return InvalidUtf8();

	end = offset + 1;
	return static_cast<unsigned char>(input[offset]) & 0x3F;
}

} // namespace

bool IsUnicodeScalarValue(long long codepoint)
{
	return (codepoint >= 0 && codepoint < 0xD800) ||
		(codepoint >= 0xE000 && codepoint <= 0x10FFFF);
}

int DecodeUtf8At(const string& input, size_t offset, size_t& end)
{
	if (offset >= input.size())
	{
		end = offset;
		return EndOfInput;
	}

	unsigned char first = static_cast<unsigned char>(input[offset]);
	if (first <= 0x7F)
	{
		end = offset + 1;
		return first;
	}

	if (first >= 0xC2 && first <= 0xDF)
	{
		size_t next;
		int tail = ReadContinuation(input, offset + 1, next);
		end = next;
		return ((first & 0x1F) << 6) | tail;
	}

	if (first >= 0xE0 && first <= 0xEF)
	{
		if (offset + 2 >= input.size())
			return InvalidUtf8();

		unsigned char second = static_cast<unsigned char>(input[offset + 1]);
		if (!IsContinuationByte(second) ||
			(first == 0xE0 && second < 0xA0) ||
			(first == 0xED && second > 0x9F))
			return InvalidUtf8();

		size_t after_second;
		int second_value = ReadContinuation(input, offset + 1, after_second);
		size_t after_third;
		int third_value = ReadContinuation(input, after_second, after_third);
		end = after_third;
		return ((first & 0x0F) << 12) | (second_value << 6) | third_value;
	}

	if (first >= 0xF0 && first <= 0xF4)
	{
		if (offset + 3 >= input.size())
			return InvalidUtf8();

		unsigned char second = static_cast<unsigned char>(input[offset + 1]);
		if (!IsContinuationByte(second) ||
			(first == 0xF0 && second < 0x90) ||
			(first == 0xF4 && second > 0x8F))
			return InvalidUtf8();

		size_t after_second;
		int second_value = ReadContinuation(input, offset + 1, after_second);
		size_t after_third;
		int third_value = ReadContinuation(input, after_second, after_third);
		size_t after_fourth;
		int fourth_value = ReadContinuation(input, after_third, after_fourth);
		end = after_fourth;
		return ((first & 0x07) << 18) | (second_value << 12) |
			(third_value << 6) | fourth_value;
	}

	return InvalidUtf8();
}

string EncodeUtf8(int codepoint)
{
	if (!IsUnicodeScalarValue(codepoint))
		throw runtime_error("Invalid unicode value");

	string result;
	if (codepoint <= 0x7F)
	{
		result.push_back(static_cast<char>(codepoint));
	}
	else if (codepoint <= 0x7FF)
	{
		result.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
		result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	}
	else if (codepoint <= 0xFFFF)
	{
		result.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
		result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
		result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	}
	else
	{
		result.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
		result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
		result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
		result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	}
	return result;
}
