#include "recog_parser.h"

#include <cstdint>
#include <limits>

using namespace std;

namespace
{

const size_t NoPosition = numeric_limits<size_t>::max();

bool IsBuiltinType(ETokenType type)
{
	switch (type)
	{
	case KW_CHAR:
	case KW_CHAR16_T:
	case KW_CHAR32_T:
	case KW_WCHAR_T:
	case KW_BOOL:
	case KW_SHORT:
	case KW_INT:
	case KW_LONG:
	case KW_SIGNED:
	case KW_UNSIGNED:
	case KW_FLOAT:
	case KW_DOUBLE:
	case KW_VOID:
	case KW_AUTO:
		return true;
	default:
		return false;
	}
}

bool IsCVQualifier(ETokenType type)
{
	return type == KW_CONST || type == KW_VOLATILE;
}

bool IsAssignmentOperator(ETokenType type)
{
	switch (type)
	{
	case OP_ASS:
	case OP_STARASS:
	case OP_DIVASS:
	case OP_MODASS:
	case OP_PLUSASS:
	case OP_MINUSASS:
	case OP_RSHIFTASS:
	case OP_LSHIFTASS:
	case OP_BANDASS:
	case OP_XORASS:
	case OP_BORASS:
		return true;
	default:
		return false;
	}
}

bool IsBracketToken(ETokenType type)
{
	return type == OP_LPAREN || type == OP_RPAREN ||
		type == OP_LSQUARE || type == OP_RSQUARE ||
		type == OP_LBRACE || type == OP_RBRACE;
}

bool IsOperatorFunctionToken(ETokenType type)
{
	switch (type)
	{
	case OP_PLUS:
	case OP_MINUS:
	case OP_STAR:
	case OP_DIV:
	case OP_MOD:
	case OP_XOR:
	case OP_AMP:
	case OP_BOR:
	case OP_COMPL:
	case OP_LNOT:
	case OP_ASS:
	case OP_LT:
	case OP_GT:
	case OP_PLUSASS:
	case OP_MINUSASS:
	case OP_STARASS:
	case OP_DIVASS:
	case OP_MODASS:
	case OP_XORASS:
	case OP_BANDASS:
	case OP_BORASS:
	case OP_LSHIFT:
	case OP_RSHIFTASS:
	case OP_LSHIFTASS:
	case OP_EQ:
	case OP_NE:
	case OP_LE:
	case OP_GE:
	case OP_LAND:
	case OP_LOR:
	case OP_INC:
	case OP_DEC:
	case OP_COMMA:
	case OP_ARROWSTAR:
	case OP_ARROW:
		return true;
	default:
		return false;
	}
}

// 7.1.6.2p2: these may appear in a decl-specifier-seq without committing it
// to a type; only a type-specifier other than a cv-qualifier commits.
bool IsNonCommittingSpecifier(ETokenType type)
{
	switch (type)
	{
	case KW_CONST:
	case KW_VOLATILE:
	case KW_REGISTER:
	case KW_STATIC:
	case KW_THREAD_LOCAL:
	case KW_EXTERN:
	case KW_MUTABLE:
	case KW_INLINE:
	case KW_VIRTUAL:
	case KW_EXPLICIT:
	case KW_FRIEND:
	case KW_TYPEDEF:
	case KW_CONSTEXPR:
		return true;
	default:
		return false;
	}
}

} // namespace

Pa6Parser::Pa6Parser(const vector<Pa6Token>& tokens)
	: tokens_(tokens), pos_(0), angle_refusal_(false), hard_failure_(false)
{
}

const Pa6Token& Pa6Parser::token(size_t at) const
{
	static const Pa6Token end_token(PA6_EOF_TOKEN, "");
	return at < tokens_.size() ? tokens_[at] : end_token;
}

bool Pa6Parser::at_end() const
{
	return token(pos_).kind == PA6_EOF_TOKEN;
}

bool Pa6Parser::is_simple(ETokenType type, size_t at) const
{
	return token(at == NoPosition ? pos_ : at).IsSimple(type);
}

bool Pa6Parser::is_kind(Pa6TokenKind kind, size_t at) const
{
	return token(at == NoPosition ? pos_ : at).kind == kind;
}

bool Pa6Parser::consume_simple(ETokenType type)
{
	if (!is_simple(type))
		return false;
	++pos_;
	return true;
}

bool Pa6Parser::consume_kind(Pa6TokenKind kind)
{
	if (!is_kind(kind))
		return false;
	++pos_;
	return true;
}

bool Pa6Parser::consume_identifier()
{
	return consume_kind(PA6_IDENTIFIER_TOKEN);
}

bool Pa6Parser::consume_literal()
{
	return consume_kind(PA6_LITERAL_TOKEN);
}

bool Pa6Parser::enter_bracket(ETokenType type)
{
	if (!consume_simple(type))
		return false;
	switch (type)
	{
	case OP_LPAREN: brackets_.push_back(BRACKET_PAREN); break;
	case OP_LSQUARE: brackets_.push_back(BRACKET_SQUARE); break;
	case OP_LBRACE: brackets_.push_back(BRACKET_BRACE); break;
	default: return false;
	}
	return true;
}

bool Pa6Parser::leave_bracket(ETokenType type)
{
	if (!is_simple(type) || brackets_.empty())
		return false;
	const BracketKind expected = type == OP_RPAREN ? BRACKET_PAREN :
		type == OP_RSQUARE ? BRACKET_SQUARE : BRACKET_BRACE;
	if (brackets_.back() != expected)
		return false;
	++pos_;
	brackets_.pop_back();
	return true;
}

bool Pa6Parser::has_angle_boundary() const
{
	for (size_t i = brackets_.size(); i != 0; --i)
	{
		if (brackets_[i - 1] == BRACKET_ANGLE)
		{
			for (size_t j = i; j < brackets_.size(); ++j)
				if (brackets_[j] != BRACKET_ANGLE)
					return false;
			return true;
		}
	}
	return false;
}

void Pa6Parser::restore(size_t position, size_t brackets)
{
	pos_ = position;
	brackets_.resize(brackets);
}

uint64_t Pa6Parser::memo_key(unsigned rule_id) const
{
	return (static_cast<uint64_t>(rule_id) << 56) ^
		(static_cast<uint64_t>(pos_) << 1) ^
		(angle_refusal_ ? 1u : 0u);
}

bool Pa6Parser::try_memoized(unsigned rule_id,
	bool (Pa6Parser::*implementation)())
{
	if (hard_failure_)
		return false;
	const size_t start = pos_;
	const size_t context = brackets_.size();
	const uint64_t key = memo_key(rule_id);
	unordered_map<uint64_t, MemoEntry>::const_iterator found =
		memo_.find(key);
	if (found != memo_.end())
	{
		if (found->second.ok)
			pos_ = found->second.end;
		else
			restore(start, context);
		return found->second.ok;
	}

	const bool ok = (this->*implementation)();
	if (!ok)
		restore(start, context);
	if (!hard_failure_)
	{
		MemoEntry entry;
		entry.ok = ok;
		entry.end = ok ? pos_ : start;
		memo_.insert(make_pair(key, entry));
	}
	return ok;
}

bool Pa6Parser::is_name_category(unsigned category) const
{
	// Name-category bits are set only on identifier tokens.
	return (token(pos_).flags & category) != 0;
}

bool Pa6Parser::is_simple_type_token() const
{
	return token(pos_).kind == PA6_SIMPLE_TOKEN &&
		(IsBuiltinType(token(pos_).simple_type) ||
		 IsCVQualifier(token(pos_).simple_type));
}

bool Pa6Parser::is_assignment_operator_token() const
{
	return token(pos_).kind == PA6_SIMPLE_TOKEN &&
		IsAssignmentOperator(token(pos_).simple_type);
}

bool Pa6Parser::ParseTranslationUnit()
{
	pos_ = 0;
	brackets_.clear();
	memo_.clear();
	angle_refusal_ = false;
	hard_failure_ = false;
	if (!parse_translation_unit() || !brackets_.empty())
		return false;
	return at_end();
}

bool Pa6Parser::parse_translation_unit()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	while (!at_end())
	{
		if (!parse_declaration())
		{
			restore(start, context);
			return false;
		}
	}
	return true;
}

bool Pa6Parser::parse_declaration()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_function_definition())
		return true;
	if (hard_failure_)
		return false;
	restore(start, context);
	if (parse_block_declaration() || parse_empty_declaration() ||
		parse_attribute_declaration())
		return true;
	if (hard_failure_)
		return false;
	restore(start, context);
	if (parse_template_declaration() ||
		parse_explicit_instantiation() || parse_explicit_specialization() ||
		parse_linkage_specification() || parse_namespace_definition())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_function_definition()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	while (parse_attribute_specifier())
	{
	}
	if (!parse_decl_specifier_seq())
	{
		restore(start, context);
		return false;
	}
	if (!parse_declarator())
	{
		restore(start, context);
		return false;
	}
	while (token(pos_).flags & (PA6_FINAL_FLAG | PA6_OVERRIDE_FLAG))
		++pos_;
	if (!parse_function_body())
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_function_body()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_ctor_initializer())
	{
		if (parse_try_block() || parse_compound_statement())
			return true;
		restore(start, context);
	}
	if (parse_try_block())
		return true;
	restore(start, context);
	if (parse_compound_statement())
		return true;
	restore(start, context);
	if (consume_simple(OP_ASS))
	{
		if (consume_simple(KW_DEFAULT) || consume_simple(KW_DELETE))
			if (consume_simple(OP_SEMICOLON))
				return true;
	}
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_simple_declaration()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	while (parse_attribute_specifier())
	{
	}
	if (!parse_decl_specifier_seq())
	{
		restore(start, context);
		return false;
	}
	if (!is_simple(OP_SEMICOLON) && !parse_init_declarator_list())
	{
		restore(start, context);
		return false;
	}
	if (!consume_simple(OP_SEMICOLON))
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_empty_declaration()
{
	return consume_simple(OP_SEMICOLON);
}

bool Pa6Parser::parse_attribute_declaration()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!parse_attribute_specifier())
		return false;
	while (parse_attribute_specifier())
	{
	}
	if (!consume_simple(OP_SEMICOLON))
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_block_declaration()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_alias_declaration() || parse_using_declaration() ||
		parse_using_directive() || parse_asm_definition() ||
		parse_namespace_alias_definition() ||
		parse_static_assert_declaration() || parse_simple_declaration())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_static_assert_declaration()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_STATIC_ASSERT) || !enter_bracket(OP_LPAREN) ||
		!parse_constant_expression() || !consume_simple(OP_COMMA) ||
		!consume_literal() || !leave_bracket(OP_RPAREN) ||
		!consume_simple(OP_SEMICOLON))
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_type_name()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_simple_template_id() || parse_class_name() ||
		parse_enum_name() || parse_typedef_name())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_class_name()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_simple_template_id())
		return true;
	restore(start, context);
	if (is_name_category(PA6_NAME_CLASS_FLAG))
	{
		++pos_;
		return true;
	}
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_enum_name()
{
	if (!is_name_category(PA6_NAME_ENUM_FLAG))
		return false;
	++pos_;
	return true;
}

bool Pa6Parser::parse_namespace_name()
{
	if (!is_name_category(PA6_NAME_NAMESPACE_FLAG))
		return false;
	++pos_;
	return true;
}

bool Pa6Parser::parse_template_name()
{
	if (!is_name_category(PA6_NAME_TEMPLATE_FLAG))
		return false;
	++pos_;
	return true;
}

bool Pa6Parser::parse_typedef_name()
{
	if (!is_name_category(PA6_NAME_TYPEDEF_FLAG))
		return false;
	++pos_;
	return true;
}

bool Pa6Parser::parse_simple_template_id()
{
	return try_memoized(MEMO_SIMPLE_TEMPLATE_ID,
		&Pa6Parser::parse_simple_template_id_impl);
}

bool Pa6Parser::parse_simple_template_id_impl()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if ((token(pos_).flags & PA6_NAME_TEMPLATE_FLAG) == 0)
		return false;
	++pos_;
	if (!is_simple(OP_LT))
	{
		restore(start, context);
		return false;
	}
	// A T-category name followed by '<' has committed to a template-id.
	// This is the 14.6/6.8 disambiguation rule used by the course fixtures.
	++pos_;
	brackets_.push_back(BRACKET_ANGLE);
	const bool has_arguments = !is_simple(OP_GT) &&
		!is_kind(PA6_RSHIFT_1_TOKEN);
	if (has_arguments && !parse_template_argument_list())
	{
		restore(start, context);
		hard_failure_ = true;
		return false;
	}
	if (!parse_close_angle_bracket())
	{
		restore(start, context);
		hard_failure_ = true;
		return false;
	}
	return true;
}

bool Pa6Parser::parse_close_angle_bracket()
{
	if (brackets_.empty() || brackets_.back() != BRACKET_ANGLE)
		return false;
	if (is_simple(OP_GT))
	{
		++pos_;
		brackets_.pop_back();
		return true;
	}
	if (is_kind(PA6_RSHIFT_1_TOKEN) || is_kind(PA6_RSHIFT_2_TOKEN))
	{
		++pos_;
		brackets_.pop_back();
		return true;
	}
	return false;
}

bool Pa6Parser::parse_id_expression()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_qualified_id())
		return true;
	if (hard_failure_)
		return false;
	restore(start, context);
	return parse_unqualified_id();
}

bool Pa6Parser::parse_unqualified_id()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (is_simple(KW_OPERATOR))
	{
		if (parse_template_id())
			return true;
		if (hard_failure_)
			return false;
		restore(start, context);
		if (parse_literal_operator_id() || parse_operator_function_id() ||
			parse_conversion_function_id())
			return true;
		restore(start, context);
		return false;
	}
	if (consume_simple(OP_COMPL))
	{
		if (parse_class_name() || parse_decltype_specifier())
			return true;
		restore(start, context);
		return false;
	}
	if (!token(pos_).IsIdentifier())
		return false;
	if ((token(pos_).flags & PA6_NAME_TEMPLATE_FLAG) != 0 &&
		is_simple(OP_LT, pos_ + 1))
	{
		if (parse_simple_template_id())
			return true;
		return false;
	}
	++pos_;
	return true;
}

bool Pa6Parser::parse_qualified_id()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!parse_nested_name_specifier())
		return false;
	if (is_simple(KW_TEMPLATE))
		++pos_;
	if (!parse_unqualified_id())
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_nested_name_specifier()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (consume_simple(OP_COLON2))
	{
		// Root :: is already a complete nested-name-specifier.
	}
	else
	{
		const size_t root = pos_;
		const size_t root_context = brackets_.size();
		if (parse_decltype_specifier() && consume_simple(OP_COLON2))
		{
		}
		else
		{
			restore(root, root_context);
			if (parse_type_name() && consume_simple(OP_COLON2))
			{
			}
			else
			{
				restore(root, root_context);
				if (!parse_namespace_name() || !consume_simple(OP_COLON2))
				{
					restore(start, context);
					return false;
				}
			}
		}
	}

	for (;;)
	{
		const size_t suffix = pos_;
		const size_t suffix_context = brackets_.size();
		if (token(pos_).IsIdentifier() && is_simple(OP_COLON2, pos_ + 1))
		{
			++pos_;
			++pos_;
			continue;
		}
		restore(suffix, suffix_context);
		if (is_simple(KW_TEMPLATE))
		{
			++pos_;
			if (parse_simple_template_id() && consume_simple(OP_COLON2))
				continue;
			if (hard_failure_)
				return false;
			restore(suffix, suffix_context);
		}
		if (parse_simple_template_id() && consume_simple(OP_COLON2))
			continue;
		if (hard_failure_)
			return false;
		restore(suffix, suffix_context);
		break;
	}
	return true;
}

bool Pa6Parser::parse_operator_function_id()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_OPERATOR))
		return false;
	if (consume_simple(KW_NEW) || consume_simple(KW_DELETE))
	{
		const size_t after_key = pos_;
		if (enter_bracket(OP_LSQUARE) && leave_bracket(OP_RSQUARE))
			return true;
		// The non-array form is valid too.
		restore(after_key, context);
		return true;
	}
	if (is_kind(PA6_RSHIFT_1_TOKEN) &&
		is_kind(PA6_RSHIFT_2_TOKEN, pos_ + 1))
	{
		pos_ += 2;
		return true;
	}
	if (token(pos_).kind == PA6_SIMPLE_TOKEN &&
		IsOperatorFunctionToken(token(pos_).simple_type))
	{
		++pos_;
		return true;
	}
	if (consume_simple(OP_LPAREN) && consume_simple(OP_RPAREN))
		return true;
	restore(start, context);
	if (!consume_simple(KW_OPERATOR) || !enter_bracket(OP_LSQUARE) ||
		!leave_bracket(OP_RSQUARE))
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_conversion_function_id()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_OPERATOR) || !parse_type_specifier_seq())
	{
		restore(start, context);
		return false;
	}
	while (parse_ptr_operator())
	{
	}
	return true;
}

bool Pa6Parser::parse_literal_operator_id()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_OPERATOR) || !consume_kind(PA6_LITERAL_TOKEN) ||
		(token(start + 1).flags & PA6_EMPTY_STRING_FLAG) == 0 ||
		!consume_identifier())
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_template_id()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_simple_template_id())
		return true;
	if (hard_failure_)
		return false;
	restore(start, context);
	if (parse_operator_function_id() || parse_literal_operator_id())
	{
		if (is_simple(OP_LT) && parse_template_argument_suffix())
			return true;
		if (is_simple(OP_LT))
			hard_failure_ = true;
	}
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_typename_specifier()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_TYPENAME) || !parse_nested_name_specifier())
	{
		restore(start, context);
		return false;
	}
	if (consume_simple(KW_TEMPLATE))
	{
		if (!parse_simple_template_id())
		{
			restore(start, context);
			return false;
		}
		return true;
	}
	if (!consume_identifier())
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_decl_specifier()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_storage_class_specifier() || parse_function_specifier())
		return true;
	restore(start, context);
	if (parse_type_specifier())
		return true;
	restore(start, context);
	if (consume_simple(KW_FRIEND) || consume_simple(KW_TYPEDEF) ||
		consume_simple(KW_CONSTEXPR))
		return true;
	restore(start, context);
	return false;
}

// Memoized: the seq is re-parsed whenever the function-definition attempt
// falls back to simple-declaration (and likewise per member), so without a
// memo each class-specifier nesting level doubles the parse of its subtree.
bool Pa6Parser::parse_decl_specifier_seq()
{
	return try_memoized(MEMO_DECL_SPECIFIER_SEQ,
		&Pa6Parser::parse_decl_specifier_seq_impl);
}

bool Pa6Parser::parse_decl_specifier_seq_impl()
{
	bool any = false;
	bool committed = false;
	for (;;)
	{
		// README/7.1.6.2p2: a type-name is part of the decl-specifier-seq
		// only if no previous type-specifier other than a cv-qualifier was
		// seen.  An identifier or qualified name here can only match via
		// type-name, so once committed it belongs to the declarator.
		if (committed &&
			(token(pos_).IsIdentifier() || is_simple(OP_COLON2)))
			break;
		const size_t before = pos_;
		if (!parse_decl_specifier())
			break;
		any = true;
		if (token(before).kind != PA6_SIMPLE_TOKEN ||
			!IsNonCommittingSpecifier(token(before).simple_type))
			committed = true;
	}
	while (parse_attribute_specifier())
	{
	}
	return any;
}

bool Pa6Parser::parse_storage_class_specifier()
{
	if (!is_simple(KW_REGISTER) && !is_simple(KW_STATIC) &&
		!is_simple(KW_THREAD_LOCAL) && !is_simple(KW_EXTERN) &&
		!is_simple(KW_MUTABLE))
		return false;
	++pos_;
	return true;
}

bool Pa6Parser::parse_function_specifier()
{
	if (!is_simple(KW_INLINE) && !is_simple(KW_VIRTUAL) &&
		!is_simple(KW_EXPLICIT))
		return false;
	++pos_;
	return true;
}

bool Pa6Parser::parse_type_specifier()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_class_specifier() || parse_enum_specifier())
		return true;
	restore(start, context);
	if (parse_trailing_type_specifier())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_trailing_type_specifier()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_simple_type_specifier() || parse_typename_specifier() ||
		parse_cv_qualifier())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_type_specifier_seq()
{
	if (!parse_type_specifier())
		return false;
	while (parse_type_specifier())
	{
	}
	while (parse_attribute_specifier())
	{
	}
	return true;
}

bool Pa6Parser::parse_trailing_type_specifier_seq()
{
	return parse_type_specifier_seq();
}

bool Pa6Parser::parse_simple_type_specifier()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (is_simple_type_token() && !is_simple(KW_DECLTYPE))
	{
		++pos_;
		return true;
	}
	if (parse_decltype_specifier())
		return true;
	restore(start, context);
	if (parse_elaborated_type_specifier())
		return true;
	restore(start, context);
	if (parse_nested_name_specifier())
	{
		if (consume_simple(KW_TEMPLATE) && parse_simple_template_id())
			return true;
		if (parse_type_name())
			return true;
	}
	restore(start, context);
	if (parse_type_name())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_elaborated_type_specifier()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	const bool class_key = consume_simple(KW_CLASS) ||
		consume_simple(KW_STRUCT) || consume_simple(KW_UNION);
	if (class_key || consume_simple(KW_ENUM))
	{
		while (parse_attribute_specifier())
		{
		}
		const size_t qualified = pos_;
		if (parse_nested_name_specifier())
		{
			if (parse_simple_template_id() || consume_identifier())
				return true;
		}
		restore(qualified, brackets_.size());
		if (parse_simple_template_id() || consume_identifier())
			return true;
	}
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_decltype_specifier()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_DECLTYPE) || !enter_bracket(OP_LPAREN) ||
		!parse_expression() || !leave_bracket(OP_RPAREN))
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_primary_expression()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (consume_simple(KW_TRUE) || consume_simple(KW_FALSE) ||
		consume_simple(KW_NULLPTR) || consume_literal() ||
		consume_simple(KW_THIS))
		return true;
	if (enter_bracket(OP_LPAREN))
	{
		if (parse_expression() && leave_bracket(OP_RPAREN))
			return true;
		restore(start, context);
	}
	if (parse_lambda_expression())
		return true;
	restore(start, context);
	return parse_id_expression();
}

bool Pa6Parser::parse_postfix_expression()
{
	if (!parse_postfix_root())
		return false;
	while (parse_postfix_suffix())
	{
	}
	return true;
}

bool Pa6Parser::parse_postfix_root()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	const ETokenType cast_keywords[] = {
		KW_DYNAMIC_CAST, KW_STATIC_CAST, KW_REINTERPET_CAST, KW_CONST_CAST
	};
	for (size_t i = 0; i < sizeof(cast_keywords) / sizeof(cast_keywords[0]); ++i)
	{
		if (consume_simple(cast_keywords[i]))
		{
			const bool opened_angle = consume_simple(OP_LT);
			if (opened_angle)
				brackets_.push_back(BRACKET_ANGLE);
			if (opened_angle &&
				parse_type_id() &&
				parse_close_angle_bracket() && enter_bracket(OP_LPAREN) &&
				parse_expression() && leave_bracket(OP_RPAREN))
				return true;
			restore(start, context);
		}
	}

	if (consume_simple(KW_TYPEID))
	{
		if (enter_bracket(OP_LPAREN))
		{
			const size_t inside = pos_;
			const size_t inside_context = brackets_.size();
			if (parse_expression() && leave_bracket(OP_RPAREN))
				return true;
			restore(inside, inside_context);
			if (parse_type_id() && leave_bracket(OP_RPAREN))
				return true;
		}
		restore(start, context);
	}

	if (parse_simple_type_specifier())
	{
		const size_t after_type = pos_;
		const size_t after_type_context = brackets_.size();
		if (enter_bracket(OP_LPAREN))
		{
			if ((!is_simple(OP_RPAREN) && parse_expression_list()) ||
				is_simple(OP_RPAREN))
			{
				if (leave_bracket(OP_RPAREN))
					return true;
			}
		}
		restore(after_type, after_type_context);
		if (parse_braced_init_list())
			return true;
		restore(start, context);
	}

	if (parse_typename_specifier())
	{
		const size_t after_type = pos_;
		const size_t after_type_context = brackets_.size();
		if (enter_bracket(OP_LPAREN))
		{
			if ((!is_simple(OP_RPAREN) && parse_expression_list()) ||
				is_simple(OP_RPAREN))
			{
				if (leave_bracket(OP_RPAREN))
					return true;
			}
		}
		restore(after_type, after_type_context);
		if (parse_braced_init_list())
			return true;
		restore(start, context);
	}

	return parse_primary_expression();
}

bool Pa6Parser::parse_postfix_suffix()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (enter_bracket(OP_LSQUARE))
	{
		if (parse_expression() && leave_bracket(OP_RSQUARE))
			return true;
		restore(start, context);
		if (enter_bracket(OP_LSQUARE) && parse_braced_init_list() &&
			leave_bracket(OP_RSQUARE))
			return true;
		restore(start, context);
	}

	if (enter_bracket(OP_LPAREN))
	{
		if (is_simple(OP_RPAREN) || parse_expression_list())
			if (leave_bracket(OP_RPAREN))
				return true;
		restore(start, context);
	}

	if (consume_simple(OP_DOT) || consume_simple(OP_ARROW))
	{
		if (parse_pseudo_destructor_name())
			return true;
		consume_simple(KW_TEMPLATE);
		if (parse_id_expression())
			return true;
		restore(start, context);
	}

	if (consume_simple(OP_INC) || consume_simple(OP_DEC))
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_expression_list()
{
	return parse_initializer_list();
}

bool Pa6Parser::parse_pseudo_destructor_name()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_nested_name_specifier() && consume_simple(OP_COMPL) &&
		parse_type_name())
		return true;
	restore(start, context);
	if (consume_simple(OP_COMPL) && parse_decltype_specifier())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_unary_expression()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_postfix_expression())
		return true;
	restore(start, context);
	if (parse_unary_operator() && parse_cast_expression())
		return true;
	restore(start, context);
	if (consume_simple(KW_SIZEOF))
	{
		const size_t after_sizeof = pos_;
		if (consume_simple(OP_DOTS) && enter_bracket(OP_LPAREN) &&
			consume_identifier() && leave_bracket(OP_RPAREN))
			return true;
		restore(after_sizeof, context);
		if (enter_bracket(OP_LPAREN) && parse_type_id() &&
			leave_bracket(OP_RPAREN))
			return true;
		restore(after_sizeof, context);
		if (parse_unary_expression())
			return true;
		restore(start, context);
	}
	if (consume_simple(KW_ALIGNOF))
	{
		if (enter_bracket(OP_LPAREN) && parse_type_id() &&
			leave_bracket(OP_RPAREN))
			return true;
		restore(start, context);
	}
	if (parse_noexcept_expression() || parse_new_expression() ||
		parse_delete_expression())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_unary_operator()
{
	const ETokenType operators[] = {
		OP_INC, OP_DEC, OP_STAR, OP_AMP, OP_PLUS, OP_MINUS, OP_LNOT, OP_COMPL
	};
	for (size_t i = 0; i < sizeof(operators) / sizeof(operators[0]); ++i)
		if (consume_simple(operators[i]))
			return true;
	return false;
}

bool Pa6Parser::parse_new_expression()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	consume_simple(OP_COLON2);
	if (!consume_simple(KW_NEW))
	{
		restore(start, context);
		return false;
	}
	const size_t after_new = pos_;
	const size_t after_new_context = brackets_.size();
	if (parse_new_placement() && parse_new_type_id())
	{
		if ((!is_simple(OP_LPAREN) && !is_simple(OP_LBRACE)) ||
			parse_new_initializer())
			return true;
	}
	restore(after_new, after_new_context);
	if (parse_new_type_id() &&
		((!is_simple(OP_LPAREN) && !is_simple(OP_LBRACE)) ||
		 parse_new_initializer()))
		return true;
	restore(after_new, after_new_context);
	if (enter_bracket(OP_LPAREN) && parse_type_id() &&
		leave_bracket(OP_RPAREN) &&
		((!is_simple(OP_LPAREN) && !is_simple(OP_LBRACE)) ||
		 parse_new_initializer()))
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_new_placement()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!enter_bracket(OP_LPAREN) || parse_expression_list() == false ||
		!leave_bracket(OP_RPAREN))
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_new_type_id()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!parse_type_specifier_seq())
		return false;
	if (!is_simple(OP_SEMICOLON) && !is_simple(OP_LBRACE) &&
		!is_simple(OP_LPAREN) && !parse_new_declarator())
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_new_declarator()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	bool any = false;
	while (parse_ptr_operator())
		any = true;
	if (parse_noptr_new_declarator())
		return true;
	if (any)
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_noptr_new_declarator()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!enter_bracket(OP_LSQUARE) || !parse_expression() ||
		!leave_bracket(OP_RSQUARE))
	{
		restore(start, context);
		return false;
	}
	while (parse_attribute_specifier())
	{
	}
	for (;;)
	{
		const size_t next = pos_;
		const size_t next_context = brackets_.size();
		if (enter_bracket(OP_LSQUARE) && parse_constant_expression() &&
			leave_bracket(OP_RSQUARE))
		{
			while (parse_attribute_specifier())
			{
			}
			continue;
		}
		restore(next, next_context);
		break;
	}
	return true;
}

bool Pa6Parser::parse_new_initializer()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (enter_bracket(OP_LPAREN))
	{
		if ((is_simple(OP_RPAREN) || parse_expression_list()) &&
			leave_bracket(OP_RPAREN))
			return true;
		restore(start, context);
	}
	if (parse_braced_init_list())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_delete_expression()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	consume_simple(OP_COLON2);
	if (!consume_simple(KW_DELETE))
	{
		restore(start, context);
		return false;
	}
	const size_t after_delete = pos_;
	const size_t after_delete_context = brackets_.size();
	if (enter_bracket(OP_LSQUARE) && leave_bracket(OP_RSQUARE) &&
		parse_cast_expression())
		return true;
	restore(after_delete, after_delete_context);
	if (parse_cast_expression())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_noexcept_expression()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_NOEXCEPT) || !enter_bracket(OP_LPAREN) ||
		!parse_expression() || !leave_bracket(OP_RPAREN))
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_cast_expression()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_cast_operator() && parse_cast_expression())
		return true;
	restore(start, context);
	return parse_unary_expression();
}

bool Pa6Parser::parse_cast_operator()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (enter_bracket(OP_LPAREN) && parse_type_id() &&
		leave_bracket(OP_RPAREN))
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_pm_expression()
{
	const size_t context = brackets_.size();
	if (!parse_cast_expression())
		return false;
	for (;;)
	{
		const size_t operator_position = pos_;
		if (!consume_simple(OP_DOTSTAR) && !consume_simple(OP_ARROWSTAR))
			break;
		if (!parse_cast_expression())
		{
			restore(operator_position, context);
			break;
		}
	}
	return true;
}

bool Pa6Parser::parse_multiplicative_expression()
{
	const size_t context = brackets_.size();
	if (!parse_pm_expression())
		return false;
	for (;;)
	{
		const size_t operator_position = pos_;
		if (!consume_simple(OP_STAR) && !consume_simple(OP_DIV) &&
			!consume_simple(OP_MOD))
			break;
		if (!parse_pm_expression())
		{
			restore(operator_position, context);
			break;
		}
	}
	return true;
}

bool Pa6Parser::parse_additive_expression()
{
	const size_t context = brackets_.size();
	if (!parse_multiplicative_expression())
		return false;
	for (;;)
	{
		const size_t operator_position = pos_;
		if (!consume_simple(OP_PLUS) && !consume_simple(OP_MINUS))
			break;
		if (!parse_multiplicative_expression())
		{
			restore(operator_position, context);
			break;
		}
	}
	return true;
}

bool Pa6Parser::parse_shift_expression()
{
	if (!parse_additive_expression())
		return false;
	for (;;)
	{
		const size_t operator_position = pos_;
		bool consumed = consume_simple(OP_LSHIFT);
		if (!consumed && !has_angle_boundary() &&
			is_kind(PA6_RSHIFT_1_TOKEN) &&
			is_kind(PA6_RSHIFT_2_TOKEN, pos_ + 1))
		{
			pos_ += 2;
			consumed = true;
		}
		if (!consumed)
			break;
		if (!parse_additive_expression())
		{
			restore(operator_position, brackets_.size());
			break;
		}
	}
	return true;
}

bool Pa6Parser::parse_relational_expression()
{
	if (!parse_shift_expression())
		return false;
	for (;;)
	{
		const size_t operator_position = pos_;
		if (has_angle_boundary() && is_simple(OP_GT))
			break;
		if (!consume_simple(OP_LT) && !consume_simple(OP_GT) &&
			!consume_simple(OP_LE) && !consume_simple(OP_GE))
			break;
		if (!parse_shift_expression())
		{
			restore(operator_position, brackets_.size());
			break;
		}
	}
	return true;
}

bool Pa6Parser::parse_equality_expression()
{
	if (!parse_relational_expression())
		return false;
	for (;;)
	{
		const size_t operator_position = pos_;
		if (!consume_simple(OP_EQ) && !consume_simple(OP_NE))
			break;
		if (!parse_relational_expression())
		{
			restore(operator_position, brackets_.size());
			break;
		}
	}
	return true;
}

bool Pa6Parser::parse_and_expression()
{
	if (!parse_equality_expression())
		return false;
	for (;;)
	{
		const size_t operator_position = pos_;
		if (!consume_simple(OP_AMP))
			break;
		if (!parse_equality_expression())
		{
			restore(operator_position, brackets_.size());
			break;
		}
	}
	return true;
}

bool Pa6Parser::parse_exclusive_or_expression()
{
	if (!parse_and_expression())
		return false;
	while (consume_simple(OP_XOR))
	{
		if (!parse_and_expression())
			return false;
	}
	return true;
}

bool Pa6Parser::parse_inclusive_or_expression()
{
	if (!parse_exclusive_or_expression())
		return false;
	while (consume_simple(OP_BOR))
	{
		if (!parse_exclusive_or_expression())
			return false;
	}
	return true;
}

bool Pa6Parser::parse_logical_and_expression()
{
	if (!parse_inclusive_or_expression())
		return false;
	while (consume_simple(OP_LAND))
	{
		if (!parse_inclusive_or_expression())
			return false;
	}
	return true;
}

bool Pa6Parser::parse_logical_or_expression()
{
	if (!parse_logical_and_expression())
		return false;
	while (consume_simple(OP_LOR))
	{
		if (!parse_logical_and_expression())
			return false;
	}
	return true;
}

bool Pa6Parser::parse_conditional_expression()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!parse_logical_or_expression())
		return false;
	if (!consume_simple(OP_QMARK))
		return true;
	if (!parse_expression() || !consume_simple(OP_COLON) ||
		!parse_assignment_expression())
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_assignment_expression()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_conditional_expression())
	{
		if (!is_assignment_operator_token())
			return true;
		if (!parse_assignment_operator() || !parse_initializer_clause())
		{
			restore(start, context);
			return false;
		}
		return true;
	}
	restore(start, context);
	if (consume_simple(KW_THROW))
	{
		if (is_simple(OP_COMMA) || is_simple(OP_COLON) ||
			is_simple(OP_RPAREN) || is_simple(OP_SEMICOLON) ||
			is_simple(OP_RBRACE))
			return true;
		if (parse_assignment_expression())
			return true;
	}
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_assignment_operator()
{
	if (!is_assignment_operator_token())
		return false;
	++pos_;
	return true;
}

bool Pa6Parser::parse_expression()
{
	return try_memoized(MEMO_EXPRESSION, &Pa6Parser::parse_expression_impl);
}

bool Pa6Parser::parse_expression_impl()
{
	if (!parse_assignment_expression())
		return false;
	while (consume_simple(OP_COMMA))
	{
		if (!parse_assignment_expression())
			return false;
	}
	return true;
}

bool Pa6Parser::parse_constant_expression()
{
	return try_memoized(MEMO_CONSTANT_EXPRESSION,
		&Pa6Parser::parse_constant_expression_impl);
}

bool Pa6Parser::parse_constant_expression_impl()
{
	return parse_conditional_expression();
}

bool Pa6Parser::parse_statement()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_declaration_statement())
		return true;
	if (hard_failure_)
		return false;
	restore(start, context);
	if (parse_labeled_statement())
		return true;
	restore(start, context);
	while (parse_attribute_specifier())
	{
	}
	if (parse_expression_statement() ||
		parse_compound_statement() || parse_selection_statement() ||
		parse_iteration_statement() || parse_jump_statement() ||
		parse_try_block())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_labeled_statement()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	while (parse_attribute_specifier())
	{
	}
	if (consume_identifier() && consume_simple(OP_COLON) && parse_statement())
		return true;
	restore(start, context);
	while (parse_attribute_specifier())
	{
	}
	if (consume_simple(KW_CASE) && parse_constant_expression() &&
		consume_simple(OP_COLON) && parse_statement())
		return true;
	restore(start, context);
	while (parse_attribute_specifier())
	{
	}
	if (consume_simple(KW_DEFAULT) && consume_simple(OP_COLON) &&
		parse_statement())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_expression_statement()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (is_simple(OP_SEMICOLON))
	{
		++pos_;
		return true;
	}
	if (parse_expression() && consume_simple(OP_SEMICOLON))
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_compound_statement()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!enter_bracket(OP_LBRACE))
		return false;
	while (!is_simple(OP_RBRACE))
	{
		if (!parse_statement())
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

bool Pa6Parser::parse_selection_statement()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (consume_simple(KW_IF))
	{
		if (enter_bracket(OP_LPAREN) && parse_condition() &&
			leave_bracket(OP_RPAREN) && parse_statement())
		{
			const size_t after_then = pos_;
			const size_t after_then_context = brackets_.size();
			if (consume_simple(KW_ELSE))
			{
				if (!parse_statement())
				{
					restore(start, context);
					return false;
				}
			}
			else
				restore(after_then, after_then_context);
			return true;
		}
		restore(start, context);
	}
	if (consume_simple(KW_SWITCH))
	{
		if (enter_bracket(OP_LPAREN) && parse_condition() &&
			leave_bracket(OP_RPAREN) && parse_statement())
			return true;
	}
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_condition_declaration()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	while (parse_attribute_specifier())
	{
	}
	if (!parse_decl_specifier_seq() || !parse_declarator())
	{
		restore(start, context);
		return false;
	}
	if (consume_simple(OP_ASS) && parse_initializer_clause())
		return true;
	restore(start, context);
	while (parse_attribute_specifier())
	{
	}
	if (parse_decl_specifier_seq() && parse_declarator() &&
		parse_braced_init_list())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_condition()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_condition_declaration())
		return true;
	restore(start, context);
	return parse_expression();
}

bool Pa6Parser::parse_iteration_statement()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (consume_simple(KW_WHILE))
	{
		if (enter_bracket(OP_LPAREN) && parse_condition() &&
			leave_bracket(OP_RPAREN) && parse_statement())
			return true;
		restore(start, context);
	}
	if (consume_simple(KW_DO))
	{
		if (parse_statement() && consume_simple(KW_WHILE) &&
			enter_bracket(OP_LPAREN) && parse_expression() &&
			leave_bracket(OP_RPAREN) && consume_simple(OP_SEMICOLON))
			return true;
		restore(start, context);
	}
	if (!consume_simple(KW_FOR))
	{
		restore(start, context);
		return false;
	}
	if (!enter_bracket(OP_LPAREN))
	{
		restore(start, context);
		return false;
	}
	const size_t for_body = pos_;
	const size_t for_context = brackets_.size();
	if (parse_for_range_declaration() && consume_simple(OP_COLON) &&
		parse_for_range_initializer() && leave_bracket(OP_RPAREN) &&
		parse_statement())
		return true;
	restore(for_body, for_context);
	if (parse_for_init_statement())
	{
		const bool has_condition = !is_simple(OP_SEMICOLON);
		if ((!has_condition || parse_condition()) &&
			consume_simple(OP_SEMICOLON))
		{
			const bool has_increment = !is_simple(OP_RPAREN);
			if ((!has_increment || parse_expression()) &&
				leave_bracket(OP_RPAREN) && parse_statement())
				return true;
		}
	}
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_for_init_statement()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_simple_declaration())
		return true;
	restore(start, context);
	return parse_expression_statement();
}

bool Pa6Parser::parse_for_range_declaration()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	while (parse_attribute_specifier())
	{
	}
	if (parse_decl_specifier_seq() && parse_declarator())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_for_range_initializer()
{
	if (parse_braced_init_list())
		return true;
	return parse_expression();
}

bool Pa6Parser::parse_jump_statement()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if ((consume_simple(KW_BREAK) || consume_simple(KW_CONTINUE)) &&
		consume_simple(OP_SEMICOLON))
		return true;
	restore(start, context);
	if (consume_simple(KW_RETURN))
	{
		const size_t after_return = pos_;
		if (parse_braced_init_list() && consume_simple(OP_SEMICOLON))
			return true;
		restore(after_return, context);
		if ((is_simple(OP_SEMICOLON) || parse_expression()) &&
			consume_simple(OP_SEMICOLON))
			return true;
	}
	restore(start, context);
	if (consume_simple(KW_GOTO) && consume_identifier() &&
		consume_simple(OP_SEMICOLON))
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_declaration_statement()
{
	return parse_block_declaration();
}

bool Pa6Parser::parse_attribute_specifier()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (is_simple(OP_LSQUARE, pos_) && is_simple(OP_LSQUARE, pos_ + 1))
	{
		if (enter_bracket(OP_LSQUARE) && enter_bracket(OP_LSQUARE) &&
			parse_attribute_list() && leave_bracket(OP_RSQUARE) &&
			leave_bracket(OP_RSQUARE))
			return true;
		restore(start, context);
	}
	if (parse_alignment_specifier())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_alignment_specifier()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(KW_ALIGNAS) || !enter_bracket(OP_LPAREN))
	{
		restore(start, context);
		return false;
	}
	const size_t inside = pos_;
	const size_t inside_context = brackets_.size();
	if (parse_type_id())
	{
		consume_simple(OP_DOTS);
		if (leave_bracket(OP_RPAREN))
			return true;
	}
	restore(inside, inside_context);
	if (parse_assignment_expression())
	{
		consume_simple(OP_DOTS);
		if (leave_bracket(OP_RPAREN))
			return true;
	}
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_attribute_list()
{
	if (!parse_attribute_part())
		return false;
	while (consume_simple(OP_COMMA))
	{
		if (!parse_attribute_part())
			return false;
	}
	return true;
}

bool Pa6Parser::parse_attribute_part()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_attribute())
	{
		consume_simple(OP_DOTS);
		return true;
	}
	restore(start, context);
	if (consume_simple(OP_DOTS))
		return true;
	return true;
}

bool Pa6Parser::parse_attribute()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!parse_attribute_token())
		return false;
	if (parse_attribute_argument_clause())
		return true;
	restore(start, context);
	if (parse_attribute_token())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_attribute_token()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_attribute_scoped_token())
		return true;
	restore(start, context);
	return consume_identifier();
}

bool Pa6Parser::parse_attribute_scoped_token()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_identifier() || !consume_simple(OP_COLON2) ||
		!consume_identifier())
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_attribute_argument_clause()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!enter_bracket(OP_LPAREN))
		return false;
	while (!is_simple(OP_RPAREN))
	{
		if (!parse_balanced_token())
		{
			restore(start, context);
			return false;
		}
	}
	if (!leave_bracket(OP_RPAREN))
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_balanced_token()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (enter_bracket(OP_LPAREN))
	{
		while (!is_simple(OP_RPAREN))
			if (!parse_balanced_token())
			{
				restore(start, context);
				return false;
			}
		if (leave_bracket(OP_RPAREN))
			return true;
	}
	restore(start, context);
	if (enter_bracket(OP_LSQUARE))
	{
		while (!is_simple(OP_RSQUARE))
			if (!parse_balanced_token())
			{
				restore(start, context);
				return false;
			}
		if (leave_bracket(OP_RSQUARE))
			return true;
	}
	restore(start, context);
	if (enter_bracket(OP_LBRACE))
	{
		while (!is_simple(OP_RBRACE))
			if (!parse_balanced_token())
			{
				restore(start, context);
				return false;
			}
		if (leave_bracket(OP_RBRACE))
			return true;
	}
	restore(start, context);
	if (at_end() || IsBracketToken(token(pos_).simple_type))
		return false;
	++pos_;
	return true;
}

bool Pa6Parser::parse_init_declarator_list()
{
	if (!parse_init_declarator())
		return false;
	while (consume_simple(OP_COMMA))
	{
		if (!parse_init_declarator())
			return false;
	}
	return true;
}

bool Pa6Parser::parse_init_declarator()
{
	if (!parse_declarator())
		return false;
	const size_t after_declarator = pos_;
	const size_t after_declarator_context = brackets_.size();
	if (!parse_initializer())
		restore(after_declarator, after_declarator_context);
	return true;
}

bool Pa6Parser::parse_declarator()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	bool any_ptr = false;
	while (parse_ptr_operator())
		any_ptr = true;
	if (!parse_noptr_declarator())
	{
		restore(start, context);
		return false;
	}
	// declarator: noptr-declarator trailing-return-type (no ptr prefix).
	// An OP_ARROW can follow a declarator only as a trailing return type.
	if (!any_ptr && is_simple(OP_ARROW) && !parse_trailing_return_type())
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_ptr_declarator()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	while (parse_ptr_operator())
	{
	}
	if (parse_noptr_declarator())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_noptr_declarator()
{
	if (!parse_noptr_declarator_root())
		return false;
	while (parse_noptr_declarator_suffix())
	{
	}
	return true;
}

bool Pa6Parser::parse_noptr_declarator_root()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_declarator_id())
	{
		while (parse_attribute_specifier())
		{
		}
		return true;
	}
	restore(start, context);
	if (enter_bracket(OP_LPAREN) && parse_ptr_declarator() &&
		leave_bracket(OP_RPAREN))
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_noptr_declarator_suffix()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_parameters_and_qualifiers())
		return true;
	restore(start, context);
	if (enter_bracket(OP_LSQUARE))
	{
		if (!is_simple(OP_RSQUARE) && !parse_constant_expression())
		{
			restore(start, context);
			return false;
		}
		if (leave_bracket(OP_RSQUARE))
		{
			while (parse_attribute_specifier())
			{
			}
			return true;
		}
	}
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_parameters_and_qualifiers()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!enter_bracket(OP_LPAREN) || !parse_parameter_declaration_clause() ||
		!leave_bracket(OP_RPAREN))
	{
		restore(start, context);
		return false;
	}
	while (parse_cv_qualifier())
	{
	}
	parse_ref_qualifier();
	parse_exception_specification();
	while (parse_attribute_specifier())
	{
	}
	return true;
}

bool Pa6Parser::parse_trailing_return_type()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!consume_simple(OP_ARROW) || !parse_trailing_type_specifier_seq())
	{
		restore(start, context);
		return false;
	}
	const size_t abstract = pos_;
	const size_t abstract_context = brackets_.size();
	if (!parse_abstract_declarator())
		restore(abstract, abstract_context);
	return true;
}

bool Pa6Parser::parse_ptr_operator()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (consume_simple(OP_STAR))
	{
		while (parse_attribute_specifier())
		{
		}
		while (parse_cv_qualifier())
		{
		}
		return true;
	}
	if (consume_simple(OP_AMP) || consume_simple(OP_LAND))
	{
		while (parse_attribute_specifier())
		{
		}
		return true;
	}
	restore(start, context);
	if (parse_nested_name_specifier() && consume_simple(OP_STAR))
	{
		while (parse_attribute_specifier())
		{
		}
		while (parse_cv_qualifier())
		{
		}
		return true;
	}
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_cv_qualifier()
{
	if (!is_simple(KW_CONST) && !is_simple(KW_VOLATILE))
		return false;
	++pos_;
	return true;
}

bool Pa6Parser::parse_ref_qualifier()
{
	if (!is_simple(OP_AMP) && !is_simple(OP_LAND))
		return false;
	++pos_;
	return true;
}

bool Pa6Parser::parse_declarator_id()
{
	consume_simple(OP_DOTS);
	return parse_id_expression();
}

bool Pa6Parser::parse_type_id()
{
	return try_memoized(MEMO_TYPE_ID, &Pa6Parser::parse_type_id_impl);
}

bool Pa6Parser::parse_type_id_impl()
{
	if (!parse_type_specifier_seq())
		return false;
	const size_t abstract = pos_;
	const size_t abstract_context = brackets_.size();
	if (!parse_abstract_declarator())
		restore(abstract, abstract_context);
	return true;
}

bool Pa6Parser::parse_abstract_declarator()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	// noptr-abstract-declarator? trailing-return-type: longest match first,
	// since a bare noptr parse would strand the OP_ARROW.
	if (parse_noptr_abstract_declarator() && parse_trailing_return_type())
		return true;
	restore(start, context);
	if (parse_trailing_return_type())
		return true;
	restore(start, context);
	if (parse_ptr_abstract_declarator() || parse_abstract_pack_declarator())
		return true;
	restore(start, context);
	if (parse_noptr_abstract_declarator())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_ptr_abstract_declarator()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	bool any = false;
	while (parse_ptr_operator())
		any = true;
	if (!any)
	{
		restore(start, context);
		return false;
	}
	const size_t after_ptr = pos_;
	const size_t after_ptr_context = brackets_.size();
	if (parse_noptr_abstract_declarator())
		return true;
	restore(after_ptr, after_ptr_context);
	return true;
}

bool Pa6Parser::parse_noptr_abstract_declarator()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_noptr_abstract_declarator_root())
	{
		while (parse_noptr_declarator_suffix())
		{
		}
		return true;
	}
	restore(start, context);
	if (!parse_noptr_declarator_suffix())
		return false;
	while (parse_noptr_declarator_suffix())
	{
	}
	return true;
}

bool Pa6Parser::parse_noptr_abstract_declarator_root()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (enter_bracket(OP_LPAREN) && parse_ptr_abstract_declarator() &&
		leave_bracket(OP_RPAREN))
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_abstract_pack_declarator()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	while (parse_ptr_operator())
	{
	}
	if (parse_noptr_abstract_pack_declarator())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_noptr_abstract_pack_declarator()
{
	if (!consume_simple(OP_DOTS))
		return false;
	while (parse_noptr_declarator_suffix())
	{
	}
	return true;
}

bool Pa6Parser::parse_parameter_declaration_clause()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (is_simple(OP_RPAREN))
		return true;
	if (!parse_parameter_declaration_list())
		return false;
	if (consume_simple(OP_DOTS))
		return true;
	if (consume_simple(OP_COMMA))
	{
		if (!consume_simple(OP_DOTS))
		{
			restore(start, context);
			return false;
		}
	}
	return true;
}

bool Pa6Parser::parse_parameter_declaration_list()
{
	if (!parse_parameter_declaration())
		return false;
	while (consume_simple(OP_COMMA))
	{
		const size_t after_comma = pos_;
		if (!parse_parameter_declaration())
		{
			pos_ = after_comma - 1;
			break;
		}
	}
	return true;
}

bool Pa6Parser::parse_parameter_declaration()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	while (parse_attribute_specifier())
	{
	}
	if (!parse_decl_specifier_seq())
	{
		restore(start, context);
		return false;
	}
	const size_t after_specifiers = pos_;
	const size_t after_specifiers_context = brackets_.size();
	if (!parse_declarator())
	{
		restore(after_specifiers, after_specifiers_context);
		parse_abstract_declarator();
	}
	if (consume_simple(OP_ASS))
	{
		if (!parse_initializer_clause())
		{
			restore(start, context);
			return false;
		}
	}
	return true;
}

bool Pa6Parser::parse_initializer()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_brace_or_equal_initializer())
		return true;
	restore(start, context);
	if (enter_bracket(OP_LPAREN) && parse_expression_list() &&
		leave_bracket(OP_RPAREN))
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_brace_or_equal_initializer()
{
	if (consume_simple(OP_ASS))
		return parse_initializer_clause();
	return parse_braced_init_list();
}

bool Pa6Parser::parse_initializer_clause()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (parse_braced_init_list())
		return true;
	restore(start, context);
	if (parse_assignment_expression())
		return true;
	restore(start, context);
	return false;
}

bool Pa6Parser::parse_initializer_list()
{
	if (!parse_initializer_clause_dots())
		return false;
	while (consume_simple(OP_COMMA))
	{
		const size_t after_comma = pos_;
		if (is_simple(OP_RBRACE))
		{
			pos_ = after_comma - 1;
			break;
		}
		if (!parse_initializer_clause_dots())
			return false;
	}
	return true;
}

bool Pa6Parser::parse_initializer_clause_dots()
{
	if (!parse_initializer_clause())
		return false;
	consume_simple(OP_DOTS);
	return true;
}

bool Pa6Parser::parse_braced_init_list()
{
	const size_t start = pos_;
	const size_t context = brackets_.size();
	if (!enter_bracket(OP_LBRACE))
		return false;
	if (leave_bracket(OP_RBRACE))
		return true;
	if (!parse_initializer_list())
	{
		restore(start, context);
		return false;
	}
	consume_simple(OP_COMMA);
	if (!leave_bracket(OP_RBRACE))
	{
		restore(start, context);
		return false;
	}
	return true;
}

bool Pa6Parser::parse_template_argument_list()
{
	if (!parse_template_argument_dots())
		return false;
	while (consume_simple(OP_COMMA))
	{
		if (!parse_template_argument_dots())
			return false;
	}
	return true;
}
