#include "ast_parser.h"

using namespace std;

namespace
{

bool IsRelational(ETokenType type)
{
	return type == OP_LT || type == OP_GT || type == OP_LE || type == OP_GE;
}

bool CanContinueTemplateIdExpression(const Pa6Token& token,
	bool nested_template_context)
{
	if (token.kind == PA6_IDENTIFIER_TOKEN || token.kind == PA6_LITERAL_TOKEN)
		return false;
	return !nested_template_context || !token.IsSimple(OP_LBRACE);
}

} // namespace

AstId Pa10Parser::parse_expression()
{
	const Mark saved = mark();
	AstId left = parse_assignment_expression();
	if (left == 0)
		return 0;
	while (is_simple(OP_COMMA))
	{
		const size_t comma_at = pos_++;
		AstId right = parse_assignment_expression();
		if (right == 0)
		{
			restore(saved);
			return 0;
		}
		const AstId binary = make_token(AST_BINARY_EXPRESSION, comma_at);
		add(binary, left);
		add(binary, right);
		left = binary;
	}
	return left;
}

// Memoized: the declaration/expression ambiguity at statement, condition and
// template-argument level re-reads the same expression from the same position
// after a failed declaration attempt.
AstId Pa10Parser::parse_assignment_expression()
{
	return try_memoized(MEMO_ASSIGNMENT_EXPRESSION,
		&Pa10Parser::parse_assignment_expression_rule);
}

AstId Pa10Parser::parse_assignment_expression_rule()
{
	const Mark saved = mark();
	AstId left = parse_conditional_expression();
	if (left == 0)
		return 0;
	if (token(pos_).kind == PA6_SIMPLE_TOKEN &&
		is_assignment_operator(token(pos_).simple_type))
	{
		const size_t op_at = pos_++;
		// An assignment's right operand is an initializer-clause, so a braced
		// scalar/array list must remain part of the assignment AST instead of
		// being rejected by the expression grammar.
		AstId right = is_simple(OP_LBRACE) ? parse_braced_init_list() :
			parse_assignment_expression();
		if (right == 0)
		{
			restore(saved);
			return 0;
		}
		const AstId result = make_token(AST_ASSIGNMENT_EXPRESSION, op_at);
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
		const size_t op_at = pos_++;
		AstId right = parse_logical_and_expression();
		if (right == 0)
			return 0;
		AstId result = make_token(AST_BINARY_EXPRESSION, op_at);
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
		const size_t op_at = pos_++;
		AstId right = parse_inclusive_or_expression();
		if (right == 0)
			return 0;
		AstId result = make_token(AST_BINARY_EXPRESSION, op_at);
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
		const size_t op_at = pos_++;
		AstId right = parse_exclusive_or_expression();
		if (right == 0)
			return 0;
		AstId result = make_token(AST_BINARY_EXPRESSION, op_at);
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
		const size_t op_at = pos_++;
		AstId right = parse_and_expression();
		if (right == 0)
			return 0;
		AstId result = make_token(AST_BINARY_EXPRESSION, op_at);
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
		const size_t op_at = pos_++;
		AstId right = parse_equality_expression();
		if (right == 0)
			return 0;
		AstId result = make_token(AST_BINARY_EXPRESSION, op_at);
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
		const size_t op_at = pos_++;
		AstId right = parse_relational_expression();
		if (right == 0)
			return 0;
		AstId result = make_token(AST_BINARY_EXPRESSION, op_at);
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
		const size_t op_at = pos_++;
		AstId right = parse_shift_expression();
		if (right == 0)
			return 0;
		AstId result = make_token(AST_BINARY_EXPRESSION, op_at);
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
		const size_t op_at = pos_;
		AstId result = 0;
		if (is_simple(OP_LSHIFT))
			result = make_token(AST_BINARY_EXPRESSION, pos_++);
		else
		{
			// The two `>>` pieces print as the one shift operator.
			pos_ += 2;
			result = make_span(AST_BINARY_EXPRESSION, op_at, pos_, "OP_RSHIFT:>>");
		}
		AstId right = parse_additive_expression();
		if (right == 0)
			return 0;
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
		const size_t op_at = pos_++;
		AstId right = parse_multiplicative_expression();
		if (right == 0)
			return 0;
		AstId result = make_token(AST_BINARY_EXPRESSION, op_at);
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
		const size_t op_at = pos_++;
		AstId right = parse_pm_expression();
		if (right == 0)
			return 0;
		AstId result = make_token(AST_BINARY_EXPRESSION, op_at);
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
		const size_t op_at = pos_++;
		AstId right = parse_cast_expression();
		if (right == 0)
			return 0;
		AstId result = make_token(AST_BINARY_EXPRESSION, op_at);
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
		const size_t op_at = pos_++;
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
		const AstId result = make_token(AST_CAST_EXPRESSION, op_at);
		add(result, type);
		add(result, expression);
		// A cast-expression is also a postfix-expression when its result is
		// callable.  Keep the suffix attached here so
		// `static_cast<R (&)()>(f)()` is parsed as one expression.
		return parse_postfix_suffixes(result);
	}
	if (is_simple(OP_LPAREN))
	{
		// (type) expr is tried only when the parenthesis holds something that
		// can start a type: a keyword, a bound type name, or an unknown name
		// followed by ')' and the start of an expression.
		const Pa6Token& first = token(pos_ + 1);
		const BindKind* binding = first.kind == PA6_IDENTIFIER_TOKEN ?
			scopes_.Lookup(first.spelling) : 0;
		bool unknown_cast_candidate = false;
		if (first.kind == PA6_IDENTIFIER_TOKEN &&
			is_simple(OP_RPAREN, pos_ + 2))
		{
			const Mark lookahead = mark();
			pos_ += 3;
			unknown_cast_candidate = can_start_expression();
			restore(lookahead);
		}
		const bool candidate = (first.kind == PA6_SIMPLE_TOKEN &&
			(is_builtin_type(first.simple_type) || is_cv_qualifier(first.simple_type)))
			|| (first.kind == PA6_IDENTIFIER_TOKEN && binding != 0 &&
				(*binding == BIND_TYPE || *binding == BIND_TEMPLATE)) ||
			unknown_cast_candidate;
		if (candidate && enter_bracket(OP_LPAREN))
		{
			AstId type = parse_type_id();
			if (type != 0 && leave_bracket(OP_RPAREN))
			{
				AstId expression = parse_cast_expression();
				if (expression != 0)
				{
					// A C-style cast prints the '(' token type with no spelling.
					const AstId result = make_span(AST_CAST_EXPRESSION,
						saved.position, saved.position + 1, "OP_LPAREN:");
					add(result, type);
					add(result, expression);
					return parse_postfix_suffixes(result);
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
	{
		AstId expression = parse_type_trait_expression();
		return expression == 0 ? 0 : parse_postfix_suffixes(expression);
	}
	if (is_simple(KW_NEW) ||
		(is_simple(OP_COLON2) && is_simple(KW_NEW, pos_ + 1)))
	{
		AstId expression = parse_new_expression();
		return expression == 0 ? 0 : parse_postfix_suffixes(expression);
	}
	if (is_simple(KW_DELETE))
		return parse_delete_expression();
	if (is_simple(OP_PLUS) || is_simple(OP_MINUS) || is_simple(OP_LNOT) ||
		is_simple(OP_COMPL) || is_simple(OP_STAR) || is_simple(OP_AMP) ||
		is_simple(OP_INC) || is_simple(OP_DEC))
	{
		const size_t op_at = pos_++;
		AstId expression = parse_unary_expression();
		if (expression == 0)
			return 0;
		const AstId result = make_token(AST_UNARY_EXPRESSION, op_at);
		add(result, expression);
		return result;
	}
	return parse_postfix_expression();
}

AstId Pa10Parser::parse_postfix_expression()
{
	AstId expression = parse_postfix_root();
	if (expression == 0)
		return 0;
	return parse_postfix_suffixes(expression);
}

AstId Pa10Parser::parse_postfix_suffixes(AstId expression)
{
	const Mark saved = mark();
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
		// C++11 list-initialization of a named class type has the same
		// postfix shape as a functional cast; retaining the braced list as
		// the argument node lets semantic analysis select constructors and
		// apply aggregate rules from the destination type.
		if (is_simple(OP_LBRACE) &&
			arena_.At(expression).kind == AST_ID_EXPRESSION &&
			(saved.position < 2 || !is_simple(OP_STAR, saved.position - 2)))
		{
			AstId arguments = parse_braced_init_list();
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
			const size_t op_at = pos_++;
			const size_t name_start = pos_;
			const bool dependent_template = consume_simple(KW_TEMPLATE);
			AstId member = 0;
			// A pseudo-destructor-id is the one postfix member form whose
			// name is not an ordinary id-expression.  Keep its complete token
			// span as the member identifier so semantic analysis can resolve
			// the object type before deciding whether this is a real destructor
			// call or the scalar no-op form.
			if (is_simple(OP_COMPL))
			{
				++pos_;
				if (!is_kind(PA6_IDENTIFIER_TOKEN))
				{
					restore(saved);
					return 0;
				}
				++pos_;
				member = make_join(AST_IDENTIFIER, name_start, pos_);
			}
			else
				member = is_simple(KW_OPERATOR) ?
					parse_operator_function_id() : parse_id_expression();
			if (member == 0)
			{
				restore(saved);
				return 0;
			}
			const string name = arena_.At(member).text;
			const AstId identifier = make_span(AST_IDENTIFIER, name_start, pos_,
				dependent_template ? "template " + name : name);
			const AstId result = make_token(AST_MEMBER_EXPRESSION, op_at);
			add(result, expression);
			add(result, identifier);
			expression = result;
			continue;
		}
		if (is_simple(OP_INC) || is_simple(OP_DEC))
		{
			const AstId result = make_token(AST_POSTFIX_EXPRESSION, pos_++);
			add(result, expression);
			expression = result;
			continue;
		}
		break;
	}
	return expression;
}

AstId Pa10Parser::parse_pack_expansion(AstId expression)
{
	if (!consume_simple(OP_DOTS))
		return expression;
	const AstId result = make(AST_PACK_EXPANSION_EXPRESSION);
	add(result, expression);
	return result;
}

AstId Pa10Parser::parse_postfix_root()
{
	// A functional cast starts with a complete fundamental type-id, not just
	// its first keyword.  Keeping the whole span on the callee lets semantic
	// analysis apply the canonical keyword-combination rules (unsigned long,
	// long long, and their cv-qualified forms).
	const Mark fundamental = mark();
	if (token(pos_).kind == PA6_SIMPLE_TOKEN &&
		(is_builtin_type(token(pos_).simple_type) ||
		 is_cv_qualifier(token(pos_).simple_type)))
	{
		const size_t start = pos_;
		bool have_fundamental = false;
		while (token(pos_).kind == PA6_SIMPLE_TOKEN &&
			(is_builtin_type(token(pos_).simple_type) ||
			 is_cv_qualifier(token(pos_).simple_type)))
		{
			have_fundamental = have_fundamental ||
				is_builtin_type(token(pos_).simple_type);
			++pos_;
		}
		if (have_fundamental && is_simple(OP_LPAREN))
		{
			const AstId callee = make_join(AST_ID_EXPRESSION, start, pos_);
			AstId arguments = parse_argument_list(AST_PAREN_ARGUMENT_LIST);
			if (arguments == 0)
			{
				restore(fundamental);
				return 0;
			}
			const AstId result = make(AST_CALL_EXPRESSION);
			add(result, callee);
			add(result, arguments);
			return result;
		}
		restore(fundamental);
	}

	// `decltype(e)(value)` has the same postfix-call AST shape as a
	// fundamental functional cast, but the parsed decltype operand must remain
	// a child of the callee for semantic type construction.
	const Mark decltype_mark = mark();
	if (is_simple(KW_DECLTYPE))
	{
		const AstId decltype_node = parse_decltype_specifier(false);
		if (decltype_node != 0 && is_simple(OP_LPAREN))
		{
			const AstId callee = make_join(AST_ID_EXPRESSION,
				decltype_mark.position, pos_);
			add(callee, decltype_node);
			AstId arguments = parse_argument_list(AST_PAREN_ARGUMENT_LIST);
			if (arguments == 0)
			{
				restore(decltype_mark);
				return 0;
			}
			const AstId result = make(AST_CALL_EXPRESSION);
			add(result, callee);
			add(result, arguments);
			return result;
		}
		restore(decltype_mark);
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
	{
		const AstId literal = make_span(AST_LITERAL, pos_, pos_ + 1,
			token(pos_).spelling);
		++pos_;
		return literal;
	}
	if (token(pos_).kind == PA6_SIMPLE_TOKEN &&
		is_keyword_literal(token(pos_).simple_type))
		return make_token(AST_KEYWORD_LITERAL, pos_++);
	if (is_kind(PA6_IDENTIFIER_TOKEN) || is_simple(OP_COLON2) ||
		is_simple(KW_DECLTYPE))
		return parse_id_expression();
	return 0;
}

// An id-expression: decltype(e)::name..., or an optionally qualified name
// whose components may be template-ids.  A template-id followed by an
// identifier or literal, or by '{' inside a template argument, is re-read as
// a comparison (a < b > c).
AstId Pa10Parser::parse_id_expression()
{
	const Mark saved = mark();
	const size_t start = pos_;
	if (is_simple(KW_DECLTYPE))
	{
		if (parse_decltype_specifier(false) == 0 ||
			!consume_simple(OP_COLON2) || !is_kind(PA6_IDENTIFIER_TOKEN))
		{
			restore(saved);
			return 0;
		}
		++pos_;
		while (consume_simple(OP_COLON2))
		{
			if (!is_kind(PA6_IDENTIFIER_TOKEN))
			{
				restore(saved);
				return 0;
			}
			++pos_;
		}
		return make_join(AST_ID_EXPRESSION, start, pos_);
	}
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
	while (true)
	{
		const Mark component = mark();
		const bool nested_template_context = has_angle_boundary();
		if (parse_simple_template_id() == 0 ||
			!CanContinueTemplateIdExpression(token(pos_), nested_template_context))
		{
			restore(component);
			++pos_;
		}
		if (!consume_simple(OP_COLON2))
			break;
		if (!is_kind(PA6_IDENTIFIER_TOKEN))
		{
			restore(saved);
			return 0;
		}
	}
	return make_join(AST_ID_EXPRESSION, start, pos_);
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
		add(result, parse_pack_expansion(argument));
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

// sizeof, alignof, typeid and noexcept applied to a parenthesized type-id or
// expression.  A sizeof node prints no operator text; the others print it.
AstId Pa10Parser::parse_type_trait_expression()
{
	const Mark saved = mark();
	const size_t op_at = pos_++;
	const bool is_sizeof = token(op_at).IsSimple(KW_SIZEOF);
	const AstKind kind = is_sizeof ? AST_SIZEOF_EXPRESSION :
		AST_TYPE_TRAIT_EXPRESSION;
	if (!enter_bracket(OP_LPAREN))
	{
		restore(saved);
		return 0;
	}
	const Mark type_mark = mark();
	const Pa6Token& first = token(pos_);
	const BindKind* binding = first.kind == PA6_IDENTIFIER_TOKEN ?
		scopes_.Lookup(first.spelling) : 0;
	const bool functional_cast = first.kind == PA6_IDENTIFIER_TOKEN &&
		is_simple(OP_LPAREN, pos_ + 1);
	const bool type_start = (first.kind == PA6_SIMPLE_TOKEN &&
		(is_builtin_type(first.simple_type) || is_cv_qualifier(first.simple_type) ||
		 first.IsSimple(KW_CLASS) || first.IsSimple(KW_STRUCT) ||
		 first.IsSimple(KW_UNION) || first.IsSimple(KW_ENUM) ||
		 first.IsSimple(KW_DECLTYPE))) ||
		(first.kind == PA6_IDENTIFIER_TOKEN && binding != 0 &&
		 (*binding == BIND_TYPE || *binding == BIND_TEMPLATE)) ||
		(first.kind == PA6_IDENTIFIER_TOKEN &&
		 (is_simple(OP_STAR, pos_ + 1) || is_simple(OP_AMP, pos_ + 1) ||
		  is_simple(OP_LAND, pos_ + 1)));
	AstId operand = 0;
	if (!functional_cast && type_start)
	{
		operand = parse_type_id();
		if (operand == 0 || !leave_bracket(OP_RPAREN))
		{
			operand = 0;
			restore(type_mark);
		}
	}
	if (operand == 0)
	{
		operand = parse_expression();
		if (operand == 0 || !leave_bracket(OP_RPAREN))
		{
			restore(saved);
			return 0;
		}
	}
	const AstId result = is_sizeof ? make_span(kind, op_at, op_at + 1, "") :
		make_token(kind, op_at);
	add(result, operand);
	return result;
}

AstId Pa10Parser::parse_new_expression()
{
	const Mark saved = mark();
	const AstId result = make(AST_NEW_EXPRESSION);
	if (is_simple(OP_COLON2))
	{
		add(result, make_span(AST_GLOBAL_SCOPE, pos_, pos_ + 1, ""));
		++pos_;
	}
	if (!consume_simple(KW_NEW))
	{
		restore(saved);
		return 0;
	}
	if (is_simple(OP_LPAREN))
	{
		// A parenthesized list is a placement only when a type follows it.
		const Mark placement_mark = mark();
		AstId placement_args = parse_argument_list(AST_PAREN_ARGUMENT_LIST);
		if (placement_args != 0 && (is_builtin_type(token(pos_).simple_type) ||
			is_kind(PA6_IDENTIFIER_TOKEN)))
		{
			const AstId placement = make_span(AST_PLACEMENT,
				placement_mark.position, pos_,
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
			if (type == 0 || !leave_bracket(OP_RPAREN))
			{
				type = 0;
				restore(parenthesized_type);
			}
		}
	}
	if (type == 0)
		type = parse_type_id(false);
	if (type == 0)
	{
		restore(saved);
		return 0;
	}
	add(result, type);
	if (is_simple(OP_LPAREN) || is_simple(OP_LBRACE))
	{
		AstId initializer = is_simple(OP_LPAREN) ? parse_paren_initializer() :
			parse_braced_init_list();
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
	if (is_simple(OP_LSQUARE) && is_simple(OP_RSQUARE, pos_ + 1))
	{
		add(result, make_span(AST_ARRAY_DELETE, pos_, pos_ + 2, ""));
		pos_ += 2;
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
	// The capture list is preserved as its joined spelling.
	while (!is_simple(OP_RSQUARE))
	{
		if (at_end())
		{
			restore(saved);
			return 0;
		}
		++pos_;
	}
	leave_bracket(OP_RSQUARE);
	const AstId result = make(AST_LAMBDA_EXPRESSION);
	add(result, make_join(AST_LAMBDA_INTRODUCER, start, pos_));
	AstId parameters = 0;
	if (is_simple(OP_LPAREN) || is_simple(KW_MUTABLE) ||
		is_simple(KW_NOEXCEPT) || is_simple(OP_ARROW))
	{
		const AstId declarator = make(AST_LAMBDA_DECLARATOR);
		if (is_simple(OP_LPAREN))
		{
			parameters = parse_parameter_clause();
			add(declarator, parameters);
		}
		if (is_simple(KW_MUTABLE))
			add(declarator, make_token(AST_LAMBDA_SPECIFIER, pos_++));
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
		if (consume_simple(OP_ARROW))
		{
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
	// Lambda parameters are values inside the body.
	scopes_.Push();
	bind_parameters(parameters);
	AstId body = parse_compound_statement();
	scopes_.Pop();
	if (body == 0)
	{
		restore(saved);
		return 0;
	}
	add(result, body);
	return result;
}
