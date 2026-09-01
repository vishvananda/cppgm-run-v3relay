// (C) 2013 CPPGM Foundation www.cppgm.org.  All rights reserved.

#include <utility>
#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include <fstream>

using namespace std;

#include "exceptions.h"
#include "preproc_host.h"

// For pragma once implementation:
// system-wide unique file id type `PA5FileId`
// bootstrap system call interface, used by PA5GetFileId

// OPTIONAL: Also search `PA5StdIncPaths` on `--stdinc` command-line switch (not by default)
vector<string> PA5StdIncPaths =
{
    "/usr/include/c++/4.7/",
    "/usr/include/c++/4.7/x86_64-linux-gnu/",
    "/usr/include/c++/4.7/backward/",
    "/usr/lib/gcc/x86_64-linux-gnu/4.7/include/",
    "/usr/local/include/",
    "/usr/lib/gcc/x86_64-linux-gnu/4.7/include-fixed/",
    "/usr/include/x86_64-linux-gnu/",
    "/usr/include/"
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
		const PreprocBuildInfo build_info = PreprocHostBuildInfo();

		if (HasBatchStdinArg(argc, argv))
			return RunNotImplementedBatchMode();

		vector<string> args;

		for (int i = 1; i < argc; i++)
			args.emplace_back(argv[i]);

		if (args.size() < 3 || args[0] != "-o")
			throw logic_error("invalid usage");

		string outfile = args[1];

		ofstream out(outfile);
		if (!out)
			throw runtime_error("unable to open output file");

		vector<string> srcfiles(args.begin() + 2, args.end());
		PreprocRun(srcfiles, out, PA5GetFileId, build_info);
	}
	catch (exception& e)
	{
		cerr << "ERROR: " << e.what() << endl;
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
