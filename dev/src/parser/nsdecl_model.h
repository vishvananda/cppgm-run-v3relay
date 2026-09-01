#pragma once

#include <map>
#include <memory>
#include <ostream>
#include <limits>
#include <string>
#include <vector>

#include "posttoken_types.h"

struct Pa8Expr;
struct Pa8Value;
typedef std::shared_ptr<Pa8Expr> Pa8ExprPtr;
typedef std::shared_ptr<Pa8Value> Pa8ValuePtr;

enum Pa8ExprKind
{
	PA8_EXPR_LITERAL,
	PA8_EXPR_IDENTIFIER,
	PA8_EXPR_UNARY,
	PA8_EXPR_BINARY,
	PA8_EXPR_CONDITIONAL
};

enum Pa7TypeKind
{
	PA7_TYPE_FUNDAMENTAL,
	PA7_TYPE_CV,
	PA7_TYPE_POINTER,
	PA7_TYPE_LVALUE_REFERENCE,
	PA7_TYPE_RVALUE_REFERENCE,
	PA7_TYPE_ARRAY,
	PA7_TYPE_FUNCTION
};

enum Pa7CV
{
	PA7_CV_NONE = 0,
	PA7_CV_CONST = 1u << 0,
	PA7_CV_VOLATILE = 1u << 1
};

struct Pa7Type
{
	Pa7TypeKind kind;
	EFundamentalType fundamental;
	unsigned cv;
	bool has_bound;
	unsigned long long bound;
	Pa8ExprPtr bound_expression;
	bool varargs;
	std::vector<std::shared_ptr<Pa7Type> > children;
	std::shared_ptr<Pa7Type> return_type;

	Pa7Type();
};

typedef std::shared_ptr<Pa7Type> Pa7TypePtr;

Pa7TypePtr MakeFundamental(EFundamentalType type);
Pa7TypePtr ApplyCV(unsigned cv, const Pa7TypePtr& type);
Pa7TypePtr MakePointer(const Pa7TypePtr& inner);
Pa7TypePtr MakeReference(bool is_rvalue, const Pa7TypePtr& inner);
Pa7TypePtr MakeArray(bool has_bound, unsigned long long bound,
	const Pa7TypePtr& element,
	const Pa8ExprPtr& bound_expression = Pa8ExprPtr());
Pa7TypePtr MakeFunction(const std::vector<Pa7TypePtr>& params, bool varargs,
	const Pa7TypePtr& ret);

std::string DescribeType(const Pa7TypePtr& type);
bool IsVoid(const Pa7TypePtr& type);
bool IsFunction(const Pa7TypePtr& type);
Pa7TypePtr AdjustParameter(const Pa7TypePtr& type);
bool SameType(const Pa7TypePtr& left, const Pa7TypePtr& right);
Pa7TypePtr MergeTypes(const Pa7TypePtr& first, const Pa7TypePtr& second);

struct Pa7Namespace;
struct Pa7Variable;
struct Pa7Function;
struct Pa7Typedef;

enum Pa7DeclKind
{
	PA7_DECL_VARIABLE,
	PA7_DECL_FUNCTION,
	PA7_DECL_TYPEDEF,
	PA7_DECL_NAMESPACE
};

enum Pa7DeclOrigin
{
	PA7_DECL_OWNED,
	PA7_DECL_NAMESPACE_ALIAS,
	PA7_DECL_USING
};

enum Pa7StorageFlags
{
	PA7_STORAGE_NONE = 0,
	PA7_STORAGE_STATIC = 1u << 0,
	PA7_STORAGE_EXTERN = 1u << 1,
	PA7_STORAGE_THREAD_LOCAL = 1u << 2
};

enum Pa7Linkage
{
	PA7_LINKAGE_UNSPECIFIED,
	PA7_LINKAGE_INTERNAL,
	PA7_LINKAGE_EXTERNAL
};

struct Pa7DeclAttributes
{
	unsigned storage;
	Pa7Linkage linkage;
	bool linkage_explicit;
	bool is_const;
	bool is_constexpr;
	bool is_inline;
	bool defined;
	std::size_t order;

	Pa7DeclAttributes();
};

enum Pa7DeclFilter
{
	PA7_FIND_ANY = 0,
	PA7_FIND_VARIABLE = 1u << 0,
	PA7_FIND_FUNCTION = 1u << 1,
	PA7_FIND_TYPE = 1u << 2,
	PA7_FIND_NAMESPACE = 1u << 3
};

struct Pa7Decl
{
	Pa7DeclKind kind;
	Pa7DeclOrigin origin;
	std::shared_ptr<Pa7Variable> variable;
	std::shared_ptr<Pa7Function> function;
	std::vector<std::shared_ptr<Pa7Function> > function_overloads;
	std::shared_ptr<Pa7Typedef> typedef_entity;
	// Namespaces are owned by the parent's member vectors; declaration map
	// entries (including aliases and using imports) only reference them.
	Pa7Namespace* namespace_entity;

	Pa7Decl();
	bool Matches(unsigned filter) const;
};

struct Pa7Variable
{
	std::string name;
	Pa7TypePtr type;
	Pa7Namespace* owner;
	unsigned storage;
	Pa7Linkage linkage;
	bool is_const;
	bool is_constexpr;
	bool defined;
	std::size_t order;
	Pa8ExprPtr initializer_expression;
	Pa8ValuePtr initializer;

	Pa7Variable(const std::string& name, const Pa7TypePtr& type,
		Pa7Namespace* owner = 0);
};

struct Pa7Function
{
	std::string name;
	Pa7TypePtr type;
	Pa7Namespace* owner;
	unsigned storage;
	Pa7Linkage linkage;
	bool is_inline;
	bool defined;
	std::size_t order;

	Pa7Function(const std::string& name, const Pa7TypePtr& type,
		Pa7Namespace* owner = 0);
};

struct Pa7Typedef
{
	std::string name;
	Pa7TypePtr type;

	Pa7Typedef(const std::string& name, const Pa7TypePtr& type);
};

struct Pa7Namespace
{
	std::string name;
	Pa7Namespace* parent;
	bool inline_namespace;
	bool unnamed;

	std::map<std::string, Pa7Decl> declarations;
	std::vector<std::shared_ptr<Pa7Variable> > variables;
	std::vector<std::shared_ptr<Pa7Function> > functions;
	std::vector<std::shared_ptr<Pa7Namespace> > namespaces;
	std::vector<Pa7Namespace*> using_directives;
	std::shared_ptr<Pa7Namespace> unnamed_child;
	std::vector<Pa8ExprPtr> static_assertions;

	Pa7Namespace(const std::string& name = std::string(),
		Pa7Namespace* parent = 0, bool inline_namespace = false,
		bool unnamed = false);

	Pa7Decl* FindDirect(const std::string& name,
		unsigned filter = PA7_FIND_ANY);
	const Pa7Decl* FindDirect(const std::string& name,
		unsigned filter = PA7_FIND_ANY) const;
	// 3.4.3.2p2 first stage: this namespace and its transitive inline set.
	const Pa7Decl* FindDirectOrInline(const std::string& name,
		unsigned filter = PA7_FIND_ANY) const;
	Pa7Namespace* AddNamespace(const std::string& name,
		bool inline_namespace = false);
	Pa7Namespace* AddUnnamedNamespace(bool inline_namespace = false);
	void AddNamespaceAlias(const std::string& name, Pa7Namespace* target);
	void AddUsingDirective(Pa7Namespace* target);
	void AddUsingDeclaration(const std::string& name, const Pa7Decl& source);
	std::shared_ptr<Pa7Variable> AddOrMergeVariable(const std::string& name,
		const Pa7TypePtr& type,
		const Pa7DeclAttributes& attributes = Pa7DeclAttributes(),
		bool strict = false);
	std::shared_ptr<Pa7Function> AddOrMergeFunction(const std::string& name,
		const Pa7TypePtr& type,
		const Pa7DeclAttributes& attributes = Pa7DeclAttributes(),
		bool strict = false);
	std::shared_ptr<Pa7Typedef> AddTypedef(const std::string& name,
		const Pa7TypePtr& type);
};

void PrintTranslationUnit(std::ostream& out, const Pa7Namespace& global);
