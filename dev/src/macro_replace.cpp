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

struct PaintTreeNode
{
	string name;
	PaintSet left;
	PaintSet right;
	size_t height;
	size_t size;

	PaintTreeNode(const string& name, const PaintSet& left,
		const PaintSet& right)
		: name(name), left(left), right(right),
			height(1 + max(left ? left->height : 0,
				right ? right->height : 0)),
			size(1 + (left ? left->size : 0) +
				(right ? right->size : 0))
	{
	}
};

namespace
{

PaintSet MakePaintNode(const string& name, const PaintSet& left,
	const PaintSet& right)
{
	return PaintSet(new PaintTreeNode(name, left, right));
}

int PaintHeight(const PaintSet& node)
{
	return node ? static_cast<int>(node->height) : 0;
}

size_t PaintSize(const PaintSet& node)
{
	return node ? node->size : 0;
}

PaintSet RotateRight(const PaintSet& node)
{
	const PaintSet pivot = node->left;
	const PaintSet moved = pivot->right;
	return MakePaintNode(pivot->name,
		pivot->left, MakePaintNode(node->name, moved, node->right));
}

PaintSet RotateLeft(const PaintSet& node)
{
	const PaintSet pivot = node->right;
	const PaintSet moved = pivot->left;
	return MakePaintNode(pivot->name,
		MakePaintNode(node->name, node->left, moved), pivot->right);
}

PaintSet BalancePaintNode(const PaintSet& node)
{
	const int balance = PaintHeight(node->left) - PaintHeight(node->right);
	if (balance > 1)
	{
		PaintSet root = node;
		if (PaintHeight(root->left->left) <
			PaintHeight(root->left->right))
		{
			root = MakePaintNode(root->name, RotateLeft(root->left),
				root->right);
		}
		return RotateRight(root);
	}
	if (balance < -1)
	{
		PaintSet root = node;
		if (PaintHeight(root->right->right) <
			PaintHeight(root->right->left))
		{
			root = MakePaintNode(root->name, root->left,
				RotateRight(root->right));
		}
		return RotateLeft(root);
	}
	return node;
}

PaintSet InsertPaint(const PaintSet& node, const string& name)
{
	if (!node)
		return MakePaintNode(name, PaintSet(), PaintSet());
	if (name == node->name)
		return node;
	if (name < node->name)
	{
		return BalancePaintNode(MakePaintNode(node->name,
			InsertPaint(node->left, name), node->right));
	}
	return BalancePaintNode(MakePaintNode(node->name, node->left,
		InsertPaint(node->right, name)));
}

void InsertPaintTree(const PaintSet& source, PaintSet& result)
{
	if (!source)
		return;
	InsertPaintTree(source->left, result);
	result = InsertPaint(result, source->name);
	InsertPaintTree(source->right, result);
}

void IntersectPaintTree(const PaintSet& source, const PaintSet& other,
	PaintSet& result)
{
	if (!source)
		return;
	IntersectPaintTree(source->left, other, result);
	if (PaintContains(other, source->name))
		result = InsertPaint(result, source->name);
	IntersectPaintTree(source->right, other, result);
}

bool IsData(const PPToken& token, const char* data)
{
	return token.data == data;
}

bool IsDirectiveHash(const PPToken& token)
{
	return IsData(token, "#") || IsData(token, "%:");
}

bool IsIdentifier(const PPToken& token)
{
	return token.kind == PP_TOKEN_IDENTIFIER;
}

void AddPaint(PaintSet& paint, const string& name)
{
	paint = PaintAdd(paint, name);
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

		// A # in a function-like replacement list must be followed by a
		// parameter, but only when the macro has parameters to name. A
		// zero-parameter macro's # instead stringizes its next replacement
		// token at invocation time (reference-pinned).
		if (macro.function_like && token.data == "#" &&
			(macro.variadic || !macro.params.empty()))
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
	vector<PPToken> commas;
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
			invocation.commas.push_back(token);
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

void NormalizeArguments(const Macro& macro, Invocation& invocation)
{
	vector<vector<PPToken> >& args = invocation.arguments;
	if (args.empty() && !macro.params.empty())
		args.push_back(vector<PPToken>());
	if (args.size() < macro.params.size())
		throw MacroError("not enough macro arguments");
	if (macro.variadic || args.size() == macro.params.size())
		return;

	// Excess arguments to a non-variadic macro merge, commas included,
	// into the last named parameter; a zero-parameter macro accepts only
	// an empty argument list (reference-pinned).
	if (macro.params.empty())
		throw MacroError("wrong number of macro arguments");
	const size_t last = macro.params.size() - 1;
	vector<PPToken>& merged = args[last];
	for (size_t i = last + 1; i < args.size(); ++i)
	{
		if (i - 1 >= invocation.commas.size())
			throw MacroError("missing macro argument separator");
		merged.push_back(invocation.commas[i - 1]);
		merged.insert(merged.end(), args[i].begin(), args[i].end());
	}
	args.resize(macro.params.size());
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

bool IsReplacementData(const PPToken& token)
{
	return token.kind != PP_TOKEN_NEW_LINE &&
		token.kind != PP_TOKEN_PLACEMARKER;
}

bool HasReplacementData(const vector<PPToken>& tokens)
{
	for (size_t i = 0; i < tokens.size(); ++i)
		if (IsReplacementData(tokens[i]))
			return true;
	return false;
}

struct ReplacementElement
{
	bool paste;
	PPToken token;

	ReplacementElement()
		: paste(true)
	{
	}

	explicit ReplacementElement(const PPToken& token)
		: paste(false), token(token)
	{
	}
};

bool HasVisibleReplacement(const vector<ReplacementElement>& replacement)
{
	for (size_t i = 0; i < replacement.size(); ++i)
		if (!replacement[i].paste &&
			replacement[i].token.kind != PP_TOKEN_PLACEMARKER)
			return true;
	return false;
}

void AppendSubstitutedArgument(vector<ReplacementElement>& replacement,
	const vector<PPToken>& argument, const PPToken& occurrence,
	const PPToken& head, const string& macro_name,
	bool replacement_list_helper_head, bool adjacent_to_paste)
{
	if (!HasReplacementData(argument))
	{
		if (adjacent_to_paste)
		{
			PPToken placemarker(PP_TOKEN_PLACEMARKER, string(),
				occurrence.preceded_by_ws);
			replacement.push_back(ReplacementElement(placemarker));
		}
		return;
	}

	for (size_t i = 0; i < argument.size(); ++i)
	{
		PPToken token = argument[i];
		if (!replacement_list_helper_head)
			token.paint = PaintUnion(token.paint, head.paint);
		AddPaint(token.paint, macro_name);
		if (i == 0)
			token.preceded_by_ws = occurrence.preceded_by_ws;
		MarkPainted(token);
		replacement.push_back(ReplacementElement(token));
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

PPToken StringizeArgument(const vector<PPToken>& argument)
{
	string spelling(1, '"');
	PaintSet paint;
	bool have_token = false;
	bool whitespace_pending = false;

	for (size_t i = 0; i < argument.size(); ++i)
	{
		const PPToken& token = argument[i];
		if (token.kind == PP_TOKEN_NEW_LINE)
		{
			if (have_token)
				whitespace_pending = true;
			continue;
		}
		if (token.kind == PP_TOKEN_PLACEMARKER)
			continue;

		if (have_token && (whitespace_pending || token.preceded_by_ws))
			spelling.push_back(' ');
		have_token = true;
		whitespace_pending = false;
		paint = PaintUnion(paint, token.paint);

		const bool literal =
			token.kind == PP_TOKEN_STRING_LITERAL ||
			token.kind == PP_TOKEN_USER_DEFINED_STRING_LITERAL ||
			token.kind == PP_TOKEN_CHARACTER_LITERAL ||
			token.kind == PP_TOKEN_USER_DEFINED_CHARACTER_LITERAL;
		for (size_t j = 0; j < token.data.size(); ++j)
		{
			const char c = token.data[j];
			if (literal && (c == '\\' || c == '"'))
				spelling.push_back('\\');
			spelling.push_back(c);
		}
	}

	spelling.push_back('"');
	PPToken result(PP_TOKEN_STRING_LITERAL, spelling, false);
	result.paint = paint;
	return result;
}

PPToken PasteTokens(const PPToken& left, const PPToken& right)
{
	if (left.kind == PP_TOKEN_PLACEMARKER &&
		right.kind == PP_TOKEN_PLACEMARKER)
	{
		return PPToken(PP_TOKEN_PLACEMARKER, string(),
			left.preceded_by_ws);
	}
	if (left.kind == PP_TOKEN_PLACEMARKER)
		return right;
	if (right.kind == PP_TOKEN_PLACEMARKER)
		return left;

	PPTokenCollector collector;
	PPTokenize(left.data + right.data + "\n", collector);
	vector<PPToken> data;
	for (size_t i = 0; i < collector.tokens.size(); ++i)
		if (collector.tokens[i].kind != PP_TOKEN_NEW_LINE)
			data.push_back(collector.tokens[i]);
	if (data.size() != 1)
		throw MacroError("token paste did not form one preprocessing token");

	PPToken result = data[0];
	result.preceded_by_ws = left.preceded_by_ws;
	result.paint = PaintUnion(left.paint, right.paint);
	MarkPainted(result);
	return result;
}

vector<PPToken> ResolvePastes(vector<ReplacementElement> replacement)
{
	for (size_t i = 0; i < replacement.size(); ++i)
	{
		if (!replacement[i].paste)
			continue;

		size_t left = i;
		while (left != 0)
		{
			--left;
			if (replacement[left].paste)
				throw MacroError("token paste without a left operand");
			if (replacement[left].token.kind != PP_TOKEN_NEW_LINE)
				break;
		}
		if (replacement[left].paste ||
			replacement[left].token.kind == PP_TOKEN_NEW_LINE)
			throw MacroError("token paste without a left operand");

		size_t right = i + 1;
		while (right < replacement.size())
		{
			if (replacement[right].paste)
				throw MacroError("token paste without a right operand");
			if (replacement[right].token.kind != PP_TOKEN_NEW_LINE)
				break;
			++right;
		}
		if (right == replacement.size() ||
			replacement[right].token.kind == PP_TOKEN_NEW_LINE)
			throw MacroError("token paste without a right operand");

		replacement[left].token = PasteTokens(replacement[left].token,
			replacement[right].token);
		replacement.erase(replacement.begin() + left + 1,
			replacement.begin() + right + 1);
		i = left;
	}

	vector<PPToken> result;
	for (size_t i = 0; i < replacement.size(); ++i)
	{
		if (replacement[i].paste)
			throw MacroError("unresolved token paste");
		if (replacement[i].token.kind != PP_TOKEN_PLACEMARKER)
			result.push_back(replacement[i].token);
	}
	return result;
}

vector<PPToken> BuildObjectReplacement(const Macro& macro,
	const PPToken& head)
{
	vector<ReplacementElement> replacement;
	for (size_t i = 0; i < macro.replacement.size(); ++i)
	{
		const PPToken& source = macro.replacement[i];
		if (IsData(source, "##"))
		{
			replacement.push_back(ReplacementElement());
			continue;
		}
		replacement.push_back(ReplacementElement(
			ObjectReplacementToken(source, head, head.data,
				!HasVisibleReplacement(replacement))));
	}
	return ResolvePastes(replacement);
}

class FunctionReplacementBuilder
{
public:
	FunctionReplacementBuilder(const Macro& macro, const Invocation& invocation,
		const PPToken& head, const MacroTable& table)
		: macro_(macro), invocation_(invocation), head_(head),
			table_(table), expanded_(macro.params.size()),
			expanded_ready_(macro.params.size(), false),
			variadic_ready_(false)
	{
		for (size_t i = 0; i < macro_.params.size(); ++i)
			parameter_indexes_[macro_.params[i]] = i;
		variadic_raw_ = BuildVariadicArgument();
	}

	vector<PPToken> Build()
	{
		vector<ReplacementElement> replacement;
		for (size_t i = 0; i < macro_.replacement.size(); ++i)
		{
			const PPToken& source = macro_.replacement[i];

			if (IsData(source, ",") && i + 2 < macro_.replacement.size() &&
				IsData(macro_.replacement[i + 1], "##") &&
				IsVariadicParameter(macro_.replacement[i + 2]))
			{
				if (HasReplacementData(variadic_raw_))
				{
					AppendLiteral(replacement, source);
					AppendArgument(replacement, macro_.replacement[i + 2],
						false, false, false);
				}
				++i;
				++i;
				continue;
			}

			if (IsData(source, "##"))
			{
				replacement.push_back(ReplacementElement());
				continue;
			}

			if (IsData(source, "#"))
			{
				if (i + 1 >= macro_.replacement.size())
					throw MacroError("stringize operator without parameter");
				bool varargs;
				size_t index;
				// In a zero-parameter macro the operand is never a
				// parameter; # stringizes the next replacement token
				// itself, even a # or ## token (reference-pinned).
				vector<PPToken> literal_operand;
				const vector<PPToken>* argument;
				if (FindParameter(macro_.replacement[i + 1], varargs, index))
					argument = &RawArgument(varargs, index);
				else
				{
					literal_operand.push_back(macro_.replacement[i + 1]);
					argument = &literal_operand;
				}
				PPToken stringized = StringizeArgument(*argument);
				stringized.preceded_by_ws = source.preceded_by_ws;
				PPToken transformed = FunctionReplacementToken(stringized, head_,
					invocation_.closing, head_.data,
					!HasVisibleReplacement(replacement));
				transformed.paint = PaintUnion(transformed.paint,
					stringized.paint);
				MarkPainted(transformed);
				replacement.push_back(ReplacementElement(transformed));
				++i;
				continue;
			}

			bool varargs;
			size_t index;
		if (FindParameter(source, varargs, index))
		{
			const bool adjacent_to_paste =
				(i != 0 && IsData(macro_.replacement[i - 1], "##")) ||
				(i + 1 < macro_.replacement.size() &&
					IsData(macro_.replacement[i + 1], "##"));
			const bool raw = adjacent_to_paste;
			const bool helper_head = i + 1 < macro_.replacement.size() &&
				IsData(macro_.replacement[i + 1], "(");
			AppendArgument(replacement, source, raw, helper_head,
				adjacent_to_paste);
			continue;
		}

			AppendLiteral(replacement, source);
		}
		return ResolvePastes(replacement);
	}

private:
	vector<PPToken> BuildVariadicArgument() const
	{
		vector<PPToken> result;
		const size_t first = macro_.params.size();
		for (size_t i = first; i < invocation_.arguments.size(); ++i)
		{
			if (i != first)
			{
				if (i - 1 >= invocation_.commas.size())
					throw MacroError("missing variadic argument separator");
				result.push_back(invocation_.commas[i - 1]);
			}
			result.insert(result.end(), invocation_.arguments[i].begin(),
				invocation_.arguments[i].end());
		}
		return result;
	}

	bool IsVariadicParameter(const PPToken& token) const
	{
		return macro_.variadic && token.kind == PP_TOKEN_IDENTIFIER &&
			token.data == "__VA_ARGS__";
	}

	bool FindParameter(const PPToken& token, bool& varargs, size_t& index) const
	{
		varargs = false;
		index = 0;
		if (token.kind != PP_TOKEN_IDENTIFIER)
			return false;
		map<string, size_t>::const_iterator parameter =
			parameter_indexes_.find(token.data);
		if (parameter != parameter_indexes_.end())
		{
			index = parameter->second;
			return true;
		}
		if (IsVariadicParameter(token))
		{
			varargs = true;
			return true;
		}
		return false;
	}

	const vector<PPToken>& RawArgument(bool varargs, size_t index) const
	{
		return varargs ? variadic_raw_ : invocation_.arguments[index];
	}

	const vector<PPToken>& ExpandedArgument(bool varargs, size_t index)
	{
		if (varargs)
		{
			if (!variadic_ready_)
			{
				variadic_expanded_ = PreExpandArgument(table_, variadic_raw_);
				variadic_ready_ = true;
			}
			return variadic_expanded_;
		}
		if (!expanded_ready_[index])
		{
			expanded_[index] = PreExpandArgument(table_,
				invocation_.arguments[index]);
			expanded_ready_[index] = true;
		}
		return expanded_[index];
	}

	void AppendLiteral(vector<ReplacementElement>& replacement,
		const PPToken& source)
	{
		replacement.push_back(ReplacementElement(FunctionReplacementToken(
			source, head_, invocation_.closing, head_.data,
			!HasVisibleReplacement(replacement))));
	}

	void AppendArgument(vector<ReplacementElement>& replacement,
		const PPToken& source, bool raw, bool helper_head,
		bool adjacent_to_paste)
	{
		bool varargs;
		size_t index;
		if (!FindParameter(source, varargs, index))
			throw MacroError("unknown macro parameter");
		const vector<PPToken>& argument = raw ? RawArgument(varargs, index) :
			ExpandedArgument(varargs, index);
		AppendSubstitutedArgument(replacement, argument, source, head_,
			head_.data, helper_head, adjacent_to_paste);
	}

	const Macro& macro_;
	const Invocation& invocation_;
	const PPToken& head_;
	const MacroTable& table_;
	vector<vector<PPToken> > expanded_;
	vector<bool> expanded_ready_;
	map<string, size_t> parameter_indexes_;
	vector<PPToken> variadic_raw_;
	vector<PPToken> variadic_expanded_;
	bool variadic_ready_;
};

vector<PPToken> BuildFunctionReplacement(const Macro& macro,
	const Invocation& invocation, const PPToken& head,
	const MacroTable& table)
{
	FunctionReplacementBuilder builder(macro, invocation, head, table);
	return builder.Build();
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

void FlushText(const vector<PPToken>& text, const MacroTable& table,
	PostTokenStream& output)
{
	if (text.empty())
		return;
	MacroExpander expander(table);
	expander.Expand(text, [&output](const PPToken& token)
	{
		ReplayToken(output, token);
	});
}

} // namespace

void MacroFlushText(const vector<PPToken>& text, const MacroTable& table,
	PostTokenStream& output)
{
	FlushText(text, table, output);
}

namespace
{

bool IsDirective(const vector<PPToken>& line, const char* name)
{
	return line.size() >= 2 && IsDirectiveHash(line[0]) &&
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
	if (!left)
		return right;
	if (!right)
		return left;
	if (PaintSize(left) < PaintSize(right))
	{
		PaintSet result = right;
		InsertPaintTree(left, result);
		return result;
	}
	PaintSet result = left;
	InsertPaintTree(right, result);
	return result;
}

PaintSet PaintIntersect(const PaintSet& left, const PaintSet& right)
{
	if (!left || !right)
		return PaintSet();
	if (left == right)
		return left;
	const PaintSet& source = PaintSize(left) < PaintSize(right) ? left : right;
	const PaintSet& other = PaintSize(left) < PaintSize(right) ? right : left;
	PaintSet result;
	IntersectPaintTree(source, other, result);
	return result;
}

bool PaintContains(const PaintSet& paint, const string& name)
{
	PaintSet node = paint;
	while (node)
	{
		if (name == node->name)
			return true;
		node = name < node->name ? node->left : node->right;
	}
	return false;
}

PaintSet PaintAdd(const PaintSet& paint, const string& name)
{
	struct PaintAddKey
	{
		PaintSet parent;
		string name;

		bool operator<(const PaintAddKey& other) const
		{
			if (parent.get() != other.parent.get())
				return std::less<const PaintTreeNode*>()(parent.get(),
					other.parent.get());
			return name < other.name;
		}
	};

	static map<PaintAddKey, PaintSet> cache;
	const PaintAddKey key = { paint, name };
	map<PaintAddKey, PaintSet>::const_iterator found = cache.find(key);
	if (found != cache.end())
		return found->second;

	PaintSet result = InsertPaint(paint, name);
	cache.insert(make_pair(key, result));
	return result;
}

PPToken::PPToken()
	: kind(PP_TOKEN_NON_WHITESPACE_CHAR), preceded_by_ws(false),
		src_file(0), src_line(0),
		noninvokable(false)
{
}

PPToken::PPToken(EPPTokenKind kind, const string& data, bool preceded_by_ws)
	: kind(kind), data(data), preceded_by_ws(preceded_by_ws),
		src_file(0), src_line(0),
		noninvokable(false)
{
}

PPTokenCollector::PPTokenCollector(int src_file)
	: whitespace_pending(false), src_file_(src_file), current_line_(0)
{
}

void PPTokenCollector::on_token_line(size_t physical_line)
{
	current_line_ = physical_line;
}

void PPTokenCollector::append(EPPTokenKind kind, const string& data)
{
	PPToken token(kind, data, whitespace_pending);
	token.src_file = src_file_;
	token.src_line = current_line_;
	tokens.push_back(token);
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
	if (directive_line.size() < 3 || !IsDirectiveHash(directive_line[0]) ||
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

		// __VA_ARGS__ is an error exactly when the rescan examines it as
		// an expansion candidate. Occurrences consumed whole by # or ##,
		// or collected into an argument that is never rescanned, are
		// legal spellings (reference-pinned).
		if (head.data == "__VA_ARGS__")
			throw MacroError("__VA_ARGS__ outside variadic macro");

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

		NormalizeArguments(*macro, invocation);
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
		else if (!line.empty() && IsDirectiveHash(line[0]))
		{
			// A line-initial # always starts a directive. The null
			// directive is a no-op; any other unrecognized directive is
			// an error (reference-pinned).
			FlushText(text, table, posttoken_output);
			text.clear();
			if (line.size() != 1)
				throw MacroError("invalid preprocessing directive");
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
