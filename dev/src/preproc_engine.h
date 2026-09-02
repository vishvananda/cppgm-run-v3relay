#pragma once

#include <functional>
#include <map>
#include <ostream>
#include <set>
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

struct IPostTokenOutputStream;

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
	void RunSingleFile(const std::string& srcfile,
		IPostTokenOutputStream& sink);

private:
	void ResetSourceFileState();
	void ProcessSourceFile(const std::string& srcfile);
	void ProcessSourceFile(const std::string& srcfile,
		PostTokenStream& output);
	void ProcessTokens(const std::vector<PPToken>& tokens,
		PostTokenStream& output, int current_file);
	void ProcessInclude(const std::vector<PPToken>& line,
		int presumed_file, PostTokenStream& output);
	void ProcessPragma(const std::vector<PPToken>& line, int presumed_file,
		PostTokenStream& output);
	void ProcessPragmaText(const std::string& text, int presumed_file,
		PostTokenStream& output);
	void MarkPragmaOnce(int presumed_file);
	bool ResolveIncludePath(const std::string& include_name,
		int presumed_file, std::string& resolved_path,
		PA5FileId& resolved_id, bool& have_resolved_id) const;
	bool PathCanBeOpened(const std::string& path) const;
	bool LookupFileId(const std::string& path, PA5FileId& file_id) const;
	bool EvaluateCondition(const std::vector<PPToken>& line);
	bool ParseLineDirective(const std::vector<PPToken>& line,
		int current_file, std::size_t& line_number,
		int& file_number);
	int RegisterFileName(const std::string& name);
	void InstallPredefineds();
	bool ResolveDynamicPredefined(const PPToken& source,
		PPToken& replacement);
	bool IsDynamicPredefined(const std::string& name) const;

	std::ostream& output_;
	FileIdLookup file_id_lookup_;
	PreprocBuildInfo build_info_;
	std::vector<std::string> file_names_;
	std::map<std::string, int> file_name_ids_;
	std::set<PA5FileId> pragma_once_files_;
	std::set<std::string> undefined_dynamic_;
	MacroTable table_;
	std::size_t counter_;
	std::size_t include_depth_;
};

void PreprocRun(const std::vector<std::string>& srcfiles, std::ostream& out,
	FileIdLookup file_id_lookup, const PreprocBuildInfo& build_info);

void PreprocRun(const std::vector<std::string>& srcfiles, std::ostream& out,
	FileIdLookup file_id_lookup);
