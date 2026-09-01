#include <iostream>
#include <sstream>
#include <string>

using namespace std;

#include "DebugPPTokenStream.h"
#include "exceptions.h"
#include "pptoken_lexer.h"

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

		DebugPPTokenStream output;
		PPTokenize(input, output);

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
