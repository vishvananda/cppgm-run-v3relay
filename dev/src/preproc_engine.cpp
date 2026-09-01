#include <fstream>
#include <iterator>
#include <string>

using namespace std;

#include "IPPTokenStream.h"
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

} // namespace

PreprocEngine::PreprocEngine(ostream& output, FileIdLookup file_id_lookup,
	const PreprocBuildInfo& build_info)
	: output_(output), file_id_lookup_(file_id_lookup),
		build_info_(build_info)
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
	ifstream input(srcfile.c_str(), ios::in | ios::binary);
	if (!input)
		throw PreprocError("unable to open source file: " + srcfile);

	const string contents((istreambuf_iterator<char>(input)),
		istreambuf_iterator<char>());
	const int src_file = RegisterFileName(srcfile);

	// Each command-line source has an independent macro environment, while
	// the output stream remains local so string-literal sequences are flushed
	// exactly once at this source file's end.
	table_ = MacroTable();
	InstallPredefineds();

	PPTokenCollector collector(src_file);
	PPTokenize(contents, collector, &collector);

	PreprocPostTokenOutputStream output(output_);
	PostTokenStream posttoken_output(output);
	ProcessTokens(collector.tokens, posttoken_output);
	posttoken_output.emit_eof();
}

void PreprocEngine::ProcessTokens(const vector<PPToken>& tokens,
	PostTokenStream& output)
{
	vector<PPToken> text;
	text.reserve(tokens.size());
	size_t line_begin = 0;

	for (size_t i = 0; i < tokens.size(); ++i)
	{
		if (tokens[i].kind != PP_TOKEN_NEW_LINE)
			continue;

		vector<PPToken> line(tokens.begin() + line_begin,
			tokens.begin() + i);
		if (IsDirective(line, "define"))
		{
			MacroFlushText(text, table_, output);
			text.clear();
			table_.Define(line);
		}
		else if (IsDirective(line, "undef"))
		{
			MacroFlushText(text, table_, output);
			text.clear();
			if (line.size() != 3 ||
				line[2].kind != PP_TOKEN_IDENTIFIER ||
				line[2].data == "__VA_ARGS__")
				throw MacroError("malformed undef directive");
			table_.Undef(line[2].data);
		}
		else if (!line.empty() && IsDirectiveHash(line[0]))
		{
			MacroFlushText(text, table_, output);
			text.clear();
			if (line.size() >= 2 && line[1].kind == PP_TOKEN_IDENTIFIER &&
				line[1].data == "error")
				throw PreprocError("#error");
			if (line.size() != 1)
				throw PreprocError("invalid preprocessing directive");
		}
		else
		{
			text.insert(text.end(), line.begin(), line.end());
			text.push_back(tokens[i]);
		}
		line_begin = i + 1;
	}

	if (line_begin < tokens.size())
		text.insert(text.end(), tokens.begin() + line_begin, tokens.end());
	MacroFlushText(text, table_, output);
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
