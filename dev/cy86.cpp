// (C) 2013 CPPGM Foundation www.cppgm.org.  All rights reserved.

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std;

#include "cy86_codegen.h"
#include "cy86_parse.h"
#include "exceptions.h"
#include "preproc_host.h"

extern "C" long int syscall(long int n, ...) throw ();

bool PA9SetFileExecutable(const string& path)
{
	return syscall(/* chmod */ 90, path.c_str(), 0755) == 0;
}

bool HasBatchStdinArg(int argc, char** argv)
{
	for (int i = 1; i < argc; ++i)
		if (string(argv[i]) == "--batch-stdin")
			return true;
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

vector<Cy86Token> CollectTranslationUnit(const string& srcfile,
	const PreprocBuildInfo& build_info)
{
	ostringstream discarded_preproc_output;
	PreprocEngine preprocessor(discarded_preproc_output, PA5GetFileId,
		build_info);
	Cy86TokenCollector collector;
	preprocessor.RunSingleFile(srcfile, collector);
	return collector.tokens;
}

vector<Cy86Token> CollectTokens(const vector<string>& srcfiles)
{
	const PreprocBuildInfo build_info = PreprocHostBuildInfo();
	vector<Cy86Token> tokens;
	for (size_t i = 0; i < srcfiles.size(); ++i)
	{
		vector<Cy86Token> unit = CollectTranslationUnit(srcfiles[i], build_info);
		if (!unit.empty() && unit.back().kind == CY86_EOF_TOKEN)
			unit.pop_back();
		tokens.insert(tokens.end(), unit.begin(), unit.end());
	}
	tokens.push_back(Cy86Token(CY86_EOF_TOKEN, ""));
	return tokens;
}

int main(int argc, char** argv)
{
	try
	{
		if (HasBatchStdinArg(argc, argv))
			return RunNotImplementedBatchMode();

		vector<string> args;
		for (int i = 1; i < argc; ++i)
			args.push_back(argv[i]);
		if (args.size() < 3)
			throw logic_error("invalid usage");

		string output_target;
		string outfile;
		vector<string> srcfiles;
		for (size_t i = 0; i < args.size(); ++i)
		{
			if (args[i] == "--target")
			{
				if (i + 1 >= args.size())
					throw logic_error("missing target after --target");
				output_target = args[++i];
				continue;
			}
			if (args[i] == "-o")
			{
				if (i + 1 >= args.size())
					throw logic_error("missing output file after -o");
				outfile = args[++i];
				continue;
			}
			srcfiles.push_back(args[i]);
		}
		(void)output_target;
		if (outfile.empty() || srcfiles.empty())
			throw logic_error("invalid usage");

		const vector<Cy86Token> tokens = CollectTokens(srcfiles);
		Cy86Parser parser(tokens);
		const vector<Cy86Statement> statements = parser.Parse();
		const vector<unsigned char> image = BuildProgramImage(statements);

		ofstream out(outfile.c_str(), ios::out | ios::binary | ios::trunc);
		if (!out)
			throw runtime_error("unable to open output file");
		if (!image.empty())
			out.write(reinterpret_cast<const char*>(image.data()), image.size());
		if (!out)
			throw runtime_error("unable to write output file");
		if (!PA9SetFileExecutable(outfile))
			throw runtime_error("unable to make output executable");
	}
	catch (exception& e)
	{
		cerr << "ERROR: " << e.what() << endl;
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
