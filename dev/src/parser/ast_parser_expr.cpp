#include "ast_parser.h"

using namespace std;

namespace
{

bool IsRelational(ETokenType type)
{
	return type == OP_LT || type == OP_GT || type == OP_LE || type == OP_GE;
}

} // namespace

AstId Pa10Parser::parse_expression()
{
	const Mark saved = mark();
	AstId left = parse_assignment_expression();
	if (left == 0)
		return 0;
	while (consume_simple(OP_COMMA))
	{
		AstId right = parse_assignment_expression();
		if (right == 0)
		{
			restore(saved);
			return 0;
		}
		const AstId binary = make(AST_BINARY_EXPRESSION, "OP_COMMA:,");
		add(binary, left);
		add(binary, right);
		left = binary;
	}
	return left;
}

AstId Pa10Parser::parse_assignment_expression()
{
	const Mark saved = mark();
	AstId left = parse_conditional_expression();
	if (left == 0)
		return 0;
	if (is_assignment_operator(token(pos_).simple_type) &&
		token(pos_).kind == PA6_SIMPLE_TOKEN)
	{
		const string op = token_label(token(pos_));
		++pos_;
		AstId right = parse_assignment_expression();
		if (right == 0)
		{
			restore(saved);
			return 0;
		}
		const AstId result = make(AST_ASSIGNMENT_EXPRESSION, op);
		add(result, left);
		add(result, right);
		return result;
	}
	return left;
}

AstId Pa10Parser::parse_conditional_expression()
{
	const Mark saved = mark();
	AstId condition = parse_logical_or_expression();
	if (condition == 0)
		return 0;
	if (!consume_simple(OP_QMARK))
		return condition;
	AstId yes = parse_expression();
	if (yes == 0 || !consume_simple(OP_COLON))
	{
		restore(saved);
		return 0;
	}
	AstId no = parse_conditional_expression();
	if (no == 0)
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_CONDITIONAL_EXPRESSION);
	add(result, condition);
	add(result, yes);
	add(result, no);
	return result;
}

AstId Pa10Parser::parse_logical_or_expression()
{
	AstId left = parse_logical_and_expression();
	while (left != 0 && is_simple(OP_LOR))
	{
		const string op = token_label(token(pos_));
		++pos_;
		AstId right = parse_logical_and_expression();
		if (right == 0)
			return 0;
		AstId result = make(AST_BINARY_EXPRESSION, op);
		add(result, left);
		add(result, right);
		left = result;
	}
	return left;
}

AstId Pa10Parser::parse_logical_and_expression()
{
	AstId left = parse_inclusive_or_expression();
	while (left != 0 && is_simple(OP_LAND))
	{
		const string op = token_label(token(pos_));
		++pos_;
		AstId right = parse_inclusive_or_expression();
		if (right == 0)
			return 0;
		AstId result = make(AST_BINARY_EXPRESSION, op);
		add(result, left);
		add(result, right);
		left = result;
	}
	return left;
}

AstId Pa10Parser::parse_inclusive_or_expression()
{
	AstId left = parse_exclusive_or_expression();
	while (left != 0 && is_simple(OP_BOR))
	{
		const string op = token_label(token(pos_));
		++pos_;
		AstId right = parse_exclusive_or_expression();
		if (right == 0)
			return 0;
		AstId result = make(AST_BINARY_EXPRESSION, op);
		add(result, left);
		add(result, right);
		left = result;
	}
	return left;
}

AstId Pa10Parser::parse_exclusive_or_expression()
{
	AstId left = parse_and_expression();
	while (left != 0 && is_simple(OP_XOR))
	{
		const string op = token_label(token(pos_));
		++pos_;
		AstId right = parse_and_expression();
		if (right == 0)
			return 0;
		AstId result = make(AST_BINARY_EXPRESSION, op);
		add(result, left);
		add(result, right);
		left = result;
	}
	return left;
}

AstId Pa10Parser::parse_and_expression()
{
	AstId left = parse_equality_expression();
	while (left != 0 && is_simple(OP_AMP))
	{
		const string op = token_label(token(pos_));
		++pos_;
		AstId right = parse_equality_expression();
		if (right == 0)
			return 0;
		AstId result = make(AST_BINARY_EXPRESSION, op);
		add(result, left);
		add(result, right);
		left = result;
	}
	return left;
}

AstId Pa10Parser::parse_equality_expression()
{
	AstId left = parse_relational_expression();
	while (left != 0 && (is_simple(OP_EQ) || is_simple(OP_NE)))
	{
		const string op = token_label(token(pos_));
		++pos_;
		AstId right = parse_relational_expression();
		if (right == 0)
			return 0;
		AstId result = make(AST_BINARY_EXPRESSION, op);
		add(result, left);
		add(result, right);
		left = result;
	}
	return left;
}

AstId Pa10Parser::parse_relational_expression()
{
	AstId left = parse_shift_expression();
	while (left != 0 && IsRelational(token(pos_).simple_type) &&
		token(pos_).kind == PA6_SIMPLE_TOKEN &&
		!(has_angle_boundary() && is_simple(OP_GT)))
	{
		const string op = token_label(token(pos_));
		++pos_;
		AstId right = parse_shift_expression();
		if (right == 0)
			return 0;
		AstId result = make(AST_BINARY_EXPRESSION, op);
		add(result, left);
		add(result, right);
		left = result;
	}
	return left;
}

AstId Pa10Parser::parse_shift_expression()
{
	AstId left = parse_additive_expression();
	while (left != 0 && (is_simple(OP_LSHIFT) ||
		(!has_angle_boundary() && is_kind(PA6_RSHIFT_1_TOKEN) &&
		 is_kind(PA6_RSHIFT_2_TOKEN, pos_ + 1))))
	{
		string op;
		if (is_simple(OP_LSHIFT))
		{
			op = token_label(token(pos_));
			++pos_;
		}
		else
		{
			op = "OP_RSHIFT:>>";
			pos_ += 2;
		}
		AstId right = parse_additive_expression();
		if (right == 0)
			return 0;
		AstId result = make(AST_BINARY_EXPRESSION, op);
		add(result, left);
		add(result, right);
		left = result;
	}
	return left;
}

AstId Pa10Parser::parse_additive_expression()
{
	AstId left = parse_multiplicative_expression();
	while (left != 0 && (is_simple(OP_PLUS) || is_simple(OP_MINUS)))
	{
		const string op = token_label(token(pos_));
		++pos_;
		AstId right = parse_multiplicative_expression();
		if (right == 0)
			return 0;
		AstId result = make(AST_BINARY_EXPRESSION, op);
		add(result, left);
		add(result, right);
		left = result;
	}
	return left;
}

AstId Pa10Parser::parse_multiplicative_expression()
{
	AstId left = parse_pm_expression();
	while (left != 0 && (is_simple(OP_STAR) || is_simple(OP_DIV) ||
		is_simple(OP_MOD)))
	{
		const string op = token_label(token(pos_));
		++pos_;
		AstId right = parse_pm_expression();
		if (right == 0)
			return 0;
		AstId result = make(AST_BINARY_EXPRESSION, op);
		add(result, left);
		add(result, right);
		left = result;
	}
	return left;
}

AstId Pa10Parser::parse_pm_expression()
{
	AstId left = parse_cast_expression();
	while (left != 0 && (is_simple(OP_DOTSTAR) || is_simple(OP_ARROWSTAR)))
	{
		const string op = token_label(token(pos_));
		++pos_;
		AstId right = parse_cast_expression();
		if (right == 0)
			return 0;
		AstId result = make(AST_BINARY_EXPRESSION, op);
		add(result, left);
		add(result, right);
		left = result;
	}
	return left;
}

AstId Pa10Parser::parse_cast_expression()
{
	const Mark saved = mark();
	if (is_simple(KW_STATIC_CAST) || is_simple(KW_DYNAMIC_CAST) ||
		is_simple(KW_CONST_CAST) || is_simple(KW_REINTERPET_CAST))
	{
		const string op = token_label(token(pos_++));
		if (!consume_simple(OP_LT))
		{
			restore(saved);
			return 0;
		}
		AstId type = parse_type_id();
		if (type == 0 || !consume_simple(OP_GT) || !enter_bracket(OP_LPAREN))
		{
			restore(saved);
			return 0;
		}
		AstId expression = parse_expression();
		if (expression == 0 || !leave_bracket(OP_RPAREN))
		{
			restore(saved);
			return 0;
		}
		const AstId result = make(AST_CAST_EXPRESSION, op);
		add(result, type);
		add(result, expression);
		return result;
	}
	if (is_simple(OP_LPAREN))
	{
		const Pa6Token& first = token(pos_ + 1);
		const BindKind* binding = first.kind == PA6_IDENTIFIER_TOKEN ?
			scopes_.Lookup(first.spelling) : 0;
		const bool candidate = (first.kind == PA6_SIMPLE_TOKEN &&
			(is_builtin_type(first.simple_type) || is_cv_qualifier(first.simple_type)))
			|| (first.kind == PA6_IDENTIFIER_TOKEN && binding != 0 &&
				(*binding == BIND_TYPE || *binding == BIND_TEMPLATE));
		if (candidate && enter_bracket(OP_LPAREN))
		{
			AstId type = parse_type_id();
			if (type != 0 && leave_bracket(OP_RPAREN))
			{
				AstId expression = parse_cast_expression();
				if (expression != 0)
				{
					const AstId result = make(AST_CAST_EXPRESSION, "OP_LPAREN:");
					add(result, type);
					add(result, expression);
					return result;
				}
			}
			restore(saved);
		}
	}
	return parse_unary_expression();
}

AstId Pa10Parser::parse_unary_expression()
{
	if (is_simple(KW_SIZEOF) || is_simple(KW_ALIGNOF) ||
		is_simple(KW_NOEXCEPT) || is_simple(KW_TYPEID))
		return parse_type_trait_expression();
	if (is_simple(KW_NEW))
		return parse_new_expression();
	if (is_simple(KW_DELETE))
		return parse_delete_expression();
	if (is_simple(OP_PLUS) || is_simple(OP_MINUS) || is_simple(OP_LNOT) ||
		is_simple(OP_COMPL) || is_simple(OP_STAR) || is_simple(OP_AMP) ||
		is_simple(OP_INC) || is_simple(OP_DEC))
	{
		const string op = token_label(token(pos_++));
		AstId expression = parse_unary_expression();
		if (expression == 0)
			return 0;
		const AstKind kind = (op == "OP_INC:++" || op == "OP_DEC:--") ?
			AST_UNARY_EXPRESSION : AST_UNARY_EXPRESSION;
		const AstId result = make(kind, op);
		add(result, expression);
		return result;
	}
	return parse_postfix_expression();
}

AstId Pa10Parser::parse_postfix_expression()
{
	const Mark saved = mark();
	AstId expression = parse_postfix_root();
	if (expression == 0)
		return 0;
	while (true)
	{
		if (is_simple(OP_LPAREN))
		{
			AstId arguments = parse_argument_list(AST_ARGUMENT_LIST);
			if (arguments == 0)
			{
				restore(saved);
				return 0;
			}
			const AstId result = make(AST_CALL_EXPRESSION);
			add(result, expression);
			add(result, arguments);
			expression = result;
			continue;
		}
		if (is_simple(OP_LSQUARE))
		{
			++pos_;
			AstId index = parse_expression();
			if (index == 0 || !consume_simple(OP_RSQUARE))
			{
				restore(saved);
				return 0;
			}
			const AstId result = make(AST_SUBSCRIPT_EXPRESSION);
			add(result, expression);
			add(result, index);
			expression = result;
			continue;
		}
		if (is_simple(OP_DOT) || is_simple(OP_ARROW))
		{
			const string op = token_label(token(pos_++));
			AstId member = parse_id_expression();
			if (member == 0)
			{
				restore(saved);
				return 0;
			}
			const AstNode& name = arena_.At(member);
			const AstId identifier = make(AST_IDENTIFIER, name.text);
			const AstId result = make(AST_MEMBER_EXPRESSION, op);
			add(result, expression);
			add(result, identifier);
			expression = result;
			continue;
		}
		if (is_simple(OP_INC) || is_simple(OP_DEC))
		{
			const string op = token_label(token(pos_++));
			const AstId result = make(AST_POSTFIX_EXPRESSION, op);
			add(result, expression);
			expression = result;
			continue;
		}
		break;
	}
	return expression;
}

AstId Pa10Parser::parse_postfix_root()
{
	if (token(pos_).kind == PA6_SIMPLE_TOKEN && is_builtin_type(token(pos_).simple_type) &&
		is_simple(OP_LPAREN, pos_ + 1))
	{
		const string name = token(pos_).spelling;
		++pos_;
		const AstId callee = make(AST_ID_EXPRESSION, name);
		AstId arguments = parse_argument_list(AST_PAREN_ARGUMENT_LIST);
		if (arguments == 0)
			return 0;
		const AstId result = make(AST_CALL_EXPRESSION);
		add(result, callee);
		add(result, arguments);
		return result;
	}
	if (is_simple(OP_LPAREN))
	{
		const Mark saved = mark();
		if (enter_bracket(OP_LPAREN))
		{
			AstId expression = parse_expression();
			if (expression != 0 && leave_bracket(OP_RPAREN))
			{
				const AstId result = make(AST_PARENTHESIZED_EXPRESSION);
				add(result, expression);
				return result;
			}
		}
		restore(saved);
	}
	if (is_simple(OP_LSQUARE))
		return parse_lambda_expression();
	return parse_primary_expression();
}

AstId Pa10Parser::parse_primary_expression()
{
	if (is_kind(PA6_LITERAL_TOKEN))
		return make(AST_LITERAL, token(pos_++).spelling);
	if (token(pos_).kind == PA6_SIMPLE_TOKEN &&
		is_keyword_literal(token(pos_).simple_type))
		return make(AST_KEYWORD_LITERAL, token_label(token(pos_++)));
	if (is_kind(PA6_IDENTIFIER_TOKEN) || is_simple(OP_COLON2))
		return parse_id_expression();
	return 0;
}

AstId Pa10Parser::parse_id_expression()
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
	return make(AST_ID_EXPRESSION, Join(start, pos_));
}

AstId Pa10Parser::parse_argument_list(AstKind kind)
{
	const Mark saved = mark();
	if (!enter_bracket(OP_LPAREN))
		return 0;
	const AstId result = make(kind);
	if (leave_bracket(OP_RPAREN))
		return result;
	while (true)
	{
		AstId argument = parse_assignment_expression();
		if (argument == 0)
		{
			restore(saved);
			return 0;
		}
		add(result, argument);
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

AstId Pa10Parser::parse_type_trait_expression()
{
	const Mark saved = mark();
	const string op = token_label(token(pos_++));
	if (!enter_bracket(OP_LPAREN))
	{
		restore(saved);
		return 0;
	}
	const Mark type_mark = mark();
	AstId type = 0;
	const Pa6Token& first = token(pos_);
	const BindKind* binding = first.kind == PA6_IDENTIFIER_TOKEN ?
		scopes_.Lookup(first.spelling) : 0;
	if ((first.kind == PA6_SIMPLE_TOKEN &&
			(is_builtin_type(first.simple_type) || is_cv_qualifier(first.simple_type))) ||
		(first.kind == PA6_IDENTIFIER_TOKEN && binding != 0 &&
			(*binding == BIND_TYPE || *binding == BIND_TEMPLATE)) ||
		(first.kind == PA6_IDENTIFIER_TOKEN &&
			(is_simple(OP_STAR, pos_ + 1) || is_simple(OP_AMP, pos_ + 1) ||
			 is_simple(OP_LAND, pos_ + 1))))
	{
		type = parse_type_id();
		if (type != 0 && leave_bracket(OP_RPAREN))
		{
			const AstId result = make(op == "KW_SIZEOF:sizeof" ?
				AST_SIZEOF_EXPRESSION : AST_TYPE_TRAIT_EXPRESSION,
				op == "KW_SIZEOF:sizeof" ? "" : op);
			add(result, type);
			return result;
		}
		restore(type_mark);
	}
	AstId expression = parse_expression();
	if (expression == 0 || !leave_bracket(OP_RPAREN))
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(op == "KW_SIZEOF:sizeof" ?
		AST_SIZEOF_EXPRESSION : AST_TYPE_TRAIT_EXPRESSION,
		op == "KW_SIZEOF:sizeof" ? "" : op);
	add(result, expression);
	return result;
}

AstId Pa10Parser::parse_new_expression()
{
	const Mark saved = mark();
	if (!consume_simple(KW_NEW))
		return 0;
	const AstId result = make(AST_NEW_EXPRESSION);
		if (is_simple(OP_LPAREN))
		{
		const Mark placement_mark = mark();
		AstId placement_args = parse_argument_list(AST_PAREN_ARGUMENT_LIST);
		if (placement_args != 0 && (is_builtin_type(token(pos_).simple_type) ||
			is_kind(PA6_IDENTIFIER_TOKEN)))
		{
			const AstId placement = make(AST_PLACEMENT,
				"(" + Join(placement_mark.position + 1, pos_ - 1) + ")");
			add(placement, placement_args);
			add(result, placement);
		}
		else
			restore(placement_mark);
	}
	AstId type = 0;
	if (is_simple(OP_LPAREN))
	{
		const Mark parenthesized_type = mark();
		if (enter_bracket(OP_LPAREN))
		{
			type = parse_type_id();
			if (type != 0 && leave_bracket(OP_RPAREN))
				;
			else
			{
				type = 0;
				restore(parenthesized_type);
			}
		}
	}
	if (type == 0)
		type = parse_type_id();
	if (type == 0)
	{
		restore(saved);
		return 0;
	}
	add(result, type);
	if (is_simple(OP_LPAREN))
	{
		AstId initializer = parse_paren_initializer();
		if (initializer == 0)
		{
			restore(saved);
			return 0;
		}
			const AstId wrapped = make(AST_INITIALIZER);
			add(wrapped, initializer);
			add(result, wrapped);
	}
		else if (is_simple(OP_LBRACE))
	{
		AstId initializer = parse_braced_init_list();
		if (initializer == 0)
		{
			restore(saved);
			return 0;
		}
			const AstId wrapped = make(AST_INITIALIZER);
			add(wrapped, initializer);
			add(result, wrapped);
	}
	return result;
}

AstId Pa10Parser::parse_delete_expression()
{
	const Mark saved = mark();
	if (!consume_simple(KW_DELETE))
		return 0;
	const AstId result = make(AST_DELETE_EXPRESSION);
	if (is_simple(OP_LSQUARE, pos_) && is_simple(OP_RSQUARE, pos_ + 1))
	{
		++pos_;
		++pos_;
		add(result, make(AST_ARRAY_DELETE));
	}
	AstId expression = parse_unary_expression();
	if (expression == 0)
	{
		restore(saved);
		return 0;
	}
	add(result, expression);
	return result;
}

AstId Pa10Parser::parse_lambda_expression()
{
	const Mark saved = mark();
	const size_t start = pos_;
	if (!enter_bracket(OP_LSQUARE))
		return 0;
	while (!is_simple(OP_RSQUARE))
	{
		if (at_end())
		{
			restore(saved);
			return 0;
		}
		++pos_;
	}
	if (!leave_bracket(OP_RSQUARE))
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_LAMBDA_EXPRESSION);
	add(result, make(AST_LAMBDA_INTRODUCER, Join(start, pos_)));
	AstId declarator = 0;
	if (is_simple(OP_LPAREN) || is_simple(KW_MUTABLE) ||
		is_simple(KW_NOEXCEPT) || is_simple(OP_ARROW))
	{
		declarator = make(AST_LAMBDA_DECLARATOR);
		if (is_simple(OP_LPAREN))
			add(declarator, parse_parameter_clause());
		if (is_simple(KW_MUTABLE))
			add(declarator, make(AST_LAMBDA_SPECIFIER,
				token_label(token(pos_++))));
		if (is_simple(KW_NOEXCEPT))
		{
			++pos_;
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
			AstId noexcept_specifier = make(AST_NOEXCEPT_SPECIFICATION);
			add(noexcept_specifier, expression);
			add(declarator, noexcept_specifier);
		}
		if (is_simple(OP_ARROW))
		{
			++pos_;
			AstId type = parse_type_id();
			if (type == 0)
			{
				restore(saved);
				return 0;
			}
			const AstId trailing = make(AST_TRAILING_RETURN_TYPE);
			add(trailing, type);
			add(declarator, trailing);
		}
		add(result, declarator);
	}
	AstId body = parse_compound_statement();
	if (body == 0)
	{
		restore(saved);
		return 0;
	}
	add(result, body);
	return result;
}
