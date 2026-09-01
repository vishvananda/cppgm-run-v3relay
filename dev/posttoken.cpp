// (C) 2013 CPPGM Foundation www.cppgm.org.  All rights reserved.

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

using namespace std;

#include "IPPTokenStream.h"
#include "exceptions.h"
#include "posttoken_stream.h"
#include "pptoken_lexer.h"

// convert integer [0,15] to hexadecimal digit
char ValueToHexChar(int c)
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
	default: throw logic_error("ValueToHexChar of nonhex value");
	}
}

// hex dump memory range
string HexDump(const void* pdata, size_t nbytes)
{
	const unsigned char* p = static_cast<const unsigned char*>(pdata);
	string s(nbytes * 2, '?');

	for (size_t i = 0; i < nbytes; i++)
	{
		s[2 * i + 0] = ValueToHexChar((p[i] & 0xF0) >> 4);
		s[2 * i + 1] = ValueToHexChar((p[i] & 0x0F) >> 0);
	}

	return s;
}

// DebugPostTokenOutputStream: helper class to produce PA2 output format
struct DebugPostTokenOutputStream : IPostTokenOutputStream
{
	// output: invalid <source>
	void emit_invalid(const string& source) override
	{
		cout << "invalid " << source << "\n";
	}

	// output: simple <source> <token_type>
	void emit_simple(const string& source, ETokenType token_type) override
	{
		cout << "simple " << source << " " << TokenTypeToStringMap.at(token_type) << "\n";
	}

	// output: identifier <source>
	void emit_identifier(const string& source) override
	{
		cout << "identifier " << source << "\n";
	}

	// output: literal <source> <type> <hexdump(data,nbytes)>
	void emit_literal(const string& source, EFundamentalType type,
		const void* data, size_t nbytes) override
	{
		cout << "literal " << source << " "
			<< FundamentalTypeToStringMap.at(type) << " "
			<< HexDump(data, nbytes) << "\n";
	}

	// output: literal <source> array of <num_elements> <type> <hexdump(data,nbytes)>
	void emit_literal_array(const string& source, size_t num_elements,
		EFundamentalType type, const void* data, size_t nbytes) override
	{
		cout << "literal " << source << " array of " << num_elements << " "
			<< FundamentalTypeToStringMap.at(type) << " "
			<< HexDump(data, nbytes) << "\n";
	}

	// output: user-defined-literal <source> <ud_suffix> character <type> <hexdump(data,nbytes)>
	void emit_user_defined_literal_character(const string& source,
		const string& ud_suffix, EFundamentalType type, const void* data,
		size_t nbytes) override
	{
		cout << "user-defined-literal " << source << " " << ud_suffix
			<< " character " << FundamentalTypeToStringMap.at(type) << " "
			<< HexDump(data, nbytes) << "\n";
	}

	// output: user-defined-literal <source> <ud_suffix> string array of <num_elements> <type> <hexdump(data, nbytes)>
	void emit_user_defined_literal_string_array(const string& source,
		const string& ud_suffix, size_t num_elements, EFundamentalType type,
		const void* data, size_t nbytes) override
	{
		cout << "user-defined-literal " << source << " " << ud_suffix
			<< " string array of " << num_elements << " "
			<< FundamentalTypeToStringMap.at(type) << " "
			<< HexDump(data, nbytes) << "\n";
	}

	// output: user-defined-literal <source> <ud_suffix> <prefix>
	void emit_user_defined_literal_integer(const string& source,
		const string& ud_suffix, const string& prefix) override
	{
		cout << "user-defined-literal " << source << " " << ud_suffix
			<< " integer " << prefix << "\n";
	}

	// output: user-defined-literal <source> <ud_suffix> <prefix>
	void emit_user_defined_literal_floating(const string& source,
		const string& ud_suffix, const string& prefix) override
	{
		cout << "user-defined-literal " << source << " " << ud_suffix
			<< " floating " << prefix << "\n";
	}

	// output : eof
	void emit_eof() override
	{
		cout << "eof" << "\n";
	}
};

bool HasBatchStdinArg(int argc, char** argv)
{
	for (int i = 1; i < argc; i++)
	{
		if (string(argv[i]) == "--batch-stdin")
			return true;
	}
	return false;
}

int RunNotImplementedBatchMode()
{
	string line;
	while (getline(cin, line))
	{
		(void)line;
		cout << "EXIT_NOT_IMPLEMENTED" << endl;
	}
	return EXIT_SUCCESS;
}

int main(int argc, char** argv)
{
	try
	{
		if (HasBatchStdinArg(argc, argv))
			return RunNotImplementedBatchMode();

		ostringstream oss;
		oss << cin.rdbuf();
		string input = oss.str();

		DebugPostTokenOutputStream output;
		PostTokenStream posttoken_output(output);
		PPTokenize(input, posttoken_output);

		return EXIT_SUCCESS;
	}
	catch (const NotImplementedException& e)
	{
		cerr << "ERROR: " << e.what() << endl;
		return CPPGM_EXIT_NOT_IMPLEMENTED;
	}
	catch (exception& e)
	{
		cerr << "ERROR: " << e.what() << endl;
		return EXIT_FAILURE;
	}
}
