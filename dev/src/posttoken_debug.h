#pragma once

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>

#include "posttoken_stream.h"

// Convert integer [0,15] to a hexadecimal digit.
inline char ValueToHexChar(int c)
{
	switch (c)
	{
	case 0: return '0';
	case 1: return '1';
	case 2: return '2';
	case 3: return '3';
	case 4: return '4';
	case 5: return '5';
	case 6: return '6';
	case 7: return '7';
	case 8: return '8';
	case 9: return '9';
	case 10: return 'A';
	case 11: return 'B';
	case 12: return 'C';
	case 13: return 'D';
	case 14: return 'E';
	case 15: return 'F';
	default: throw std::logic_error("ValueToHexChar of nonhex value");
	}
}

inline std::string HexDump(const void* pdata, std::size_t nbytes)
{
	const unsigned char* p = static_cast<const unsigned char*>(pdata);
	std::string s(nbytes * 2, '?');
	for (std::size_t i = 0; i < nbytes; ++i)
	{
		s[2 * i + 0] = ValueToHexChar((p[i] & 0xF0) >> 4);
		s[2 * i + 1] = ValueToHexChar((p[i] & 0x0F) >> 0);
	}
	return s;
}

// DebugPostTokenOutputStream: helper class to produce PA2 output format.
struct DebugPostTokenOutputStream : IPostTokenOutputStream
{
	explicit DebugPostTokenOutputStream(std::ostream& out = std::cout)
		: out_(out)
	{
	}

	void emit_invalid(const std::string& source) override
	{
		out_ << "invalid " << source << "\n";
	}

	void emit_simple(const std::string& source, ETokenType token_type) override
	{
		out_ << "simple " << source << " "
			<< TokenTypeToStringMap.at(token_type) << "\n";
	}

	void emit_identifier(const std::string& source) override
	{
		out_ << "identifier " << source << "\n";
	}

	void emit_literal(const std::string& source, EFundamentalType type,
		const void* data, std::size_t nbytes) override
	{
		out_ << "literal " << source << " "
			<< FundamentalTypeToStringMap.at(type) << " "
			<< HexDump(data, nbytes) << "\n";
	}

	void emit_literal_array(const std::string& source,
		std::size_t num_elements, EFundamentalType type, const void* data,
		std::size_t nbytes) override
	{
		out_ << "literal " << source << " array of " << num_elements
			<< " " << FundamentalTypeToStringMap.at(type) << " "
			<< HexDump(data, nbytes) << "\n";
	}

	void emit_user_defined_literal_character(const std::string& source,
		const std::string& ud_suffix, EFundamentalType type, const void* data,
		std::size_t nbytes) override
	{
		out_ << "user-defined-literal " << source << " " << ud_suffix
			<< " character " << FundamentalTypeToStringMap.at(type) << " "
			<< HexDump(data, nbytes) << "\n";
	}

	void emit_user_defined_literal_string_array(const std::string& source,
		const std::string& ud_suffix, std::size_t num_elements,
		EFundamentalType type, const void* data, std::size_t nbytes) override
	{
		out_ << "user-defined-literal " << source << " " << ud_suffix
			<< " string array of " << num_elements << " "
			<< FundamentalTypeToStringMap.at(type) << " "
			<< HexDump(data, nbytes) << "\n";
	}

	void emit_user_defined_literal_integer(const std::string& source,
		const std::string& ud_suffix, const std::string& prefix) override
	{
		out_ << "user-defined-literal " << source << " " << ud_suffix
			<< " integer " << prefix << "\n";
	}

	void emit_user_defined_literal_floating(const std::string& source,
		const std::string& ud_suffix, const std::string& prefix) override
	{
		out_ << "user-defined-literal " << source << " " << ud_suffix
			<< " floating " << prefix << "\n";
	}

	void emit_eof() override
	{
		out_ << "eof\n";
	}

private:
	std::ostream& out_;
};
