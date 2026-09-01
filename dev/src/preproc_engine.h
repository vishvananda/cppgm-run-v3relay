#pragma once

#include <functional>
#include <map>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "macro_replace.h"

typedef std::pair<unsigned long int, unsigned long int> PA5FileId;
typedef std::function<bool(const std::string&, PA5FileId&)> FileIdLookup;

struct PreprocBuildInfo
{
	std::string date;
	std::string time;
	std::string author;
};

class PreprocError : public std::runtime_error
{
public:
	explicit PreprocError(const std::string& message)
		: std::runtime_error(message)
	{
	}
};

class PreprocEngine
{
public:
	PreprocEngine(std::ostream& output, FileIdLookup file_id_lookup,
		const PreprocBuildInfo& build_info);

	void Run(const std::vector<std::string>& srcfiles);

private:
	void ProcessSourceFile(const std::string& srcfile);
	void ProcessTokens(const std::vector<PPToken>& tokens,
		PostTokenStream& output);
	int RegisterFileName(const std::string& name);
	void InstallPredefineds();

	std::ostream& output_;
	FileIdLookup file_id_lookup_;
	PreprocBuildInfo build_info_;
	std::vector<std::string> file_names_;
	std::map<std::string, int> file_name_ids_;
	MacroTable table_;
};

void PreprocRun(const std::vector<std::string>& srcfiles, std::ostream& out,
	FileIdLookup file_id_lookup, const PreprocBuildInfo& build_info);

void PreprocRun(const std::vector<std::string>& srcfiles, std::ostream& out,
	FileIdLookup file_id_lookup);
