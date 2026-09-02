#include "ast_parser.h"

using namespace std;

namespace
{

bool IsTypeLikeBinding(const BindKind* binding)
{
	return binding == 0 || *binding == BIND_TYPE || *binding == BIND_TEMPLATE;
}

string Unquote(const string& spelling)
{
	if (spelling.size() >= 2 && spelling[0] == '"' &&
		spelling[spelling.size() - 1] == '"')
		return spelling.substr(1, spelling.size() - 2);
	return spelling;
}

} // namespace

AstId Pa10Parser::parse_template_declaration(bool member_context)
{
	const Mark saved = mark();
	if (!consume_simple(KW_TEMPLATE) || !consume_simple(OP_LT))
		return 0;
	brackets_.push_back(BRACKET_ANGLE);
	scopes_.Push();
	const AstId parameters = parse_template_parameter_clause();
	if (parameters == 0 || !parse_close_angle_bracket())
	{
		scopes_.Pop();
		restore(saved);
		return 0;
	}
	const AstId declaration = parse_declaration(member_context);
	if (declaration == 0)
	{
		scopes_.Pop();
		restore(saved);
		return 0;
	}
	scopes_.Pop();
	const AstId result = make(AST_TEMPLATE_DECLARATION);
	add(result, parameters);
	add(result, declaration);
	bind_template_declaration(declaration);
	return result;
}

AstId Pa10Parser::parse_explicit_instantiation_declaration()
{
	const Mark saved = mark();
	consume_simple(KW_EXTERN);
	if (!consume_simple(KW_TEMPLATE) || is_simple(OP_LT))
	{
		restore(saved);
		return 0;
	}
	const AstId declaration = parse_declaration();
	if (declaration == 0)
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_EXPLICIT_INSTANTIATION_DECLARATION);
	add(result, declaration);
	return result;
}

AstId Pa10Parser::parse_template_parameter_clause()
{
	const AstId result = make(AST_TEMPLATE_PARAMETER_CLAUSE);
	if (is_simple(OP_GT) || is_kind(PA6_RSHIFT_1_TOKEN) ||
		is_kind(PA6_RSHIFT_2_TOKEN))
		return result;
	const AstId list = parse_template_parameter_list();
	if (list == 0)
		return 0;
	add(result, list);
	return result;
}

AstId Pa10Parser::parse_template_parameter_list()
{
	const Mark saved = mark();
	const AstId result = make(AST_TEMPLATE_PARAMETER_LIST);
	AstId parameter = parse_template_parameter();
	if (parameter == 0)
	{
		restore(saved);
		return 0;
	}
	add(result, parameter);
	while (consume_simple(OP_COMMA))
	{
		parameter = parse_template_parameter();
		if (parameter == 0)
		{
			restore(saved);
			return 0;
		}
		add(result, parameter);
	}
	return result;
}

AstId Pa10Parser::parse_template_parameter()
{
	if (is_simple(KW_TEMPLATE))
		return parse_template_template_parameter();
	if (is_simple(KW_CLASS) || is_simple(KW_TYPENAME))
		return parse_type_template_parameter();
	return parse_non_type_template_parameter();
}

AstId Pa10Parser::parse_parameter_pack()
{
	if (!is_simple(OP_DOTS))
		return 0;
	const AstId pack = make_span(AST_PARAMETER_PACK, pos_, pos_ + 1, "...");
	++pos_;
	return pack;
}

AstId Pa10Parser::parse_type_template_parameter()
{
	const Mark saved = mark();
	if (!is_simple(KW_CLASS) && !is_simple(KW_TYPENAME))
		return 0;
	const AstId result = make(AST_TYPE_PARAMETER);
	add(result, make_token(AST_PARAMETER_KEY, pos_++));
	add(result, parse_parameter_pack());
	string name;
	if (is_kind(PA6_IDENTIFIER_TOKEN))
	{
		name = token(pos_).spelling;
		add(result, make_span(AST_IDENTIFIER, pos_, pos_ + 1, name));
		++pos_;
	}
	if (consume_simple(OP_ASS))
	{
		const AstId type = parse_type_id();
		if (type == 0)
		{
			restore(saved);
			return 0;
		}
		const AstId default_argument = make(AST_DEFAULT_TEMPLATE_ARGUMENT);
		add(default_argument, type);
		add(result, default_argument);
	}
	scopes_.Bind(name, BIND_TYPE);
	return result;
}

AstId Pa10Parser::parse_template_template_parameter()
{
	const Mark saved = mark();
	if (!consume_simple(KW_TEMPLATE) || !consume_simple(OP_LT))
		return 0;
	brackets_.push_back(BRACKET_ANGLE);
	const AstId nested_clause = parse_template_parameter_clause();
	if (nested_clause == 0 || !parse_close_angle_bracket() ||
		(!is_simple(KW_CLASS) && !is_simple(KW_TYPENAME)))
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_TYPE_PARAMETER);
	add(result, make(AST_TEMPLATE_TEMPLATE_PARAMETER));
	add(result, nested_clause);
	add(result, make_token(AST_PARAMETER_KEY, pos_++));
	add(result, parse_parameter_pack());
	string name;
	if (is_kind(PA6_IDENTIFIER_TOKEN))
	{
		name = token(pos_).spelling;
		add(result, make_span(AST_IDENTIFIER, pos_, pos_ + 1, name));
		++pos_;
	}
	if (consume_simple(OP_ASS))
	{
		const AstId type = parse_type_id();
		if (type == 0)
		{
			restore(saved);
			return 0;
		}
		const AstId default_argument = make(AST_DEFAULT_TEMPLATE_ARGUMENT);
		add(default_argument, type);
		add(result, default_argument);
	}
	scopes_.Bind(name, BIND_TEMPLATE);
	return result;
}

AstId Pa10Parser::parse_non_type_template_parameter()
{
	const Mark saved = mark();
	const AstId specifiers = parse_decl_specifier_seq();
	if (specifiers == 0)
		return 0;
	const AstId result = make(AST_NON_TYPE_TEMPLATE_PARAMETER);
	add(result, specifiers);
	add(result, parse_parameter_pack());
	AstId declarator = 0;
	if (is_kind(PA6_IDENTIFIER_TOKEN) || is_simple(OP_COLON2) ||
		is_simple(OP_STAR) || is_simple(OP_AMP) || is_simple(OP_LAND) ||
		is_simple(OP_LPAREN))
	{
		declarator = parse_declarator(true);
		if (declarator == 0)
		{
			restore(saved);
			return 0;
		}
	}
	add(result, declarator);
	if (consume_simple(OP_ASS))
	{
		const size_t default_start = pos_;
		AstId expression = parse_assignment_expression();
		if (expression == 0)
		{
			restore(saved);
			return 0;
		}
		// Fixture-pinned: the default of a declarator-less parameter whose
		// type is a single keyword prints as the token (literal TT_LITERAL:0)
		// rather than as a literal spelling.
		const vector<AstId>& types = arena_.At(specifiers).children;
		const AstNode& type = arena_.At(types.size() == 1 ? types[0] : specifiers);
		const AstNode& value = arena_.At(expression);
		const bool keyword_type = declarator == 0 && types.size() == 1 &&
			type.kind == AST_DECL_SPECIFIER && type.last == type.first + 1 &&
			token(type.first).kind == PA6_SIMPLE_TOKEN &&
			is_builtin_type(token(type.first).simple_type);
		const bool single_literal = value.kind == AST_LITERAL &&
			value.first == default_start && value.last == default_start + 1;
		if (keyword_type && single_literal)
			expression = make_token(AST_LITERAL, default_start);
		const AstId default_argument = make(AST_DEFAULT_TEMPLATE_ARGUMENT);
		add(default_argument, expression);
		add(result, default_argument);
	}
	bind_declarator_name(declarator, BIND_VALUE);
	return result;
}

AstId Pa10Parser::parse_namespace_definition()
{
	const Mark saved = mark();
	const size_t inline_at = pos_;
	const bool is_inline = consume_simple(KW_INLINE);
	if (!consume_simple(KW_NAMESPACE))
	{
		restore(saved);
		return 0;
	}
	string name;
	AstId result = 0;
	if (is_kind(PA6_IDENTIFIER_TOKEN))
	{
		name = token(pos_).spelling;
		result = make_span(AST_NAMESPACE_DEFINITION, pos_, pos_ + 1, name);
		++pos_;
	}
	else
		result = make_span(AST_NAMESPACE_DEFINITION, 0, 0, "<unnamed>");
	if (!enter_bracket(OP_LBRACE))
	{
		restore(saved);
		return 0;
	}
	scopes_.Bind(name, BIND_NAMESPACE);
	scopes_.Push();
	if (is_inline)
		add(result, make_span(AST_INLINE, inline_at, inline_at + 1, ""));
	while (!is_simple(OP_RBRACE))
	{
		const AstId declaration = at_end() ? 0 : parse_declaration();
		if (declaration == 0)
		{
			scopes_.Pop();
			restore(saved);
			return 0;
		}
		add(result, declaration);
	}
	if (!leave_bracket(OP_RBRACE))
	{
		scopes_.Pop();
		restore(saved);
		return 0;
	}
	scopes_.Pop();
	return result;
}

AstId Pa10Parser::parse_namespace_alias_definition()
{
	const Mark saved = mark();
	if (!consume_simple(KW_NAMESPACE) || !is_kind(PA6_IDENTIFIER_TOKEN))
	{
		restore(saved);
		return 0;
	}
	const size_t name_at = pos_++;
	if (!consume_simple(OP_ASS))
	{
		restore(saved);
		return 0;
	}
	const size_t target_start = pos_;
	if (parse_qualified_name() == 0 || !consume_simple(OP_SEMICOLON))
	{
		restore(saved);
		return 0;
	}
	const AstId result = make_span(AST_NAMESPACE_ALIAS_DEFINITION, name_at,
		name_at + 1, token(name_at).spelling);
	add(result, make_join(AST_TARGET, target_start, pos_ - 1));
	scopes_.Bind(token(name_at).spelling, BIND_NAMESPACE);
	return result;
}

AstId Pa10Parser::parse_using_directive()
{
	const Mark saved = mark();
	if (!consume_simple(KW_USING) || !consume_simple(KW_NAMESPACE))
	{
		restore(saved);
		return 0;
	}
	const size_t target_start = pos_;
	if (parse_qualified_name() == 0 || !consume_simple(OP_SEMICOLON))
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_USING_DIRECTIVE);
	add(result, make_join(AST_TARGET, target_start, pos_ - 1));
	return result;
}

AstId Pa10Parser::parse_using_declaration()
{
	const Mark saved = mark();
	if (!consume_simple(KW_USING))
	{
		restore(saved);
		return 0;
	}
	if (consume_simple(KW_TYPENAME) && !is_kind(PA6_IDENTIFIER_TOKEN) &&
		!is_simple(OP_COLON2))
	{
		restore(saved);
		return 0;
	}
	const size_t target_start = pos_;
	if (parse_qualified_name() == 0 || !consume_simple(OP_SEMICOLON))
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_USING_DECLARATION);
	add(result, make_join(AST_TARGET, target_start, pos_ - 1));
	return result;
}

AstId Pa10Parser::parse_linkage_specification()
{
	const Mark saved = mark();
	if (!consume_simple(KW_EXTERN) || !is_kind(PA6_LITERAL_TOKEN))
	{
		restore(saved);
		return 0;
	}
	const size_t language_at = pos_++;
	const AstId result = make_span(AST_LINKAGE_SPECIFICATION, language_at,
		language_at + 1, Unquote(token(language_at).spelling));
	if (enter_bracket(OP_LBRACE))
	{
		while (!is_simple(OP_RBRACE))
		{
			if (at_end())
			{
				restore(saved);
				return 0;
			}
			const AstId declaration = parse_declaration();
			if (declaration == 0)
			{
				restore(saved);
				return 0;
			}
			add(result, declaration);
		}
		if (!leave_bracket(OP_RBRACE))
		{
			restore(saved);
			return 0;
		}
		return result;
	}

	const AstId declaration = parse_declaration();
	if (declaration == 0)
	{
		restore(saved);
		return 0;
	}
	add(result, declaration);
	return result;
}

AstId Pa10Parser::parse_base_clause()
{
	const Mark saved = mark();
	if (!consume_simple(OP_COLON))
		return 0;
	const AstId result = make(AST_BASE_CLAUSE);
	while (true)
	{
		const AstId base = make(AST_BASE_SPECIFIER);
		while (is_simple(KW_VIRTUAL) || is_simple(KW_PUBLIC) ||
			is_simple(KW_PROTECTED) || is_simple(KW_PRIVATE))
		{
			add(base, make_token(is_simple(KW_VIRTUAL) ? AST_VIRTUAL :
				AST_ACCESS_SPECIFIER, pos_));
			++pos_;
		}
		const size_t name_start = pos_;
		const AstId base_name = is_simple(KW_DECLTYPE) ?
			parse_decltype_specifier(false) : parse_qualified_name();
		if (base_name == 0)
		{
			restore(saved);
			return 0;
		}
		add(base, make_join(AST_BASE_NAME, name_start, pos_));
		add(result, base);
		if (!consume_simple(OP_COMMA))
			break;
	}
	return result;
}

AstId Pa10Parser::parse_class_specifier()
{
	const Mark saved = mark();
	const size_t key_at = pos_;
	if (!is_simple(KW_CLASS) && !is_simple(KW_STRUCT) &&
		!is_simple(KW_UNION))
		return 0;
	++pos_;
	(void)consume_attribute_specifiers();
	const size_t name_start = pos_;
	if (is_kind(PA6_IDENTIFIER_TOKEN) &&
		(!is_simple(OP_LT, pos_ + 1) || parse_simple_template_id() == 0))
		++pos_;
	const size_t name_end = pos_;
	AstId bases = 0;
	if (is_simple(OP_COLON))
	{
		bases = parse_base_clause();
		if (bases == 0)
		{
			restore(saved);
			return 0;
		}
	}
	if (!is_simple(OP_LBRACE))
	{
		if (bases != 0)
		{
			restore(saved);
			return 0;
		}
		const AstId result = make_join(AST_CLASS_FORWARD_DECLARATION,
			name_start, name_end);
		add(result, make_token(AST_CLASS_KEY, key_at));
		scopes_.Bind(declared_identifier(result), BIND_TYPE);
		return result;
	}
	const AstId result = make_join(AST_CLASS_SPECIFIER, name_start, name_end);
	// The class name is visible inside its own body; special members are
	// recognised by the unqualified name even for partial specializations.
	scopes_.Bind(declared_identifier(result), BIND_TYPE);
	const string class_name = template_name(result);
	enter_bracket(OP_LBRACE);
	scopes_.Push();
	add(result, make_token(AST_CLASS_KEY, key_at));
	add(result, bases);
	while (!is_simple(OP_RBRACE))
	{
		if ((is_simple(KW_PUBLIC) || is_simple(KW_PROTECTED) ||
			is_simple(KW_PRIVATE)) && is_simple(OP_COLON, pos_ + 1))
		{
			add(result, make_token(AST_ACCESS_SPECIFIER, pos_));
			pos_ += 2;
			continue;
		}
		const AstId member = at_end() ? 0 : parse_member_declaration(class_name);
		if (member == 0)
		{
			scopes_.Pop();
			restore(saved);
			return 0;
		}
		add(result, member);
	}
	if (!leave_bracket(OP_RBRACE))
	{
		scopes_.Pop();
		restore(saved);
		return 0;
	}
	scopes_.Pop();
	return result;
}

AstId Pa10Parser::parse_enum_specifier()
{
	const Mark saved = mark();
	if (!consume_simple(KW_ENUM))
		return 0;
	const size_t key_at = pos_;
	const bool scoped = is_simple(KW_CLASS) || is_simple(KW_STRUCT);
	if (scoped)
		++pos_;
	AstId result = 0;
	if (is_kind(PA6_IDENTIFIER_TOKEN))
	{
		const size_t name_start = pos_;
		if (parse_qualified_name() == 0)
		{
			restore(saved);
			return 0;
		}
		result = make_join(AST_ENUM_SPECIFIER, name_start, pos_);
	}
	else
		result = make(AST_ENUM_SPECIFIER);
	AstId underlying = 0;
	if (consume_simple(OP_COLON))
	{
		underlying = parse_type_id();
		if (underlying == 0)
		{
			restore(saved);
			return 0;
		}
	}
	scopes_.Bind(declared_identifier(result), BIND_TYPE);
	if (scoped)
		add(result, make_token(AST_ENUM_KEY, key_at));
	add(result, underlying);
	if (!is_simple(OP_LBRACE))
	{
		// No body: an opaque declaration or an elaborated specifier.  The
		// node keeps its name span; sema tells the two apart by context.
		arena_.At(result).kind = AST_ENUM_DECLARATION;
		return result;
	}
	enter_bracket(OP_LBRACE);
	while (!is_simple(OP_RBRACE))
	{
		if (!is_kind(PA6_IDENTIFIER_TOKEN))
		{
			restore(saved);
			return 0;
		}
		const AstId enumerator = make_span(AST_ENUMERATOR, pos_, pos_ + 1,
			token(pos_).spelling);
		++pos_;
		if (consume_simple(OP_ASS))
		{
			const AstId value = parse_assignment_expression();
			if (value == 0)
			{
				restore(saved);
				return 0;
			}
			add(enumerator, value);
		}
		add(result, enumerator);
		if (!consume_simple(OP_COMMA))
			break;
	}
	if (!leave_bracket(OP_RBRACE))
	{
		restore(saved);
		return 0;
	}
	// Unscoped enumerators are values in the enclosing scope.  Scoped
	// enumerators are deliberately not visible to the parser outside the
	// enum; semantic lookup supplies their actual enum scope later.
	if (!scoped)
	{
		const vector<AstId>& members = arena_.At(result).children;
		for (size_t i = 0; i < members.size(); ++i)
			if (arena_.At(members[i]).kind == AST_ENUMERATOR)
				scopes_.Bind(token(arena_.At(members[i]).first).spelling,
					BIND_VALUE);
	}
	return result;
}

AstId Pa10Parser::parse_mem_initializer()
{
	const Mark saved = mark();
	const size_t name_start = pos_;
	const AstId name = is_simple(KW_DECLTYPE) ?
		parse_decltype_specifier(false) : parse_qualified_name();
	if (name == 0)
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_MEM_INITIALIZER);
	add(result, make_join(AST_MEM_INITIALIZER_ID, name_start, pos_));
	AstId arguments = 0;
	if (is_simple(OP_LPAREN))
		arguments = parse_argument_list(AST_PAREN_ARGUMENT_LIST);
	else if (is_simple(OP_LBRACE))
		arguments = parse_braced_init_list();
	if (arguments == 0)
	{
		restore(saved);
		return 0;
	}
	add(result, arguments);
	return result;
}

AstId Pa10Parser::parse_ctor_initializer()
{
	const Mark saved = mark();
	if (!consume_simple(OP_COLON))
		return 0;
	const AstId result = make(AST_CTOR_INITIALIZER);
	AstId initializer = parse_mem_initializer();
	if (initializer == 0)
	{
		restore(saved);
		return 0;
	}
	add(result, initializer);
	while (consume_simple(OP_COMMA))
	{
		initializer = parse_mem_initializer();
		if (initializer == 0)
		{
			restore(saved);
			return 0;
		}
		add(result, initializer);
	}
	return result;
}

AstId Pa10Parser::parse_throw_specification()
{
	const Mark saved = mark();
	if (!consume_simple(KW_THROW) || !enter_bracket(OP_LPAREN))
	{
		restore(saved);
		return 0;
	}
	if (!is_simple(OP_RPAREN))
	{
		while (true)
		{
			const AstId type = parse_type_id();
			if (type == 0)
			{
				restore(saved);
				return 0;
			}
			(void)type;
			if (!consume_simple(OP_COMMA))
				break;
		}
	}
	if (!leave_bracket(OP_RPAREN))
	{
		restore(saved);
		return 0;
	}
	return make_join(AST_FUNCTION_QUALIFIER, saved.position, pos_);
}

AstId Pa10Parser::parse_noexcept_qualifier()
{
	const Mark saved = mark();
	const size_t start = pos_;
	if (!consume_simple(KW_NOEXCEPT))
		return 0;
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
	const AstId qualifier = make_join(AST_FUNCTION_QUALIFIER, start, pos_);
	add(qualifier, expression);
	return qualifier;
}

// The suffixes a function declarator may carry after its parameter clause:
// cv- and ref-qualifiers, exception specifications, virt-specifiers and a
// trailing return type.  Returns false only when a started suffix is
// malformed.
bool Pa10Parser::parse_function_suffixes(AstId declarator)
{
	while (true)
	{
		if (is_simple(KW_CONST) || is_simple(KW_VOLATILE))
		{
			add(declarator, make_token(AST_CV_QUALIFIER, pos_++));
			continue;
		}
		if (is_simple(OP_AMP) || is_simple(OP_LAND))
		{
			add(declarator, make_token(AST_REF_QUALIFIER, pos_++));
			continue;
		}
		if (is_kind(PA6_IDENTIFIER_TOKEN) &&
			(token(pos_).flags & (PA6_FINAL_FLAG | PA6_OVERRIDE_FLAG)) != 0)
		{
			add(declarator, make_token(AST_VIRT_SPECIFIER, pos_++));
			continue;
		}
		AstId suffix = 0;
		if (is_simple(KW_NOEXCEPT))
			suffix = parse_noexcept_qualifier();
		else if (is_simple(KW_THROW))
			suffix = parse_throw_specification();
		else if (is_simple(OP_ARROW))
			suffix = parse_trailing_return_type();
		else
			return true;
		if (suffix == 0)
			return false;
		add(declarator, suffix);
	}
}

AstId Pa10Parser::parse_member_specifier()
{
	const size_t at = pos_++;
	if (token(at).IsSimple(KW_EXPLICIT))
		return make_span(AST_SPECIFIER, at, at + 1, "explicit");
	return make_token(AST_SPECIFIER, at);
}

AstId Pa10Parser::parse_special_member_declaration(const string& class_name)
{
	const Mark saved = mark();
	vector<AstId> member_specifiers;
	(void)consume_attribute_specifiers();
	while (is_simple(KW_INLINE) || is_simple(KW_EXPLICIT) ||
		is_simple(KW_VIRTUAL))
		member_specifiers.push_back(parse_member_specifier());
	(void)consume_attribute_specifiers();

	AstId identifier = 0;
	if (!class_name.empty() && is_kind(PA6_IDENTIFIER_TOKEN) &&
		token(pos_).spelling == class_name && is_simple(OP_LPAREN, pos_ + 1))
	{
		identifier = make_join(AST_IDENTIFIER, pos_, pos_ + 1);
		++pos_;
	}
	else if (!class_name.empty() && is_simple(OP_COMPL) &&
		is_kind(PA6_IDENTIFIER_TOKEN, pos_ + 1) &&
		token(pos_ + 1).spelling == class_name &&
		is_simple(OP_LPAREN, pos_ + 2))
	{
		identifier = make_join(AST_IDENTIFIER, pos_, pos_ + 2);
		pos_ += 2;
	}
	else if (is_simple(KW_OPERATOR))
		identifier = parse_operator_function_id();
	if (identifier == 0)
	{
		restore(saved);
		return 0;
	}

	const AstId declarator = make(AST_DECLARATOR);
	add(declarator, identifier);
	const AstId parameters = parse_parameter_clause();
	if (parameters == 0 || !(add(declarator, parameters),
		parse_function_suffixes(declarator)))
	{
		restore(saved);
		return 0;
	}

	// Parameters are visible in the mem-initializers and the body.
	scopes_.Push();
	bind_parameters(parameters);
	AstId ctor_initializer = 0;
	if (is_simple(OP_COLON))
		ctor_initializer = parse_ctor_initializer();
	AstId body = 0;
	if ((ctor_initializer != 0 || !is_simple(OP_COLON)) && is_simple(OP_LBRACE))
		body = parse_compound_statement();
	scopes_.Pop();
	if ((is_simple(OP_COLON) && ctor_initializer == 0) ||
		(is_simple(OP_LBRACE) && body == 0))
	{
		restore(saved);
		return 0;
	}
	AstId initializer = 0;
	if (body == 0 && ctor_initializer == 0 && consume_simple(OP_ASS))
	{
		if (!is_simple(KW_DEFAULT) && !is_simple(KW_DELETE))
		{
			restore(saved);
			return 0;
		}
		initializer = make(AST_INITIALIZER);
		add(initializer, make_span(AST_SPECIAL_INITIALIZER, pos_, pos_ + 1,
			token(pos_).spelling));
		++pos_;
	}
	if (body == 0 && !consume_simple(OP_SEMICOLON))
	{
		restore(saved);
		return 0;
	}
	const AstNode name = arena_.At(identifier);
	const AstId result = make_span(body == 0 ? AST_SPECIAL_MEMBER_DECLARATION :
		AST_SPECIAL_MEMBER_DEFINITION, name.first, name.last, name.text);
	if (!member_specifiers.empty())
	{
		const AstId specifiers = make(AST_MEMBER_SPECIFIERS);
		for (size_t i = 0; i < member_specifiers.size(); ++i)
			add(specifiers, member_specifiers[i]);
		add(result, specifiers);
	}
	add(result, declarator);
	add(result, ctor_initializer);
	add(result, initializer);
	add(result, body);
	return result;
}

AstId Pa10Parser::parse_member_declaration(const string& class_name)
{
	const Mark saved = mark();
	const AstId special = parse_special_member_declaration(class_name);
	if (special != 0)
		return special;
	restore(saved);
	const AstId declaration = parse_declaration(true);
	if (declaration == 0)
		restore(saved);
	return declaration;
}

// A constructor, destructor or conversion function defined without a
// decl-specifier-seq, either in its class or qualified at namespace scope
// (Box<T>::Box, C<T>::~C, C::operator int).
AstId Pa10Parser::parse_special_member_definition()
{
	const Mark saved = mark();
	vector<AstId> member_specifiers;
	(void)consume_attribute_specifiers();
	while (is_simple(KW_INLINE) || is_simple(KW_EXPLICIT))
	{
		member_specifiers.push_back(parse_member_specifier());
		(void)consume_attribute_specifiers();
	}
	const size_t name_start = pos_;
	if (!is_kind(PA6_IDENTIFIER_TOKEN))
	{
		restore(saved);
		return 0;
	}
	const string first_name = token(pos_).spelling;
	size_t cursor = pos_ + 1;
	if (is_simple(OP_LT, cursor))
	{
		const Mark lookahead = mark();
		if (parse_simple_template_id() == 0)
		{
			restore(saved);
			return 0;
		}
		cursor = pos_;
		restore(lookahead);
	}
	bool qualified = false;
	bool conversion = false;
	bool destructor = false;
	string final_name;
	while (is_simple(OP_COLON2, cursor))
	{
		qualified = true;
		++cursor;
		if (token(cursor).IsSimple(KW_OPERATOR))
		{
			// Only a conversion-function-id may omit the return type: after
			// `operator` a type must follow, never an operator token.
			const Pa6Token& next = token(cursor + 1);
			conversion = next.IsIdentifier() || next.IsSimple(OP_COLON2) ||
				next.IsSimple(KW_DECLTYPE) || next.IsSimple(KW_TYPENAME) ||
				(next.kind == PA6_SIMPLE_TOKEN &&
				 (is_builtin_type(next.simple_type) ||
				  is_cv_qualifier(next.simple_type)));
			if (!conversion)
			{
				restore(saved);
				return 0;
			}
			++cursor;
			while (!token(cursor).IsSimple(OP_LPAREN))
			{
				if (token(cursor).kind == PA6_EOF_TOKEN ||
					token(cursor).IsSimple(OP_SEMICOLON) ||
					token(cursor).IsSimple(OP_LBRACE))
				{
					restore(saved);
					return 0;
				}
				++cursor;
			}
			break;
		}
		if (token(cursor).IsSimple(OP_COMPL))
		{
			destructor = true;
			++cursor;
			if (!token(cursor).IsIdentifier())
			{
				restore(saved);
				return 0;
			}
			final_name = token(cursor++).spelling;
			break;
		}
		if (!token(cursor).IsIdentifier())
		{
			restore(saved);
			return 0;
		}
		final_name = token(cursor++).spelling;
	}
	if (!token(cursor).IsSimple(OP_LPAREN) ||
		(qualified && !conversion && !destructor && final_name != first_name))
	{
		restore(saved);
		return 0;
	}
	pos_ = cursor;
	const AstId declarator = make(AST_DECLARATOR);
	add(declarator, make_join(AST_IDENTIFIER, name_start, pos_));
	const AstId parameters = parse_parameter_clause();
	if (parameters == 0 || !(add(declarator, parameters),
		parse_function_suffixes(declarator)))
	{
		restore(saved);
		return 0;
	}
	scopes_.Push();
	bind_parameters(parameters);
	AstId ctor_initializer = 0;
	if (is_simple(OP_COLON))
		ctor_initializer = parse_ctor_initializer();
	const AstId body = (is_simple(OP_COLON) && ctor_initializer == 0) ?
		0 : parse_compound_statement();
	scopes_.Pop();
	if (body == 0)
	{
		restore(saved);
		return 0;
	}
	const AstId result = make_join(AST_SPECIAL_MEMBER_DEFINITION, name_start,
		cursor);
	if (!member_specifiers.empty())
	{
		const AstId specifiers = make(AST_MEMBER_SPECIFIERS);
		for (size_t i = 0; i < member_specifiers.size(); ++i)
			add(specifiers, member_specifiers[i]);
		add(result, specifiers);
	}
	add(result, declarator);
	add(result, ctor_initializer);
	add(result, body);
	return result;
}

AstId Pa10Parser::parse_decl_specifier_seq()
{
	const Mark saved = mark();
	const AstId result = make(AST_DECL_SPECIFIER_SEQ);
	bool have = false;
	bool committed_type = false;
	while (true)
	{
		const Pa6Token& current = token(pos_);
		const size_t start = pos_;
		if (current.kind == PA6_SIMPLE_TOKEN &&
			(is_storage_or_function_specifier(current.simple_type) ||
			 is_cv_qualifier(current.simple_type) ||
			 is_builtin_type(current.simple_type)))
		{
			add(result, make_token(AST_DECL_SPECIFIER, pos_++));
			have = true;
			committed_type = committed_type ||
				is_builtin_type(current.simple_type);
			continue;
		}
		if (current.IsSimple(OP_COLON2))
		{
			if (parse_qualified_name() == 0)
			{
				restore(saved);
				return 0;
			}
			add(result, make_join(AST_DECL_SPECIFIER, start, pos_));
			have = true;
			committed_type = true;
			continue;
		}
		if (current.IsSimple(KW_CLASS) || current.IsSimple(KW_STRUCT) ||
			current.IsSimple(KW_UNION) || current.IsSimple(KW_ENUM) ||
			current.IsSimple(KW_DECLTYPE))
		{
			const AstId specifier = current.IsSimple(KW_ENUM) ?
				parse_enum_specifier() : current.IsSimple(KW_DECLTYPE) ?
				parse_decltype_specifier(false) : parse_class_specifier();
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
			++pos_;
			if (parse_qualified_name() == 0)
			{
				restore(saved);
				return 0;
			}
			// The typename keyword is not printed but stays in the span.
			add(result, make_span(AST_DECL_SPECIFIER, start, pos_,
				Join(start + 1, pos_)));
			have = true;
			committed_type = true;
			continue;
		}
		if (current.kind == PA6_IDENTIFIER_TOKEN)
		{
			// 7.1.6.2p2: after a type-specifier the next name belongs to the
			// declarator, and a known value or namespace is not a type.
			if (committed_type)
				break;
			const BindKind* binding = scopes_.Lookup(current.spelling);
			if (binding != 0 && !IsTypeLikeBinding(binding) &&
				!is_simple(OP_COLON2, pos_ + 1))
				break;
			if (parse_qualified_name() == 0)
			{
				restore(saved);
				return 0;
			}
			add(result, pos_ == start + 1 ?
				make_token(AST_DECL_SPECIFIER, start) :
				make_join(AST_DECL_SPECIFIER, start, pos_));
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

AstId Pa10Parser::parse_type_specifier_seq()
{
	const Mark saved = mark();
	const AstId result = make(AST_TYPE_SPECIFIER_SEQ);
	bool have = false;
	bool committed = false;
	while (true)
	{
		const Pa6Token& current = token(pos_);
		const size_t start = pos_;
		if (current.kind == PA6_SIMPLE_TOKEN &&
			(is_cv_qualifier(current.simple_type) ||
			 is_builtin_type(current.simple_type)))
		{
			const bool type = is_builtin_type(current.simple_type);
			add(result, make_token(type ? AST_TYPE_SPECIFIER : AST_CV_QUALIFIER,
				pos_++));
			have = true;
			committed = committed || type;
			continue;
		}
		if (current.IsSimple(OP_COLON2))
		{
			if (parse_qualified_name() == 0)
			{
				restore(saved);
				return 0;
			}
			add(result, make_join(AST_TYPE_NAME, start, pos_));
			have = true;
			committed = true;
			continue;
		}
		if (current.IsSimple(KW_CLASS) || current.IsSimple(KW_STRUCT) ||
			current.IsSimple(KW_UNION) || current.IsSimple(KW_ENUM) ||
			current.IsSimple(KW_DECLTYPE))
		{
			const AstId specifier = current.IsSimple(KW_ENUM) ?
				parse_enum_specifier() : current.IsSimple(KW_DECLTYPE) ?
				parse_decltype_specifier(true) : parse_class_specifier();
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
			++pos_;
			if (parse_qualified_name() == 0)
			{
				restore(saved);
				return 0;
			}
			add(result, make_span(AST_TYPE_NAME, start, pos_,
				Join(start + 1, pos_)));
			have = true;
			committed = true;
			continue;
		}
		if (current.kind == PA6_IDENTIFIER_TOKEN)
		{
			if (committed)
				break;
			const BindKind* binding = scopes_.Lookup(current.spelling);
			if (binding != 0 && !IsTypeLikeBinding(binding) &&
				!is_simple(OP_COLON2, pos_ + 1))
				break;
			if (parse_qualified_name() == 0)
			{
				restore(saved);
				return 0;
			}
			add(result, make_join(AST_TYPE_NAME, start, pos_));
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
	const AstId result = make_join(type_context ? AST_DECLTYPE_SPECIFIER :
		AST_DECL_SPECIFIER, start, pos_);
	add(result, expression);
	return result;
}

AstId Pa10Parser::parse_type_id(bool allow_function_abstract)
{
	AstId specifiers = parse_type_specifier_seq();
	if (specifiers == 0)
		return 0;
	AstId declarator = 0;
	if (is_simple(OP_STAR) || is_simple(OP_AMP) || is_simple(OP_LAND) ||
		(allow_function_abstract && is_simple(OP_LPAREN)))
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
	if (is_simple(OP_STAR) || is_simple(OP_AMP) || is_simple(OP_LAND))
		return make_token(AST_PTR_OPERATOR, pos_++);

	// A qualified prefix followed by '*' is a pointer-to-member operator.
	if (is_simple(OP_COLON2) || is_kind(PA6_IDENTIFIER_TOKEN))
	{
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
			return make_join(AST_PTR_OPERATOR, start, pos_);
	}
	restore(saved);
	return 0;
}

AstId Pa10Parser::parse_declarator_id()
{
	if (is_simple(KW_OPERATOR))
		return parse_operator_function_id();
	if (!is_kind(PA6_IDENTIFIER_TOKEN) && !is_simple(OP_COLON2))
		return 0;
	const size_t start = pos_;
	if (parse_qualified_name() == 0)
		return 0;
	return make_join(AST_IDENTIFIER, start, pos_);
}

AstId Pa10Parser::parse_nested_declarator(bool allow_abstract)
{
	const Mark saved = mark();
	if (!enter_bracket(OP_LPAREN))
		return 0;
	const AstId nested = parse_declarator(allow_abstract);
	if (nested == 0 || !leave_bracket(OP_RPAREN))
	{
		restore(saved);
		return 0;
	}
	const AstId wrapper = make(AST_NESTED_DECLARATOR);
	add(wrapper, nested);
	return wrapper;
}

AstId Pa10Parser::parse_array_suffix()
{
	const Mark saved = mark();
	if (!consume_simple(OP_LSQUARE))
		return 0;
	AstId extent = 0;
	if (!is_simple(OP_RSQUARE))
		extent = parse_expression();
	if ((extent == 0 && !is_simple(OP_RSQUARE)) || !consume_simple(OP_RSQUARE))
	{
		restore(saved);
		return 0;
	}
	const AstId suffix = make(AST_ARRAY_SUFFIX);
	add(suffix, extent);
	return suffix;
}

AstId Pa10Parser::parse_declarator(bool allow_abstract)
{
	const Mark saved = mark();
	vector<AstId> pointers;
	while (true)
	{
		const AstId pointer = parse_ptr_operator();
		if (pointer == 0)
			break;
		pointers.push_back(pointer);
		while (is_simple(KW_CONST) || is_simple(KW_VOLATILE))
			pointers.push_back(make_token(AST_CV_QUALIFIER, pos_++));
	}

	AstId direct = 0;
	AstId pack = 0;
	AstId leading_parameters = 0;
	if (is_kind(PA6_IDENTIFIER_TOKEN) || is_simple(OP_COLON2) ||
		is_simple(KW_OPERATOR))
		direct = parse_declarator_id();
	else if (is_simple(OP_DOTS))
	{
		pack = parse_parameter_pack();
		if (is_kind(PA6_IDENTIFIER_TOKEN) || is_simple(OP_COLON2) ||
			is_simple(KW_OPERATOR))
			direct = parse_declarator_id();
		else if (!allow_abstract && pointers.empty())
		{
			restore(saved);
			return 0;
		}
	}
	else if (is_simple(OP_LPAREN))
	{
		// In an abstract declarator a parenthesis opens a parameter clause
		// unless it only nests another declarator.
		if (allow_abstract)
			leading_parameters = parse_parameter_clause();
		if (leading_parameters == 0)
		{
			direct = parse_nested_declarator(allow_abstract);
			if (direct == 0)
			{
				restore(saved);
				return 0;
			}
		}
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
	add(result, pack);
	add(result, direct);
	add(result, leading_parameters);

	while (true)
	{
		if (is_simple(OP_LPAREN))
		{
			const AstId parameters = parse_parameter_clause();
			if (parameters != 0)
			{
				add(result, parameters);
				continue;
			}
		}
		if (is_simple(OP_LSQUARE))
		{
			const AstId suffix = parse_array_suffix();
			if (suffix == 0)
				break;
			add(result, suffix);
			continue;
		}
		const size_t before = pos_;
		if (!parse_function_suffixes(result))
		{
			restore(saved);
			return 0;
		}
		if (pos_ == before)
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
		if (is_simple(OP_DOTS))
		{
			add(result, parse_parameter_pack());
			break;
		}
		const AstId parameter = parse_parameter_declaration();
		if (parameter == 0)
		{
			restore(saved);
			return 0;
		}
		add(result, parameter);
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

AstId Pa10Parser::parse_parameter_declaration()
{
	const Mark saved = mark();
	const AstId specifiers = parse_decl_specifier_seq();
	if (specifiers == 0)
		return 0;
	const AstId leading_pack = parse_parameter_pack();
	AstId declarator = 0;
	if (!is_simple(OP_COMMA) && !is_simple(OP_RPAREN) &&
		!is_simple(OP_ASS) && !is_simple(OP_DOTS))
	{
		declarator = parse_declarator(true);
		if (declarator == 0)
		{
			restore(saved);
			return 0;
		}
	}
	if (leading_pack != 0)
	{
		if (declarator == 0)
			declarator = make(AST_DECLARATOR);
		vector<AstId>& children = arena_.At(declarator).children;
		children.insert(children.begin(), leading_pack);
	}
	const AstId result = make(AST_PARAMETER_DECLARATION);
	add(result, specifiers);
	add(result, declarator);
	if (leading_pack == 0 && is_simple(OP_DOTS))
	{
		const AstId pack = parse_parameter_pack();
		if (declarator == 0)
		{
			declarator = make(AST_DECLARATOR);
			add(result, declarator);
		}
		add(declarator, pack);
		if (is_kind(PA6_IDENTIFIER_TOKEN) || is_simple(OP_COLON2) ||
			is_simple(KW_OPERATOR))
		{
			const AstId name = parse_declarator_id();
			if (name == 0)
			{
				restore(saved);
				return 0;
			}
			add(declarator, name);
		}
	}
	(void)consume_attribute_specifiers();
	if (consume_simple(OP_ASS))
	{
		const AstId expression = is_simple(OP_LBRACE) ?
			parse_braced_init_list() : parse_assignment_expression();
		if (expression == 0)
		{
			restore(saved);
			return 0;
		}
		const AstId initializer = make(AST_INITIALIZER);
		add(initializer, expression);
		const AstId default_argument = make(AST_DEFAULT_ARGUMENT);
		add(default_argument, initializer);
		add(result, default_argument);
	}
	return result;
}

AstId Pa10Parser::parse_init_declarator()
{
	const AstId declarator = parse_declarator();
	return declarator == 0 ? 0 : finish_init_declarator(declarator);
}

AstId Pa10Parser::finish_init_declarator(AstId declarator)
{
	AstId initializer = 0;
	if (is_simple(OP_ASS) || is_simple(OP_LBRACE) || is_simple(OP_LPAREN))
	{
		initializer = parse_initializer();
		if (initializer == 0)
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
	AstId value = 0;
	if (consume_simple(OP_ASS))
	{
		if (is_simple(KW_DEFAULT) || is_simple(KW_DELETE))
		{
			value = make_span(AST_SPECIAL_INITIALIZER, pos_, pos_ + 1,
				token(pos_).spelling);
			++pos_;
		}
		else
			value = is_simple(OP_LBRACE) ? parse_braced_init_list() :
				parse_assignment_expression();
	}
	else if (is_simple(OP_LBRACE))
		value = parse_braced_init_list();
	else if (is_simple(OP_LPAREN))
		value = parse_paren_initializer();
	if (value == 0)
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_INITIALIZER);
	add(result, value);
	return result;
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
		AstId expression = is_simple(OP_LBRACE) ?
			parse_braced_init_list() : parse_assignment_expression();
		if (expression == 0)
		{
			restore(saved);
			return 0;
		}
		add(result, parse_pack_expansion(expression));
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
		add(result, parse_pack_expansion(expression));
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
	const AstId result = make_join(AST_TRAILING_RETURN_TYPE, start, pos_);
	add(result, type);
	return result;
}

// An optionally qualified name whose components may be simple-template-ids:
// N::f, ::std::vector<int>, T::template get<U>.
AstId Pa10Parser::parse_qualified_name()
{
	const Mark saved = mark();
	const size_t start = pos_;
	if (consume_simple(OP_COLON2) && !is_kind(PA6_IDENTIFIER_TOKEN))
	{
		restore(saved);
		return 0;
	}
	if (!is_kind(PA6_IDENTIFIER_TOKEN))
		return 0;
	if (parse_simple_template_id() == 0)
		++pos_;
	while (consume_simple(OP_COLON2))
	{
		(void)consume_simple(KW_TEMPLATE);
		// An operator-function-id is a valid final component of a
		// qualified declarator-id (`Box::operator!`).  It is tokenized with
		// `operator` as a keyword, so it cannot go through the ordinary
		// identifier branch below.
		if (is_simple(KW_OPERATOR))
		{
			if (parse_operator_function_id() == 0)
			{
				restore(saved);
				return 0;
			}
			break;
		}
		if (!is_kind(PA6_IDENTIFIER_TOKEN))
		{
			restore(saved);
			return 0;
		}
		if (parse_simple_template_id(true) == 0)
			++pos_;
	}
	return make_join(AST_ID_EXPRESSION, start, pos_);
}

// identifier < template-argument-list? >.  The result is memoized because
// every `id <` in an expression with an unknown left operand is first tried
// as a template-id and then re-read as a comparison.
AstId Pa10Parser::parse_simple_template_id(bool qualified)
{
	if (!is_kind(PA6_IDENTIFIER_TOKEN) || !is_simple(OP_LT, pos_ + 1))
		return 0;
	return qualified ?
		try_memoized(MEMO_QUALIFIED_TEMPLATE_ID,
			&Pa10Parser::parse_qualified_template_id_rule) :
		try_memoized(MEMO_SIMPLE_TEMPLATE_ID,
			&Pa10Parser::parse_unqualified_template_id_rule);
}

AstId Pa10Parser::parse_unqualified_template_id_rule()
{
	return parse_template_id_rule(false);
}

AstId Pa10Parser::parse_qualified_template_id_rule()
{
	return parse_template_id_rule(true);
}

AstId Pa10Parser::parse_template_id_rule(bool qualified)
{
	const Mark saved = mark();
	const size_t start = pos_;
	// A name bound as a value is never a template-name, so `__count < _Dt`
	// stays a comparison.  A qualified component is resolved elsewhere.
	const BindKind* binding = scopes_.Lookup(token(pos_).spelling);
	if (!qualified && binding != 0 && *binding == BIND_VALUE)
		return 0;
	pos_ += 2;
	brackets_.push_back(BRACKET_ANGLE);
	if (!is_simple(OP_GT) && !is_kind(PA6_RSHIFT_1_TOKEN) &&
		!is_kind(PA6_RSHIFT_2_TOKEN) && parse_template_argument_list() == 0)
	{
		restore(saved);
		return 0;
	}
	if (!parse_close_angle_bracket())
	{
		restore(saved);
		return 0;
	}
	return make_join(AST_ID_EXPRESSION, start, pos_);
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
	if (is_simple(KW_TYPENAME))
	{
		++pos_;
		if (parse_qualified_name() == 0)
		{
			restore(saved);
			return 0;
		}
		return make_join(AST_TEMPLATE_ARGUMENT, start, pos_);
	}
	if (is_simple(KW_CLASS))
	{
		++pos_;
		if (is_kind(PA6_IDENTIFIER_TOKEN))
			++pos_;
		return make_join(AST_TEMPLATE_ARGUMENT, start, pos_);
	}
	AstId value = parse_assignment_expression();
	if (value == 0)
	{
		restore(saved);
		value = parse_type_id();
	}
	if (value == 0)
		return 0;
	const AstId result = make_join(AST_TEMPLATE_ARGUMENT, start, pos_);
	add(result, value);
	return result;
}

bool Pa10Parser::is_overloadable_operator(ETokenType type) const
{
	switch (type)
	{
	case OP_PLUS: case OP_MINUS: case OP_STAR: case OP_DIV: case OP_MOD:
	case OP_XOR: case OP_AMP: case OP_BOR: case OP_COMPL: case OP_LNOT:
	case OP_ASS: case OP_LT: case OP_GT: case OP_PLUSASS: case OP_MINUSASS:
	case OP_STARASS: case OP_DIVASS: case OP_MODASS: case OP_XORASS:
	case OP_BANDASS: case OP_BORASS: case OP_LSHIFT: case OP_RSHIFTASS:
	case OP_LSHIFTASS: case OP_EQ: case OP_NE: case OP_LE: case OP_GE:
	case OP_LAND: case OP_LOR: case OP_INC: case OP_DEC: case OP_COMMA:
	case OP_ARROWSTAR: case OP_ARROW:
		return true;
	default:
		return false;
	}
}

// operator-function-id, conversion-function-id or literal-operator-id.  The
// node text glues "operator" to the spellings that follow.
AstId Pa10Parser::parse_operator_function_id()
{
	const Mark saved = mark();
	const size_t start = pos_;
	if (!consume_simple(KW_OPERATOR))
		return 0;
	const Pa6Token& current = token(pos_);
	if (current.IsSimple(KW_NEW) || current.IsSimple(KW_DELETE))
	{
		++pos_;
		if (is_simple(OP_LSQUARE) &&
			!(++pos_, consume_simple(OP_RSQUARE)))
		{
			restore(saved);
			return 0;
		}
		return make_operator_id(start);
	}
	if (current.IsLiteral() ||
		(current.kind == PA6_SIMPLE_TOKEN && is_builtin_type(current.simple_type)))
	{
		++pos_;
		return make_operator_id(start);
	}
	if (current.IsSimple(OP_COLON2))
	{
		if (parse_qualified_name() == 0)
		{
			restore(saved);
			return 0;
		}
		while (is_simple(OP_STAR) || is_simple(OP_AMP) || is_simple(OP_LAND))
			++pos_;
		return make_operator_id(start);
	}
	if (current.IsIdentifier())
	{
		if (parse_simple_template_id() == 0)
			++pos_;
		return make_operator_id(start);
	}
	if (current.IsSimple(OP_LSQUARE) || current.IsSimple(OP_LPAREN))
	{
		++pos_;
		if (!consume_simple(current.IsSimple(OP_LSQUARE) ? OP_RSQUARE : OP_RPAREN))
		{
			restore(saved);
			return 0;
		}
		return make_operator_id(start);
	}
	if (current.IsRshiftPart() && token(pos_ + 1).IsRshiftPart())
	{
		pos_ += 2;
		return make_operator_id(start);
	}
	if (current.kind == PA6_SIMPLE_TOKEN &&
		is_overloadable_operator(current.simple_type))
	{
		++pos_;
		return make_operator_id(start);
	}
	restore(saved);
	return 0;
}

AstId Pa10Parser::parse_alias_declaration()
{
	const Mark saved = mark();
	if (!consume_simple(KW_USING) || !is_kind(PA6_IDENTIFIER_TOKEN))
	{
		restore(saved);
		return 0;
	}
	const size_t name_at = pos_++;
	if (!consume_simple(OP_ASS))
	{
		restore(saved);
		return 0;
	}
	const AstId type = parse_type_id();
	if (type == 0 || !consume_simple(OP_SEMICOLON))
	{
		restore(saved);
		return 0;
	}
	const AstId result = make_span(AST_ALIAS_DECLARATION, name_at, name_at + 1,
		token(name_at).spelling);
	add(result, type);
	scopes_.Bind(token(name_at).spelling, BIND_TYPE);
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
	const size_t message_at = pos_++;
	if (!leave_bracket(OP_RPAREN) || !consume_simple(OP_SEMICOLON))
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_STATIC_ASSERT_DECLARATION);
	add(result, expression);
	add(result, make_span(AST_MESSAGE, message_at, message_at + 1,
		token(message_at).spelling));
	return result;
}
