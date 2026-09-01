// (C) 2013 CPPGM Foundation www.cppgm.org.  All rights reserved.

#include <vector>
#include <string>
#include <stdexcept>
#include <fstream>
#include <iostream>
#include <sstream>
#include <ctime>
#include <utility>

#include "exceptions.h"
#include "preproc_engine.h"
#include "parser/recog_parser.h"

using namespace std;

extern "C" long int syscall(long int n, ...) throw ();

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

bool PA6_IsClassName(const string& identifier)
{
	return (NameCategoryMask(identifier) & PA6_NAME_CLASS_FLAG) != 0;
}

bool PA6_IsTemplateName(const string& identifier)
{
	return (NameCategoryMask(identifier) & PA6_NAME_TEMPLATE_FLAG) != 0;
}

bool PA6_IsTypedefName(const string& identifier)
{
	return (NameCategoryMask(identifier) & PA6_NAME_TYPEDEF_FLAG) != 0;
}

bool PA6_IsEnumName(const string& identifier)
{
	return (NameCategoryMask(identifier) & PA6_NAME_ENUM_FLAG) != 0;
}

bool PA6_IsNamespaceName(const string& identifier)
{
	return (NameCategoryMask(identifier) & PA6_NAME_NAMESPACE_FLAG) != 0;
}

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

void DoRecog(const string& srcfile)
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

	ostringstream discarded_preproc_output;
	PreprocEngine preprocessor(discarded_preproc_output, PA5GetFileId,
		build_info);
	Pa6TokenCollector collector;
	preprocessor.RunSingleFile(srcfile, collector);
	Pa6Parser parser(collector.tokens);
	if (!parser.ParseTranslationUnit())
		throw runtime_error("recognition failed");
}

int main(int argc, char** argv)
{
	try
	{
		if (HasBatchStdinArg(argc, argv))
			return RunNotImplementedBatchMode();

		vector<string> args;

		for (int i = 1; i < argc; i++)
			args.emplace_back(argv[i]);

		if (args.size() < 3 || args[0] != "-o")
			throw logic_error("invalid usage");

		string outfile = args[1];
		size_t nsrcfiles = args.size() - 2;

		ofstream out(outfile);
		if (!out)
			throw runtime_error("unable to open output file");

		out << "recog " << nsrcfiles << endl;

		for (size_t i = 0; i < nsrcfiles; i++)
		{
			string srcfile = args[i+2];

			try
			{
				DoRecog(srcfile);
				out << srcfile << " OK" << endl;
			}
			catch (const exception& e)
			{
				cerr << e.what() << endl;
				out << srcfile << " BAD" << endl;
			}
		}
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
	return EXIT_SUCCESS;
}
