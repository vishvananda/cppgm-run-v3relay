// (C) 2013 CPPGM Foundation www.cppgm.org.  All rights reserved.

#include <utility>
#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include <fstream>
#include <ctime>

using namespace std;

#include "exceptions.h"
#include "preproc_engine.h"

// For pragma once implementation:
// system-wide unique file id type `PA5FileId`
// bootstrap system call interface, used by PA5GetFileId
extern "C" long int syscall(long int n, ...) throw ();

// PA5GetFileId returns true iff file found at path `path`.
// out parameter `out_fileid` is set to file id
bool PA5GetFileId(const string& path, PA5FileId& out_fileid)
{
	struct
	{
			unsigned long int dev;
			unsigned long int ino;
			long int unused[16];
	} data;

	int res = syscall(4, path.c_str(), &data);

	out_fileid = make_pair(data.dev, data.ino);

	return res == 0;
}

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
		time_t now = time(nullptr);
		tm* local_time = localtime(&now);
		if (local_time == nullptr)
			throw runtime_error("unable to obtain build time");
		const char* asctime_snapshot = asctime(local_time);
		if (asctime_snapshot == nullptr)
			throw runtime_error("unable to obtain build time");
		const string build_stamp(asctime_snapshot);
		if (build_stamp.size() < 24)
			throw runtime_error("invalid build time");
		PreprocBuildInfo build_info;
		build_info.date = build_stamp.substr(4, 7) +
			build_stamp.substr(20, 4);
		build_info.time = build_stamp.substr(11, 8);
		build_info.author = "Vishvananda";

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
