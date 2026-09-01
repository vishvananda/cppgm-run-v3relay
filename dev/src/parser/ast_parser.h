#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast_model.h"
#include "ast_scope.h"
#include "recog_token.h"

class Pa10Parser
{
public:
	Pa10Parser(const std::vector<Pa6Token>& tokens, AstArena& arena);

	AstId ParseTranslationUnit();

private:
	enum BracketKind
	{
		BRACKET_PAREN,
		BRACKET_SQUARE,
		BRACKET_BRACE,
		BRACKET_ANGLE
	};

	struct Mark
	{
		std::size_t position;
		std::size_t brackets;
		std::size_t scope;
	};

	struct MemoEntry
	{
		AstId node;
		std::size_t end;
	};

	typedef AstId (Pa10Parser::*AstRule)();

	// Translation units, declarations and statements.
	AstId parse_translation_unit();
	AstId parse_declaration();
	AstId parse_template_declaration();
	AstId parse_explicit_instantiation_declaration();
	AstId parse_function_definition();
	AstId parse_special_member_definition();
	AstId parse_simple_declaration();
	AstId parse_empty_declaration();
	AstId parse_static_assert_declaration();
	AstId parse_alias_declaration();
	AstId parse_namespace_definition();
	AstId parse_namespace_alias_definition();
	AstId parse_linkage_specification();
	AstId parse_using_directive();
	AstId parse_using_declaration();
	AstId parse_class_specifier(bool declaration_context = false);
	AstId parse_base_clause();
	AstId parse_member_declaration(const std::string& class_name);
	AstId parse_special_member_declaration(const std::string& class_name);
	AstId parse_enum_specifier();
	AstId parse_bit_field_declaration();
	AstId parse_ctor_initializer();
	AstId parse_mem_initializer();
	AstId parse_throw_specification();
	bool parse_member_function_suffixes(AstId declarator);
	AstId parse_block_declaration();
	AstId parse_statement();
	AstId parse_labeled_statement();
	AstId parse_expression_statement();
	AstId parse_compound_statement();
	AstId parse_selection_statement();
	AstId parse_condition();
	AstId parse_iteration_statement();
	AstId parse_for_init_statement();
	AstId parse_jump_statement();
	AstId parse_declaration_statement();
	AstId parse_try_block();
	AstId parse_handler();
	AstId parse_exception_declaration();

	// Declaration and type rules.
	AstId parse_decl_specifier();
	AstId parse_decl_specifier_seq();
	AstId parse_type_specifier_seq();
	AstId parse_type_specifier();
	AstId parse_decltype_specifier(bool type_context);
	AstId parse_type_id(bool allow_function_abstract = true);
	AstId parse_declarator(bool allow_abstract = false);
	AstId parse_ptr_operator();
	AstId parse_declarator_id();
	AstId parse_parameter_clause();
	AstId parse_parameter_declaration();
	AstId parse_init_declarator_list();
	AstId parse_init_declarator();
	AstId parse_initializer();
	AstId parse_braced_init_list();
	AstId parse_paren_initializer();
	AstId parse_trailing_return_type();
	AstId parse_function_suffixes(AstId declarator);
	AstId parse_qualified_name(bool declarator_name);
	AstId parse_nested_name_specifier();
	AstId parse_simple_template_id(bool qualified = false);
	AstId parse_template_argument_list();
	AstId parse_template_argument();
	AstId parse_template_parameter_clause();
	AstId parse_template_parameter_list();
	AstId parse_template_parameter();
	AstId parse_type_template_parameter();
	AstId parse_template_template_parameter();
	AstId parse_non_type_template_parameter();
	AstId parse_operator_function_id();
	AstId parse_literal_operator_id();

	// Expression rules, in precedence order.
	AstId parse_expression();
	AstId parse_assignment_expression();
	AstId parse_conditional_expression();
	AstId parse_logical_or_expression();
	AstId parse_logical_and_expression();
	AstId parse_inclusive_or_expression();
	AstId parse_exclusive_or_expression();
	AstId parse_and_expression();
	AstId parse_equality_expression();
	AstId parse_relational_expression();
	AstId parse_shift_expression();
	AstId parse_additive_expression();
	AstId parse_multiplicative_expression();
	AstId parse_pm_expression();
	AstId parse_cast_expression();
	AstId parse_unary_expression();
	AstId parse_postfix_expression();
	AstId parse_postfix_suffixes(AstId expression);
	AstId parse_pack_expansion(AstId expression);
	AstId parse_postfix_root();
	AstId parse_primary_expression();
	AstId parse_id_expression();
	AstId parse_argument_list(AstKind kind);
	AstId parse_lambda_expression();
	AstId parse_new_expression();
	AstId parse_delete_expression();
	AstId parse_type_trait_expression();

	// Small parser primitives shared by all translation units.
	const Pa6Token& token(std::size_t at) const;
	bool at_end() const;
	bool is_simple(ETokenType type, std::size_t at = static_cast<std::size_t>(-1)) const;
	bool is_kind(Pa6TokenKind kind, std::size_t at = static_cast<std::size_t>(-1)) const;
	bool consume_simple(ETokenType type);
	bool consume_kind(Pa6TokenKind kind);
	bool enter_bracket(ETokenType type);
	bool leave_bracket(ETokenType type);
	bool has_angle_boundary() const;
	bool parse_close_angle_bracket();
	void restore(const Mark& mark);
	Mark mark() const;
	AstId try_memoized(unsigned rule, AstRule implementation);
	std::uint64_t memo_key(unsigned rule) const;

	AstId make(AstKind kind, const std::string& text = "");
	AstId make(AstKind kind, const std::string& text,
		const std::vector<AstId>& children);
	void add(AstId parent, AstId child);
	AstId leaf(AstKind kind, const Pa6Token& tok);
	std::string Join(std::size_t first, std::size_t last) const;
	std::string Concat(std::size_t first, std::size_t last) const;
	std::string token_label(const Pa6Token& tok) const;
	std::string operator_name(std::size_t first, std::size_t last) const;

	bool is_builtin_type(ETokenType type) const;
	bool is_cv_qualifier(ETokenType type) const;
	bool is_storage_or_function_specifier(ETokenType type) const;
	bool is_keyword_literal(ETokenType type) const;
	bool is_assignment_operator(ETokenType type) const;
	bool can_start_expression() const;
	bool can_start_declaration() const;
	bool consume_attribute_specifiers();
	bool node_has_kind(AstId node, AstKind kind) const;
	void collect_identifier_names(AstId node, std::vector<std::string>& names) const;
	void bind_declarator(AstId declarator, BindKind kind);
	void bind_parameters(AstId declarator);
	void bind_template_declaration(AstId declaration);

	const std::vector<Pa6Token>& tokens_;
	AstArena& arena_;
	std::size_t pos_;
	std::vector<BracketKind> brackets_;
	SyntaxScopes scopes_;
	std::unordered_map<std::uint64_t, MemoEntry> memo_;
	bool angle_refusal_;
	bool hard_failure_;
};
