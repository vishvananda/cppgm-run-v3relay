#include <algorithm>
#include <deque>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace std;

#include "macro_replace.h"
#include "posttoken_stream.h"
#include "pptoken_lexer.h"

namespace
{

bool IsData(const PPToken& token, const char* data)
{
	return token.data == data;
}

bool IsIdentifier(const PPToken& token)
{
	return token.kind == PP_TOKEN_IDENTIFIER;
}

void AddPaint(PaintSet& paint, const string& name)
{
	PaintSet::iterator position = lower_bound(paint.begin(), paint.end(), name);
	if (position == paint.end() || *position != name)
		paint.insert(position, name);
}

void MarkPainted(PPToken& token)
{
	if (token.kind == PP_TOKEN_IDENTIFIER &&
		PaintContains(token.paint, token.data))
		token.noninvokable = true;
}

bool HasSemanticToken(const vector<PPToken>& tokens)
{
	for (size_t i = 0; i < tokens.size(); ++i)
		if (tokens[i].kind != PP_TOKEN_NEW_LINE)
			return true;
	return false;
}

bool MacroEquivalent(const Macro& left, const Macro& right)
{
	if (left.function_like != right.function_like ||
		left.variadic != right.variadic ||
		left.params != right.params ||
		left.replacement.size() != right.replacement.size())
		return false;

	for (size_t i = 0; i < left.replacement.size(); ++i)
	{
		const PPToken& a = left.replacement[i];
		const PPToken& b = right.replacement[i];
		if (a.kind != b.kind || a.data != b.data)
			return false;
		// The first replacement token's whitespace comes from the
		// invocation site when the macro expands. All later flags are
		// part of the replacement-list identity.
		if (i != 0 && a.preceded_by_ws != b.preceded_by_ws)
			return false;
	}
	return true;
}

void ValidateMacro(const string& name, const Macro& macro)
{
	if (name.empty() || name == "__VA_ARGS__")
		throw MacroError("invalid macro name");

	map<string, bool> parameters;
	for (size_t i = 0; i < macro.params.size(); ++i)
	{
		const string& parameter = macro.params[i];
		if (parameter.empty() || parameter == "__VA_ARGS__" ||
			parameters.find(parameter) != parameters.end())
			throw MacroError("invalid or repeated macro parameter");
		parameters[parameter] = true;
	}

	if (!macro.replacement.empty() &&
		(macro.replacement.front().data == "##" ||
		 macro.replacement.back().data == "##"))
		throw MacroError("token paste at replacement-list boundary");

	for (size_t i = 0; i < macro.replacement.size(); ++i)
	{
		const PPToken& token = macro.replacement[i];
		if (token.kind == PP_TOKEN_IDENTIFIER &&
			token.data == "__VA_ARGS__" && !macro.variadic)
			throw MacroError("__VA_ARGS__ outside variadic macro");

		if (macro.function_like && token.data == "#")
		{
			if (i + 1 >= macro.replacement.size())
				throw MacroError("stringize operator without parameter");
			const PPToken& following = macro.replacement[i + 1];
			const bool parameter =
				IsIdentifier(following) &&
				parameters.find(following.data) != parameters.end();
			const bool varargs = macro.variadic &&
				IsIdentifier(following) && following.data == "__VA_ARGS__";
			if (!parameter && !varargs)
				throw MacroError("stringize operator without parameter");
		}
	}
}

void InstallMacro(map<string, Macro>& macros, const string& name,
	const Macro& macro)
{
	ValidateMacro(name, macro);
	map<string, Macro>::iterator old = macros.find(name);
	if (old != macros.end())
	{
		if (!MacroEquivalent(old->second, macro))
			throw MacroError("incompatible macro redefinition");
		return;
	}
	macros.insert(make_pair(name, macro));
}

struct Invocation
{
	vector<vector<PPToken> > arguments;
	PPToken closing;
};

bool TakeInvocation(deque<PPToken>& pending, Invocation& invocation)
{
	size_t opening_index = 0;
	while (opening_index < pending.size() &&
		pending[opening_index].kind == PP_TOKEN_NEW_LINE)
		++opening_index;

	if (opening_index == pending.size() ||
		!IsData(pending[opening_index], "("))
		return false;

	vector<PPToken> current;
	vector<vector<PPToken> > arguments;
	bool saw_top_level_comma = false;
	int depth = 0;
	size_t closing_index = pending.size();

	for (size_t i = opening_index + 1; i < pending.size(); ++i)
	{
		const PPToken& token = pending[i];
		if (IsData(token, "("))
		{
			++depth;
			current.push_back(token);
			continue;
		}

		if (IsData(token, ")"))
		{
			if (depth != 0)
			{
				--depth;
				current.push_back(token);
				continue;
			}

			closing_index = i;
			invocation.closing = token;
			break;
		}

		if (IsData(token, ",") && depth == 0)
		{
			arguments.push_back(current);
			current.clear();
			saw_top_level_comma = true;
			continue;
		}
		current.push_back(token);
	}

	if (closing_index == pending.size())
		throw MacroError("unterminated macro invocation");

	if (saw_top_level_comma || HasSemanticToken(current))
		arguments.push_back(current);
	invocation.arguments.swap(arguments);
	pending.erase(pending.begin(), pending.begin() + closing_index + 1);
	return true;
}

void ValidateArgumentCount(const Macro& macro, vector<vector<PPToken> >& args)
{
	if (!macro.variadic)
	{
		if (args.empty() && !macro.params.empty())
			args.push_back(vector<PPToken>());
		if (args.size() != macro.params.size())
			throw MacroError("wrong number of macro arguments");
		return;
	}

	if (args.empty() && !macro.params.empty())
		args.push_back(vector<PPToken>());
	if (args.size() < macro.params.size())
		throw MacroError("not enough macro arguments");
}

bool UsesUnsupportedReplacementOperator(const Macro& macro)
{
	for (size_t i = 0; i < macro.replacement.size(); ++i)
		if (macro.replacement[i].data == "#" ||
			macro.replacement[i].data == "##")
			return true;
	return false;
}

void Prepend(deque<PPToken>& pending, const vector<PPToken>& tokens)
{
	for (vector<PPToken>::const_reverse_iterator it = tokens.rbegin();
		it != tokens.rend(); ++it)
		pending.push_front(*it);
}

PPToken ObjectReplacementToken(const PPToken& source, const PPToken& head,
	const string& macro_name, bool first)
{
	PPToken result = source;
	result.paint = PaintUnion(source.paint, head.paint);
	AddPaint(result.paint, macro_name);
	if (first)
		result.preceded_by_ws = head.preceded_by_ws;
	MarkPainted(result);
	return result;
}

PPToken FunctionReplacementToken(const PPToken& source,
	const PPToken& head, const PPToken& closing, const string& macro_name,
	bool first)
{
	PPToken result = source;
	result.paint = PaintIntersect(head.paint, closing.paint);
	AddPaint(result.paint, macro_name);
	if (first)
		result.preceded_by_ws = head.preceded_by_ws;
	MarkPainted(result);
	return result;
}

void AppendSubstitutedArgument(vector<PPToken>& replacement,
	const vector<PPToken>& argument, const PPToken& occurrence,
	const PPToken& head, const string& macro_name,
	bool replacement_list_helper_head)
{
	for (size_t i = 0; i < argument.size(); ++i)
	{
		PPToken token = argument[i];
		if (!replacement_list_helper_head)
			token.paint = PaintUnion(token.paint, head.paint);
		AddPaint(token.paint, macro_name);
		if (i == 0)
			token.preceded_by_ws = occurrence.preceded_by_ws;
		MarkPainted(token);
		replacement.push_back(token);
	}
}

vector<PPToken> PreExpandArgument(const MacroTable& table,
	const vector<PPToken>& argument)
{
	vector<PPToken> result;
	MacroExpander expander(table);
	expander.Expand(argument, [&result](const PPToken& token)
	{
		result.push_back(token);
	});
	return result;
}

vector<PPToken> BuildObjectReplacement(const Macro& macro,
	const PPToken& head)
{
	vector<PPToken> result;
	for (size_t i = 0; i < macro.replacement.size(); ++i)
		result.push_back(ObjectReplacementToken(macro.replacement[i], head,
			head.data, i == 0));
	return result;
}

vector<PPToken> BuildFunctionReplacement(const Macro& macro,
	const Invocation& invocation, const PPToken& head,
	const MacroTable& table)
{
	vector<vector<PPToken> > expanded(macro.params.size());
	vector<bool> expanded_ready(macro.params.size(), false);
	map<string, size_t> parameter_indexes;
	for (size_t i = 0; i < macro.params.size(); ++i)
		parameter_indexes[macro.params[i]] = i;

	vector<PPToken> result;
	for (size_t i = 0; i < macro.replacement.size(); ++i)
	{
		const PPToken& source = macro.replacement[i];
		if (source.kind == PP_TOKEN_IDENTIFIER)
		{
			map<string, size_t>::const_iterator parameter =
				parameter_indexes.find(source.data);
			if (parameter != parameter_indexes.end())
			{
				const size_t index = parameter->second;
				if (!expanded_ready[index])
				{
					expanded[index] = PreExpandArgument(table,
						invocation.arguments[index]);
					expanded_ready[index] = true;
				}
				const bool helper_head = i + 1 < macro.replacement.size() &&
					IsData(macro.replacement[i + 1], "(");
				AppendSubstitutedArgument(result, expanded[index], source,
					head, head.data, helper_head);
				continue;
			}
			if (source.data == "__VA_ARGS__")
				throw MacroError("variadic substitution is not available");
		}

		result.push_back(FunctionReplacementToken(source, head,
			invocation.closing, head.data, result.empty()));
	}
	return result;
}

void ReplayToken(PostTokenStream& output, const PPToken& token)
{
	switch (token.kind)
	{
	case PP_TOKEN_HEADER_NAME:
		output.emit_header_name(token.data);
		break;
	case PP_TOKEN_IDENTIFIER:
		output.emit_identifier(token.data);
		break;
	case PP_TOKEN_PP_NUMBER:
		output.emit_pp_number(token.data);
		break;
	case PP_TOKEN_CHARACTER_LITERAL:
		output.emit_character_literal(token.data);
		break;
	case PP_TOKEN_USER_DEFINED_CHARACTER_LITERAL:
		output.emit_user_defined_character_literal(token.data);
		break;
	case PP_TOKEN_STRING_LITERAL:
		output.emit_string_literal(token.data);
		break;
	case PP_TOKEN_USER_DEFINED_STRING_LITERAL:
		output.emit_user_defined_string_literal(token.data);
		break;
	case PP_TOKEN_PREPROCESSING_OP_OR_PUNC:
		output.emit_preprocessing_op_or_punc(token.data);
		break;
	case PP_TOKEN_NON_WHITESPACE_CHAR:
		output.emit_non_whitespace_char(token.data);
		break;
	case PP_TOKEN_NEW_LINE:
		output.emit_new_line();
		break;
	case PP_TOKEN_PLACEMARKER:
		throw MacroError("placemarker escaped replacement");
	}
}

void ValidateTextTokens(const vector<PPToken>& tokens)
{
	for (size_t i = 0; i < tokens.size(); ++i)
		if (tokens[i].kind == PP_TOKEN_IDENTIFIER &&
			tokens[i].data == "__VA_ARGS__")
			throw MacroError("__VA_ARGS__ outside variadic macro");
}

void FlushText(const vector<PPToken>& text, const MacroTable& table,
	PostTokenStream& output)
{
	if (text.empty())
		return;
	ValidateTextTokens(text);
	MacroExpander expander(table);
	expander.Expand(text, [&output](const PPToken& token)
	{
		ReplayToken(output, token);
	});
}

bool IsDirective(const vector<PPToken>& line, const char* name)
{
	return line.size() >= 2 && IsData(line[0], "#") &&
		line[1].kind == PP_TOKEN_IDENTIFIER && line[1].data == name;
}

void ProcessUndef(const vector<PPToken>& line, MacroTable& table)
{
	if (line.size() != 3 || !IsIdentifier(line[2]) ||
		line[2].data == "__VA_ARGS__")
		throw MacroError("malformed undef directive");
	table.Undef(line[2].data);
}

} // namespace

PaintSet PaintUnion(const PaintSet& left, const PaintSet& right)
{
	PaintSet result;
	set_union(left.begin(), left.end(), right.begin(), right.end(),
		back_inserter(result));
	return result;
}

PaintSet PaintIntersect(const PaintSet& left, const PaintSet& right)
{
	PaintSet result;
	set_intersection(left.begin(), left.end(), right.begin(), right.end(),
		back_inserter(result));
	return result;
}

bool PaintContains(const PaintSet& paint, const string& name)
{
	return binary_search(paint.begin(), paint.end(), name);
}

PPToken::PPToken()
	: kind(PP_TOKEN_NON_WHITESPACE_CHAR), preceded_by_ws(false),
		noninvokable(false)
{
}

PPToken::PPToken(EPPTokenKind kind, const string& data, bool preceded_by_ws)
	: kind(kind), data(data), preceded_by_ws(preceded_by_ws),
		noninvokable(false)
{
}

PPTokenCollector::PPTokenCollector()
	: whitespace_pending(false)
{
}

void PPTokenCollector::append(EPPTokenKind kind, const string& data)
{
	tokens.push_back(PPToken(kind, data, whitespace_pending));
	whitespace_pending = false;
}

void PPTokenCollector::emit_whitespace_sequence()
{
	whitespace_pending = true;
}

void PPTokenCollector::emit_new_line()
{
	append(PP_TOKEN_NEW_LINE, string());
}

void PPTokenCollector::emit_header_name(const string& data)
{
	append(PP_TOKEN_HEADER_NAME, data);
}

void PPTokenCollector::emit_identifier(const string& data)
{
	append(PP_TOKEN_IDENTIFIER, data);
}

void PPTokenCollector::emit_pp_number(const string& data)
{
	append(PP_TOKEN_PP_NUMBER, data);
}

void PPTokenCollector::emit_character_literal(const string& data)
{
	append(PP_TOKEN_CHARACTER_LITERAL, data);
}

void PPTokenCollector::emit_user_defined_character_literal(const string& data)
{
	append(PP_TOKEN_USER_DEFINED_CHARACTER_LITERAL, data);
}

void PPTokenCollector::emit_string_literal(const string& data)
{
	append(PP_TOKEN_STRING_LITERAL, data);
}

void PPTokenCollector::emit_user_defined_string_literal(const string& data)
{
	append(PP_TOKEN_USER_DEFINED_STRING_LITERAL, data);
}

void PPTokenCollector::emit_preprocessing_op_or_punc(const string& data)
{
	append(PP_TOKEN_PREPROCESSING_OP_OR_PUNC, data);
}

void PPTokenCollector::emit_non_whitespace_char(const string& data)
{
	append(PP_TOKEN_NON_WHITESPACE_CHAR, data);
}

void PPTokenCollector::emit_eof()
{
}

Macro::Macro()
	: function_like(false), variadic(false)
{
}

void MacroTable::Define(const vector<PPToken>& directive_line)
{
	if (directive_line.size() < 3 || !IsData(directive_line[0], "#") ||
		directive_line[1].kind != PP_TOKEN_IDENTIFIER ||
		directive_line[1].data != "define")
		throw MacroError("malformed define directive");

	size_t cursor = 2;
	if (!IsIdentifier(directive_line[cursor]) ||
		directive_line[cursor].data == "__VA_ARGS__")
		throw MacroError("missing or invalid macro name");
	const string name = directive_line[cursor].data;
	++cursor;

	Macro macro;
	if (cursor < directive_line.size() &&
		IsData(directive_line[cursor], "(") &&
		!directive_line[cursor].preceded_by_ws)
	{
		macro.function_like = true;
		++cursor;
		if (cursor >= directive_line.size())
			throw MacroError("unterminated macro parameter list");

		if (IsData(directive_line[cursor], ")"))
		{
			++cursor;
		}
		else
		{
			while (true)
			{
				if (cursor >= directive_line.size())
					throw MacroError("unterminated macro parameter list");

				if (IsData(directive_line[cursor], "..."))
				{
					macro.variadic = true;
					++cursor;
					if (cursor >= directive_line.size() ||
						!IsData(directive_line[cursor], ")"))
						throw MacroError("variadic parameter must be last");
					++cursor;
					break;
				}

				if (!IsIdentifier(directive_line[cursor]) ||
					directive_line[cursor].data == "__VA_ARGS__")
					throw MacroError("invalid macro parameter");
				macro.params.push_back(directive_line[cursor].data);
				++cursor;

				if (cursor >= directive_line.size())
					throw MacroError("unterminated macro parameter list");
				if (IsData(directive_line[cursor], ")"))
				{
					++cursor;
					break;
				}
				if (!IsData(directive_line[cursor], ","))
					throw MacroError("invalid macro parameter separator");
				++cursor;
			}
		}
	}
	else if (cursor < directive_line.size() &&
		!directive_line[cursor].preceded_by_ws)
	{
		throw MacroError("object-like replacement needs whitespace");
	}

	macro.replacement.assign(directive_line.begin() + cursor,
		directive_line.end());
	InstallMacro(macros_, name, macro);
}

void MacroTable::Define(const string& name, const Macro& macro)
{
	InstallMacro(macros_, name, macro);
}

void MacroTable::Undef(const string& name)
{
	macros_.erase(name);
}

const Macro* MacroTable::Lookup(const string& name) const
{
	map<string, Macro>::const_iterator it = macros_.find(name);
	return it == macros_.end() ? 0 : &it->second;
}

MacroExpander::MacroExpander(const MacroTable& table)
	: table_(table)
{
}

void MacroExpander::Expand(const vector<PPToken>& input,
	const Output& output) const
{
	deque<PPToken> pending(input.begin(), input.end());
	while (!pending.empty())
	{
		PPToken head = pending.front();
		pending.pop_front();

		if (head.kind != PP_TOKEN_IDENTIFIER)
		{
			output(head);
			continue;
		}

		const Macro* macro = table_.Lookup(head.data);
		if (macro == 0 || head.noninvokable ||
			PaintContains(head.paint, head.data))
		{
			if (PaintContains(head.paint, head.data))
				head.noninvokable = true;
			output(head);
			continue;
		}

		if (!macro->function_like)
		{
			Prepend(pending, BuildObjectReplacement(*macro, head));
			continue;
		}

		Invocation invocation;
		if (!TakeInvocation(pending, invocation))
		{
			output(head);
			continue;
		}

		ValidateArgumentCount(*macro, invocation.arguments);
		if (UsesUnsupportedReplacementOperator(*macro))
			throw MacroError("replacement operators are not available");

		Prepend(pending, BuildFunctionReplacement(*macro, invocation, head,
			table_));
	}
}

void MacroProcessFile(const string& input, IPostTokenOutputStream& output)
{
	PPTokenCollector collector;
	PPTokenize(input, collector);

	PostTokenStream posttoken_output(output);
	MacroTable table;
	vector<PPToken> text;
	size_t line_begin = 0;

	for (size_t i = 0; i < collector.tokens.size(); ++i)
	{
		if (collector.tokens[i].kind != PP_TOKEN_NEW_LINE)
			continue;

		vector<PPToken> line(collector.tokens.begin() + line_begin,
			collector.tokens.begin() + i);
		if (IsDirective(line, "define"))
		{
			FlushText(text, table, posttoken_output);
			text.clear();
			table.Define(line);
		}
		else if (IsDirective(line, "undef"))
		{
			FlushText(text, table, posttoken_output);
			text.clear();
			ProcessUndef(line, table);
		}
		else
		{
			text.insert(text.end(), line.begin(), line.end());
			text.push_back(collector.tokens[i]);
		}
		line_begin = i + 1;
	}

	if (line_begin < collector.tokens.size())
	{
		text.insert(text.end(), collector.tokens.begin() + line_begin,
			collector.tokens.end());
	}
	FlushText(text, table, posttoken_output);
	posttoken_output.emit_eof();
}
