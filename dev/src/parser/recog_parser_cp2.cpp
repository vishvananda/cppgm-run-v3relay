#include "recog_parser.h"

using namespace std;

bool Pa6Parser::parse_template_argument_dots()
{
	if (!parse_template_argument())
		return false;
	consume_simple(OP_DOTS);
	return true;
}

bool Pa6Parser::parse_template_argument()
{
	return try_memoized(MEMO_TEMPLATE_ARGUMENT,
		&Pa6Parser::parse_template_argument_impl);
}

bool Pa6Parser::parse_template_argument_impl()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	const bool old_angle_refusal = angle_refusal_;
	angle_refusal_ = true;
	if (parse_constant_expression())
	{
		angle_refusal_ = old_angle_refusal;
		return true;
	}
	if (hard_failure_)
	{
		angle_refusal_ = old_angle_refusal;
		return false;
	}
	restore(start, context);
	angle_refusal_ = true;
	if (parse_type_id())
	{
		angle_refusal_ = old_angle_refusal;
		return true;
	}
	if (hard_failure_)
	{
		angle_refusal_ = old_angle_refusal;
		return false;
	}
	restore(start, context);
	angle_refusal_ = true;
	if (parse_id_expression())
	{
		angle_refusal_ = old_angle_refusal;
		return true;
	}
	angle_refusal_ = old_angle_refusal;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_template_argument_suffix()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(OP_LT))
		return false;
	brackets_.push_back(BRACKET_ANGLE);
	if ((!is_simple(OP_GT) && !is_kind(PA6_RSHIFT_1_TOKEN)) &&
		!parse_template_argument_list())
	{
		restore(start, context);
		return false;
	}
	if (!parse_close_angle_bracket())
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_template_parameter_list()
{
	if (!parse_template_parameter())
		return false;
	while (consume_simple(OP_COMMA))
		if (!parse_template_parameter())
			return false;
	return true;
}

bool Pa6Parser::parse_template_parameter()
{
	if (is_simple(KW_CLASS) || is_simple(KW_TYPENAME) ||
		is_simple(KW_TEMPLATE))
		return parse_type_parameter();
	return parse_parameter_declaration();
}

bool Pa6Parser::parse_type_parameter()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (consume_simple(KW_TEMPLATE))
	{
		if (!consume_simple(OP_LT))
			goto fail;
		brackets_.push_back(BRACKET_ANGLE);
		if (!parse_template_parameter_list() || !parse_close_angle_bracket() ||
			!consume_simple(KW_CLASS))
			goto fail;
		consume_simple(OP_DOTS);
		consume_identifier();
		if (consume_simple(OP_ASS) && !parse_id_expression())
			goto fail;
		return true;
	}
	if (!consume_simple(KW_CLASS) && !consume_simple(KW_TYPENAME))
		goto fail;
	consume_simple(OP_DOTS);
	consume_identifier();
	if (consume_simple(OP_ASS) && !parse_type_id())
		goto fail;
	return true;
fail:
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_class_specifier()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_CLASS) && !consume_simple(KW_STRUCT) &&
		!consume_simple(KW_UNION))
		return false;
	while (parse_attribute_specifier())
	{
	}
	if (!is_simple(OP_LBRACE) && !is_simple(OP_COLON))
	{
		const size_t qualified = pos_;
		const size_t qualified_context = brackets_.size();
		if (parse_nested_name_specifier())
		{
			if (!parse_class_name() && !consume_identifier())
			{
				restore(start, context);
				return false;
			}
		}
		else
		{
			restore(qualified, qualified_context);
			if (!parse_class_name() && !consume_identifier())
			{
				restore(start, context);
				return false;
			}
		}
		if (token(pos_).flags & PA6_FINAL_FLAG)
			++pos_;
	}
	parse_base_clause();
	if (!enter_bracket(OP_LBRACE))
	{
		restore(start, context);
		return false;
	}
	while (!is_simple(OP_RBRACE))
	{
		if ((is_simple(KW_PRIVATE) || is_simple(KW_PROTECTED) ||
			is_simple(KW_PUBLIC, pos_)) && is_simple(OP_COLON, pos_ + 1))
		{
			pos_ += 2;
			continue;
		}
		if (!parse_member_declaration())
		{
			restore(start, context);
			return false;
		}
	}
	if (!leave_bracket(OP_RBRACE))
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_enum_specifier()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_ENUM))
		return false;
	consume_simple(KW_CLASS) || consume_simple(KW_STRUCT);
	while (parse_attribute_specifier())
	{
	}
	if (!is_simple(OP_LBRACE) && !is_simple(OP_COLON))
	{
		const size_t qualified = pos_;
		const size_t qualified_context = brackets_.size();
		if (parse_nested_name_specifier() && consume_identifier())
		{
		}
		else
		{
			restore(qualified, qualified_context);
			if (!consume_identifier())
			{
				restore(start, context);
				return false;
			}
		}
	}
	if (consume_simple(OP_COLON) && !parse_type_specifier_seq())
	{
		restore(start, context);
		return false;
	}
	if (!is_simple(OP_LBRACE))
		return true;
	if (!enter_bracket(OP_LBRACE))
		goto fail;
	if (!is_simple(OP_RBRACE))
	{
		for (;;)
		{
			if (!consume_identifier())
				goto fail;
			if (consume_simple(OP_ASS) && !parse_constant_expression())
				goto fail;
			if (!consume_simple(OP_COMMA))
				break;
			if (is_simple(OP_RBRACE))
				break;
		}
	}
	if (!leave_bracket(OP_RBRACE))
		goto fail;
	return true;
fail:
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_namespace_alias_definition()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_NAMESPACE) || !consume_identifier() ||
		!consume_simple(OP_ASS))
		goto fail;
	{
		const size_t qualified = pos_;
		const size_t qualified_context = brackets_.size();
		if (parse_nested_name_specifier())
		{
			if (!parse_namespace_name())
				goto fail;
		}
		else
		{
			restore(qualified, qualified_context);
			if (!parse_namespace_name())
				goto fail;
		}
	}
	if (!consume_simple(OP_SEMICOLON))
		goto fail;
	return true;
fail:
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_namespace_definition()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	consume_simple(KW_INLINE);
	if (!consume_simple(KW_NAMESPACE))
		goto fail;
	if (token(pos_).IsIdentifier())
		++pos_;
	if (!enter_bracket(OP_LBRACE))
		goto fail;
	while (!is_simple(OP_RBRACE))
	{
		if (!parse_declaration())
			goto fail;
	}
	if (!leave_bracket(OP_RBRACE))
		goto fail;
	consume_simple(OP_SEMICOLON);
	return true;
fail:
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_template_declaration()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_TEMPLATE) || !consume_simple(OP_LT))
		goto fail;
	brackets_.push_back(BRACKET_ANGLE);
	if (!parse_template_parameter_list() || !parse_close_angle_bracket() ||
		!parse_declaration())
		goto fail;
	return true;
fail:
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_explicit_instantiation()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	consume_simple(KW_EXTERN);
	if (!consume_simple(KW_TEMPLATE) || !parse_declaration())
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_explicit_specialization()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_TEMPLATE) || !consume_simple(OP_LT))
		goto fail;
	brackets_.push_back(BRACKET_ANGLE);
	if (!parse_close_angle_bracket() || !parse_declaration())
		goto fail;
	return true;
fail:
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_linkage_specification()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_EXTERN) || !consume_literal())
		goto fail;
	if (enter_bracket(OP_LBRACE))
	{
		while (!is_simple(OP_RBRACE))
			if (!parse_declaration())
			{
				restore(start, context);
				return false;
			}
		if (!leave_bracket(OP_RBRACE))
			goto fail;
		consume_simple(OP_SEMICOLON);
		return true;
	}
	if (parse_declaration())
		return true;
fail:
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_alias_declaration()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_USING) || !consume_identifier())
	{
		restore(start, context);
		return false;
	}
	while (parse_attribute_specifier())
	{
	}
	if (!consume_simple(OP_ASS) || !parse_type_id() ||
		!consume_simple(OP_SEMICOLON))
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_using_declaration()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_USING))
		return false;
	consume_simple(KW_TYPENAME);
	if (!consume_simple(OP_COLON2) && !parse_nested_name_specifier())
		goto fail;
	if (!parse_unqualified_id() || !consume_simple(OP_SEMICOLON))
		goto fail;
	return true;
fail:
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_using_directive()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	while (parse_attribute_specifier())
	{
	}
	if (!consume_simple(KW_USING) || !consume_simple(KW_NAMESPACE))
	{
		restore(start, context);
		return false;
	}
	const size_t qualified = pos_;
	const size_t qualified_context = brackets_.size();
	if (parse_nested_name_specifier() && consume_identifier() &&
		consume_simple(OP_SEMICOLON))
		return true;
	restore(qualified, qualified_context);
	if (consume_identifier() && consume_simple(OP_SEMICOLON))
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_asm_definition()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_ASM) || !enter_bracket(OP_LPAREN) ||
		!consume_literal() || !leave_bracket(OP_RPAREN) ||
		!consume_simple(OP_SEMICOLON))
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_exception_specification()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (consume_simple(KW_THROW))
	{
		if (!enter_bracket(OP_LPAREN))
			goto fail;
		if (!is_simple(OP_RPAREN))
		{
			if (!parse_type_id())
				goto fail;
			while (consume_simple(OP_COMMA))
			{
				if (!parse_type_id())
					goto fail;
			}
		}
		if (!leave_bracket(OP_RPAREN))
			goto fail;
		return true;
	}
	restore(start, context);
	if (!consume_simple(KW_NOEXCEPT))
		return false;
	if (enter_bracket(OP_LPAREN))
	{
		if (!parse_constant_expression() || !leave_bracket(OP_RPAREN))
			goto fail;
	}
	return true;
fail:
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_lambda_expression()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!enter_bracket(OP_LSQUARE))
		return false;
	if (!is_simple(OP_RSQUARE))
	{
		const bool default_capture = consume_simple(OP_AMP) ||
			consume_simple(OP_ASS);
		if (!default_capture && !consume_simple(KW_THIS) &&
			!consume_identifier())
			goto fail;
		if (default_capture)
		{
			if (consume_simple(OP_COMMA))
			{
				if (is_simple(OP_RSQUARE))
					goto fail;
				if (consume_simple(KW_THIS) || consume_simple(OP_AMP))
				{
					if (!consume_identifier())
						goto fail;
				}
				else if (!consume_identifier())
					goto fail;
				consume_simple(OP_DOTS);
				while (consume_simple(OP_COMMA))
				{
					if (consume_simple(KW_THIS))
					{
					}
					else
					{
						consume_simple(OP_AMP);
						if (!consume_identifier())
							goto fail;
					}
					consume_simple(OP_DOTS);
				}
			}
		}
		else
		{
			consume_simple(OP_DOTS);
			while (consume_simple(OP_COMMA))
			{
				if (consume_simple(KW_THIS))
				{
				}
				else
				{
					consume_simple(OP_AMP);
					if (!consume_identifier())
						goto fail;
				}
				consume_simple(OP_DOTS);
			}
		}
	}
	if (!leave_bracket(OP_RSQUARE))
		goto fail;
	if (is_simple(OP_LPAREN))
	{
		if (!enter_bracket(OP_LPAREN) || !parse_parameter_declaration_clause() ||
			!leave_bracket(OP_RPAREN))
			goto fail;
		consume_simple(KW_MUTABLE);
		parse_exception_specification();
		while (parse_attribute_specifier())
		{
		}
		if (is_simple(OP_ARROW) && !parse_trailing_return_type())
			goto fail;
	}
	if (!parse_compound_statement())
		goto fail;
	return true;
fail:
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_base_clause()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(OP_COLON) || !parse_base_specifier_list())
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_base_specifier_list()
{
	if (!parse_base_specifier())
		return false;
	while (consume_simple(OP_COMMA))
		if (!parse_base_specifier())
			return false;
	return true;
}

bool Pa6Parser::parse_base_specifier()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	while (parse_attribute_specifier())
	{
	}
	if (consume_simple(KW_VIRTUAL))
	{
		consume_simple(KW_PRIVATE) || consume_simple(KW_PROTECTED) ||
			consume_simple(KW_PUBLIC);
	}
	else if (is_simple(KW_PRIVATE) || is_simple(KW_PROTECTED) ||
		is_simple(KW_PUBLIC))
	{
		++pos_;
		consume_simple(KW_VIRTUAL);
	}
	const size_t qualified = pos_;
	const size_t qualified_context = brackets_.size();
	if (parse_nested_name_specifier() && parse_class_name())
	{
		consume_simple(OP_DOTS);
		return true;
	}
	restore(qualified, qualified_context);
	if (parse_class_name() || parse_decltype_specifier())
	{
		consume_simple(OP_DOTS);
		return true;
	}
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_member_declaration()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_function_definition())
	{
		consume_simple(OP_SEMICOLON);
		return true;
	}
	restore(start, context);
	if (parse_using_declaration() || parse_static_assert_declaration() ||
		parse_template_declaration() || parse_alias_declaration())
		return true;
	restore(start, context);
	while (parse_attribute_specifier())
	{
	}
	if (!parse_decl_specifier_seq())
	{
		restore(start, context);
		return false;
	}
	if (is_simple(OP_SEMICOLON))
	{
		++pos_;
		return true;
	}
	for (;;)
	{
		if (is_simple(OP_COLON))
		{
			++pos_;
		}
		else if (parse_declarator())
		{
			while (token(pos_).flags & (PA6_FINAL_FLAG | PA6_OVERRIDE_FLAG))
				++pos_;
			if (is_simple(OP_COLON))
			{
				++pos_;
			}
			else
			{
				const size_t after_declarator = pos_;
				const size_t after_context = brackets_.size();
				if (!parse_brace_or_equal_initializer())
					restore(after_declarator, after_context);
			}
		}
		else
		{
			restore(start, context);
			return false;
		}
		if (start != pos_ && token(pos_ - 1).IsSimple(OP_COLON))
		{
			if (!parse_constant_expression())
			{
				restore(start, context);
				return false;
			}
		}
		if (!consume_simple(OP_COMMA))
			break;
	}
	if (!consume_simple(OP_SEMICOLON))
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_ctor_initializer()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(OP_COLON) || !parse_mem_initializer_list())
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_mem_initializer_list()
{
	if (!parse_mem_initializer())
		return false;
	while (consume_simple(OP_COMMA))
		if (!parse_mem_initializer())
			return false;
	return true;
}

bool Pa6Parser::parse_mem_initializer()
{
	if (!parse_id_expression())
		return false;
	if (enter_bracket(OP_LPAREN))
	{
		if ((!is_simple(OP_RPAREN) && !parse_expression_list()) ||
			!leave_bracket(OP_RPAREN))
			return false;
	}
	else if (!parse_braced_init_list())
		return false;
	consume_simple(OP_DOTS);
	return true;
}

bool Pa6Parser::parse_try_block()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_TRY) || !parse_compound_statement() ||
		!parse_handler())
		goto fail;
	while (parse_handler())
	{
	}
	return true;
fail:
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_handler()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_CATCH) || !enter_bracket(OP_LPAREN) ||
		!parse_exception_declaration() || !leave_bracket(OP_RPAREN) ||
		!parse_compound_statement())
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_exception_declaration()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	while (parse_attribute_specifier())
	{
	}
	if (consume_simple(OP_DOTS))
		return true;
	if (!parse_type_specifier_seq())
	{
		restore(start, context);
		return false;
	}
	while (parse_attribute_specifier())
	{
	}
	const size_t declarator = pos_;
	const size_t declarator_context = brackets_.size();
	if (!parse_declarator())
		restore(declarator, declarator_context);
	return true;
}
