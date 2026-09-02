#include "ast_model.h"

#include <ostream>
#include <stdexcept>

namespace
{

const char* const Names[AST_KIND_COUNT] =
{
	"translation-unit",
	"simple-declaration",
	"empty-declaration",
	"function-definition",
	"decl-specifier-seq",
	"decl-specifier",
	"init-declarator-list",
	"init-declarator",
	"declarator",
	"nested-declarator",
	"identifier",
	"parameter-clause",
	"parameter-declaration",
	"parameter-pack",
	"abstract-declarator",
	"ptr-operator",
	"array-suffix",
	"cv-qualifier",
	"ref-qualifier",
	"function-qualifier",
	"noexcept-specification",
	"virt-specifier",
	"trailing-return-type",
	"type-id",
	"type-specifier-seq",
	"type-specifier",
	"type-name",
	"decltype-specifier",
	"initializer",
	"braced-init-list",
	"paren-initializer",
	"special-initializer",
	"compound-statement",
	"expression-statement",
	"if-statement",
	"then",
	"else",
	"while-statement",
	"do-statement",
	"for-statement",
	"for-init-statement",
	"iteration",
	"switch-statement",
	"case-statement",
	"default-statement",
	"labeled-statement",
	"goto-statement",
	"break-statement",
	"continue-statement",
	"return-statement",
	"throw-statement",
	"try-block",
	"handler",
	"exception-declaration",
	"ellipsis",
	"condition",
	"condition-declaration",
	"binary-expression",
	"assignment-expression",
	"conditional-expression",
	"unary-expression",
	"postfix-expression",
	"call-expression",
	"argument-list",
	"paren-argument-list",
	"subscript-expression",
	"member-expression",
	"parenthesized-expression",
	"cast-expression",
	"sizeof-expression",
	"type-trait-expression",
	"new-expression",
	"global-scope",
	"delete-expression",
	"array-delete",
	"lambda-expression",
	"lambda-introducer",
	"lambda-declarator",
	"lambda-specifier",
	"pack-expansion-expression",
	"id-expression",
	"literal",
	"keyword-literal",
	"alias-declaration",
	"static-assert-declaration",
	"message",
	"placement",
	"target",
	"class-specifier",
	"class-key",
	"class-forward-declaration",
	"base-clause",
	"base-specifier",
	"base-name",
	"access-specifier",
	"member-specifiers",
	"specifier",
	"special-member-declaration",
	"special-member-definition",
	"ctor-initializer",
	"mem-initializer",
	"mem-initializer-id",
	"bit-field-declaration",
	"bit-field-declarator",
	"enum-specifier",
	"enum-specifier",
	"enumerator",
	"namespace-definition",
	"namespace-alias-definition",
	"using-directive",
	"using-declaration",
	"linkage-specification",
	"explicit-instantiation-declaration",
	"template-declaration",
	"template-parameter-clause",
	"template-parameter-list",
	"type-parameter",
	"parameter-key",
	"template-template-parameter",
	"non-type-template-parameter",
	"default-template-argument",
	"default-argument",
	"template-argument-list",
	"template-argument",
	"inline",
	"virtual",
	"enum-key"
};

} // namespace

const char* AstKindName(AstKind kind)
{
	const std::size_t index = static_cast<std::size_t>(kind);
	return index < AST_KIND_COUNT ? Names[index] : "invalid";
}

AstArena::AstArena()
	: nodes_(1, AstNode(AST_KIND_COUNT, std::string(), 0, 0))
{
}

AstId AstArena::Make(AstKind kind, const std::string& text, std::size_t first,
	std::size_t last)
{
	nodes_.push_back(AstNode(kind, text, first, last));
	return nodes_.size() - 1;
}

AstNode& AstArena::At(AstId id)
{
	if (id == 0 || id >= nodes_.size())
		throw std::out_of_range("invalid AST id");
	return nodes_[id];
}

const AstNode& AstArena::At(AstId id) const
{
	if (id == 0 || id >= nodes_.size())
		throw std::out_of_range("invalid AST id");
	return nodes_[id];
}

void AstArena::Add(AstId parent, AstId child)
{
	if (child != 0)
		At(parent).children.push_back(child);
}

void AstArena::RecordDeclarationExtent(AstId id, std::size_t first,
	std::size_t last)
{
	Extent extent;
	extent.node = id;
	extent.first = first;
	extent.last = last;
	extents_.push_back(extent);
}

bool AstArena::DeclarationExtent(AstId id, std::size_t& first,
	std::size_t& last) const
{
	for (std::size_t i = 0; i < extents_.size(); ++i)
		if (extents_[i].node == id)
		{
			first = extents_[i].first;
			last = extents_[i].last;
			return true;
		}
	return false;
}

void PrintAst(std::ostream& out, const AstArena& arena, AstId root,
	unsigned depth)
{
	if (root == 0)
		return;
	const AstNode& node = arena.At(root);
	for (unsigned i = 0; i < depth; ++i)
		out << "  ";
	out << AstKindName(node.kind);
	if (!node.text.empty())
		out << ' ' << node.text;
	out << '\n';
	for (std::size_t i = 0; i < node.children.size(); ++i)
		PrintAst(out, arena, node.children[i], depth + 1);
}

void PrintHeader(std::ostream& out, std::size_t n)
{
	out << n << " translation units\n";
}

void PrintUnit(std::ostream& out, std::size_t k, const AstArena& arena,
	AstId root)
{
	out << "start translation unit " << k << "\n";
	PrintAst(out, arena, root, 0);
	out << "end translation unit\n";
}
