#pragma once

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using std::string;

#include "IPPTokenStream.h"
#include "pptoken_lexer.h"

enum EPPTokenKind
{
	PP_TOKEN_HEADER_NAME,
	PP_TOKEN_IDENTIFIER,
	PP_TOKEN_PP_NUMBER,
	PP_TOKEN_CHARACTER_LITERAL,
	PP_TOKEN_USER_DEFINED_CHARACTER_LITERAL,
	PP_TOKEN_STRING_LITERAL,
	PP_TOKEN_USER_DEFINED_STRING_LITERAL,
	PP_TOKEN_PREPROCESSING_OP_OR_PUNC,
	PP_TOKEN_NON_WHITESPACE_CHAR,
	PP_TOKEN_NEW_LINE,
	PP_TOKEN_PLACEMARKER
};

struct PaintTreeNode;
typedef std::shared_ptr<const PaintTreeNode> PaintSet;

bool PaintContains(const PaintSet& paint, const string& name);
PaintSet PaintUnion(const PaintSet& left, const PaintSet& right);
PaintSet PaintIntersect(const PaintSet& left, const PaintSet& right);
PaintSet PaintAdd(const PaintSet& paint, const string& name);

struct PPToken
{
	EPPTokenKind kind;
	string data;
	bool preceded_by_ws;
	int src_file;
	size_t src_line;
	int presumed_file;
	size_t presumed_line;
	PaintSet paint;
	bool noninvokable;

	PPToken();
	PPToken(EPPTokenKind kind, const string& data, bool preceded_by_ws);
};

struct PPTokenCollector : IPPTokenStream, IPPTokenPositionSink
{
	std::vector<PPToken> tokens;
	bool whitespace_pending;

	explicit PPTokenCollector(int src_file = 0);

	void on_token_line(size_t physical_line) override;

	void emit_whitespace_sequence() override;
	void emit_new_line() override;
	void emit_header_name(const string& data) override;
	void emit_identifier(const string& data) override;
	void emit_pp_number(const string& data) override;
	void emit_character_literal(const string& data) override;
	void emit_user_defined_character_literal(const string& data) override;
	void emit_string_literal(const string& data) override;
	void emit_user_defined_string_literal(const string& data) override;
	void emit_preprocessing_op_or_punc(const string& data) override;
	void emit_non_whitespace_char(const string& data) override;
	void emit_eof() override;

private:
	void append(EPPTokenKind kind, const string& data);

	int src_file_;
	size_t current_line_;
};

struct Macro
{
	bool function_like;
	bool variadic;
	std::vector<string> params;
	std::vector<PPToken> replacement;

	Macro();
};

typedef std::function<bool(const PPToken&, PPToken&)>
	MacroDynamicResolver;
typedef std::function<bool(const string&)> MacroDefinedPredicate;
typedef std::function<void(const string&)> MacroPragmaHandler;

class MacroError : public std::runtime_error
{
public:
	explicit MacroError(const string& message)
		: std::runtime_error(message)
	{
	}
};

class MacroTable
{
public:
	// Parse and install a complete #define directive line. The line must
	// contain the leading '#' and 'define' tokens, but not its newline.
	void Define(const std::vector<PPToken>& directive_line);

	// Install an already parsed macro for callers that build a table
	// programmatically.
	void Define(const string& name, const Macro& macro);

	void Undef(const string& name);
	const Macro* Lookup(const string& name) const;

	void SetDynamicResolver(const MacroDynamicResolver& resolver,
		const MacroDefinedPredicate& is_defined);
	bool ResolveDynamic(const PPToken& source, PPToken& replacement) const;
	bool IsDefined(const string& name) const;

private:
	std::map<string, Macro> macros_;
	MacroDynamicResolver dynamic_resolver_;
	MacroDefinedPredicate is_defined_;
};

class MacroExpander
{
public:
	typedef std::function<void(const PPToken&)> Output;

	explicit MacroExpander(const MacroTable& table);

	void Expand(const std::vector<PPToken>& input, const Output& output) const;

private:
	const MacroTable& table_;
};

struct IPostTokenOutputStream;
struct PostTokenStream;

void MacroProcessFile(const string& input, IPostTokenOutputStream& output);
void MacroFlushText(const std::vector<PPToken>& text, const MacroTable& table,
	PostTokenStream& output, const MacroPragmaHandler& pragma_handler);
