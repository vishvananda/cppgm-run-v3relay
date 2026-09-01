#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "recog_token.h"

class Pa6Parser
{
public:
	explicit Pa6Parser(const std::vector<Pa6Token>& tokens);

	bool ParseTranslationUnit();

private:
	enum BracketKind
	{
		BRACKET_PAREN,
		BRACKET_SQUARE,
		BRACKET_BRACE,
		BRACKET_ANGLE
	};

	// Single authority for memo rule ids shared by all parser translation
	// units.  Every memoized rule must key on (rule, position, angle refusal)
	// and be net-zero on the bracket stack.
	enum MemoRule
	{
		MEMO_SIMPLE_TEMPLATE_ID = 1,
		MEMO_EXPRESSION,
		MEMO_CONSTANT_EXPRESSION,
		MEMO_TYPE_ID,
		MEMO_TEMPLATE_ARGUMENT,
		MEMO_DECL_SPECIFIER_SEQ
	};

	struct MemoEntry
	{
		bool ok;
		std::size_t end;
	};

	bool parse_translation_unit();
	bool parse_declaration();
	bool parse_function_definition();
	bool parse_function_body();
	bool parse_simple_declaration();
	bool parse_empty_declaration();
	bool parse_attribute_declaration();
	bool parse_block_declaration();
	bool parse_static_assert_declaration();

	bool parse_primary_expression();
	bool parse_id_expression();
	bool parse_unqualified_id();
	bool parse_qualified_id();
	bool parse_nested_name_specifier();
	bool parse_simple_template_id();
	bool parse_simple_template_id_impl();
	bool parse_close_angle_bracket();
	bool parse_type_name();
	bool parse_class_name();
	bool parse_enum_name();
	bool parse_namespace_name();
	bool parse_template_name();
	bool parse_typedef_name();

	bool parse_postfix_expression();
	bool parse_postfix_root();
	bool parse_postfix_suffix();
	bool parse_expression_list();
	bool parse_pseudo_destructor_name();
	bool parse_unary_expression();
	bool parse_unary_operator();
	bool parse_new_expression();
	bool parse_new_placement();
	bool parse_new_type_id();
	bool parse_new_declarator();
	bool parse_noptr_new_declarator();
	bool parse_new_initializer();
	bool parse_delete_expression();
	bool parse_noexcept_expression();
	bool parse_cast_expression();
	bool parse_cast_operator();
	bool parse_pm_expression();
	bool parse_multiplicative_expression();
	bool parse_additive_expression();
	bool parse_shift_expression();
	bool parse_relational_expression();
	bool parse_equality_expression();
	bool parse_and_expression();
	bool parse_exclusive_or_expression();
	bool parse_inclusive_or_expression();
	bool parse_logical_and_expression();
	bool parse_logical_or_expression();
	bool parse_conditional_expression();
	bool parse_assignment_expression();
	bool parse_assignment_operator();
	bool parse_expression();
	bool parse_expression_impl();
	bool parse_constant_expression();
	bool parse_constant_expression_impl();

	bool parse_statement();
	bool parse_labeled_statement();
	bool parse_expression_statement();
	bool parse_compound_statement();
	bool parse_selection_statement();
	bool parse_condition_declaration();
	bool parse_condition();
	bool parse_iteration_statement();
	bool parse_for_init_statement();
	bool parse_for_range_declaration();
	bool parse_for_range_initializer();
	bool parse_jump_statement();
	bool parse_declaration_statement();

	bool parse_decl_specifier();
	bool parse_decl_specifier_seq();
	bool parse_decl_specifier_seq_impl();
	bool parse_storage_class_specifier();
	bool parse_function_specifier();
	bool parse_type_specifier();
	bool parse_trailing_type_specifier();
	bool parse_type_specifier_seq();
	bool parse_trailing_type_specifier_seq();
	bool parse_simple_type_specifier();
	bool parse_elaborated_type_specifier();
	bool parse_decltype_specifier();

	bool parse_attribute_specifier();
	bool parse_alignment_specifier();
	bool parse_attribute_list();
	bool parse_attribute_part();
	bool parse_attribute();
	bool parse_attribute_token();
	bool parse_attribute_scoped_token();
	bool parse_attribute_argument_clause();
	bool parse_balanced_token();

	bool parse_init_declarator_list();
	bool parse_init_declarator();
	bool parse_declarator();
	bool parse_ptr_declarator();
	bool parse_noptr_declarator();
	bool parse_noptr_declarator_root();
	bool parse_noptr_declarator_suffix();
	bool parse_parameters_and_qualifiers();
	bool parse_trailing_return_type();
	bool parse_ptr_operator();
	bool parse_cv_qualifier();
	bool parse_ref_qualifier();
	bool parse_declarator_id();
	bool parse_type_id();
	bool parse_type_id_impl();
	bool parse_abstract_declarator();
	bool parse_ptr_abstract_declarator();
	bool parse_noptr_abstract_declarator();
	bool parse_noptr_abstract_declarator_root();
	bool parse_abstract_pack_declarator();
	bool parse_noptr_abstract_pack_declarator();
	bool parse_parameter_declaration_clause();
	bool parse_parameter_declaration_list();
	bool parse_parameter_declaration();

	bool parse_initializer();
	bool parse_brace_or_equal_initializer();
	bool parse_initializer_clause();
	bool parse_initializer_list();
	bool parse_initializer_clause_dots();
	bool parse_braced_init_list();

	bool parse_operator_function_id();
	bool parse_conversion_function_id();
	bool parse_literal_operator_id();
	bool parse_template_id();
	bool parse_template_argument_list();
	bool parse_template_argument_dots();
	bool parse_template_argument();
	bool parse_template_argument_impl();
	bool parse_template_argument_suffix();
	bool parse_template_parameter_list();
	bool parse_template_parameter();
	bool parse_type_parameter();
	bool parse_typename_specifier();

	bool parse_class_specifier();
	bool parse_enum_specifier();
	bool parse_namespace_definition();
	bool parse_namespace_alias_definition();
	bool parse_template_declaration();
	bool parse_explicit_instantiation();
	bool parse_explicit_specialization();
	bool parse_linkage_specification();
	bool parse_alias_declaration();
	bool parse_using_declaration();
	bool parse_using_directive();
	bool parse_asm_definition();
	bool parse_exception_specification();
	bool parse_lambda_expression();
	bool parse_base_clause();
	bool parse_base_specifier_list();
	bool parse_base_specifier();
	bool parse_member_declaration();
	bool parse_ctor_initializer();
	bool parse_mem_initializer_list();
	bool parse_mem_initializer();
	bool parse_try_block();
	bool parse_handler();
	bool parse_exception_declaration();

	const Pa6Token& token(std::size_t at) const;
	bool at_end() const;
	bool is_simple(ETokenType type, std::size_t at = static_cast<std::size_t>(-1)) const;
	bool is_kind(Pa6TokenKind kind, std::size_t at = static_cast<std::size_t>(-1)) const;
	bool consume_simple(ETokenType type);
	bool consume_kind(Pa6TokenKind kind);
	bool consume_identifier();
	bool consume_literal();
	bool enter_bracket(ETokenType type);
	bool leave_bracket(ETokenType type);
	bool has_angle_boundary() const;
	void restore(std::size_t position, std::size_t brackets);
	bool try_memoized(unsigned rule_id, bool (Pa6Parser::*implementation)());
	std::uint64_t memo_key(unsigned rule_id) const;
	bool is_name_category(unsigned category) const;
	bool is_assignment_operator_token() const;
	bool is_simple_type_token() const;

	const std::vector<Pa6Token>& tokens_;
	std::size_t pos_;
	std::vector<BracketKind> brackets_;
	std::unordered_map<std::uint64_t, MemoEntry> memo_;
	bool angle_refusal_;
	bool hard_failure_;
};
