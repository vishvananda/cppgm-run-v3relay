#include "ast_model.h"

#include <ostream>
#include <stdexcept>

namespace
{

const char* Names[] =
{
	"invalid",
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
	"delete-expression",
	"array-delete",
	"lambda-expression",
	"lambda-introducer",
	"lambda-declarator",
	"lambda-specifier",
	"braced-init-expression",
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
	"member-declaration",
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
	"template-template-parameter",
	"non-type-template-parameter",
	"default-template-argument",
	"default-argument",
	"template-argument-list",
	"template-argument",
	"pack-expansion",
	"noexcept",
	"trailing-type",
	"unknown",
	"inline",
	"virtual",
	"enum-key"
};

} // namespace

const char* AstKindName(AstKind kind)
{
	const std::size_t count = sizeof(Names) / sizeof(Names[0]);
	const std::size_t index = static_cast<std::size_t>(kind);
	return index < count ? Names[index] : "unknown";
}

AstArena::AstArena()
	: nodes_(1, AstNode())
{
}

AstId AstArena::Make(AstKind kind, std::string text)
{
	nodes_.push_back(AstNode(kind, text));
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

void AstArena::SetSpan(AstId id, std::size_t first, std::size_t last)
{
	AstNode& node = At(id);
	node.first = first;
	node.last = last;
}

void AstArena::Add(AstId parent, AstId child)
{
	if (child != 0)
		At(parent).children.push_back(child);
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
