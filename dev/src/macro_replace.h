#pragma once

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using std::string;

#include "IPPTokenStream.h"

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

typedef std::vector<string> PaintSet;

bool PaintContains(const PaintSet& paint, const string& name);
PaintSet PaintUnion(const PaintSet& left, const PaintSet& right);
PaintSet PaintIntersect(const PaintSet& left, const PaintSet& right);

struct PPToken
{
	EPPTokenKind kind;
	string data;
	bool preceded_by_ws;
	PaintSet paint;
	bool noninvokable;

	PPToken();
	PPToken(EPPTokenKind kind, const string& data, bool preceded_by_ws);
};

struct PPTokenCollector : IPPTokenStream
{
	std::vector<PPToken> tokens;
	bool whitespace_pending;

	PPTokenCollector();

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
};

struct Macro
{
	bool function_like;
	bool variadic;
	std::vector<string> params;
	std::vector<PPToken> replacement;

	Macro();
};

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

private:
	std::map<string, Macro> macros_;
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

void MacroProcessFile(const string& input, IPostTokenOutputStream& output);
