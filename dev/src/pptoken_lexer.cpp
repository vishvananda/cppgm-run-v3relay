#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace std;

#include "IPPTokenStream.h"
#include "exceptions.h"
#include "pptoken_lexer.h"

namespace
{

constexpr int EndOfFile = -1;

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

int DecodeUtf8At(const string& input, size_t offset, size_t& end)
{
	if (offset >= input.size())
	{
		end = offset;
		return EndOfFile;
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

int HexValue(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

int TrigraphReplacement(int c)
{
	switch (c)
	{
	case '=': return '#';
	case '/': return '\\';
	case '\'': return '^';
	case '(': return '[';
	case ')': return ']';
	case '!': return '|';
	case '<': return '{';
	case '>': return '}';
	case '-': return '~';
	default: return EndOfFile;
	}
}

string encode_utf8(int codepoint)
{
	if (codepoint < 0 || codepoint > 0x10FFFF ||
		(codepoint >= 0xD800 && codepoint <= 0xDFFF))
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

class SourceDecoder
{
public:
	const string& buf;
	size_t pos;
	bool raw;
	bool last_was_ucn;

	SourceDecoder(const string& buf)
		: buf(buf), pos(0), raw(false), last_was_ucn(false)
	{
		if (buf.size() >= 3 &&
			static_cast<unsigned char>(buf[0]) == 0xEF &&
			static_cast<unsigned char>(buf[1]) == 0xBB &&
			static_cast<unsigned char>(buf[2]) == 0xBF)
			pos = 3;
	}

	int next()
	{
		last_was_ucn = false;
		while (true)
		{
			int codepoint = NextPhysical();
			if (codepoint == EndOfFile || raw)
				return codepoint;

			if (codepoint == '?')
			{
				size_t after_second;
				int second = PeekPhysical(0, &after_second);
				if (second == '?')
				{
					size_t after_third;
					int third = PeekPhysical(1, &after_third);
					int replacement = TrigraphReplacement(third);
					if (replacement != EndOfFile)
					{
						pos = after_third;
						codepoint = replacement;
					}
				}
			}

			if (codepoint == '\\')
			{
				if (ConsumeLineSplice())
					continue;

				int universal;
				if (TryDecodeUniversalCharacterName(universal))
				{
					last_was_ucn = true;
					return universal;
				}
			}
			return codepoint;
		}
	}

	int peek(size_t lookahead = 0)
	{
		size_t saved = mark();
		bool saved_last_was_ucn = last_was_ucn;
		try
		{
			int result = EndOfFile;
			for (size_t i = 0; i <= lookahead; ++i)
				result = next();
			rewind(saved);
			last_was_ucn = saved_last_was_ucn;
			return result;
		}
		catch (...)
		{
			rewind(saved);
			last_was_ucn = saved_last_was_ucn;
			throw;
		}
	}

	bool peek_is_ucn(size_t lookahead = 0)
	{
		size_t saved = mark();
		bool saved_last_was_ucn = last_was_ucn;
		try
		{
			bool result = false;
			for (size_t i = 0; i <= lookahead; ++i)
			{
				next();
				if (i == lookahead)
					result = last_was_ucn;
			}
			rewind(saved);
			last_was_ucn = saved_last_was_ucn;
			return result;
		}
		catch (...)
		{
			rewind(saved);
			last_was_ucn = saved_last_was_ucn;
			throw;
		}
	}

	size_t mark() const
	{
		return pos;
	}

	void rewind(size_t saved)
	{
		pos = saved;
	}

private:

	int NextPhysical()
	{
		size_t end;
		int result = DecodeUtf8At(buf, pos, end);
		pos = end;
		return result;
	}

	int PeekPhysical(size_t index, size_t* end) const
	{
		size_t cursor = pos;
		for (size_t i = 0; i <= index; ++i)
		{
			size_t next_end;
			int result = DecodeUtf8At(buf, cursor, next_end);
			if (result == EndOfFile)
				return EndOfFile;
			cursor = next_end;
			if (i == index)
			{
				if (end != nullptr)
					*end = next_end;
				return result;
			}
		}
		return EndOfFile;
	}

	bool ConsumeLineSplice()
	{
		size_t after_first;
		int first = PeekPhysical(0, &after_first);
		if (first == '\n')
		{
			pos = after_first;
			return true;
		}
		if (first == '\r' && PeekPhysical(1, &after_first) == '\n')
		{
			pos = after_first;
			return true;
		}
		return false;
	}

	bool TryDecodeUniversalCharacterName(int& result)
	{
		size_t cursor;
		int prefix = PeekPhysical(0, &cursor);
		if (prefix != 'u' && prefix != 'U')
			return false;

		const int digits = prefix == 'u' ? 4 : 8;
		unsigned long value = 0;
		for (int i = 0; i < digits; ++i)
		{
			size_t next_cursor;
			int digit = DecodeUtf8At(buf, cursor, next_cursor);
			int hex = HexValue(digit);
			if (hex < 0)
				return false;
			value = value * 16 + hex;
			cursor = next_cursor;
		}

		if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
			throw runtime_error("Invalid unicode escape value");

		pos = cursor;
		result = static_cast<int>(value);
		return true;
	}
};

// See C++ standard 2.11 Identifiers and Appendix/Annex E.1
const vector<pair<int, int>> AnnexE1_Allowed_RangesSorted =
{
	{0xA8,0xA8},
	{0xAA,0xAA},
	{0xAD,0xAD},
	{0xAF,0xAF},
	{0xB2,0xB5},
	{0xB7,0xBA},
	{0xBC,0xBE},
	{0xC0,0xD6},
	{0xD8,0xF6},
	{0xF8,0xFF},
	{0x100,0x167F},
	{0x1681,0x180D},
	{0x180F,0x1FFF},
	{0x200B,0x200D},
	{0x202A,0x202E},
	{0x203F,0x2040},
	{0x2054,0x2054},
	{0x2060,0x206F},
	{0x2070,0x218F},
	{0x2460,0x24FF},
	{0x2776,0x2793},
	{0x2C00,0x2DFF},
	{0x2E80,0x2FFF},
	{0x3004,0x3007},
	{0x3021,0x302F},
	{0x3031,0x303F},
	{0x3040,0xD7FF},
	{0xF900,0xFD3D},
	{0xFD40,0xFDCF},
	{0xFDF0,0xFE44},
	{0xFE47,0xFFFD},
	{0x10000,0x1FFFD},
	{0x20000,0x2FFFD},
	{0x30000,0x3FFFD},
	{0x40000,0x4FFFD},
	{0x50000,0x5FFFD},
	{0x60000,0x6FFFD},
	{0x70000,0x7FFFD},
	{0x80000,0x8FFFD},
	{0x90000,0x9FFFD},
	{0xA0000,0xAFFFD},
	{0xB0000,0xBFFFD},
	{0xC0000,0xCFFFD},
	{0xD0000,0xDFFFD},
	{0xE0000,0xEFFFD}
};

// See C++ standard 2.11 Identifiers and Appendix/Annex E.2
const vector<pair<int, int>> AnnexE2_DisallowedInitially_RangesSorted =
{
	{0x300,0x36F},
	{0x1DC0,0x1DFF},
	{0x20D0,0x20FF},
	{0xFE20,0xFE2F}
};

// See C++ standard 2.13 Operators and punctuators
const unordered_set<string> Digraph_IdentifierLike_Operators =
{
	"new", "delete", "and", "and_eq", "bitand",
	"bitor", "compl", "not", "not_eq", "or",
	"or_eq", "xor", "xor_eq"
};

bool InRanges(const vector<pair<int, int>>& ranges, int codepoint)
{
	size_t low = 0;
	size_t high = ranges.size();
	while (low < high)
	{
		size_t middle = low + (high - low) / 2;
		if (codepoint < ranges[middle].first)
			high = middle;
		else if (codepoint > ranges[middle].second)
			low = middle + 1;
		else
			return true;
	}
	return false;
}

bool IsAsciiDigit(int codepoint)
{
	return codepoint >= '0' && codepoint <= '9';
}

bool IsAsciiNondigit(int codepoint)
{
	return (codepoint >= 'a' && codepoint <= 'z') ||
		(codepoint >= 'A' && codepoint <= 'Z') || codepoint == '_';
}

bool IsIdentifierNondigit(int codepoint)
{
	return IsAsciiNondigit(codepoint) ||
		InRanges(AnnexE1_Allowed_RangesSorted, codepoint) ||
		InRanges(AnnexE2_DisallowedInitially_RangesSorted, codepoint);
}

bool IsIdentifierStart(int codepoint)
{
	return IsAsciiNondigit(codepoint) ||
		(InRanges(AnnexE1_Allowed_RangesSorted, codepoint) &&
		 !InRanges(AnnexE2_DisallowedInitially_RangesSorted, codepoint));
}

bool IsIdentifierContinue(int codepoint)
{
	return IsAsciiDigit(codepoint) || IsIdentifierNondigit(codepoint);
}

bool IsWhitespace(int codepoint)
{
	return codepoint == ' ' || codepoint == '\t' || codepoint == '\v' ||
		codepoint == '\f' || codepoint == '\r';
}

void AppendCodePoint(string& token, int codepoint)
{
	token += encode_utf8(codepoint);
}

string ScanIdentifier(SourceDecoder& decoder)
{
	string token;
	while (IsIdentifierContinue(decoder.peek()))
		AppendCodePoint(token, decoder.next());
	return token;
}

string ScanPPNumber(SourceDecoder& decoder)
{
	string token;
	AppendCodePoint(token, decoder.next());

	while (true)
	{
		int codepoint = decoder.peek();
		if ((codepoint == 'e' || codepoint == 'E') &&
			(decoder.peek(1) == '+' || decoder.peek(1) == '-'))
		{
			AppendCodePoint(token, decoder.next());
			AppendCodePoint(token, decoder.next());
			continue;
		}
		if (IsAsciiDigit(codepoint) || IsIdentifierNondigit(codepoint) ||
			codepoint == '.')
		{
			AppendCodePoint(token, decoder.next());
			continue;
		}
		return token;
	}
}

bool MatchAscii(SourceDecoder& decoder, const char* spelling)
{
	for (size_t i = 0; spelling[i] != '\0'; ++i)
		if (decoder.peek(i) != static_cast<unsigned char>(spelling[i]))
			return false;
	return true;
}

string TakeAscii(SourceDecoder& decoder, const char* spelling)
{
	for (size_t i = 0; spelling[i] != '\0'; ++i)
		decoder.next();
	return string(spelling);
}

bool IsOctalDigit(int codepoint)
{
	return codepoint >= '0' && codepoint <= '7';
}

bool IsSimpleEscape(int codepoint)
{
	return codepoint == '\'' || codepoint == '"' || codepoint == '?' ||
		codepoint == '\\' || codepoint == 'a' || codepoint == 'b' ||
		codepoint == 'f' || codepoint == 'n' || codepoint == 'r' ||
		codepoint == 't' || codepoint == 'v';
}

bool IsRawDelimiterCharacter(int codepoint)
{
	return codepoint >= 0x20 && codepoint <= 0x7E &&
		codepoint != '(' && codepoint != ')' && codepoint != '\\' &&
		codepoint != '\t' && codepoint != '\v' && codepoint != '\f';
}

void ScanEscapeSequence(SourceDecoder& decoder, string& token)
{
	AppendCodePoint(token, decoder.next()); // backslash

	int escaped = decoder.peek();
	if (escaped == EndOfFile || escaped == '\n' || escaped == '\r' ||
		decoder.peek_is_ucn())
		throw runtime_error("Invalid escape sequence");

	if (IsSimpleEscape(escaped))
	{
		AppendCodePoint(token, decoder.next());
		return;
	}

	if (escaped == 'x')
	{
		AppendCodePoint(token, decoder.next());
		if (HexValue(decoder.peek()) < 0)
			throw runtime_error("Invalid hex escape sequence");
		while (HexValue(decoder.peek()) >= 0)
			AppendCodePoint(token, decoder.next());
		return;
	}

	if (IsOctalDigit(escaped))
	{
		for (int digits = 0; digits < 3 && IsOctalDigit(decoder.peek()); ++digits)
			AppendCodePoint(token, decoder.next());
		return;
	}

	throw runtime_error("Invalid escape sequence");
}

string ScanDelimitedLiteral(SourceDecoder& decoder, char delimiter,
	const char* prefix, bool character_literal)
{
	string token = TakeAscii(decoder, prefix);
	AppendCodePoint(token, decoder.next()); // opening quote
	bool has_content = false;

	while (true)
	{
		int codepoint = decoder.peek();
		bool from_ucn = decoder.peek_is_ucn();
		if (codepoint == EndOfFile || codepoint == '\n' || codepoint == '\r')
			throw runtime_error("Unterminated literal");

		if (!from_ucn && codepoint == delimiter)
		{
			AppendCodePoint(token, decoder.next());
			if (character_literal && !has_content)
				throw runtime_error("Empty character literal");
			return token;
		}

		if (!from_ucn && codepoint == '\\')
			ScanEscapeSequence(decoder, token);
		else
			AppendCodePoint(token, decoder.next());
		has_content = true;
	}
}

string ScanRawStringLiteral(SourceDecoder& decoder, const char* prefix)
{
	string token = TakeAscii(decoder, prefix);
	TakeAscii(decoder, "\"");
	token += '"';

	string delimiter;
	while (true)
	{
		int codepoint = decoder.peek();
		if (codepoint == EndOfFile)
			throw runtime_error("Unterminated raw string literal");
		if (codepoint == '(')
		{
			AppendCodePoint(token, decoder.next());
			break;
		}
		if (delimiter.size() == 16)
			throw runtime_error("Raw string delimiter too long");
		if (!IsRawDelimiterCharacter(codepoint))
			throw runtime_error("Invalid raw string delimiter");
		string character;
		AppendCodePoint(character, decoder.next());
		delimiter += character;
		token += character;
	}

	string closing = ")" + delimiter + '"';
	decoder.raw = true;
	try
	{
		while (true)
		{
			if (decoder.peek() == ')' && MatchAscii(decoder, closing.c_str()))
			{
				token += TakeAscii(decoder, closing.c_str());
				decoder.raw = false;
				return token;
			}

			if (decoder.peek() == EndOfFile)
				throw runtime_error("Unterminated raw string literal");
			AppendCodePoint(token, decoder.next());
		}
	}
	catch (...)
	{
		decoder.raw = false;
		throw;
	}
}

const char* RawStringPrefix(SourceDecoder& decoder)
{
	if (MatchAscii(decoder, "u8R\"")) return "u8R";
	if (MatchAscii(decoder, "uR\"")) return "uR";
	if (MatchAscii(decoder, "UR\"")) return "UR";
	if (MatchAscii(decoder, "LR\"")) return "LR";
	if (MatchAscii(decoder, "R\"")) return "R";
	return nullptr;
}

string ScanUserDefinedSuffix(SourceDecoder& decoder)
{
	if (!IsIdentifierStart(decoder.peek()))
		return string();
	return ScanIdentifier(decoder);
}

string ScanLessOperator(SourceDecoder& decoder)
{
	int fourth = decoder.peek(3);
	if (MatchAscii(decoder, "<::") && fourth != ':' && fourth != '>')
		return TakeAscii(decoder, "<");
	if (MatchAscii(decoder, "<<="))
		return TakeAscii(decoder, "<<=");
	if (MatchAscii(decoder, "<<"))
		return TakeAscii(decoder, "<<");
	if (MatchAscii(decoder, "<:"))
		return TakeAscii(decoder, "<:");
	if (MatchAscii(decoder, "<%"))
		return TakeAscii(decoder, "<%");
	if (MatchAscii(decoder, "<="))
		return TakeAscii(decoder, "<=");
	return TakeAscii(decoder, "<");
}

string ScanPercentOperator(SourceDecoder& decoder)
{
	if (MatchAscii(decoder, "%:%:"))
		return TakeAscii(decoder, "%:%:");
	if (MatchAscii(decoder, "%="))
		return TakeAscii(decoder, "%=");
	if (MatchAscii(decoder, "%>"))
		return TakeAscii(decoder, "%>");
	if (MatchAscii(decoder, "%:"))
		return TakeAscii(decoder, "%:");
	return TakeAscii(decoder, "%");
}

string ScanDotOperator(SourceDecoder& decoder)
{
	if (MatchAscii(decoder, "..."))
		return TakeAscii(decoder, "...");
	if (MatchAscii(decoder, ".*"))
		return TakeAscii(decoder, ".*");
	return TakeAscii(decoder, ".");
}

string ScanMinusOperator(SourceDecoder& decoder)
{
	if (MatchAscii(decoder, "->*"))
		return TakeAscii(decoder, "->*");
	if (MatchAscii(decoder, "--"))
		return TakeAscii(decoder, "--");
	if (MatchAscii(decoder, "-="))
		return TakeAscii(decoder, "-=");
	if (MatchAscii(decoder, "->"))
		return TakeAscii(decoder, "->");
	return TakeAscii(decoder, "-");
}

string ScanOperator(SourceDecoder& decoder)
{
	switch (decoder.peek())
	{
	case '<': return ScanLessOperator(decoder);
	case '%': return ScanPercentOperator(decoder);
	case '.': return ScanDotOperator(decoder);
	case '-': return ScanMinusOperator(decoder);
	case ':':
		if (MatchAscii(decoder, "::")) return TakeAscii(decoder, "::");
		if (MatchAscii(decoder, ":>")) return TakeAscii(decoder, ":>");
		return TakeAscii(decoder, ":");
	case '+':
		if (MatchAscii(decoder, "++")) return TakeAscii(decoder, "++");
		if (MatchAscii(decoder, "+=")) return TakeAscii(decoder, "+=");
		return TakeAscii(decoder, "+");
	case '*':
		if (MatchAscii(decoder, "*=")) return TakeAscii(decoder, "*=");
		return TakeAscii(decoder, "*");
	case '/':
		if (MatchAscii(decoder, "/=")) return TakeAscii(decoder, "/=");
		return TakeAscii(decoder, "/");
	case '^':
		if (MatchAscii(decoder, "^=")) return TakeAscii(decoder, "^=");
		return TakeAscii(decoder, "^");
	case '&':
		if (MatchAscii(decoder, "&&")) return TakeAscii(decoder, "&&");
		if (MatchAscii(decoder, "&=")) return TakeAscii(decoder, "&=");
		return TakeAscii(decoder, "&");
	case '|':
		if (MatchAscii(decoder, "||")) return TakeAscii(decoder, "||");
		if (MatchAscii(decoder, "|=")) return TakeAscii(decoder, "|=");
		return TakeAscii(decoder, "|");
	case '!':
		if (MatchAscii(decoder, "!=")) return TakeAscii(decoder, "!=");
		return TakeAscii(decoder, "!");
	case '=':
		if (MatchAscii(decoder, "==")) return TakeAscii(decoder, "==");
		return TakeAscii(decoder, "=");
	case '>':
		if (MatchAscii(decoder, ">>=")) return TakeAscii(decoder, ">>=");
		if (MatchAscii(decoder, ">>")) return TakeAscii(decoder, ">>");
		if (MatchAscii(decoder, ">=")) return TakeAscii(decoder, ">=");
		return TakeAscii(decoder, ">");
	case '#':
		if (MatchAscii(decoder, "##")) return TakeAscii(decoder, "##");
		return TakeAscii(decoder, "#");
	case '{': return TakeAscii(decoder, "{");
	case '}': return TakeAscii(decoder, "}");
	case '[': return TakeAscii(decoder, "[");
	case ']': return TakeAscii(decoder, "]");
	case '(' : return TakeAscii(decoder, "(");
	case ')' : return TakeAscii(decoder, ")");
	case ';': return TakeAscii(decoder, ";");
	case '?': return TakeAscii(decoder, "?");
	case '~': return TakeAscii(decoder, "~");
	case ',': return TakeAscii(decoder, ",");
	default: return string();
	}
}

void ConsumeBlockComment(SourceDecoder& decoder)
{
	decoder.next();
	decoder.next();
	while (true)
	{
		int codepoint = decoder.next();
		if (codepoint == EndOfFile)
			throw runtime_error("Unterminated comment");
		if (codepoint == '*' && decoder.peek() == '/')
		{
			decoder.next();
			return;
		}
	}
}

bool ConsumeWhitespaceSequence(SourceDecoder& decoder)
{
	bool consumed = false;
	while (true)
	{
		int codepoint = decoder.peek();
		if (IsWhitespace(codepoint))
		{
			decoder.next();
			consumed = true;
			continue;
		}
		if (codepoint != '/' || decoder.peek(1) != '/')
		{
			if (codepoint == '/' && decoder.peek(1) == '*')
			{
				ConsumeBlockComment(decoder);
				consumed = true;
				continue;
			}
			return consumed;
		}

		decoder.next();
		decoder.next();
		consumed = true;
		while (decoder.peek() != EndOfFile && decoder.peek() != '\n')
			decoder.next();
	}
}

void EmitCoreToken(SourceDecoder& decoder, IPPTokenStream& output)
{
	const char* raw_prefix = RawStringPrefix(decoder);
	if (raw_prefix != nullptr)
	{
		string token = ScanRawStringLiteral(decoder, raw_prefix);
		string suffix = ScanUserDefinedSuffix(decoder);
		token += suffix;
		if (suffix.empty())
			output.emit_string_literal(token);
		else
			output.emit_user_defined_string_literal(token);
		return;
	}

	const char* character_prefix = nullptr;
	if (MatchAscii(decoder, "u'")) character_prefix = "u";
	else if (MatchAscii(decoder, "U'")) character_prefix = "U";
	else if (MatchAscii(decoder, "L'")) character_prefix = "L";
	else if (decoder.peek() == '\'') character_prefix = "";

	if (character_prefix != nullptr)
	{
		string token = ScanDelimitedLiteral(decoder, '\'', character_prefix, true);
		string suffix = ScanUserDefinedSuffix(decoder);
		token += suffix;
		if (suffix.empty())
			output.emit_character_literal(token);
		else
			output.emit_user_defined_character_literal(token);
		return;
	}

	const char* string_prefix = nullptr;
	if (MatchAscii(decoder, "u8\"")) string_prefix = "u8";
	else if (MatchAscii(decoder, "u\"")) string_prefix = "u";
	else if (MatchAscii(decoder, "U\"")) string_prefix = "U";
	else if (MatchAscii(decoder, "L\"")) string_prefix = "L";
	else if (decoder.peek() == '"') string_prefix = "";

	if (string_prefix != nullptr)
	{
		string token = ScanDelimitedLiteral(decoder, '"', string_prefix, false);
		string suffix = ScanUserDefinedSuffix(decoder);
		token += suffix;
		if (suffix.empty())
			output.emit_string_literal(token);
		else
			output.emit_user_defined_string_literal(token);
		return;
	}

	int codepoint = decoder.peek();

	if (IsIdentifierStart(codepoint))
	{
		string token = ScanIdentifier(decoder);
		if (Digraph_IdentifierLike_Operators.count(token) != 0)
			output.emit_preprocessing_op_or_punc(token);
		else
			output.emit_identifier(token);
		return;
	}

	if (IsAsciiDigit(codepoint) ||
		(codepoint == '.' && IsAsciiDigit(decoder.peek(1))))
	{
		output.emit_pp_number(ScanPPNumber(decoder));
		return;
	}

	string operator_token = ScanOperator(decoder);
	if (!operator_token.empty())
	{
		output.emit_preprocessing_op_or_punc(operator_token);
		return;
	}

	output.emit_non_whitespace_char(encode_utf8(decoder.next()));
}

} // namespace

void PPTokenize(const string& input, IPPTokenStream& output)
{
	SourceDecoder decoder(input);
	bool saw_logical_character = false;
	bool ended_with_new_line = false;

	while (decoder.peek() != EndOfFile)
	{
		if (ConsumeWhitespaceSequence(decoder))
		{
			saw_logical_character = true;
			ended_with_new_line = false;
			output.emit_whitespace_sequence();
			continue;
		}

		if (decoder.peek() == '\n')
		{
			decoder.next();
			saw_logical_character = true;
			ended_with_new_line = true;
			output.emit_new_line();
			continue;
		}

		EmitCoreToken(decoder, output);
		saw_logical_character = true;
		ended_with_new_line = false;
	}

	if (saw_logical_character && !ended_with_new_line)
		output.emit_new_line();
	output.emit_eof();
}
