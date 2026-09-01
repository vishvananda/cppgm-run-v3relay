// (C) 2013 CPPGM Foundation www.cppgm.org.  All rights reserved.

#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std;

#include "exceptions.h"
#include "preproc_engine.h"
#include "parser/nsdecl_model.h"
#include "parser/nsdecl_parser.h"
#include "parser/recog_token.h"

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

struct TranslationUnitModel
{
	string source;
	shared_ptr<Pa7Namespace> global;
};

shared_ptr<Pa7Namespace> AnalyzeTranslationUnit(const string& srcfile)
{
	time_t now = time(nullptr);
	tm* local_time = localtime(&now);
	if (!local_time)
		throw runtime_error("unable to obtain build time");
	const char* asctime_snapshot = asctime(local_time);
	if (!asctime_snapshot)
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

	shared_ptr<Pa7Namespace> global(new Pa7Namespace);
	Pa7Parser parser(collector.tokens, global.get());
	if (!parser.ParseTranslationUnit())
		throw runtime_error("declaration parsing failed");
	return global;
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
		if (args.size() < 3 || args[0] != "-o")
			throw logic_error("invalid usage");

		vector<TranslationUnitModel> units;
		for (size_t i = 2; i < args.size(); ++i)
		{
			TranslationUnitModel unit;
			unit.source = args[i];
			unit.global = AnalyzeTranslationUnit(args[i]);
			units.push_back(unit);
		}

		ofstream out(args[1]);
		if (!out)
			throw runtime_error("unable to open output file");
		out << units.size() << " translation units" << endl;
		for (size_t i = 0; i < units.size(); ++i)
		{
			out << "start translation unit " << units[i].source << endl;
			PrintTranslationUnit(out, *units[i].global);
			out << "end translation unit" << endl;
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
