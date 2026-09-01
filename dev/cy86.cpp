// (C) 2013 CPPGM Foundation www.cppgm.org.  All rights reserved.

#include <ctime>
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
#include "preproc_engine.h"

struct ElfHeader
{
	unsigned char ident[16] =
	{
		0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};
	short int type = 2;
	short int machine = 0x3E;
	int version = 1;
	long int entry;
	long int phoff = 64;
	long int shoff = 0;
	int processor_flags = 0;
	short int ehsize = 64;
	short int phentsize = 56;
	short int phnum = 1;
	short int shentsize = 0;
	short int shnum = 0;
	short int shstrndx = 0;
};

struct ProgramSegmentHeader
{
	int type = 1;

	static constexpr int executable = 1 << 0;
	static constexpr int writable = 1 << 1;
	static constexpr int readable = 1 << 2;

	int flags = executable | writable | readable;
	long int offset = 0;
	long int vaddr = 0x400000;
	long int paddr = 0;
	long int filesz;
	long int memsz;
	long int align = 0;
};

extern "C" long int syscall(long int n, ...) throw ();

bool PA5GetFileId(const string& path, PA5FileId& out_fileid)
{
	struct
	{
		unsigned long int dev;
		unsigned long int ino;
		long int unused[16];
	} data;
	const int result = syscall(4, path.c_str(), &data);
	out_fileid = make_pair(data.dev, data.ino);
	return result == 0;
}

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

PreprocBuildInfo BuildInfo()
{
	time_t now = time(nullptr);
	tm* local_time = localtime(&now);
	if (!local_time)
		throw runtime_error("unable to obtain build time");
	const char* asctime_snapshot = asctime(local_time);
	if (!asctime_snapshot)
		throw runtime_error("unable to obtain build time");
	const string stamp(asctime_snapshot);
	if (stamp.size() < 24)
		throw runtime_error("invalid build time");
	PreprocBuildInfo result;
	result.date = stamp.substr(4, 7) + stamp.substr(20, 4);
	result.time = stamp.substr(11, 8);
	result.author = "Vishvananda";
	return result;
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
	const PreprocBuildInfo build_info = BuildInfo();
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
