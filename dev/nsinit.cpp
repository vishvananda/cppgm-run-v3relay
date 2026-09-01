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
#include "parser/nsinit_image.h"
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

shared_ptr<Pa7Namespace> AnalyzeTranslationUnit(const string& srcfile)
{
	const PreprocBuildInfo build_info = PreprocHostBuildInfo();

	ostringstream discarded_preproc_output;
	PreprocEngine preprocessor(discarded_preproc_output, PA5GetFileId,
		build_info);
	Pa6TokenCollector collector;
	preprocessor.RunSingleFile(srcfile, collector);

	shared_ptr<Pa7Namespace> global(new Pa7Namespace);
	Pa7Parser parser(collector.tokens, global.get(), true);
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

		vector<shared_ptr<Pa7Namespace> > globals;
		for (size_t i = 2; i < args.size(); ++i)
			globals.push_back(AnalyzeTranslationUnit(args[i]));

		Pa8ImageBuilder image_builder(globals);
		const vector<unsigned char> image = image_builder.Build();
		ofstream out(args[1].c_str(), ios::out | ios::binary | ios::trunc);
		if (!out)
			throw runtime_error("unable to open output file");
		if (!image.empty())
			out.write(reinterpret_cast<const char*>(image.data()), image.size());
		if (!out)
			throw runtime_error("unable to write output file");
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
