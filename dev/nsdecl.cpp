// (C) 2013 CPPGM Foundation www.cppgm.org.  All rights reserved.

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
#include "preproc_host.h"
#include "parser/nsdecl_model.h"
#include "parser/nsdecl_parser.h"
#include "parser/recog_token.h"


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
	const PreprocBuildInfo build_info = PreprocHostBuildInfo();

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
