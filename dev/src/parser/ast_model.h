#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

typedef std::size_t AstId;

// PA10 deliberately keeps the syntax tree independent of later semantic
// passes.  Structure lives in kind and children.  The token spellings that the
// --emit-ast dump prints live in text, rendered once by the parser; a node
// whose text was rendered from source tokens also records that token range so
// later passes classify names from the typed tokens instead of splitting text.
enum AstKind
{
	AST_TRANSLATION_UNIT,
	AST_SIMPLE_DECLARATION,
	AST_EMPTY_DECLARATION,
	AST_FUNCTION_DEFINITION,
	AST_DECL_SPECIFIER_SEQ,
	AST_DECL_SPECIFIER,
	AST_INIT_DECLARATOR_LIST,
	AST_INIT_DECLARATOR,
	AST_DECLARATOR,
	AST_NESTED_DECLARATOR,
	AST_IDENTIFIER,
	AST_PARAMETER_CLAUSE,
	AST_PARAMETER_DECLARATION,
	AST_PARAMETER_PACK,
	AST_ABSTRACT_DECLARATOR,
	AST_PTR_OPERATOR,
	AST_ARRAY_SUFFIX,
	AST_CV_QUALIFIER,
	AST_REF_QUALIFIER,
	AST_FUNCTION_QUALIFIER,
	AST_NOEXCEPT_SPECIFICATION,
	AST_VIRT_SPECIFIER,
	AST_TRAILING_RETURN_TYPE,
	AST_TYPE_ID,
	AST_TYPE_SPECIFIER_SEQ,
	AST_TYPE_SPECIFIER,
	AST_TYPE_NAME,
	AST_DECLTYPE_SPECIFIER,
	AST_INITIALIZER,
	AST_BRACED_INIT_LIST,
	AST_PAREN_INITIALIZER,
	AST_SPECIAL_INITIALIZER,
	AST_COMPOUND_STATEMENT,
	AST_EXPRESSION_STATEMENT,
	AST_IF_STATEMENT,
	AST_THEN,
	AST_ELSE,
	AST_WHILE_STATEMENT,
	AST_DO_STATEMENT,
	AST_FOR_STATEMENT,
	AST_FOR_INIT_STATEMENT,
	AST_ITERATION,
	AST_SWITCH_STATEMENT,
	AST_CASE_STATEMENT,
	AST_DEFAULT_STATEMENT,
	AST_LABELED_STATEMENT,
	AST_GOTO_STATEMENT,
	AST_BREAK_STATEMENT,
	AST_CONTINUE_STATEMENT,
	AST_RETURN_STATEMENT,
	AST_THROW_STATEMENT,
	AST_TRY_BLOCK,
	AST_HANDLER,
	AST_EXCEPTION_DECLARATION,
	AST_ELLIPSIS,
	AST_CONDITION,
	AST_CONDITION_DECLARATION,
	AST_BINARY_EXPRESSION,
	AST_ASSIGNMENT_EXPRESSION,
	AST_CONDITIONAL_EXPRESSION,
	AST_UNARY_EXPRESSION,
	AST_POSTFIX_EXPRESSION,
	AST_CALL_EXPRESSION,
	AST_ARGUMENT_LIST,
	AST_PAREN_ARGUMENT_LIST,
	AST_SUBSCRIPT_EXPRESSION,
	AST_MEMBER_EXPRESSION,
	AST_PARENTHESIZED_EXPRESSION,
	AST_CAST_EXPRESSION,
	AST_SIZEOF_EXPRESSION,
	AST_TYPE_TRAIT_EXPRESSION,
	AST_NEW_EXPRESSION,
	AST_GLOBAL_SCOPE,
	AST_DELETE_EXPRESSION,
	AST_ARRAY_DELETE,
	AST_LAMBDA_EXPRESSION,
	AST_LAMBDA_INTRODUCER,
	AST_LAMBDA_DECLARATOR,
	AST_LAMBDA_SPECIFIER,
	AST_PACK_EXPANSION_EXPRESSION,
	AST_ID_EXPRESSION,
	AST_LITERAL,
	AST_KEYWORD_LITERAL,
	AST_ALIAS_DECLARATION,
	AST_STATIC_ASSERT_DECLARATION,
	AST_MESSAGE,
	AST_PLACEMENT,
	AST_TARGET,
	AST_CLASS_SPECIFIER,
	AST_CLASS_KEY,
	AST_CLASS_FORWARD_DECLARATION,
	AST_BASE_CLAUSE,
	AST_BASE_SPECIFIER,
	AST_BASE_NAME,
	AST_ACCESS_SPECIFIER,
	AST_MEMBER_SPECIFIERS,
	AST_SPECIFIER,
	AST_SPECIAL_MEMBER_DECLARATION,
	AST_SPECIAL_MEMBER_DEFINITION,
	AST_CTOR_INITIALIZER,
	AST_MEM_INITIALIZER,
	AST_MEM_INITIALIZER_ID,
	AST_BIT_FIELD_DECLARATION,
	AST_BIT_FIELD_DECLARATOR,
	AST_ENUM_SPECIFIER,
	// enum-key name enum-base? without a body: an opaque-enum-declaration or
	// an elaborated enum specifier; sema decides by context.  Printed like an
	// enum-specifier.
	AST_ENUM_DECLARATION,
	AST_ENUMERATOR,
	AST_NAMESPACE_DEFINITION,
	AST_NAMESPACE_ALIAS_DEFINITION,
	AST_USING_DIRECTIVE,
	AST_USING_DECLARATION,
	AST_LINKAGE_SPECIFICATION,
	AST_EXPLICIT_INSTANTIATION_DECLARATION,
	AST_TEMPLATE_DECLARATION,
	AST_TEMPLATE_PARAMETER_CLAUSE,
	AST_TEMPLATE_PARAMETER_LIST,
	AST_TYPE_PARAMETER,
	AST_PARAMETER_KEY,
	AST_TEMPLATE_TEMPLATE_PARAMETER,
	AST_NON_TYPE_TEMPLATE_PARAMETER,
	AST_DEFAULT_TEMPLATE_ARGUMENT,
	AST_DEFAULT_ARGUMENT,
	AST_TEMPLATE_ARGUMENT_LIST,
	AST_TEMPLATE_ARGUMENT,
	AST_INLINE,
	AST_VIRTUAL,
	AST_ENUM_KEY,
	AST_KIND_COUNT
};

const char* AstKindName(AstKind kind);

struct AstNode
{
	AstKind kind;
	std::string text;
	std::vector<AstId> children;
	// Half-open token range [first, last) the node was built from; both zero
	// for structural nodes and for text that has no source tokens.
	std::size_t first;
	std::size_t last;

	AstNode(AstKind kind, const std::string& text, std::size_t first,
		std::size_t last)
		: kind(kind), text(text), first(first), last(last)
	{
	}
};

// Append-only node store; id 0 is the null node.
class AstArena
{
public:
	AstArena();

	AstId Make(AstKind kind, const std::string& text, std::size_t first,
		std::size_t last);
	AstNode& At(AstId id);
	const AstNode& At(AstId id) const;

	void Add(AstId parent, AstId child);

	// Token extent of the whole declaration that a bare unnamed class or enum
	// specifier forms.  Such a node has no name span; later passes derive a
	// stable identity for the unnamed type from this extent.  Cold sidecar:
	// recorded only for those rare nodes.
	void RecordDeclarationExtent(AstId id, std::size_t first, std::size_t last);
	bool DeclarationExtent(AstId id, std::size_t& first, std::size_t& last) const;

private:
	struct Extent
	{
		AstId node;
		std::size_t first;
		std::size_t last;
	};

	std::vector<AstNode> nodes_;
	std::vector<Extent> extents_;
};

void PrintAst(std::ostream& out, const AstArena& arena, AstId root,
	unsigned depth);
void PrintHeader(std::ostream& out, std::size_t n);
void PrintUnit(std::ostream& out, std::size_t k, const AstArena& arena,
	AstId root);
