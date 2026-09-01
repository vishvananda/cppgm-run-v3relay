#include "ast_parser.h"

#include <map>

using namespace std;

namespace
{

bool IsTypeLikeBinding(const BindKind* binding)
{
	return binding == 0 || *binding == BIND_TYPE || *binding == BIND_TEMPLATE;
}

} // namespace

AstId Pa10Parser::parse_decl_specifier_seq()
{
	const Mark saved = mark();
	const AstId result = make(AST_DECL_SPECIFIER_SEQ);
	bool have = false;
	bool committed_type = false;
	while (true)
	{
		const Pa6Token& current = token(pos_);
		if (current.kind == PA6_SIMPLE_TOKEN &&
			is_storage_or_function_specifier(current.simple_type))
		{
			AstId specifier = make(AST_DECL_SPECIFIER, token_label(current));
			++pos_;
			add(result, specifier);
			have = true;
			continue;
		}
		if (current.kind == PA6_SIMPLE_TOKEN && is_cv_qualifier(current.simple_type))
		{
			AstId specifier = make(AST_DECL_SPECIFIER, token_label(current));
			++pos_;
			add(result, specifier);
			have = true;
			continue;
		}
		if (current.kind == PA6_SIMPLE_TOKEN && is_builtin_type(current.simple_type))
		{
			AstId specifier = make(AST_DECL_SPECIFIER, token_label(current));
			++pos_;
			add(result, specifier);
			have = true;
			committed_type = true;
			continue;
		}
		if (current.IsSimple(KW_DECLTYPE))
		{
			AstId specifier = parse_decltype_specifier(false);
			if (specifier == 0)
			{
				restore(saved);
				return 0;
			}
			add(result, specifier);
			have = true;
			committed_type = true;
			continue;
		}
		if (current.IsSimple(KW_TYPENAME))
		{
			const size_t start = pos_++;
			AstId name = parse_qualified_name(false);
			if (name == 0)
			{
				restore(saved);
				return 0;
			}
			const string text = Join(start + 1, pos_);
			AstId specifier = make(AST_DECL_SPECIFIER, text);
			add(result, specifier);
			have = true;
			committed_type = true;
			continue;
		}
		if (current.kind == PA6_IDENTIFIER_TOKEN)
		{
			if (committed_type)
				break;
			const BindKind* binding = scopes_.Lookup(current.spelling);
			if (binding != 0 && !IsTypeLikeBinding(binding))
				break;
			const size_t start = pos_;
			AstId name = parse_qualified_name(false);
			if (name == 0)
			{
				restore(saved);
				return 0;
			}
			string text = Join(start, pos_);
			if (pos_ == start + 1)
				text = "TT_IDENTIFIER:" + text;
			AstId specifier = make(AST_DECL_SPECIFIER, text);
			add(result, specifier);
			have = true;
			committed_type = true;
			continue;
		}
		break;
	}
	if (!have)
	{
		restore(saved);
		return 0;
	}
	return result;
}

AstId Pa10Parser::parse_decl_specifier()
{
	const Mark saved = mark();
	AstId sequence = parse_decl_specifier_seq();
	if (sequence == 0 || arena_.At(sequence).children.size() != 1)
	{
		restore(saved);
		return 0;
	}
	return arena_.At(sequence).children[0];
}

AstId Pa10Parser::parse_type_specifier_seq()
{
	const Mark saved = mark();
	const AstId result = make(AST_TYPE_SPECIFIER_SEQ);
	bool have = false;
	bool committed = false;
	while (true)
	{
		const Pa6Token& current = token(pos_);
		if (current.kind == PA6_SIMPLE_TOKEN && is_cv_qualifier(current.simple_type))
		{
			AstId qualifier = make(AST_CV_QUALIFIER, token_label(current));
			++pos_;
			add(result, qualifier);
			have = true;
			continue;
		}
		if (current.kind == PA6_SIMPLE_TOKEN && is_builtin_type(current.simple_type))
		{
			AstId specifier = make(AST_TYPE_SPECIFIER, token_label(current));
			++pos_;
			add(result, specifier);
			have = true;
			committed = true;
			continue;
		}
		if (current.IsSimple(KW_DECLTYPE))
		{
			AstId specifier = parse_decltype_specifier(true);
			if (specifier == 0)
			{
				restore(saved);
				return 0;
			}
			add(result, specifier);
			have = true;
			committed = true;
			continue;
		}
		if (current.IsSimple(KW_TYPENAME))
		{
			const size_t start = pos_++;
			AstId name = parse_qualified_name(false);
			if (name == 0)
			{
				restore(saved);
				return 0;
			}
			add(result, make(AST_TYPE_NAME, Join(start + 1, pos_)));
			have = true;
			committed = true;
			continue;
		}
		if (current.kind == PA6_IDENTIFIER_TOKEN)
		{
			if (committed)
				break;
			const BindKind* binding = scopes_.Lookup(current.spelling);
			if (binding != 0 && !IsTypeLikeBinding(binding))
				break;
			const size_t start = pos_;
			AstId name = parse_qualified_name(false);
			if (name == 0)
			{
				restore(saved);
				return 0;
			}
			add(result, make(AST_TYPE_NAME, Join(start, pos_)));
			have = true;
			committed = true;
			continue;
		}
		break;
	}
	if (!have)
	{
		restore(saved);
		return 0;
	}
	return result;
}

AstId Pa10Parser::parse_type_specifier()
{
	const Mark saved = mark();
	AstId sequence = parse_type_specifier_seq();
	if (sequence == 0 || arena_.At(sequence).children.size() != 1)
	{
		restore(saved);
		return 0;
	}
	return arena_.At(sequence).children[0];
}

AstId Pa10Parser::parse_decltype_specifier(bool type_context)
{
	const Mark saved = mark();
	const size_t start = pos_;
	if (!consume_simple(KW_DECLTYPE) || !enter_bracket(OP_LPAREN))
		return 0;
	AstId expression = parse_expression();
	if (expression == 0 || !leave_bracket(OP_RPAREN))
	{
		restore(saved);
		return 0;
	}
	const AstKind kind = type_context ? AST_DECLTYPE_SPECIFIER : AST_DECL_SPECIFIER;
	const AstId result = make(kind, Join(start, pos_));
	add(result, expression);
	return result;
}

AstId Pa10Parser::parse_type_id()
{
	AstId specifiers = parse_type_specifier_seq();
	if (specifiers == 0)
		return 0;
	AstId declarator = 0;
	if (is_simple(OP_STAR) || is_simple(OP_AMP) || is_simple(OP_LAND) ||
		is_simple(OP_LPAREN))
		declarator = parse_declarator(true);
	if (declarator != 0)
	{
		const AstId abstract = make(AST_ABSTRACT_DECLARATOR);
		const AstNode& parsed = arena_.At(declarator);
		for (size_t i = 0; i < parsed.children.size(); ++i)
			add(abstract, parsed.children[i]);
		declarator = abstract;
	}
	const AstId result = make(AST_TYPE_ID);
	add(result, specifiers);
	add(result, declarator);
	return result;
}

AstId Pa10Parser::parse_ptr_operator()
{
	const Mark saved = mark();
	const size_t start = pos_;
	if (is_simple(OP_STAR))
	{
		++pos_;
		return make(AST_PTR_OPERATOR, token_label(token(start)));
	}
	if (is_simple(OP_AMP) || is_simple(OP_LAND))
	{
		++pos_;
		return make(AST_PTR_OPERATOR, token_label(token(start)));
	}

	// A qualified prefix followed by '*' is a pointer-to-member operator.
	if (is_simple(OP_COLON2) || is_kind(PA6_IDENTIFIER_TOKEN))
	{
		if (is_simple(OP_COLON2))
			++pos_;
		else
			++pos_;
		bool qualified = false;
		while (is_simple(OP_COLON2))
		{
			++pos_;
			qualified = true;
			if (is_simple(OP_STAR))
				break;
			if (!is_kind(PA6_IDENTIFIER_TOKEN))
			{
				restore(saved);
				return 0;
			}
			++pos_;
		}
		if (qualified && consume_simple(OP_STAR))
			return make(AST_PTR_OPERATOR, Join(start, pos_));
	}
	restore(saved);
	return 0;
}

AstId Pa10Parser::parse_declarator_id()
{
	const Mark saved = mark();
	const size_t start = pos_;
	if (is_simple(KW_OPERATOR))
	{
		AstId name = parse_operator_function_id();
		if (name != 0)
			return name;
		restore(saved);
		return 0;
	}
	if (!is_kind(PA6_IDENTIFIER_TOKEN) && !is_simple(OP_COLON2))
		return 0;
	AstId name = parse_qualified_name(true);
	if (name == 0)
	{
		restore(saved);
		return 0;
	}
	return make(AST_IDENTIFIER, Join(start, pos_));
}

AstId Pa10Parser::parse_declarator(bool allow_abstract)
{
	const Mark saved = mark();
	vector<AstId> pointers;
	while (true)
	{
		const Mark before = mark();
		AstId pointer = parse_ptr_operator();
		if (pointer == 0)
		{
			restore(before);
			break;
		}
		pointers.push_back(pointer);
		while (is_simple(KW_CONST) || is_simple(KW_VOLATILE))
			pointers.push_back(make(AST_CV_QUALIFIER,
				token_label(token(pos_++))));
	}

	AstId direct = 0;
	vector<AstId> leading_suffixes;
	if (is_kind(PA6_IDENTIFIER_TOKEN) || is_simple(OP_COLON2) ||
		is_simple(KW_OPERATOR))
		direct = parse_declarator_id();
	else if (allow_abstract && is_simple(OP_LPAREN))
	{
		const Mark parameter_mark = mark();
		AstId parameters = parse_parameter_clause();
		if (parameters != 0)
			leading_suffixes.push_back(parameters);
		else
		{
			restore(parameter_mark);
			if (!enter_bracket(OP_LPAREN))
			{
				restore(saved);
				return 0;
			}
			AstId nested = parse_declarator(allow_abstract);
			if (nested == 0 || !leave_bracket(OP_RPAREN))
			{
				restore(saved);
				return 0;
			}
			const AstId wrapper = make(AST_NESTED_DECLARATOR);
			add(wrapper, nested);
			direct = wrapper;
		}
	}
	else if (enter_bracket(OP_LPAREN))
	{
		AstId nested = parse_declarator(allow_abstract);
		if (nested == 0 || !leave_bracket(OP_RPAREN))
		{
			restore(saved);
			return 0;
		}
		const AstId wrapper = make(AST_NESTED_DECLARATOR);
		add(wrapper, nested);
		direct = wrapper;
	}
	else if (!allow_abstract && pointers.empty())
	{
		restore(saved);
		return 0;
	}

	if (direct == 0 && pointers.empty() && !allow_abstract)
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_DECLARATOR);
	for (size_t i = 0; i < pointers.size(); ++i)
		add(result, pointers[i]);
	add(result, direct);
	for (size_t i = 0; i < leading_suffixes.size(); ++i)
		add(result, leading_suffixes[i]);

	while (true)
	{
		if (is_simple(OP_LPAREN))
		{
			const Mark suffix_mark = mark();
			AstId parameters = parse_parameter_clause();
			if (parameters != 0)
			{
				add(result, parameters);
				continue;
			}
			restore(suffix_mark);
		}
		if (is_simple(OP_LSQUARE))
		{
			const Mark suffix_mark = mark();
			++pos_;
			AstId extent = 0;
			if (!is_simple(OP_RSQUARE))
				extent = parse_expression();
			if (extent == 0 && !is_simple(OP_RSQUARE))
			{
				restore(suffix_mark);
				break;
			}
			if (!consume_simple(OP_RSQUARE))
			{
				restore(suffix_mark);
				break;
			}
			const AstId suffix = make(AST_ARRAY_SUFFIX);
			add(suffix, extent);
			add(result, suffix);
			continue;
		}
		if (is_simple(KW_CONST) || is_simple(KW_VOLATILE))
		{
			add(result, make(AST_CV_QUALIFIER, token_label(token(pos_++))));
			continue;
		}
		if (is_simple(OP_AMP) || is_simple(OP_LAND))
		{
			add(result, make(AST_REF_QUALIFIER, token_label(token(pos_++))));
			continue;
		}
		if (is_simple(KW_NOEXCEPT))
		{
			const size_t start = pos_++;
			AstId expression = 0;
			if (enter_bracket(OP_LPAREN))
			{
				expression = parse_expression();
				if (expression == 0 || !leave_bracket(OP_RPAREN))
				{
					restore(saved);
					return 0;
				}
			}
				const AstId suffix = make(AST_FUNCTION_QUALIFIER,
					expression == 0 ? "noexcept" : Join(start, pos_));
				add(suffix, expression);
				add(result, suffix);
				continue;
		}
		if (is_kind(PA6_IDENTIFIER_TOKEN) &&
			(token(pos_).flags & (PA6_FINAL_FLAG | PA6_OVERRIDE_FLAG)) != 0)
		{
			add(result, make(AST_VIRT_SPECIFIER, token_label(token(pos_++))));
			continue;
		}
		if (is_simple(OP_ARROW))
		{
			AstId trailing = parse_trailing_return_type();
			if (trailing == 0)
			{
				restore(saved);
				return 0;
			}
			add(result, trailing);
			continue;
		}
		break;
	}
	return result;
}

AstId Pa10Parser::parse_parameter_clause()
{
	const Mark saved = mark();
	if (!enter_bracket(OP_LPAREN))
		return 0;
	const AstId result = make(AST_PARAMETER_CLAUSE);
	if (leave_bracket(OP_RPAREN))
		return result;
	while (true)
	{
		if (consume_simple(OP_DOTS))
		{
			add(result, make(AST_PARAMETER_PACK, "..."));
			if (!leave_bracket(OP_RPAREN))
			{
				restore(saved);
				return 0;
			}
			return result;
		}
		AstId parameter = parse_parameter_declaration();
		if (parameter == 0)
		{
			restore(saved);
			return 0;
		}
		add(result, parameter);
		if (consume_simple(OP_COMMA))
		{
			if (is_simple(OP_DOTS))
				continue;
			continue;
		}
		if (!leave_bracket(OP_RPAREN))
		{
			restore(saved);
			return 0;
		}
		return result;
	}
}

AstId Pa10Parser::parse_parameter_declaration()
{
	const Mark saved = mark();
	AstId specifiers = parse_decl_specifier_seq();
	if (specifiers == 0)
		return 0;
	AstId declarator = 0;
	if (!is_simple(OP_COMMA) && !is_simple(OP_RPAREN) &&
		!is_simple(OP_ASS) && !is_simple(OP_DOTS))
		declarator = parse_declarator(true);
	if (declarator == 0 && !is_simple(OP_COMMA) && !is_simple(OP_RPAREN) &&
		!is_simple(OP_ASS) && !is_simple(OP_DOTS))
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_PARAMETER_DECLARATION);
	add(result, specifiers);
	add(result, declarator);
	if (consume_simple(OP_DOTS))
		add(result, make(AST_PARAMETER_PACK, "..."));
	if (consume_simple(OP_ASS))
	{
		AstId initializer = parse_initializer();
		if (initializer == 0)
		{
			restore(saved);
			return 0;
		}
		const AstId default_argument = make(AST_DEFAULT_ARGUMENT);
		add(default_argument, initializer);
		add(result, default_argument);
	}
	return result;
}

AstId Pa10Parser::parse_init_declarator_list()
{
	const Mark saved = mark();
	AstId result = make(AST_INIT_DECLARATOR_LIST);
	AstId item = parse_init_declarator();
	if (item == 0)
	{
		restore(saved);
		return 0;
	}
	add(result, item);
	while (consume_simple(OP_COMMA))
	{
		item = parse_init_declarator();
		if (item == 0)
		{
			restore(saved);
			return 0;
		}
		add(result, item);
	}
	return result;
}

AstId Pa10Parser::parse_init_declarator()
{
	const Mark saved = mark();
	AstId declarator = parse_declarator();
	if (declarator == 0)
		return 0;
	AstId initializer = 0;
	if (is_simple(OP_ASS) || is_simple(OP_LBRACE) || is_simple(OP_LPAREN))
		initializer = parse_initializer();
	if ((is_simple(OP_ASS) || is_simple(OP_LBRACE) || is_simple(OP_LPAREN)) &&
		initializer == 0)
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_INIT_DECLARATOR);
	add(result, declarator);
	add(result, initializer);
	return result;
}

AstId Pa10Parser::parse_initializer()
{
	const Mark saved = mark();
	if (consume_simple(OP_ASS))
	{
		if (is_simple(KW_DEFAULT) || is_simple(KW_DELETE))
		{
			const string spelling = token(pos_++).spelling;
			return make(AST_INITIALIZER, "", vector<AstId>(1,
				make(AST_SPECIAL_INITIALIZER, spelling)));
		}
		AstId expression = parse_assignment_expression();
		if (expression == 0)
		{
			restore(saved);
			return 0;
		}
		const AstId result = make(AST_INITIALIZER);
		add(result, expression);
		return result;
	}
	if (is_simple(OP_LBRACE))
	{
		AstId braces = parse_braced_init_list();
		if (braces == 0)
		{
			restore(saved);
			return 0;
		}
		const AstId result = make(AST_INITIALIZER);
		add(result, braces);
		return result;
	}
	if (is_simple(OP_LPAREN))
	{
		AstId parens = parse_paren_initializer();
		if (parens == 0)
		{
			restore(saved);
			return 0;
		}
		const AstId result = make(AST_INITIALIZER);
		add(result, parens);
		return result;
	}
	restore(saved);
	return 0;
}

AstId Pa10Parser::parse_braced_init_list()
{
	const Mark saved = mark();
	if (!enter_bracket(OP_LBRACE))
		return 0;
	const AstId result = make(AST_BRACED_INIT_LIST);
	if (leave_bracket(OP_RBRACE))
		return result;
	while (true)
	{
		AstId expression = parse_assignment_expression();
		if (expression == 0)
		{
			restore(saved);
			return 0;
		}
		add(result, expression);
		if (!consume_simple(OP_COMMA))
			break;
		if (is_simple(OP_RBRACE))
			break;
	}
	if (!leave_bracket(OP_RBRACE))
	{
		restore(saved);
		return 0;
	}
	return result;
}

AstId Pa10Parser::parse_paren_initializer()
{
	const Mark saved = mark();
	if (!enter_bracket(OP_LPAREN))
		return 0;
	const AstId result = make(AST_PAREN_INITIALIZER);
	if (leave_bracket(OP_RPAREN))
		return result;
	while (true)
	{
		AstId expression = parse_assignment_expression();
		if (expression == 0)
		{
			restore(saved);
			return 0;
		}
		add(result, expression);
		if (!consume_simple(OP_COMMA))
			break;
	}
	if (!leave_bracket(OP_RPAREN))
	{
		restore(saved);
		return 0;
	}
	return result;
}

AstId Pa10Parser::parse_trailing_return_type()
{
	const Mark saved = mark();
	if (!consume_simple(OP_ARROW))
		return 0;
	const size_t start = pos_;
	AstId type = parse_type_id();
	if (type == 0)
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_TRAILING_RETURN_TYPE, Join(start, pos_));
	add(result, type);
	return result;
}

AstId Pa10Parser::parse_function_suffixes(AstId declarator)
{
	return declarator;
}

AstId Pa10Parser::parse_qualified_name(bool declarator_name)
{
	const Mark saved = mark();
	const size_t start = pos_;
	if (consume_simple(OP_COLON2) && !is_kind(PA6_IDENTIFIER_TOKEN))
	{
		restore(saved);
		return 0;
	}
	if (!is_kind(PA6_IDENTIFIER_TOKEN))
	{
		restore(saved);
		return 0;
	}
	++pos_;
	if (is_simple(OP_LT))
	{
		const Mark template_mark = mark();
		--pos_;
		if (parse_simple_template_id() == 0)
			restore(template_mark);
	}
	while (consume_simple(OP_COLON2))
	{
		if (!is_kind(PA6_IDENTIFIER_TOKEN))
		{
			restore(saved);
			return 0;
		}
		++pos_;
		if (is_simple(OP_LT))
		{
			const Mark template_mark = mark();
			--pos_;
			if (parse_simple_template_id() == 0)
				restore(template_mark);
		}
	}
	(void)declarator_name;
	return make(AST_ID_EXPRESSION, Join(start, pos_));
}

AstId Pa10Parser::parse_nested_name_specifier()
{
	return parse_qualified_name(false);
}

AstId Pa10Parser::parse_simple_template_id()
{
	const Mark saved = mark();
	if (!is_kind(PA6_IDENTIFIER_TOKEN) || !is_simple(OP_LT, pos_ + 1))
		return 0;
	const BindKind* binding = scopes_.Lookup(token(pos_).spelling);
	if (binding != 0 && *binding == BIND_VALUE)
		return 0;
	const size_t start = pos_;
	++pos_;
	++pos_;
	brackets_.push_back(BRACKET_ANGLE);
	if (!is_simple(OP_GT) && !is_kind(PA6_RSHIFT_1_TOKEN) &&
		!is_kind(PA6_RSHIFT_2_TOKEN))
	{
		if (parse_template_argument_list() == 0)
		{
			restore(saved);
			return 0;
		}
	}
	if (!parse_close_angle_bracket())
	{
		restore(saved);
		return 0;
	}
	return make(AST_ID_EXPRESSION, Join(start, pos_));
}

AstId Pa10Parser::parse_template_argument_list()
{
	const Mark saved = mark();
	const AstId result = make(AST_TEMPLATE_ARGUMENT_LIST);
	AstId argument = parse_template_argument();
	if (argument == 0)
	{
		restore(saved);
		return 0;
	}
	add(result, argument);
	while (consume_simple(OP_COMMA))
	{
		argument = parse_template_argument();
		if (argument == 0)
		{
			restore(saved);
			return 0;
		}
		add(result, argument);
	}
	return result;
}

AstId Pa10Parser::parse_template_argument()
{
	const Mark saved = mark();
	const size_t start = pos_;
	if (is_simple(KW_CLASS) || is_simple(KW_TYPENAME))
	{
		++pos_;
		if (is_kind(PA6_IDENTIFIER_TOKEN))
			++pos_;
		return make(AST_TEMPLATE_ARGUMENT, Join(start, pos_));
	}
	AstId expression = parse_assignment_expression();
	if (expression != 0)
		return make(AST_TEMPLATE_ARGUMENT, Join(start, pos_),
			vector<AstId>(1, expression));
	restore(saved);
	AstId type = parse_type_id();
	if (type == 0)
		return 0;
	return make(AST_TEMPLATE_ARGUMENT, Join(start, pos_), vector<AstId>(1, type));
}

AstId Pa10Parser::parse_operator_function_id()
{
	const Mark saved = mark();
	const size_t start = pos_;
	if (!consume_simple(KW_OPERATOR))
		return 0;
	if (is_simple(KW_NEW) || is_simple(KW_DELETE))
	{
		++pos_;
		if (is_simple(OP_LSQUARE))
		{
			++pos_;
			if (!consume_simple(OP_RSQUARE))
			{
				restore(saved);
				return 0;
			}
		}
		return make(AST_IDENTIFIER, operator_name(start, pos_));
	}
	if (is_kind(PA6_LITERAL_TOKEN))
	{
		++pos_;
		return make(AST_IDENTIFIER, operator_name(start, pos_));
	}
	if (is_builtin_type(token(pos_).simple_type) &&
		token(pos_).kind == PA6_SIMPLE_TOKEN)
	{
		++pos_;
		return make(AST_IDENTIFIER, operator_name(start, pos_));
	}
	if (is_kind(PA6_IDENTIFIER_TOKEN))
	{
		++pos_;
		if (is_simple(OP_LT))
		{
			const Mark template_mark = mark();
			if (parse_simple_template_id() == 0)
				restore(template_mark);
		}
		return make(AST_IDENTIFIER, operator_name(start, pos_));
	}
	const ETokenType type = token(pos_).simple_type;
	if (type == OP_LSQUARE || type == OP_RSQUARE || type == OP_ARROW ||
		type == OP_ARROWSTAR || type == OP_PLUS || type == OP_MINUS ||
		type == OP_STAR || type == OP_AMP || type == OP_LAND ||
		type == OP_LOR || type == OP_EQ || type == OP_NE || type == OP_LT ||
		type == OP_GT || type == OP_COMMA || type == OP_LNOT)
	{
		++pos_;
		return make(AST_IDENTIFIER, operator_name(start, pos_));
	}
	restore(saved);
	return 0;
}

AstId Pa10Parser::parse_literal_operator_id()
{
	return parse_operator_function_id();
}

AstId Pa10Parser::parse_simple_declaration()
{
	const Mark saved = mark();
	AstId specifiers = parse_decl_specifier_seq();
	if (specifiers == 0)
		return 0;
	if (consume_simple(OP_SEMICOLON))
		return make(AST_SIMPLE_DECLARATION, "", vector<AstId>(1, specifiers));
	AstId list = parse_init_declarator_list();
	if (list == 0 || !consume_simple(OP_SEMICOLON))
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_SIMPLE_DECLARATION);
	add(result, specifiers);
	add(result, list);
	bool is_typedef = false;
	for (size_t i = 0; i < arena_.At(specifiers).children.size(); ++i)
	{
		const AstNode& specifier = arena_.At(arena_.At(specifiers).children[i]);
		if (specifier.text.find("KW_TYPEDEF:") == 0)
			is_typedef = true;
	}
	for (size_t i = 0; i < arena_.At(list).children.size(); ++i)
	{
		const AstId item = arena_.At(list).children[i];
		if (is_typedef)
			bind_declarator(arena_.At(item).children[0], BIND_TYPE);
		else
			bind_declarator(arena_.At(item).children[0], BIND_VALUE);
	}
	return result;
}

AstId Pa10Parser::parse_alias_declaration()
{
	const Mark saved = mark();
	if (!consume_simple(KW_USING) || !is_kind(PA6_IDENTIFIER_TOKEN))
	{
		restore(saved);
		return 0;
	}
	const string name = token(pos_++).spelling;
	if (!consume_simple(OP_ASS))
	{
		restore(saved);
		return 0;
	}
	AstId type = parse_type_id();
	if (type == 0 || !consume_simple(OP_SEMICOLON))
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_ALIAS_DECLARATION, name);
	add(result, type);
	scopes_.Bind(name, BIND_TYPE);
	return result;
}

AstId Pa10Parser::parse_static_assert_declaration()
{
	const Mark saved = mark();
	if (!consume_simple(KW_STATIC_ASSERT) || !enter_bracket(OP_LPAREN))
		return 0;
	AstId expression = parse_assignment_expression();
	if (expression == 0 || !consume_simple(OP_COMMA) ||
		!is_kind(PA6_LITERAL_TOKEN))
	{
		restore(saved);
		return 0;
	}
	const string message = token(pos_++).spelling;
	if (!leave_bracket(OP_RPAREN) || !consume_simple(OP_SEMICOLON))
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_STATIC_ASSERT_DECLARATION);
	add(result, expression);
	add(result, make(AST_MESSAGE, message));
	return result;
}
