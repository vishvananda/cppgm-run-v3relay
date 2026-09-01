#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>

using namespace std;

#include "IPPTokenStream.h"
#include "ctrlexpr_eval.h"
#include "posttoken_debug.h"
#include "posttoken_stream.h"
#include "preproc_engine.h"
#include "pptoken_lexer.h"

namespace
{

bool IsDirectiveHash(const PPToken& token)
{
	return token.data == "#" || token.data == "%:";
}

bool IsDirective(const vector<PPToken>& line, const char* name)
{
	return line.size() >= 2 && IsDirectiveHash(line[0]) &&
		line[1].kind == PP_TOKEN_IDENTIFIER && line[1].data == name;
}

string QuoteString(const string& value)
{
	string result(1, '"');
	for (size_t i = 0; i < value.size(); ++i)
	{
		const char c = value[i];
		if (c == '\\' || c == '"')
			result.push_back('\\');
		result.push_back(c);
	}
	result.push_back('"');
	return result;
}

bool UnquoteStringLiteral(const string& literal, string& value)
{
	if (literal.size() < 2 || literal[0] != '"' ||
		literal[literal.size() - 1] != '"')
		return false;

	value.clear();
	for (size_t i = 1; i + 1 < literal.size(); ++i)
	{
		if (literal[i] != '\\' || i + 2 >= literal.size())
		{
			value.push_back(literal[i]);
			continue;
		}

		const char escaped = literal[++i];
		switch (escaped)
		{
		case 'a': value.push_back('\a'); break;
		case 'b': value.push_back('\b'); break;
		case 'f': value.push_back('\f'); break;
		case 'n': value.push_back('\n'); break;
		case 'r': value.push_back('\r'); break;
		case 't': value.push_back('\t'); break;
		case 'v': value.push_back('\v'); break;
		case '\\': value.push_back('\\'); break;
		case '"': value.push_back('"'); break;
		default:
			value.push_back('\\');
			value.push_back(escaped);
			break;
		}
	}
	return true;
}

bool UnquoteHeaderName(const string& header, string& value)
{
	if (header.size() < 2 ||
		(header[0] != '"' && header[0] != '<') ||
		header[header.size() - 1] !=
			(header[0] == '"' ? '"' : '>'))
		return false;
	value = header.substr(1, header.size() - 2);
	return true;
}

struct ConditionalGroup
{
	bool parent_active;
	bool taken;
	bool in_else;
	bool active;

	ConditionalGroup(bool parent_active, bool taken)
		: parent_active(parent_active), taken(taken), in_else(false),
			active(parent_active && taken)
	{
	}
};

void DefineObjectMacro(MacroTable& table, const string& name,
	EPPTokenKind kind, const string& data)
{
	Macro macro;
	macro.replacement.push_back(PPToken(kind, data, false));
	table.Define(name, macro);
}

class PreprocPostTokenOutputStream : public DebugPostTokenOutputStream
{
public:
	explicit PreprocPostTokenOutputStream(ostream& output)
		: DebugPostTokenOutputStream(output)
	{
	}

	void emit_invalid(const string& source) override
	{
		throw PreprocError("invalid posttoken: " + source);
	}
};

const size_t MaxIncludeDepth = 256;

class IncludeDepthGuard
{
public:
	explicit IncludeDepthGuard(size_t& depth)
		: depth_(depth)
	{
		++depth_;
	}

	~IncludeDepthGuard()
	{
		--depth_;
	}

private:
	size_t& depth_;
};

} // namespace

PreprocEngine::PreprocEngine(ostream& output, FileIdLookup file_id_lookup,
	const PreprocBuildInfo& build_info)
	: output_(output), file_id_lookup_(file_id_lookup),
		build_info_(build_info), counter_(0), include_depth_(0)
{
}

void PreprocEngine::Run(const vector<string>& srcfiles)
{
	output_ << "preproc " << srcfiles.size() << "\n";
	for (size_t i = 0; i < srcfiles.size(); ++i)
	{
		output_ << "sof " << srcfiles[i] << "\n";
		ProcessSourceFile(srcfiles[i]);
	}
}

int PreprocEngine::RegisterFileName(const string& name)
{
	map<string, int>::const_iterator found = file_name_ids_.find(name);
	if (found != file_name_ids_.end())
		return found->second;

	const int id = static_cast<int>(file_names_.size()) + 1;
	file_names_.push_back(name);
	file_name_ids_.insert(make_pair(name, id));
	return id;
}

void PreprocEngine::InstallPredefineds()
{
	DefineObjectMacro(table_, "__CPPGM__", PP_TOKEN_PP_NUMBER, "201303L");
	DefineObjectMacro(table_, "__cplusplus", PP_TOKEN_PP_NUMBER, "201103L");
	DefineObjectMacro(table_, "__STDC_HOSTED__", PP_TOKEN_PP_NUMBER, "1");
	DefineObjectMacro(table_, "__CPPGM_AUTHOR__", PP_TOKEN_STRING_LITERAL,
		build_info_.author.empty() ? "\"Vishvananda\"" :
			"\"" + build_info_.author + "\"");
	DefineObjectMacro(table_, "__DATE__", PP_TOKEN_STRING_LITERAL,
		"\"" + build_info_.date + "\"");
	DefineObjectMacro(table_, "__TIME__", PP_TOKEN_STRING_LITERAL,
		"\"" + build_info_.time + "\"");
}

void PreprocEngine::ProcessSourceFile(const string& srcfile)
{
	// Command-line source files are independent preprocessing translation
	// units. Includes below this call intentionally share this state.
	file_names_.clear();
	file_name_ids_.clear();
	pragma_once_files_.clear();
	include_depth_ = 0;
	table_ = MacroTable();
	InstallPredefineds();
	counter_ = 0;
	table_.SetDynamicResolver(
		[this](const PPToken& source, PPToken& replacement)
		{
			return ResolveDynamicPredefined(source, replacement);
		},
		[this](const string& name)
		{
			return IsDynamicPredefined(name);
		});

	PreprocPostTokenOutputStream output(output_);
	PostTokenStream posttoken_output(output);
	ProcessSourceFile(srcfile, posttoken_output);
	posttoken_output.emit_eof();
}

void PreprocEngine::ProcessSourceFile(const string& srcfile,
	PostTokenStream& output)
{
	if (include_depth_ >= MaxIncludeDepth)
		throw PreprocError("include nesting too deep");
	IncludeDepthGuard depth_guard(include_depth_);

	ifstream input(srcfile.c_str(), ios::in | ios::binary);
	if (!input)
		throw PreprocError("unable to open source file: " + srcfile);

	const string contents((istreambuf_iterator<char>(input)),
		istreambuf_iterator<char>());
	const int src_file = RegisterFileName(srcfile);

	PPTokenCollector collector(src_file);
	PPTokenize(contents, collector, &collector);
	ProcessTokens(collector.tokens, output, src_file);
}

bool PreprocEngine::EvaluateCondition(const vector<PPToken>& line)
{
	CtrlExprIsDefined is_defined = [this](const string& name)
	{
		return table_.IsDefined(name);
	};
	vector<PPToken> expression(line.begin() + 2, line.end());
	if (!ResolveDefinedOperands(expression, is_defined))
		throw PreprocError("malformed defined operator");

	vector<PPToken> expanded;
	MacroExpander expander(table_);
	try
	{
		expander.Expand(expression, [&expanded](const PPToken& token)
		{
			expanded.push_back(token);
		});
	}
	catch (const MacroError& error)
	{
		throw PreprocError(error.what());
	}

	if (!ResolveDefinedOperands(expanded, is_defined))
		throw PreprocError("malformed defined operator");
	const EvalResult result =
		EvaluateControllingExpression(expanded, is_defined);
	if (result.error)
		throw PreprocError("error in controlling expression");
	return result.value != 0;
}

bool PreprocEngine::ParseLineDirective(const vector<PPToken>& line,
	int current_file, size_t& line_number, int& file_number)
{
	vector<PPToken> values(line.begin() + 2, line.end());
	vector<PPToken> expanded;
	MacroExpander expander(table_);
	try
	{
		expander.Expand(values, [&expanded](const PPToken& token)
		{
			expanded.push_back(token);
		});
	}
	catch (const MacroError& error)
	{
		throw PreprocError(error.what());
	}

	if (expanded.size() < 1 || expanded.size() > 2 ||
		expanded[0].kind != PP_TOKEN_PP_NUMBER)
		return false;

	const string& number = expanded[0].data;
	if (number.empty())
		return false;
	for (size_t i = 0; i < number.size(); ++i)
		if (number[i] < '0' || number[i] > '9')
			return false;

	errno = 0;
	char* end = 0;
	const unsigned long long parsed =
		strtoull(number.c_str(), &end, 10);
	if (errno == ERANGE || end == 0 || *end != '\0' ||
		parsed > static_cast<unsigned long long>(
			std::numeric_limits<size_t>::max()))
		return false;
	line_number = static_cast<size_t>(parsed);
	file_number = current_file;

	if (expanded.size() == 2)
	{
		string filename;
		if (expanded[1].kind != PP_TOKEN_STRING_LITERAL ||
			!UnquoteStringLiteral(expanded[1].data, filename))
			return false;
		file_number = RegisterFileName(filename);
	}
	return true;
}

bool PreprocEngine::PathCanBeOpened(const string& path) const
{
	ifstream input(path.c_str(), ios::in | ios::binary);
	return static_cast<bool>(input);
}

bool PreprocEngine::LookupFileId(const string& path,
	PA5FileId& file_id) const
{
	return file_id_lookup_ && file_id_lookup_(path, file_id);
}

bool PreprocEngine::ResolveIncludePath(const string& include_name,
	int presumed_file, string& resolved_path, PA5FileId& resolved_id,
	bool& have_resolved_id) const
{
	vector<string> candidates;
	if (presumed_file > 0 &&
		static_cast<size_t>(presumed_file) <= file_names_.size())
	{
		const string& presumed_name =
			file_names_[static_cast<size_t>(presumed_file) - 1];
		const size_t slash = presumed_name.rfind('/');
		if (slash != string::npos)
			candidates.push_back(presumed_name.substr(0, slash + 1) +
				include_name);
	}
	candidates.push_back(include_name);

	for (size_t i = 0; i < candidates.size(); ++i)
	{
		if (i != 0 && candidates[i] == candidates[i - 1])
			continue;
		if (!PathCanBeOpened(candidates[i]))
			continue;

		PA5FileId file_id = make_pair(0UL, 0UL);
		const bool have_file_id = LookupFileId(candidates[i], file_id);
		if (file_id_lookup_ && !have_file_id)
			continue;

		resolved_path = candidates[i];
		resolved_id = file_id;
		have_resolved_id = have_file_id;
		return true;
	}
	return false;
}

void PreprocEngine::ProcessInclude(const vector<PPToken>& line,
	int presumed_file, PostTokenStream& output)
{
	string include_name;
	if (line.size() == 3 && line[2].kind == PP_TOKEN_HEADER_NAME)
	{
		if (!UnquoteHeaderName(line[2].data, include_name))
			throw PreprocError("malformed include directive");
	}
	else
	{
		vector<PPToken> values(line.begin() + 2, line.end());
		vector<PPToken> expanded;
		MacroExpander expander(table_);
		try
		{
			expander.Expand(values, [&expanded](const PPToken& token)
			{
				expanded.push_back(token);
			});
		}
		catch (const MacroError& error)
		{
			throw PreprocError(error.what());
		}
		if (expanded.size() != 1 ||
			expanded[0].kind != PP_TOKEN_STRING_LITERAL ||
			!UnquoteStringLiteral(expanded[0].data, include_name))
			throw PreprocError("malformed include directive");
	}

	string resolved_path;
	PA5FileId resolved_id = make_pair(0UL, 0UL);
	bool have_resolved_id = false;
	if (!ResolveIncludePath(include_name, presumed_file, resolved_path,
		resolved_id, have_resolved_id))
		throw PreprocError("unable to resolve include: " + include_name);
	if (have_resolved_id &&
		pragma_once_files_.find(resolved_id) != pragma_once_files_.end())
		return;

	ProcessSourceFile(resolved_path, output);
}

void PreprocEngine::MarkPragmaOnce(int current_file)
{
	if (current_file <= 0 ||
		static_cast<size_t>(current_file) > file_names_.size())
		return;

	PA5FileId file_id = make_pair(0UL, 0UL);
	if (LookupFileId(file_names_[static_cast<size_t>(current_file) - 1],
		file_id))
		pragma_once_files_.insert(file_id);
}

void PreprocEngine::ProcessPragma(const vector<PPToken>& line,
	int current_file)
{
	if (line.size() == 3 && line[2].kind == PP_TOKEN_IDENTIFIER &&
		line[2].data == "once")
		MarkPragmaOnce(current_file);
}

void PreprocEngine::ProcessPragmaText(const string& text, int current_file)
{
	if (text == "once")
		MarkPragmaOnce(current_file);
}

void PreprocEngine::ProcessTokens(const vector<PPToken>& tokens,
	PostTokenStream& output, int current_file)
{
	vector<PPToken> text;
	text.reserve(tokens.size());
	vector<ConditionalGroup> groups;
	bool active = true;
	size_t line_begin = 0;
	size_t next_physical_line = 1;
	size_t presumed_line = 1;
	int presumed_file = current_file;
	bool line_mapping_reset = false;

	auto flush_text = [this, &text, &output, current_file]()
	{
		if (!text.empty())
		{
			MacroFlushText(text, table_, output,
				[this, current_file](const string& pragma)
				{
					ProcessPragmaText(pragma, current_file);
				});
			text.clear();
		}
	};

	for (size_t i = 0; i < tokens.size(); ++i)
	{
		if (tokens[i].kind != PP_TOKEN_NEW_LINE)
			continue;

		vector<PPToken> line(tokens.begin() + line_begin,
			tokens.begin() + i);
		const size_t physical_start = line.empty() ? tokens[i].src_line :
			line.front().src_line;
		if (!line_mapping_reset && physical_start > next_physical_line)
			presumed_line += physical_start - next_physical_line;
		line_mapping_reset = false;

		for (size_t j = 0; j < line.size(); ++j)
		{
			const size_t offset = line[j].src_line >= physical_start
				? line[j].src_line - physical_start : 0;
			line[j].presumed_file = presumed_file;
			line[j].presumed_line = presumed_line + offset;
		}
		PPToken newline = tokens[i];
		newline.presumed_file = presumed_file;
		newline.presumed_line = presumed_line +
			(newline.src_line >= physical_start
				? newline.src_line - physical_start : 0);

		bool line_override = false;
		size_t overridden_line = 0;
		int overridden_file = presumed_file;

		if (IsDirective(line, "if"))
		{
			flush_text();
			const bool parent_active = active;
			bool condition = false;
			if (parent_active)
				condition = EvaluateCondition(line);
			groups.push_back(ConditionalGroup(parent_active, condition));
			active = groups.back().active;
		}
		else if (IsDirective(line, "ifdef") ||
			IsDirective(line, "ifndef"))
		{
			flush_text();
			const bool parent_active = active;
			bool condition = false;
			if (parent_active)
			{
				if (line.size() != 3 ||
					line[2].kind != PP_TOKEN_IDENTIFIER)
					throw PreprocError("malformed conditional directive");
				condition = table_.IsDefined(line[2].data);
				if (IsDirective(line, "ifndef"))
					condition = !condition;
			}
			groups.push_back(ConditionalGroup(parent_active, condition));
			active = groups.back().active;
		}
		else if (IsDirective(line, "elif"))
		{
			flush_text();
			if (groups.empty() || groups.back().in_else)
				throw PreprocError("misplaced elif");
			ConditionalGroup& group = groups.back();
			if (!group.parent_active || group.taken)
			{
				group.active = false;
				active = false;
			}
			else
			{
				if (!IsDirective(line, "elif"))
					throw PreprocError("malformed elif");
				const bool condition = EvaluateCondition(line);
				group.active = condition;
				group.taken = condition;
				active = condition;
			}
		}
		else if (IsDirective(line, "else"))
		{
			flush_text();
			if (groups.empty() || groups.back().in_else)
				throw PreprocError("misplaced else");
			ConditionalGroup& group = groups.back();
			const bool branch_active = group.parent_active && !group.taken;
			if (branch_active && line.size() != 2)
				throw PreprocError("malformed else");
			group.in_else = true;
			group.active = branch_active;
			group.taken = true;
			active = branch_active;
		}
		else if (IsDirective(line, "endif"))
		{
			flush_text();
			if (groups.empty())
				throw PreprocError("misplaced endif");
			if (groups.back().active && line.size() != 2)
				throw PreprocError("malformed endif");
			groups.pop_back();
			active = groups.empty() ? true : groups.back().active;
		}
		else if (active && IsDirective(line, "define"))
		{
			flush_text();
			table_.Define(line);
		}
		else if (active && IsDirective(line, "undef"))
		{
			flush_text();
			if (line.size() != 3 ||
				line[2].kind != PP_TOKEN_IDENTIFIER ||
				line[2].data == "__VA_ARGS__")
				throw PreprocError("malformed undef directive");
			table_.Undef(line[2].data);
		}
			else if (active && IsDirective(line, "line"))
			{
				flush_text();
				if (!ParseLineDirective(line, presumed_file, overridden_line,
					overridden_file))
					throw PreprocError("malformed line directive");
				line_override = true;
			}
			else if (active && IsDirective(line, "include"))
			{
				flush_text();
				ProcessInclude(line, presumed_file, output);
			}
			else if (active && IsDirective(line, "pragma"))
			{
				flush_text();
				ProcessPragma(line, current_file);
			}
			else if (!line.empty() && IsDirectiveHash(line[0]))
		{
			if (active)
			{
				flush_text();
				if (line.size() >= 2 &&
					line[1].kind == PP_TOKEN_IDENTIFIER &&
					line[1].data == "error")
					throw PreprocError("#error");
				if (line.size() != 1)
					throw PreprocError("invalid preprocessing directive");
			}
		}
		else if (active)
		{
			text.insert(text.end(), line.begin(), line.end());
			text.push_back(newline);
		}

		const size_t physical_end = tokens[i].src_line;
		next_physical_line = physical_end + 1;
		if (line_override)
		{
			presumed_line = overridden_line;
			presumed_file = overridden_file;
			line_mapping_reset = true;
		}
		else
		{
			presumed_line += physical_end >= physical_start
				? physical_end - physical_start + 1 : 1;
		}
		line_begin = i + 1;
	}

	if (line_begin < tokens.size() && active)
	{
		text.insert(text.end(), tokens.begin() + line_begin, tokens.end());
	}
	flush_text();
	if (!groups.empty())
		throw PreprocError("unterminated conditional group");
}

bool PreprocEngine::IsDynamicPredefined(const string& name) const
{
	return name == "__FILE__" || name == "__LINE__" ||
		name == "__COUNTER__";
}

bool PreprocEngine::ResolveDynamicPredefined(const PPToken& source,
	PPToken& replacement)
{
	if (!IsDynamicPredefined(source.data))
		return false;

	if (source.data == "__COUNTER__")
	{
		ostringstream value;
		value << counter_++;
		replacement = PPToken(PP_TOKEN_PP_NUMBER, value.str(),
			source.preceded_by_ws);
	}
	else if (source.data == "__LINE__")
	{
		ostringstream value;
		value << (source.presumed_line != 0 ? source.presumed_line :
			source.src_line);
		replacement = PPToken(PP_TOKEN_PP_NUMBER, value.str(),
			source.preceded_by_ws);
	}
	else
	{
		const int file = source.presumed_file != 0 ? source.presumed_file :
			source.src_file;
		if (file <= 0 || static_cast<size_t>(file) > file_names_.size())
			return false;
		replacement = PPToken(PP_TOKEN_STRING_LITERAL,
			QuoteString(file_names_[static_cast<size_t>(file) - 1]),
			source.preceded_by_ws);
	}

	replacement.src_file = source.src_file;
	replacement.src_line = source.src_line;
	replacement.presumed_file = source.presumed_file;
	replacement.presumed_line = source.presumed_line;
	replacement.paint = source.paint;
	return true;
}

void PreprocRun(const vector<string>& srcfiles, ostream& out,
	FileIdLookup file_id_lookup, const PreprocBuildInfo& build_info)
{
	PreprocEngine engine(out, file_id_lookup, build_info);
	engine.Run(srcfiles);
}

void PreprocRun(const vector<string>& srcfiles, ostream& out,
	FileIdLookup file_id_lookup)
{
	PreprocBuildInfo build_info;
	PreprocRun(srcfiles, out, file_id_lookup, build_info);
}
